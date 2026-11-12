#include <visionpro/workflow/workflow_engine.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/workflow/execution_plan.hpp>
#include "control_schedule.hpp"
#include "node_state_service.hpp"
#include "observation_hub.hpp"

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace visionpro::workflow {
namespace {
using namespace core;

Diagnostic error(ErrorCode code, std::string message, const RunRequest& request, std::optional<NodeId> node = {})
{
    DiagnosticLocation location;
    location.solution = request.snapshot().solution().id;
    location.workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    location.node = node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

Diagnostic query_error(ErrorCode code, const RunIdentity& run, const InvocationKey& invocation, std::string message)
{
    DiagnosticLocation location;
    location.workflow = run.workflow;
    location.node = invocation.node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

Diagnostic call_failure(Diagnostic diagnostic, const RunRequest& request, NodeId node)
{
    diagnostic.location.solution = request.snapshot().solution().id;
    diagnostic.location.workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    diagnostic.location.node = node;
    diagnostic.severity = Severity::Error;
    if (!is_valid_diagnostic(diagnostic)) {
        diagnostic.message = "Execution failed with an invalid diagnostic.";
        diagnostic.external.reset();
    }
    return diagnostic;
}

bool terminal(ExecutionStatus status)
{ return status != ExecutionStatus::Pending && status != ExecutionStatus::Running; }

NodeSkipReason stop_reason(ExecutionStatus status)
{
    switch (status) {
    case ExecutionStatus::Cancelled: return NodeSkipReason::RunCancelled;
    case ExecutionStatus::TimedOut: return NodeSkipReason::RunTimedOut;
    default: return NodeSkipReason::FailFast;
    }
}

ControlCloseReason close_reason(ExecutionStatus status)
{
    switch (status) {
    case ExecutionStatus::Cancelled: return ControlCloseReason::Cancelled;
    case ExecutionStatus::TimedOut: return ControlCloseReason::TimedOut;
    default: return ControlCloseReason::Failed;
    }
}

Result<NodeResult> terminal_record(const PreparedInvocation& invocation, ToolCompletionStatus status,
    std::vector<Diagnostic> diagnostics = {})
{
    return checked_node_result(invocation, {invocation.context.data().identity, status,
        ToolJudgement::NotEvaluated, {}, std::move(diagnostics)});
}

struct SchedulerEvents {
    std::mutex mutex;
    std::condition_variable changed;
};

// Each active attempt has one coalescing notification flag. The operation owns
// the progress, so callbacks do not need to allocate event payloads.
class RunWakeup final : public InvocationWakeup {
public:
    explicit RunWakeup(const std::shared_ptr<SchedulerEvents>& events) : events_(events) {}

    void signal(const ToolInvocationIdentity& identity, InvocationWaitTarget) noexcept override
    {
        if (const auto events = events_.lock()) {
            const std::lock_guard lock(events->mutex);
            const auto found = pending_.find(identity);
            if (found != pending_.end()) {
                found->second = true;
                ready_ = true;
                events->changed.notify_one();
            }
        }
    }

    void request() noexcept
    {
        if (const auto events = events_.lock()) {
            const std::lock_guard lock(events->mutex);
            ready_ = true;
            events->changed.notify_one();
        }
    }

    void expect(const ToolInvocationIdentity& identity)
    {
        if (const auto events = events_.lock()) {
            const std::lock_guard lock(events->mutex);
            pending_.emplace(identity, false);
        }
    }

    bool consume(const ToolInvocationIdentity& identity)
    {
        if (const auto events = events_.lock()) {
            const std::lock_guard lock(events->mutex);
            const auto found = pending_.find(identity);
            return found != pending_.end() && std::exchange(found->second, false);
        }
        return false;
    }

    void forget(const ToolInvocationIdentity& identity)
    {
        if (const auto events = events_.lock()) {
            const std::lock_guard lock(events->mutex);
            pending_.erase(identity);
        }
    }

    // The scheduler calls these with the event mutex held.
    bool ready() const noexcept { return ready_; }
    void clear() noexcept { ready_ = false; }
private:
    std::weak_ptr<SchedulerEvents> events_;
    std::map<ToolInvocationIdentity, bool> pending_;
    bool ready_{true};
};

enum class CallDecision { Waiting, Advance, Stop, Cancel };

class EngineRun final : public RunControl {
public:
    EngineRun(const RunRequest& request, WorkflowRunId id, detail::ControlSchedule schedule,
        const std::shared_ptr<SchedulerEvents>& events, const std::shared_ptr<detail::ObservationHub>& observers)
        : RunControl(request, id), wakeup_(std::make_shared<RunWakeup>(events)), observers_(observers),
          schedule_(std::move(schedule)),
          remaining_(schedule_.nodes().size()), eligible_(remaining_), results_(identity().snapshot.session)
    {
        progress_.nodes.reserve(remaining_);
        for (const auto& node : schedule_.nodes()) { progress_.nodes.push_back({node}); }
        apply_skips();
    }

    Result<RunProgress> inspect() const override
    {
        const std::lock_guard lock(mutex_);
        return snapshot();
    }

    Result<RunWaitResult> wait(RunWaitTarget target, std::chrono::steady_clock::time_point deadline) override
    {
        std::unique_lock lock(mutex_);
        const bool ready = changed_.wait_until(lock, deadline, [&] {
            return target == RunWaitTarget::Completion ? terminal(progress_.status) : progress_.quiescent;
        });
        return RunWaitResult{ready ? RunWaitOutcome::Satisfied : RunWaitOutcome::DeadlineExpired, snapshot()};
    }

    Result<RunCancelDisposition> cancel() override
    {
        {
            const std::lock_guard lock(mutex_);
            if (terminal(progress_.status)) { return RunCancelDisposition::AlreadyCompleted; }
            if (cancel_requested_ || stopping_) { return RunCancelDisposition::AlreadyRequested; }
            cancel_requested_ = true;
        }
        wakeup_->request();
        return RunCancelDisposition::Requested;
    }

    Result<std::optional<NodeResult>> lookup(const InvocationKey& invocation) const override
    {
        const std::lock_guard lock(mutex_);
        return results_.lookup(identity(), invocation);
    }

    // Only the worker mutates this store. Its own binding reads can overlap
    // client reads; publication takes the same lock as lookup and cancellation.
    const ResultStore& results_for_binding() const noexcept { return results_; }
    const std::shared_ptr<RunWakeup>& wakeup() const noexcept { return wakeup_; }
    std::span<const NodeId> nodes() const noexcept { return schedule_.nodes(); }

    void begin_step()
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (progress_.status == ExecutionStatus::Pending) { progress_.status = ExecutionStatus::Running; }
        finish_if_done();
    }

    bool can_continue()
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        return !stopping_ && !terminal(progress_.status);
    }

    bool stopping() const
    {
        const std::lock_guard lock(mutex_);
        return stopping_.has_value();
    }

    bool finished() const
    {
        const std::lock_guard lock(mutex_);
        return terminal(progress_.status);
    }

    bool has_ready() const
    {
        const std::lock_guard lock(mutex_);
        return !stopping_ && !terminal(progress_.status) && schedule_.has_ready();
    }

    std::optional<std::size_t> take_ready()
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        return stopping_ || terminal(progress_.status) ? std::nullopt : schedule_.take_ready();
    }

    ResolutionContext context_for(std::size_t index, const InvocationKey& key) const
    {
        const std::lock_guard lock(mutex_);
        const auto globals = request().globals();
        ResolutionContext context{identity(), key, {{key.scope, {}, {}, {}}}, {}, {}, {globals.begin(), globals.end()}};
        for (const auto producer : schedule_.producers(index)) {
            const auto& source = progress_.nodes[producer];
            if (const auto absent = schedule_.source_absence(producer)) {
                context.sources.push_back({source.node, key.scope, *absent});
            } else if (source.invocation) {
                context.sources.push_back({source.node, key.scope, source.invocation->invocation});
            } else {
                context.sources.push_back({source.node, key.scope, AwaitingSource{}});
            }
        }
        return context;
    }

    void wait_for_data(std::size_t index, std::vector<RunBindingTrace> bindings)
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_) { return; }
        progress_.nodes[index].bindings = std::move(bindings);
        schedule_.wait_for_data(index);
    }

    bool start_call(std::size_t index, const ToolInvocationIdentity& invocation, std::vector<RunBindingTrace> bindings)
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_ || terminal(progress_.status)) { return false; }
        auto& node = progress_.nodes[index];
        node.bindings = std::move(bindings);
        node.invocation = invocation.invocation;
        node.status = ExecutionStatus::Running;
        eligible_[index] = invocation;
        return true;
    }

    void preparation_failed(std::size_t index, Diagnostic diagnostic, std::vector<RunBindingTrace> bindings = {})
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_) { return; }
        progress_.nodes[index].bindings = std::move(bindings);
        fail_step(index, std::move(diagnostic));
    }

    CallDecision accept_completion(std::size_t index, const PreparedInvocation& invocation,
        detail::InvocationStateLease* state, std::optional<Result<NodeResult>> projected)
    {
        const std::lock_guard lock(mutex_);
        // Eligibility, cancellation, publication and the successor's control
        // permission change in one critical section.
        const auto& call = invocation.context.data().identity;
        if (eligible_[index] != call) {
            const auto& node = progress_.nodes[index];
            if (node.invocation == call.invocation && terminal(node.status)) { return CallDecision::Stop; }
            return CallDecision::Waiting;
        }
        observe_cancellation();
        const bool stopped = stopping_.has_value();
        if (stopped) { projected = terminal_record(invocation, ToolCompletionStatus::Cancelled); }
        if (!projected) { return CallDecision::Waiting; }
        if (auto* failure = std::get_if<Diagnostic>(&*projected)) {
            if (state) { state->settle(false); }
            fail_step(index, std::move(*failure));
            return CallDecision::Stop;
        }

        const auto& result = std::get<NodeResult>(*projected);
        auto diagnostics = result.data().diagnostics;
        for (const auto& output : result.data().outputs) {
            if (const auto* invalid = std::get_if<InvalidOutput>(&output.content)) {
                diagnostics.push_back(invalid->diagnostic);
            }
        }
        for (auto& diagnostic : diagnostics) {
            if (!diagnostic.location.solution) { diagnostic.location.solution = request().snapshot().solution().id; }
            if (!diagnostic.location.workflow) { diagnostic.location.workflow = identity().workflow; }
            if (!diagnostic.location.node) { diagnostic.location.node = progress_.nodes[index].node; }
        }
        progress_.diagnostics.reserve(progress_.diagnostics.size() + diagnostics.size());
        if (auto committed = results_.insert(request().snapshot(), result);
            auto* failure = std::get_if<Diagnostic>(&committed)) {
            if (state) { state->settle(false); }
            fail_step(index, std::move(*failure));
            return CallDecision::Stop;
        }
        const auto status = result.data().status;
        if (state) { state->settle(status == ExecutionStatus::Succeeded); }
        for (auto& diagnostic : diagnostics) { progress_.diagnostics.push_back(std::move(diagnostic)); }
        eligible_[index].reset();
        progress_.nodes[index].status = status;
        --remaining_;
        publish(RunObservationKind::ResultPublished, status, result.data().identity);
        if (status == ExecutionStatus::Succeeded) { schedule_.source_ready(index); }
        else { stop(status); }
        finish_if_done();
        if (stopped) { return CallDecision::Cancel; }
        return status == ExecutionStatus::Succeeded ? CallDecision::Advance : CallDecision::Stop;
    }

    void complete_control(std::size_t index, const ToolInvocationIdentity& call)
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_) { return; }
        const auto found = results_.lookup(identity(), call.invocation);
        const auto* record = std::get_if<std::optional<NodeResult>>(&found);
        if (!record || !*record || (*record)->data().identity.attempt != call.attempt) {
            throw std::logic_error("The completed control source has no accepted result.");
        }
        schedule_.complete(index, (*record)->data().selected_control_outputs);
        apply_skips();
        finish_if_done();
    }

    void report_cancellation_error(Diagnostic diagnostic, NodeId node)
    {
        const std::lock_guard lock(mutex_);
        progress_.diagnostics.push_back(call_failure(std::move(diagnostic), request(), node));
    }

    void unexpected_failure()
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (terminal(progress_.status)) { return; }
        progress_.diagnostics.push_back(error(ErrorCode::ExecutionBoundaryFailure,
            "The workflow worker raised an unexpected exception.", request()));
        stop(ExecutionStatus::Failed);
        finish_if_done();
    }

    void blocked()
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (terminal(progress_.status)) { return; }
        progress_.diagnostics.push_back(error(ErrorCode::InvalidResolutionContext,
            "The workflow still has unresolved nodes and no active or ready producers.", request()));
        stop(ExecutionStatus::Failed);
        finish_if_done();
    }

    void mark_quiescent()
    {
        const std::lock_guard lock(mutex_);
        progress_.quiescent = true;
        publish(RunObservationKind::RunQuiescent, progress_.status);
        changed_.notify_all();
    }

