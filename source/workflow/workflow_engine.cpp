#include <visionpro/workflow/workflow_engine.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/workflow/execution_plan.hpp>
#include "scoped_schedule.hpp"
#include "runtime_values.hpp"
#include "loop_execution.hpp"
#include "node_state_service.hpp"
#include "observation_hub.hpp"

#include <algorithm>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <set>
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

struct GroupExports {
    std::size_t frame;
    std::vector<RunExportTrace> sources;
};

class EngineRun final : public RunControl {
public:
    EngineRun(const RunRequest& request, WorkflowRunId id, detail::ControlSchedule schedule,
        const std::shared_ptr<SchedulerEvents>& events, const std::shared_ptr<detail::ObservationHub>& observers)
        : RunControl(request, id), wakeup_(std::make_shared<RunWakeup>(events)), observers_(observers),
          schedule_(std::move(schedule)),
          remaining_(schedule_.nodes().size()), eligible_(remaining_), results_(identity().snapshot.session)
    {
        progress_.nodes.reserve(remaining_);
        progress_.call_order.reserve(remaining_);
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
        ResolutionContext context{identity(), key, schedule_.ancestry(schedule_.frame_of(index)), {}, {}, {globals.begin(), globals.end()}};
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

    ExecutionScopeId scope_for(std::size_t slot) const { return schedule_.scope(schedule_.frame_of(slot)).id; }

    std::optional<std::size_t> enter_body(const ExecutionPlan& plan, const InvocationKey& call,
        const GroupConfiguration& group, std::vector<RunInput> inputs, std::size_t slot, ExecutionScopeId id,
        std::optional<std::int64_t> iteration = {})
    {
        const auto& scopes = plan.dependencies().scopes;
        const auto body = std::ranges::find(scopes, std::optional{call.node}, &ScopeDependencies::group);
        if (body == scopes.end()) { throw std::logic_error("Missing validated region body."); }
        const auto& flow = *find_workflow(request().snapshot().solution(), body->workflow);
        detail::ControlSchedule control(*body, flow, group.entries);
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_) { return {}; }
        const auto previous_size = progress_.nodes.size();
        const auto frame = schedule_.enter(std::move(control), {id, call.scope, call.node, std::move(inputs)}, schedule_.frame_of(slot));
        for (const auto node : schedule_.nodes().subspan(previous_size)) {
            RunNodeProgress progress{node};
            progress.scope = id;
            progress_.nodes.push_back(std::move(progress));
        }
        remaining_ += progress_.nodes.size() - previous_size;
        eligible_.resize(progress_.nodes.size());
        if (iteration) {
            frame_trace_indices_.push_back(progress_.iterations.size());
            progress_.iterations.push_back({id, call, *iteration});
        } else {
            frame_trace_indices_.push_back(progress_.groups.size());
            progress_.groups.push_back({id, call});
        }
        apply_skips();
        return frame;
    }
    void record_iteration_exports(std::size_t frame, std::vector<RunExportTrace> traces)
    {
        const std::lock_guard lock(mutex_);
        progress_.iterations.at(frame_trace_indices_.at(frame - 1)).exports = std::move(traces);
    }
    bool body_done(std::size_t frame) const { return schedule_.settled(frame); }
    void retire_body(std::size_t frame) { schedule_.retire(frame); }

    std::optional<NodeResult> body_result(std::size_t frame, NodeId node) const
    {
        const auto at = schedule_.find(frame, node);
        if (!at || !progress_.nodes[*at].invocation) { return {}; }
        auto found = results_.lookup(identity(), *progress_.nodes[*at].invocation);
        if (const auto* value = std::get_if<std::optional<NodeResult>>(&found)) { return *value; }
        return {};
    }