private:
    // These transitions run with mutex_ held, except initialization before
    // the new run is published.
    RunProgress snapshot() const
    {
        auto result = progress_;
        const auto links = schedule_.links();
        result.control_links.assign(links.begin(), links.end());
        return result;
    }

    void observe_cancellation()
    {
        if (cancel_requested_) {
            stop(ExecutionStatus::Cancelled);
            finish_if_done();
        }
    }

    void fail_step(std::size_t index, Diagnostic diagnostic)
    {
        auto& node = progress_.nodes[index];
        progress_.diagnostics.push_back(call_failure(std::move(diagnostic), request(), node.node));
        node.status = ExecutionStatus::Failed;
        eligible_[index].reset();
        --remaining_;
        stop(ExecutionStatus::Failed);
        finish_if_done();
    }

    void apply_skips()
    {
        for (const auto& [index, reason] : schedule_.take_skipped()) {
            auto& node = progress_.nodes[index];
            node.status = ExecutionStatus::Skipped;
            node.skip_reason = reason;
            --remaining_;
        }
    }

    void stop(ExecutionStatus status)
    {
        if (stopping_ || terminal(progress_.status)) { return; }
        stopping_ = status;
        schedule_.stop(close_reason(status));
        for (auto& node : progress_.nodes) {
            if (node.status != ExecutionStatus::Pending) { continue; }
            node.status = ExecutionStatus::Skipped;
            node.skip_reason = stop_reason(status);
            --remaining_;
        }
    }

    void finish_if_done()
    {
        if (remaining_ != 0 || terminal(progress_.status)) { return; }
        progress_.status = stopping_.value_or(ExecutionStatus::Succeeded);
        publish(RunObservationKind::RunCompleted, progress_.status);
        changed_.notify_all();
    }

    void publish(RunObservationKind kind, ExecutionStatus status, std::optional<ResultIdentity> result = {}) noexcept
    {
        if (const auto observers = observers_.lock()) { observers->publish(identity(), kind, status, result); }
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    const std::shared_ptr<RunWakeup> wakeup_;
    const std::weak_ptr<detail::ObservationHub> observers_;
    detail::ControlSchedule schedule_;
    RunProgress progress_;
    bool cancel_requested_{false};
    std::optional<ExecutionStatus> stopping_;
    std::size_t remaining_;
    std::vector<std::optional<ToolInvocationIdentity>> eligible_;
    ResultStore results_;
};

std::vector<RunBindingTrace> trace_bindings(const InvocationPreparation& preparation)
{
    const auto& bindings = std::visit([](const auto& value) -> const std::vector<TargetResolution>& {
        return value.bindings;
    }, preparation);
    std::vector<RunBindingTrace> traces;
    traces.reserve(bindings.size());
    for (const auto& binding : bindings) {
        traces.push_back({binding.target, std::visit([](const auto& value) { return value.sources; }, binding.resolution)});
    }
    return traces;
}

Diagnostic preparation_error(const InvocationPreparation& preparation, const RunRequest& request, NodeId node)
{
    if (const auto* failure = std::get_if<InvocationFailure>(&preparation)) { return failure->diagnostic; }
    if (const auto* unavailable = std::get_if<UnavailableInvocation>(&preparation)) {
        for (const auto& binding : unavailable->bindings) {
            if (const auto* source = std::get_if<UnavailableBinding>(&binding.resolution)) { return source->diagnostic; }
        }
    }
    return error(ErrorCode::InvalidResolutionContext, "Invocation preparation produced an unexpected state.", request, node);
}

Result<NodeResult> failed_call(const RunRequest& request, const PreparedInvocation& invocation, Diagnostic diagnostic)
{
    const auto node = invocation.context.data().identity.invocation.node;
    return terminal_record(invocation, ToolCompletionStatus::Failed, {call_failure(std::move(diagnostic), request, node)});
}