    bool start_region(std::size_t slot, const InvocationKey& call, std::vector<RunBindingTrace> traces)
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_ || terminal(progress_.status)) { return false; }
        auto& progress = progress_.nodes[slot];
        progress.bindings = std::move(traces);
        progress.invocation = call;
        progress.status = ExecutionStatus::Running;
        progress_.call_order.push_back(call.node);
        active_regions_.insert(slot);
        return true;
    }

    void finish_region(std::size_t slot, Result<std::vector<OutputSlot>> outputs,
        std::vector<ControlOutputId> outlets = {}, std::optional<GroupExports> exports = {})
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (!active_regions_.contains(slot)) { return; }
        if (auto* values = std::get_if<std::vector<OutputSlot>>(&outputs)) {
            auto failure = commit_region(slot, ExecutionStatus::Succeeded, std::move(*values), {}, outlets);
            if (!failure) {
                if (exports) {
                    progress_.groups.at(frame_trace_indices_.at(exports->frame - 1)).exports = std::move(exports->sources);
                }
                schedule_.source_ready(slot);
                schedule_.complete(slot, outlets);
                apply_skips();
                finish_if_done();
                return;
            }
            outputs = std::move(*failure);
        }
        auto failure = call_failure(std::get<Diagnostic>(std::move(outputs)), request(), progress_.nodes[slot].node);
        terminate_region(slot, ExecutionStatus::Failed, {failure});
        progress_.diagnostics.push_back(std::move(failure));
        stop(ExecutionStatus::Failed);
        finish_if_done();
    }

    void wait_for_data(std::size_t index, std::vector<RunBindingTrace> bindings)
    {
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_) { return; }
        progress_.nodes[index].bindings = std::move(bindings);
        schedule_.wait_for_data(index);
    }

    bool start_call(std::size_t index, const InvocationContext& context, std::vector<RunBindingTrace> bindings)
    {
        const auto& invocation = context.data().identity;
        std::vector<RunResourceTrace> resources;
        resources.reserve(context.data().resources.size());
        for (const auto& resource : context.data().resources) {
            resources.push_back({resource.slot, resource.lease->reference()});
        }
        const std::lock_guard lock(mutex_);
        observe_cancellation();
        if (stopping_ || terminal(progress_.status)) { return false; }
        auto& node = progress_.nodes[index];
        progress_.call_order.push_back(node.node);
        node.bindings = std::move(bindings);
        node.resources = std::move(resources);
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
        schedule_.release_inputs();
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
        result.control_links = schedule_.links();
        return result;
    }

    std::optional<Diagnostic> commit_region(std::size_t slot, ExecutionStatus status, std::vector<OutputSlot> outputs,
        std::vector<Diagnostic> diagnostics, std::vector<ControlOutputId> outlets = {})
    {
        const auto attempt = std::get<AttemptId>(RuntimeIdFactory::from_value<AttemptId>(0));
        auto& node = progress_.nodes[slot];
        auto result = NodeResult::create({{identity(), *node.invocation, attempt}, status, Judgement::NotEvaluated,
            std::move(outputs), std::move(diagnostics), {}, std::move(outlets)});
        if (auto* bad = std::get_if<Diagnostic>(&result)) { return std::move(*bad); }
        const auto& record = std::get<NodeResult>(result);
        auto inserted = results_.insert(request().snapshot(), record);
        if (auto* bad = std::get_if<Diagnostic>(&inserted)) { return std::move(*bad); }
        node.status = status;
        active_regions_.erase(slot);
        --remaining_;
        publish(RunObservationKind::ResultPublished, status, record.data().identity);
        return {};
    }

    void terminate_region(std::size_t slot, ExecutionStatus status, std::vector<Diagnostic> diagnostics)
    {
        // Terminal records have no business outputs or control outlets to validate.
        if (auto invalid = commit_region(slot, status, {}, std::move(diagnostics))) { throw std::logic_error(invalid->message); }
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
        while (!active_regions_.empty()) {
            const auto slot = *active_regions_.begin();
            std::vector<Diagnostic> diagnostics;
            if (status == ExecutionStatus::Failed) {
                diagnostics.push_back(error(ErrorCode::SourceFailed, "Region did not complete after a failure in the running workflow.", request(), progress_.nodes[slot].node));
            }
            terminate_region(slot, status, std::move(diagnostics));
        }
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
    detail::ScopedSchedule schedule_;
    std::set<std::size_t> active_regions_;
    std::vector<std::size_t> frame_trace_indices_;
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
        RunBindingTrace trace{binding.target, std::visit([](const auto& value) { return value.sources; }, binding.resolution)};
        if (const auto* resolved = std::get_if<ResolvedBinding>(&binding.resolution)) { trace.regions = resolved->regions; }
        traces.push_back(std::move(trace));
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

struct GroupBody {
    const GroupConfiguration* configuration;
    std::size_t frame;
};

struct Work {
    Work(ExecutionPlan configured, std::shared_ptr<EngineRun> backend,
        std::shared_ptr<InvocationResourceProvider> provider)
        : resources(std::move(provider)), plan(std::move(configured)), run(std::move(backend)),
          issued(run->nodes().size()) {}

    const std::shared_ptr<InvocationResourceProvider> resources;
    ExecutionPlan plan;
    std::shared_ptr<EngineRun> run;
    IdSequence<InvocationId> calls;
    IdSequence<ExecutionScopeId> scopes{std::get<ExecutionScopeId>(RuntimeIdFactory::from_value<ExecutionScopeId>(1))};
    std::map<std::size_t, detail::LoopExecution> loops;
    std::map<std::size_t, GroupBody> groups;
    std::optional<std::chrono::steady_clock::time_point> timer;
    std::vector<std::optional<InvocationId>> issued;
    std::map<std::size_t, ActiveCall> active_calls;
    detail::RunStateCells run_states;
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
    const RunRequest& request, detail::NodeStateService& states, detail::RunStateCells& run_states)
{
    auto& invocation = call.invocation;
    const auto& configuration = invocation.context.data().configuration.data();
    const auto& flow = *find_workflow(invocation.snapshot.solution(), configuration.workflow);
    const auto& node = *find_node(flow, configuration.node);
    auto acquired = states.acquire(invocation.context, node, request.debug_state() == DebugStatePolicy::Isolated, *instance, run_states);
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

void start_region(Work& work, std::size_t slot, const ResolutionContext& context)
{
    auto& run = *work.run;
    const auto& flow = *find_workflow(run.request().snapshot().solution(), run.identity().workflow);
    const auto& group = std::get<GroupConfiguration>(find_node(flow, context.consumer.node)->configuration);
    std::vector<RunBindingTrace> traces;
    std::vector<RunInput> inputs;
    for (const auto& port : group.interface.inputs) {
        if (group.loop && port.id.text() == "index") { continue; }
        auto resolution = work.plan.resolver().resolve(context, port.id, run.results_for_binding());
        traces.push_back({port.id, std::visit([](const auto& value) { return value.sources; }, resolution)});
        if (auto* fixed = std::get_if<ResolvedBinding>(&resolution)) {
            traces.back().regions = std::move(fixed->regions);
            inputs.push_back({port.id, std::move(fixed->value)});
        } else if (std::holds_alternative<PendingBinding>(resolution)) {
            run.wait_for_data(slot, std::move(traces));
            return;
        } else {
            auto diagnostic = std::holds_alternative<BindingFailure>(resolution)
                ? std::get<BindingFailure>(resolution).diagnostic : std::get<UnavailableBinding>(resolution).diagnostic;
            run.preparation_failed(slot, std::move(diagnostic), std::move(traces));
            return;
        }
    }
    if (work.resources) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            auto checked = detail::validate_selected_resources(*work.resources, run.request().snapshot().solution(),
                inputs[i].id, inputs[i].value, traces[i].sources);
            if (auto* failure = std::get_if<Diagnostic>(&checked)) {
                run.preparation_failed(slot, std::move(*failure), std::move(traces));
                return;
            }
        }
    }
    if (!group.loop) {
        const auto id = work.scopes.next();
        if (const auto* failure = std::get_if<Diagnostic>(&id)) {
            run.preparation_failed(slot, *failure, std::move(traces));
            return;
        }
        if (run.start_region(slot, context.consumer, std::move(traces))) {
            auto frame = run.enter_body(work.plan, context.consumer, group, std::move(inputs), slot, std::get<ExecutionScopeId>(id));
            if (frame) { work.groups.emplace(slot, GroupBody{&group, *frame}); }
        }
        return;
    }
    auto created = detail::LoopExecution::create(group, context.consumer, std::move(inputs));
    if (auto* bad = std::get_if<Diagnostic>(&created)) {
        run.preparation_failed(slot, *bad, std::move(traces));
        return;
    }
    if (run.start_region(slot, context.consumer, std::move(traces))) {
        work.loops.emplace(slot, std::get<detail::LoopExecution>(std::move(created)));
    }
}