struct ActiveCall {
    PreparedInvocation invocation;
    std::shared_ptr<detail::InvocationStateLease> state{};
    std::optional<InvocationOperation> operation{};
    CallDecision decision{CallDecision::Waiting};
};

struct Work {
    Work(ExecutionPlan configured, std::shared_ptr<EngineRun> backend)
        : plan(std::move(configured)), run(std::move(backend)), issued(run->nodes().size()) {}

    ExecutionPlan plan;
    std::shared_ptr<EngineRun> run;
    IdSequence<InvocationId> calls;
    std::vector<std::optional<InvocationId>> issued;
    std::map<std::size_t, ActiveCall> active_calls;
};

enum class WorkStep { Parked, Ready, Finished };

bool settle_call(EngineRun& run, std::size_t index, ActiveCall& call)
{
    const auto& identity = call.invocation.context.data().identity;
    if (call.decision == CallDecision::Waiting) {
        std::optional<Result<NodeResult>> projected;
        if (call.operation && !run.stopping()) {
            const auto progress = call.operation->poll();
            if (progress.completion) {
                auto result = checked_node_result(call.invocation, *progress.completion);
                if (auto* failure = std::get_if<Diagnostic>(&result)) {
                    result = failed_call(run.request(), call.invocation, std::move(*failure));
                }
                projected = std::move(result);
            }
        }
        call.decision = run.accept_completion(index, call.invocation, call.state.get(), std::move(projected));
    }
    if (call.decision == CallDecision::Cancel) {
        call.decision = CallDecision::Stop;
        const auto& policy = call.invocation.context.data().configuration.data().tool.execution;
        if (call.operation && policy.cancellation == ToolCancellation::Cooperative) {
            if (auto cancelled = call.operation->request_cancel(); auto* failure = std::get_if<Diagnostic>(&cancelled)) {
                run.report_cancellation_error(std::move(*failure), identity.invocation.node);
            }
        }
    }
    return !call.operation || (call.decision != CallDecision::Waiting && call.operation->physically_exited());
}

void settle_notified(Work& work)
{
    auto& run = *work.run;
    const auto& wakeup = run.wakeup();
    for (auto next = work.active_calls.begin(); next != work.active_calls.end();) {
        const auto current = next++;
        auto& [index, call] = *current;
        const auto identity = call.invocation.context.data().identity;
        const bool notified = wakeup->consume(identity);
        if (call.operation && !notified && !(run.stopping() && call.decision == CallDecision::Waiting)) { continue; }
        if (!settle_call(run, index, call)) { continue; }
        const auto source = index;
        const bool advance = call.decision == CallDecision::Advance;
        wakeup->forget(identity);
        work.active_calls.erase(current); // Releases the prepared inputs and leases.
        if (advance) { run.complete_control(source, identity); }
    }
}

Result<InvocationOperation> launch_call(ActiveCall& call, std::shared_ptr<ToolInstance> instance,
    const RunRequest& request, detail::NodeStateService& states)
{
    auto& invocation = call.invocation;
    const auto& configuration = invocation.context.data().configuration.data();
    const auto& flow = *find_workflow(invocation.snapshot.solution(), configuration.workflow);
    const auto& node = *find_node(flow, configuration.node);
    auto acquired = states.acquire(invocation.context, node, request.debug_state() == DebugStatePolicy::Isolated, *instance);
    if (auto* failure = std::get_if<Diagnostic>(&acquired)) { return std::move(*failure); }
    call.state = std::get<std::shared_ptr<detail::InvocationStateLease>>(std::move(acquired));
    if (call.state) {
        auto data = invocation.context.data();
        data.state = call.state;
        auto fixed = InvocationContext::create(data);
        if (auto* failure = std::get_if<Diagnostic>(&fixed)) { return std::move(*failure); }
        invocation.context = std::get<InvocationContext>(std::move(fixed));
    }
    return InvocationOperation::start(std::move(instance), invocation.context);
}