Result<bool> collect_iteration(EngineRun& run, detail::LoopExecution& loop)
{
    std::vector<RunLoopExportTrace> traces;
    for (const auto& spec : loop.group->loop->exports) {
        const auto& source = std::ranges::find(loop.group->outputs, spec.output, &OutputMapping::output)->source;
        const auto record = run.body_result(*loop.frame, source.node);
        if (!record) { return error(ErrorCode::InputUnavailable, "Loop export has no invocation in this iteration.", run.request(), source.node); }
        auto value = detail::project_region_export(*record, source);
        if (const auto* bad = std::get_if<Diagnostic>(&value)) { return *bad; }
        traces.push_back({spec.output, source, record->data().identity});
        auto saved = loop.collect(spec, std::get<DataValue>(value));
        if (const auto* bad = std::get_if<Diagnostic>(&saved)) { return *bad; }
    }
    run.record_iteration_exports(*loop.frame, std::move(traces));
    if (const auto target = loop.group->loop->break_source) {
        const auto record = run.body_result(*loop.frame, *target);
        if (record && record->data().status == ExecutionStatus::Succeeded) { return false; }
    }
    return loop.advance();
}

std::chrono::steady_clock::time_point interval_deadline(std::int64_t milliseconds)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    return milliseconds >= available.count() ? Clock::time_point::max() : now + std::chrono::milliseconds(milliseconds);
}