void start_node(Work& work, std::size_t index, InvocationResourceProvider* resources, detail::NodeStateService& states)
{
    auto& run = *work.run;
    const auto& request = work.plan.request();
    const auto node = run.nodes()[index];
    const auto root = std::get<ExecutionScopeId>(RuntimeIdFactory::from_value<ExecutionScopeId>(0));
    const auto attempt = std::get<AttemptId>(RuntimeIdFactory::from_value<AttemptId>(0));
    auto& issued = work.issued[index];
    if (!issued) {
        auto next = work.calls.next();
        if (auto* failure = std::get_if<Diagnostic>(&next)) {
            run.preparation_failed(index, std::move(*failure));
            return;
        }
        issued = std::get<InvocationId>(next);
    }
    const auto context = run.context_for(index, {run.identity().run, node, root, *issued});
    const auto& tool = *work.plan.tool(node);
    auto prepared = resources ? tool.prepare(context, attempt, run.results_for_binding(), *resources)
                              : tool.prepare(context, attempt, run.results_for_binding());
    auto trace = trace_bindings(prepared);
    if (std::holds_alternative<PendingInvocation>(prepared)) {
        run.wait_for_data(index, std::move(trace));
        return;
    }
    auto* invocation = std::get_if<PreparedInvocation>(&prepared);
    if (!invocation) {
        run.preparation_failed(index, preparation_error(prepared, request, node), std::move(trace));
        return;
    }
    if (!run.can_continue()) { return; }
    auto created = work.plan.create_instance(node);
    if (auto* failure = std::get_if<Diagnostic>(&created)) {
        run.preparation_failed(index, std::move(*failure), std::move(trace));
        return;
    }
    const auto [entry, inserted] = work.active_calls.emplace(index, ActiveCall{std::move(*invocation)});
    if (!inserted) { throw std::logic_error("The node already owns an active invocation."); }
    auto& call = entry->second;
    const auto identity = call.invocation.context.data().identity;
    const auto& wakeup = run.wakeup();
    wakeup->expect(identity);
    if (!run.start_call(index, identity, std::move(trace))) {
        wakeup->forget(identity);
        work.active_calls.erase(entry);
        return;
    }
    auto started = launch_call(call, std::get<std::shared_ptr<ToolInstance>>(std::move(created)), request, states);
    if (auto* failure = std::get_if<Diagnostic>(&started)) {
        auto result = failed_call(request, call.invocation, std::move(*failure));
        static_cast<void>(run.accept_completion(index, call.invocation, call.state.get(), std::move(result)));
        wakeup->forget(identity);
        work.active_calls.erase(entry);
        return;
    }
    call.operation = std::get<InvocationOperation>(std::move(started));
    // Register exit first so logical publication failures still leave a way
    // to observe physical cleanup. Already reached events signal immediately.
    const auto exit = call.operation->notify_when(InvocationWaitTarget::PhysicalExit, wakeup);
    const auto completion = call.operation->notify_when(InvocationWaitTarget::LogicalCompletion, wakeup);
    const auto* failure = std::get_if<Diagnostic>(&exit);
    if (!failure) { failure = std::get_if<Diagnostic>(&completion); }
    if (failure) {
        call.decision = run.accept_completion(index, call.invocation, call.state.get(),
            failed_call(request, call.invocation, *failure));
        wakeup->signal(identity, InvocationWaitTarget::LogicalCompletion);
    }
}

WorkStep execute_step(Work& work, InvocationResourceProvider* resources, detail::NodeStateService& states)
{
    auto& run = *work.run;
    run.begin_step();
    settle_notified(work);
    if (const auto ready = run.take_ready()) { start_node(work, *ready, resources, states); }
    if (run.has_ready() || (run.stopping() && std::ranges::any_of(work.active_calls,
        [](const auto& entry) { return entry.second.decision == CallDecision::Waiting; }))) {
        return WorkStep::Ready;
    }
    if (!work.active_calls.empty()) { return WorkStep::Parked; }
    if (!run.finished()) { run.blocked(); }
    return WorkStep::Finished;
}
} // namespace

struct WorkflowEngine::Impl {
    Impl(SolutionSessionId id, const ToolRegistry& tools, std::shared_ptr<InvocationResourceProvider> provider,
        WorkflowEngineOptions options)
        : session(id), registry(tools), resources(std::move(provider)), capabilities(profile(resources.get())),
          history(options.completed_run_limit),
          worker([this] { work_loop(); }) {}

    ~Impl() { static_cast<void>(close()); }

    Result<std::monostate> close()
    {
        if (worker_engine == this) {
            return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Cannot wait for session closure on its workflow worker."};
        }
        {
            const std::lock_guard lock(events->mutex);
            if (!stopping) {
                stopping = true;
                observers->close();
                events->changed.notify_one();
            }
        }
        std::call_once(joined, [this] { worker.join(); });
        return std::monostate{};
    }

    RunSubmission submit(const RunRequest& request, WorkflowId workflow)
    {
        std::shared_ptr<InvocationResourceProvider> provider;
        {
            const std::lock_guard lock(events->mutex);
            if (auto refusal = check_slot(request, workflow)) { return std::move(*refusal); }
            provider = resources;
        }
        auto planned = ExecutionPlan::create(request, registry, capabilities);
        if (auto* failure = std::get_if<Diagnostic>(&planned)) { return RunRejected{std::move(*failure)}; }
        auto plan = std::get<ExecutionPlan>(std::move(planned));
        if (provider) {
            auto checked = provider->preflight(plan);
            if (auto* failure = std::get_if<Diagnostic>(&checked)) { return RunRejected{std::move(*failure)}; }
        }
        detail::ControlSchedule schedule(plan);

        const std::lock_guard lock(events->mutex);
        // Planning does not reserve a slot. Another submit may have won it.
        if (auto refusal = check_slot(request, workflow)) { return std::move(*refusal); }
        const auto next = last_admitted ? RuntimeIdFactory::next(*last_admitted)
                                        : RuntimeIdFactory::from_value<WorkflowRunId>(1);
        if (const auto* failure = std::get_if<Diagnostic>(&next)) { return RunRejected{*failure}; }
        auto run = std::make_shared<EngineRun>(plan.request(), std::get<WorkflowRunId>(next), std::move(schedule), events, observers);
        auto handle = RunHandle::from_accepted(run);
        if (auto* failure = std::get_if<Diagnostic>(&handle)) { return RunRejected{std::move(*failure)}; }

        auto work = std::make_unique<Work>(std::move(plan), run);
        const auto slot = active.emplace(workflow, run).first;
        try {
            admitted.push_back(std::move(work));
        } catch (...) {
            active.erase(slot);
            throw;
        }
        last_admitted = run->identity().run;
        events->changed.notify_one();
        return std::get<RunHandle>(std::move(handle));
    }

    Result<std::monostate> reset_node_state(const NodeTarget& target)
    {
        const std::lock_guard lock(events->mutex);
        if (stopping || active.contains(target.workflow)) {
            DiagnosticLocation location;
            location.workflow = target.workflow;
            location.node = target.node;
            return Diagnostic{ErrorCode::InvalidState, Severity::Error, std::move(location),
                "State reset requires an idle workflow slot."};
        }
        return states.reset(target.workflow, target.node);
    }

    Result<std::optional<NodeResult>> lookup_result(const RunIdentity& run, const InvocationKey& invocation) const
    {
        const auto mismatch = [&] {
            return query_error(ErrorCode::ResultIdentityMismatch, run, invocation,
                "Result lookup requires an exact run and invocation from this session.");
        };
        if (run.snapshot.session != session || invocation.workflow_run != run.run) { return mismatch(); }
        std::shared_ptr<EngineRun> backend;
        {
            const std::lock_guard lock(events->mutex);
            const auto in_flight = std::ranges::find_if(active, [&](const auto& slot) {
                return slot.second->identity().run == run.run;
            });
            if (in_flight != active.end()) {
                backend = in_flight->second;
            } else if (const auto retained = std::ranges::find_if(history, [&](const auto& item) {
                return item && item->identity().run == run.run;
            }); retained != history.end()) {
                backend = *retained;
            } else if (last_admitted && run.run <= *last_admitted) {
                return query_error(ErrorCode::ResultRetired, run, invocation, "Run results are outside the session history.");
            }
        }
        if (!backend || backend->identity() != run) { return mismatch(); }
        return std::get<RunHandle>(RunHandle::from_accepted(std::move(backend))).lookup(invocation);
    }

    Result<RunObservation> observe(std::size_t capacity) { return observers->subscribe(capacity); }

    const SolutionSessionId session;
    const ToolRegistry registry;

private:
    using WorkList = std::list<std::unique_ptr<Work>>;

    static ExecutionCapabilities profile(const InvocationResourceProvider* provider)
    {
        ExecutionCapabilities supported;
        supported.asynchronous = true;
        supported.control_graph = true;
        supported.node_state = true;
        if (provider) {
            const auto kinds = provider->kinds();
            supported.resource_leases.assign(kinds.begin(), kinds.end());
        }
        return supported;
    }

    std::optional<RunSubmission> check_slot(const RunRequest& request, WorkflowId workflow) const
    {
        if (stopping) {
            return RunRejected{error(ErrorCode::InvalidRunRequest, "The workflow engine is draining accepted work.", request)};
        }
        const auto slot = active.find(workflow);
        if (slot != active.end()) { return RunBusy{slot->second->identity()}; }
        return std::nullopt;
    }

    std::optional<WorkList::iterator> take_ready()
    {
        std::unique_lock lock(events->mutex);
        for (;;) {
            auto next = std::ranges::find_if(admitted, [](const auto& work) { return work->run->wakeup()->ready(); });
            if (next != admitted.end()) {
                (*next)->run->wakeup()->clear();
                return next;
            }
            if (stopping && admitted.empty()) { return std::nullopt; }
            events->changed.wait(lock);
        }
    }

    void retire(WorkList::iterator entry)
    {
        const auto run = (*entry)->run;
        std::unique_ptr<Work> finished;
        {
            const std::lock_guard lock(events->mutex);
            finished = std::move(*entry);
            admitted.erase(entry);
        }
        finished.reset(); // Destruction and backend cleanup stay outside scheduler locks.
        std::shared_ptr<EngineRun> evicted;
        {
            const std::lock_guard lock(events->mutex);
            if (!history.empty()) {
                evicted = std::exchange(history[history_cursor], run);
                if (++history_cursor == history.size()) { history_cursor = 0; }
            }
            active.erase(run->identity().workflow);
            run->mark_quiescent();
        }
    }