Result<std::vector<OutputSlot>> group_outputs(EngineRun& run, const GroupBody& group, std::vector<RunExportTrace>& traces)
{
    std::vector<OutputSlot> outputs;
    for (const auto& mapping : group.configuration->outputs) {
        const auto& port = *std::ranges::find(group.configuration->interface.outputs, mapping.output, &OutputDescriptor::id);
        const auto record = run.body_result(group.frame, mapping.source.node);
        if (!record) {
            auto failure = error(ErrorCode::InputUnavailable, "Group output producer did not run in this call.", run.request(), mapping.source.node);
            failure.location.output = port.id;
            return failure;
        }
        traces.push_back({port.id, mapping.source, record->data().identity});
        if (const auto* source_port = std::get_if<OutputId>(&mapping.source.output)) {
            const auto* source = record->output(*source_port);
            const auto* absent = source ? std::get_if<NotProduced>(&source->content) : nullptr;
            if (absent && allows_output_absence(port, absent->reason)) {
                outputs.push_back({port.id, port.type, *absent});
                continue;
            }
        }
        auto value = detail::project_region_export(*record, mapping.source);
        if (auto* failure = std::get_if<Diagnostic>(&value)) {
            failure->location.output = port.id;
            return *failure;
        }
        outputs.push_back({port.id, port.type, std::get<DataValue>(std::move(value))});
    }
    return outputs;
}

bool finish_groups(Work& work)
{
    auto& run = *work.run;
    bool changed = false;
    for (auto current = work.groups.begin(); current != work.groups.end() && run.can_continue();) {
        const auto& [slot, group] = *current;
        if (!run.body_done(group.frame)) { ++current; continue; }
        std::vector<RunExportTrace> traces;
        auto outputs = group_outputs(run, group, traces);
        std::vector<ControlOutputId> selected;
        for (const auto& mapping : group.configuration->control_exports) {
            const auto result = run.body_result(group.frame, mapping.source);
            if (!result || result->data().status != ExecutionStatus::Succeeded) { continue; }
            const auto& outlets = result->data().selected_control_outputs;
            if (!mapping.source_output || std::ranges::find(outlets, *mapping.source_output) != outlets.end()) {
                selected.push_back(mapping.output);
            }
        }
        run.retire_body(group.frame);
        run.finish_region(slot, std::move(outputs), std::move(selected), GroupExports{group.frame, std::move(traces)});
        current = work.groups.erase(current);
        changed = true;
    }
    return changed;
}

bool advance_loops(Work& work)
{
    auto& run = *work.run;
    bool changed = false;
    work.timer.reset();
    for (auto it = work.loops.begin(); it != work.loops.end() && run.can_continue();) {
        auto current = it++;
        const auto slot = current->first;
        auto& loop = current->second;
        if (loop.frame) {
            if (!run.body_done(*loop.frame)) { continue; }
            const auto more = collect_iteration(run, loop);
            run.retire_body(*loop.frame);
            loop.frame.reset();
            changed = true;
            if (const auto* bad = std::get_if<Diagnostic>(&more)) {
                run.finish_region(slot, *bad);
                work.loops.erase(current);
                continue;
            }
            if (!std::get<bool>(more)) {
                run.finish_region(slot, loop.outputs());
                work.loops.erase(current);
                continue;
            }
            loop.due = interval_deadline(loop.group->loop->interval_ms);
        }
        if (!loop.in_range()) {
            run.finish_region(slot, loop.outputs());
            work.loops.erase(current);
            changed = true;
        } else if (std::chrono::steady_clock::now() < loop.due) {
            if (!work.timer || loop.due < *work.timer) { work.timer = loop.due; }
        } else {
            const auto id = work.scopes.next();
            if (const auto* bad = std::get_if<Diagnostic>(&id)) {
                run.finish_region(slot, *bad);
                work.loops.erase(current);
            } else {
                loop.frame = run.enter_body(work.plan, loop.call, *loop.group, loop.body_inputs(), slot, std::get<ExecutionScopeId>(id), loop.index);
            }
            changed = true;
        }
    }
    return changed;
}