    void work_loop()
    {
        worker_engine = this;
        while (const auto ready = take_ready()) {
            const auto entry = *ready;
            auto& work = **entry;
            auto next = WorkStep::Finished;
            try { next = execute_step(work, resources.get(), states); }
            catch (...) {
                work.run->unexpected_failure();
                if (!work.active_calls.empty()) {
                    // Keep in-flight operations until their exit notifications
                    // and recheck any progress already consumed by this step.
                    for (const auto& item : work.active_calls) {
                        work.run->wakeup()->signal(item.second.invocation.context.data().identity,
                            InvocationWaitTarget::PhysicalExit);
                    }
                    next = WorkStep::Ready;
                }
            }
            if (next == WorkStep::Finished) { retire(entry); }
            else if (next == WorkStep::Ready) { work.run->wakeup()->request(); }
        }
        states.clear();
        std::vector<std::shared_ptr<EngineRun>> retained;
        {
            const std::lock_guard lock(events->mutex);
            retained.swap(history);
        }
        retained.clear();
        resources.reset();
    }

    inline static thread_local const Impl* worker_engine{nullptr};
    std::shared_ptr<InvocationResourceProvider> resources;
    const ExecutionCapabilities capabilities;
    const std::shared_ptr<SchedulerEvents> events{std::make_shared<SchedulerEvents>()};
    const std::shared_ptr<detail::ObservationHub> observers{std::make_shared<detail::ObservationHub>()};
    bool stopping{false};
    std::optional<WorkflowRunId> last_admitted;
    std::map<WorkflowId, std::shared_ptr<EngineRun>> active;
    WorkList admitted;
    detail::NodeStateService states;
    std::vector<std::shared_ptr<EngineRun>> history;
    std::size_t history_cursor{0};
    std::once_flag joined;
    std::thread worker;
};

WorkflowEngine::WorkflowEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkflowEngine::~WorkflowEngine() = default;

core::Result<std::unique_ptr<WorkflowEngine>> WorkflowEngine::create(const core::ToolRegistry& registry,
    std::shared_ptr<InvocationResourceProvider> resources, WorkflowEngineOptions options)
{
    try {
        auto session = IdFactory::generate<SolutionSessionId>();
        if (const auto* failure = std::get_if<Diagnostic>(&session)) { return *failure; }
        return std::unique_ptr<WorkflowEngine>{new WorkflowEngine(std::make_unique<Impl>(
            std::get<SolutionSessionId>(session), registry, std::move(resources), options))};
    } catch (...) {
        return Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not start the workflow worker."};
    }
}

core::SolutionSessionId WorkflowEngine::session() const noexcept { return impl_->session; }

RunSubmission WorkflowEngine::submit(const RunRequest& request)
{
    try {
        if (request.snapshot().identity().session != session()) {
            return RunRejected{error(ErrorCode::ResultIdentityMismatch, "The request snapshot belongs to another session.", request)};
        }
        return impl_->submit(request, std::visit([](const auto& target) { return target.workflow; }, request.target()));
    } catch (...) {
        return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Workflow submission raised an unexpected exception.", request)};
    }
}

core::Result<std::monostate> WorkflowEngine::reset_node_state(const NodeTarget& target)
{
    try {
        return impl_->reset_node_state(target);
    } catch (...) {
        DiagnosticLocation location;
        location.workflow = target.workflow;
        location.node = target.node;
        return Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, std::move(location),
            "Node state reset raised an unexpected exception."};
    }
}

core::Result<std::optional<NodeResult>> WorkflowEngine::lookup_result(
    const RunIdentity& run, const InvocationKey& invocation) const
{
    try {
        return impl_->lookup_result(run, invocation);
    } catch (...) {
        return query_error(ErrorCode::ExecutionBoundaryFailure, run, invocation, "Session result lookup raised an unexpected exception.");
    }
}

core::Result<RunObservation> WorkflowEngine::observe(std::size_t capacity)
{
    try {
        return impl_->observe(capacity);
    } catch (...) {
        return Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not create a run observation subscription."};
    }
}

core::Result<std::monostate> WorkflowEngine::close()
{
    try {
        return impl_->close();
    } catch (...) {
        return Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not drain and close the workflow engine."};
    }
}

} // namespace visionpro::workflow