void start_node(Work& work, std::size_t index, detail::NodeStateService& states)
{
    auto& run = *work.run;
    const auto& request = work.plan.request();
    const auto node = run.nodes()[index];
    const auto scope = run.scope_for(index);
    const auto attempt = std::get<AttemptId>(RuntimeIdFactory::from_value<AttemptId>(0));
    work.issued.resize(run.nodes().size());
    auto& issued = work.issued[index];
    if (!issued) {
        auto next = work.calls.next();
        if (auto* failure = std::get_if<Diagnostic>(&next)) {
            run.preparation_failed(index, std::move(*failure));
            return;
        }
        issued = std::get<InvocationId>(next);
    }
    const auto context = run.context_for(index, {run.identity().run, node, scope, *issued});
    const auto* prepared_tool = work.plan.tool(node);
    if (!prepared_tool) {
        start_region(work, index, context);
        return;
    }
    const auto& tool = *prepared_tool;
    auto* resources = work.resources.get();
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
    if (!run.start_call(index, call.invocation.context, std::move(trace))) {
        wakeup->forget(identity);
        work.active_calls.erase(entry);
        return;
    }
    auto started = launch_call(call, std::get<std::shared_ptr<ToolInstance>>(std::move(created)), request, states, work.run_states);
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

WorkStep execute_step(Work& work, detail::NodeStateService& states)
{
    auto& run = *work.run;
    run.begin_step();
    settle_notified(work);
    const bool group_progress = finish_groups(work);
    const bool loop_progress = advance_loops(work);
    const auto ready = run.take_ready();
    if (ready) { start_node(work, *ready, states); }
    if (((!work.loops.empty() || !work.groups.empty()) && !run.stopping() && (ready || loop_progress || group_progress)) || run.has_ready() || (run.stopping() && std::ranges::any_of(work.active_calls,
        [](const auto& entry) { return entry.second.decision == CallDecision::Waiting; }))) {
        return WorkStep::Ready;
    }
    if (!work.active_calls.empty() || (!run.stopping() && work.timer)) { return WorkStep::Parked; }
    if (!run.finished()) { run.blocked(); }
    return WorkStep::Finished;
}
} // namespace

struct WorkflowEngine::PreparedSubmission {
    PreparedSubmission(const ExecutionPlan& configured, WorkflowId target,
        std::shared_ptr<InvocationResourceProvider> provider)
        : plan(configured), workflow(target), resources(std::move(provider)), schedule(plan) {}

    const ExecutionPlan plan;
    const WorkflowId workflow;
    const std::shared_ptr<InvocationResourceProvider> resources;
    detail::ControlSchedule schedule;
    bool consumed{false}; // Protected by the originating engine's scheduler mutex.
};

struct WorkflowEngine::Impl {
    Impl(SolutionSessionId id, const ToolRegistry& tools, std::shared_ptr<InvocationResourceProvider> provider,
        WorkflowEngineOptions options)
        : session(id), registry(tools), default_resources(std::move(provider)),
          default_capabilities(profile(default_resources.get())),
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

    Preparation prepare(const RunRequest& request,
        std::optional<std::shared_ptr<InvocationResourceProvider>> supplied = std::nullopt)
    {
        try {
            if (request.snapshot().identity().session != session) {
                return RunRejected{error(ErrorCode::ResultIdentityMismatch, "The request snapshot belongs to another session.", request)};
            }
            const auto workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
            std::shared_ptr<InvocationResourceProvider> provider;
            {
                const std::lock_guard lock(events->mutex);
                if (auto refusal = check_slot<Preparation>(request, workflow)) { return std::move(*refusal); }
                provider = supplied ? std::move(*supplied) : default_resources;
            }
            const auto capabilities = supplied ? profile(provider.get()) : default_capabilities;
            auto planned = ExecutionPlan::create(request, registry, capabilities);
            if (auto* failure = std::get_if<Diagnostic>(&planned)) { return RunRejected{std::move(*failure)}; }
            const auto& plan = std::get<ExecutionPlan>(planned);
            if (provider) {
                auto checked = provider->preflight(plan);
                if (auto* failure = std::get_if<Diagnostic>(&checked)) { return RunRejected{std::move(*failure)}; }
            }
            return std::make_shared<PreparedSubmission>(plan, workflow, std::move(provider));
        } catch (...) {
            return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Workflow preparation raised an unexpected exception.", request)};
        }
    }

    RunSubmission admit(PreparedSubmission& prepared)
    {
        const auto& request = prepared.plan.request();
        const auto workflow = prepared.workflow;
        try {
            if (request.snapshot().identity().session != session) {
                return RunRejected{error(ErrorCode::ResultIdentityMismatch, "The prepared run belongs to another session.", request)};
            }
            const std::lock_guard lock(events->mutex);
            if (std::exchange(prepared.consumed, true)) {
                return RunRejected{error(ErrorCode::InvalidRunRequest, "The prepared run has already been consumed.", request)};
            }
            // Planning does not reserve a slot. Another submit may have won it.
            if (auto refusal = check_slot<RunSubmission>(request, workflow)) { return std::move(*refusal); }
            const auto next = last_admitted ? RuntimeIdFactory::next(*last_admitted)
                                            : RuntimeIdFactory::from_value<WorkflowRunId>(1);
            if (const auto* failure = std::get_if<Diagnostic>(&next)) { return RunRejected{*failure}; }
            auto run = std::make_shared<EngineRun>(request, std::get<WorkflowRunId>(next),
                std::move(prepared.schedule), events, observers);
            auto handle = RunHandle::from_accepted(run);
            if (auto* failure = std::get_if<Diagnostic>(&handle)) { return RunRejected{std::move(*failure)}; }
            auto accepted = std::get<RunHandle>(std::move(handle));

            auto work = std::make_unique<Work>(prepared.plan, run, prepared.resources);
            const auto slot = active.emplace(workflow, run).first;
            try {
                admitted.push_back(std::move(work));
            } catch (...) {
                active.erase(slot);
                throw;
            }
            last_admitted = run->identity().run;
            events->changed.notify_one();
            return accepted;
        } catch (...) {
            return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Workflow admission raised an unexpected exception.", request)};
        }
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
        supported.structured_loops = true;
        supported.composite_groups = true;
        supported.node_state = true;
        if (provider) {
            const auto kinds = provider->kinds();
            supported.resource_leases.assign(kinds.begin(), kinds.end());
        }
        return supported;
    }

    template<class Outcome>
    std::optional<Outcome> check_slot(const RunRequest& request, WorkflowId workflow) const
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
            const auto now = std::chrono::steady_clock::now();
            auto next = std::ranges::find_if(admitted, [&](const auto& work) {
                return work->run->wakeup()->ready() || (work->timer && *work->timer <= now);
            });
            if (next != admitted.end()) {
                (*next)->run->wakeup()->clear();
                return next;
            }
            if (stopping && admitted.empty()) { return std::nullopt; }
            std::optional<std::chrono::steady_clock::time_point> deadline;
            for (const auto& work : admitted) {
                if (work->timer && (!deadline || *work->timer < *deadline)) { deadline = work->timer; }
            }
            if (deadline) { events->changed.wait_until(lock, *deadline); }
            else { events->changed.wait(lock); }
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
            try { next = execute_step(work, states); }
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
        std::shared_ptr<InvocationResourceProvider> released;
        std::vector<std::shared_ptr<EngineRun>> retained;
        {
            const std::lock_guard lock(events->mutex);
            retained.swap(history);
            released.swap(default_resources);
        }
    }

    inline static thread_local const Impl* worker_engine{nullptr};
    std::shared_ptr<InvocationResourceProvider> default_resources;
    const ExecutionCapabilities default_capabilities;
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
    auto preparation = impl_->prepare(request);
    if (auto* prepared = std::get_if<std::shared_ptr<PreparedSubmission>>(&preparation)) {
        return admit(std::move(*prepared));
    }
    if (auto* failure = std::get_if<RunRejected>(&preparation)) { return std::move(*failure); }
    return std::get<RunBusy>(preparation);
}

WorkflowEngine::Preparation WorkflowEngine::prepare(const RunRequest& request,
    std::shared_ptr<InvocationResourceProvider> provider)
{
    return impl_->prepare(request, std::move(provider));
}

RunSubmission WorkflowEngine::admit(std::shared_ptr<PreparedSubmission> prepared)
{
    if (!prepared) {
        return RunRejected{Diagnostic{ErrorCode::InvalidRunRequest, Severity::Error, {}, "The prepared run is missing."}};
    }
    return impl_->admit(*prepared);
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
