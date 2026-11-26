#include <visionpro/workflow/workflow_session.hpp>
#include <visionpro/workflow/trigger_control.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>
#include "solution_run.hpp"

#include <limits>
#include <mutex>
#include <set>
#include <utility>

namespace visionpro::workflow {
namespace {
using namespace core;

Diagnostic error(ErrorCode code, std::string message, std::optional<SolutionId> solution = {},
    std::optional<WorkflowId> workflow = {}, std::optional<NodeId> node = {})
{
    DiagnosticLocation where;
    where.solution = solution;
    where.workflow = workflow;
    where.node = node;
    return {code, Severity::Error, std::move(where), std::move(message)};
}

Result<std::monostate> validate_candidate(const Solution& candidate, const ToolRegistry& registry)
{
    auto validated = validate_solution(candidate, registry.descriptors());
    if (auto* failure = std::get_if<Diagnostic>(&validated)) { return std::move(*failure); }
    std::set<ToolTypeId> checked;
    for (const auto& flow : candidate.workflows) {
        for (const auto& node : flow.nodes) {
            const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
            if (!tool || !checked.insert(tool->type).second) { continue; }
            // Model validation has located every referenced descriptor. The
            // backend's support is specific to each tool registration.
            auto supported = registry.validate(*registry.find_descriptor(tool->type));
            if (auto* failure = std::get_if<Diagnostic>(&supported)) {
                failure->location.solution = candidate.id;
                failure->location.workflow = flow.id;
                failure->location.node = node.id;
                return std::move(*failure);
            }
        }
    }
    return std::monostate{};
}
} // namespace

struct WorkflowSession::Impl {
    Impl(const ToolRegistry& tools, std::shared_ptr<InvocationResourceProvider> provider, WorkflowEngineOptions delivery)
        : registry(tools), resources(std::move(provider)), options(delivery) {}

    Result<ConfigSnapshot> activate(const Solution& candidate, std::shared_ptr<InvocationResourceProvider> provider)
    {
        {
            const std::lock_guard lock(mutex);
            auto begun = begin_operation(SessionOperationKind::Activate);
            if (auto* failure = std::get_if<Diagnostic>(&begun)) { return std::move(*failure); }
        }
        const FinishOperation finish{*this};
        auto validated = validate_candidate(candidate, registry);
        if (auto* failure = std::get_if<Diagnostic>(&validated)) { return std::move(*failure); }
        if (!provider) { provider = resources; }
        auto created = WorkflowEngine::create(registry, {}, options);
        if (auto* failure = std::get_if<Diagnostic>(&created)) { return std::move(*failure); }
        std::shared_ptr<WorkflowEngine> replacement = std::get<std::unique_ptr<WorkflowEngine>>(std::move(created));
        const auto version = std::get<ConfigSnapshotId>(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
        auto captured = ConfigSnapshot::capture(candidate, registry.descriptors(), {replacement->session(), version});
        if (auto* failure = std::get_if<Diagnostic>(&captured)) { return std::move(*failure); }
        const auto snapshot = std::get<ConfigSnapshot>(std::move(captured));
        auto initialized = replacement->publish_shared(snapshot);
        if (auto* failure = std::get_if<Diagnostic>(&initialized)) { return std::move(*failure); }

        {
            const std::lock_guard lock(mutex);
            progress.status = WorkflowSessionStatus::Switching;
        }
        auto drained = drain_engine();
        if (auto* failure = std::get_if<Diagnostic>(&drained)) { return std::move(*failure); }
        std::shared_ptr<WorkflowEngine> retired;
        std::shared_ptr<InvocationResourceProvider> previous;
        {
            const std::lock_guard lock(mutex);
            retired = std::exchange(engine, std::move(replacement));
            previous = std::exchange(active_provider, std::move(provider));
            last_snapshot = version;
            progress.active = snapshot;
            progress.status = WorkflowSessionStatus::Active;
            round_shutdown = std::stop_source{};
        }
        return snapshot;
    }

    Result<ConfigSnapshot> publish(const ConfigSnapshot& base, const Solution& draft,
        std::shared_ptr<InvocationResourceProvider> provider)
    {
        auto reserved = begin_publication(base, draft.id, {});
        if (auto* failure = std::get_if<Diagnostic>(&reserved)) { return std::move(*failure); }
        const FinishOperation finish{*this};
        auto validated = validate_candidate(draft, registry);
        if (auto* failure = std::get_if<Diagnostic>(&validated)) { return std::move(*failure); }
        auto captured = ConfigSnapshot::capture(draft, registry.descriptors(), std::get<SnapshotIdentity>(reserved));
        if (auto* failure = std::get_if<Diagnostic>(&captured)) { return std::move(*failure); }
        const auto& snapshot = std::get<ConfigSnapshot>(captured);
        std::shared_ptr<InvocationResourceProvider> previous;
        const std::lock_guard lock(mutex);
        auto published = engine->publish_shared(snapshot);
        if (auto* failure = std::get_if<Diagnostic>(&published)) { return std::move(*failure); }
        previous = std::exchange(active_provider, std::move(provider));
        progress.active = snapshot;
        return captured;
    }

    RunSubmission publish_and_submit(const ConfigSnapshot& base, const Solution& draft, const RunArguments& arguments,
        std::shared_ptr<InvocationResourceProvider> provider)
    {
        const auto workflow = std::visit([](const auto& target) { return target.workflow; }, arguments.target);
        auto reserved = begin_publication(base, draft.id, workflow);
        if (auto* failure = std::get_if<Diagnostic>(&reserved)) { return RunRejected{std::move(*failure)}; }
        const FinishOperation finish{*this};

        auto captured = ConfigSnapshot::capture(draft, registry.descriptors(), std::get<SnapshotIdentity>(reserved));
        if (auto* failure = std::get_if<Diagnostic>(&captured)) { return RunRejected{std::move(*failure)}; }
        const auto& snapshot = std::get<ConfigSnapshot>(captured);
        auto request = RunRequest::create(snapshot, arguments.target, arguments.inputs, arguments.trigger,
            arguments.correlation, arguments.debug_state, arguments.globals);
        if (auto* failure = std::get_if<Diagnostic>(&request)) { return RunRejected{std::move(*failure)}; }

        // The management reservation protects the engine while validation and
        // provider preflight run without the session lock.
        auto prepared = engine->prepare(std::get<RunRequest>(request), provider);
        if (auto* failure = std::get_if<RunRejected>(&prepared)) { return std::move(*failure); }
        if (const auto* busy = std::get_if<RunBusy>(&prepared)) { return *busy; }

        std::shared_ptr<InvocationResourceProvider> previous;
        const std::lock_guard lock(mutex);
        auto submitted = engine->admit(std::get<0>(prepared));
        if (std::holds_alternative<RunHandle>(submitted)) {
            previous = std::exchange(active_provider, std::move(provider));
            progress.active = snapshot;
        }
        return submitted;
    }

    RunSubmission submit(const RunRequest& request, WorkflowId workflow, const AdmissionGate* gate)
    {
        std::shared_ptr<WorkflowEngine> selected;
        std::shared_ptr<InvocationResourceProvider> provider;
        {
            const std::lock_guard lock(mutex);
            if (auto refusal = check_capture(request.snapshot(), workflow)) { return std::move(*refusal); }
            selected = engine;
            provider = active_provider;
        }
        auto prepared = selected->prepare(request, std::move(provider));
        if (auto* failure = std::get_if<RunRejected>(&prepared)) { return std::move(*failure); }
        if (const auto* busy = std::get_if<RunBusy>(&prepared)) { return *busy; }

        const std::lock_guard lock(mutex);
        // Preparation can overlap an intervening publication, replacement or
        // close. Recheck the capture before the prepared work enters a slot.
        if (auto refusal = check_capture(request.snapshot(), workflow)) { return std::move(*refusal); }
        const auto admit = [&] { return selected->admit(std::get<0>(prepared)); };
        return gate ? (*gate)(admit) : admit();
    }

    Result<SolutionRunReport> run_solution(const ConfigSnapshot& snapshot, std::span<const WorkflowRunInputs> inputs,
        RunTrigger trigger, std::stop_token caller, std::optional<CorrelationId> correlation)
    {
        const auto& solution = snapshot.solution();
        if (!solution.graph) { return error(ErrorCode::InvalidRunRequest, "No explicit solution graph is configured.", solution.id); }
        auto valid = validate_solution_graph(solution);
        if (auto* failure = std::get_if<Diagnostic>(&valid)) { return std::move(*failure); }
        auto wake = std::make_shared<detail::SolutionWake>();
        std::shared_ptr<WorkflowEngine> selected;
        std::shared_ptr<WorkflowEngine::SolutionCapture> pinned;
        std::shared_ptr<InvocationResourceProvider> provider;
        std::stop_token shutdown;
        std::uint64_t sequence;
        {
            const std::lock_guard lock(mutex);
            if (progress.status != WorkflowSessionStatus::Active || !snapshot.same_capture(*progress.active)) {
                return error(ErrorCode::ResultIdentityMismatch, "Solution run requires the current active capture.", solution.id);
            }
            if (coordinating == this || engine->on_worker_thread()) {
                return error(ErrorCode::InvalidState, "A solution run cannot wait on its own coordinator or worker.", solution.id);
            }
            if (last_round == std::numeric_limits<std::uint64_t>::max()) {
                return error(ErrorCode::IdentityExhausted, "Solution run identities are exhausted.", solution.id);
            }
            auto capture = engine->pin_solution(snapshot, wake);
            if (auto* failure = std::get_if<Diagnostic>(&capture)) { return std::move(*failure); }
            pinned = std::get<std::shared_ptr<WorkflowEngine::SolutionCapture>>(std::move(capture));
            selected = engine;
            provider = active_provider;
            sequence = ++last_round;
            shutdown = round_shutdown.get_token();
            ++active_rounds;
        }
        struct ReleaseRound {
            Impl& owner;
            const Impl* previous;
            ~ReleaseRound() {
                coordinating = previous;
                const std::lock_guard lock(owner.mutex);
                --owner.active_rounds;
                owner.rounds_changed.notify_all();
            }
        } release{*this, std::exchange(coordinating, this)};
        std::stop_source stopping;
        const auto cancel = [&] { stopping.request_stop(); };
        std::stop_callback user_cancel(caller, cancel);
        std::stop_callback session_cancel(shutdown, cancel);
        const auto submit_child = [&](const RunRequest& request) -> RunSubmission {
            auto prepared = selected->prepare(request, provider, pinned);
            if (auto* failure = std::get_if<RunRejected>(&prepared)) { return std::move(*failure); }
            if (auto* busy = std::get_if<RunBusy>(&prepared)) { return *busy; }
            const std::lock_guard lock(mutex);
            if (stopping.stop_requested() || progress.status != WorkflowSessionStatus::Active || engine != selected) {
                return RunRejected{error(ErrorCode::SourceCancelled, "Solution run stopped before child admission.", solution.id)};
            }
            return selected->admit(std::get<0>(std::move(prepared)));
        };
        return detail::coordinate_solution(snapshot, sequence, inputs, std::move(trigger), stopping.get_token(), *wake,
            submit_child, std::move(correlation));
    }

    Result<std::monostate> close()
    {
        {
            const std::lock_guard lock(mutex);
            if (progress.status == WorkflowSessionStatus::Closed) { return std::monostate{}; }
            auto begun = begin_operation(SessionOperationKind::Close);
            if (auto* failure = std::get_if<Diagnostic>(&begun)) { return std::move(*failure); }
            progress.status = WorkflowSessionStatus::Closing;
        }
        const FinishOperation finish{*this};
        auto drained = drain_engine();
        if (auto* failure = std::get_if<Diagnostic>(&drained)) { return std::move(*failure); }
        std::shared_ptr<WorkflowEngine> retired;
        std::shared_ptr<InvocationResourceProvider> previous;
        std::shared_ptr<InvocationResourceProvider> default_provider;
        {
            const std::lock_guard lock(mutex);
            retired = std::move(engine);
            previous = std::move(active_provider);
            default_provider = std::move(resources);
            progress.active.reset();
            progress.status = WorkflowSessionStatus::Closed;
        }
        return std::monostate{};
    }

    const ToolRegistry registry;
    std::shared_ptr<InvocationResourceProvider> resources;
    const WorkflowEngineOptions options;
    mutable std::mutex mutex;
    using SenderKey = std::tuple<SnapshotIdentity, WorkflowId, InputId, SchemaId>;
    std::map<SenderKey, std::weak_ptr<TriggerControl>> sdk_senders;
    WorkflowSessionProgress progress;
    std::shared_ptr<InvocationResourceProvider> active_provider;
    std::shared_ptr<WorkflowEngine> engine;

private:
    struct FinishOperation {
        Impl& owner;
        ~FinishOperation()
        {
            const std::lock_guard lock(owner.mutex);
            auto& current = owner.progress;
            current.operation.reset();
            if (current.status == WorkflowSessionStatus::Switching || current.status == WorkflowSessionStatus::Closing) {
                current.status = current.active ? WorkflowSessionStatus::Active : WorkflowSessionStatus::Empty;
            }
        }
    };

    Result<std::monostate> begin_operation(SessionOperationKind kind)
    {
        if (progress.operation) { return error(ErrorCode::InvalidState, "A session management operation is already in progress."); }
        if (kind != SessionOperationKind::Publish && (coordinating == this || (engine && engine->on_worker_thread()))) {
            return error(ErrorCode::InvalidState, "Cannot drain a session from its own coordinator or worker.");
        }
        if (progress.status == WorkflowSessionStatus::Closed) {
            return error(ErrorCode::InvalidState, "The workflow session has been closed.");
        }
        if (last_operation == std::numeric_limits<std::uint64_t>::max()) {
            return error(ErrorCode::IdentityExhausted, "Session management operation identities are exhausted.");
        }
        progress.operation = SessionOperation{{++last_operation}, kind};
        return std::monostate{};
    }

    Result<SnapshotIdentity> begin_publication(const ConfigSnapshot& base, SolutionId solution, std::optional<WorkflowId> workflow)
    {
        const std::lock_guard lock(mutex);
        if (progress.status != WorkflowSessionStatus::Active) {
            return error(ErrorCode::InvalidState, "Publishing requires an active workflow session.", solution, workflow);
        }
        if (!base.same_capture(*progress.active) || solution != base.solution().id) {
            return error(ErrorCode::ResultIdentityMismatch, "The draft does not match the active solution and base capture.",
                solution, workflow);
        }
        auto next = RuntimeIdFactory::next(*last_snapshot);
        if (auto* failure = std::get_if<Diagnostic>(&next)) { return std::move(*failure); }
        auto begun = begin_operation(SessionOperationKind::Publish);
        if (auto* failure = std::get_if<Diagnostic>(&begun)) { return std::move(*failure); }
        // Preflight can see the candidate's identity even when admission fails.
        last_snapshot = std::get<ConfigSnapshotId>(next);
        return SnapshotIdentity{base.identity().session, *last_snapshot};
    }

    std::optional<RunRejected> check_capture(const ConfigSnapshot& snapshot, WorkflowId workflow) const
    {
        const auto solution = snapshot.solution().id;
        if (progress.status != WorkflowSessionStatus::Active) {
            return RunRejected{error(ErrorCode::InvalidRunRequest, "Run submission requires an active workflow session.", solution, workflow)};
        }
        if (!snapshot.same_capture(*progress.active)) {
            return RunRejected{error(ErrorCode::ResultIdentityMismatch, "Run request does not belong to the active configuration capture.", solution, workflow)};
        }
        return std::nullopt;
    }

    Result<std::monostate> drain_engine()
    {
        // Only the reserved management operation can change the engine while
        // the session is Switching or Closing. Waiting never holds this lock.
        round_shutdown.request_stop();
        if (engine) {
            auto drained = engine->close();
            if (auto* failure = std::get_if<Diagnostic>(&drained)) { return std::move(*failure); }
        }
        std::unique_lock lock(mutex);
        rounds_changed.wait(lock, [&] { return active_rounds == 0; });
        return std::monostate{};
    }

    std::uint64_t last_operation{0};
    inline static thread_local const Impl* coordinating{};
    std::uint64_t last_round{0};
    std::size_t active_rounds{};
    std::condition_variable rounds_changed;
    std::stop_source round_shutdown;
    std::optional<ConfigSnapshotId> last_snapshot;
};

WorkflowSession::WorkflowSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkflowSession::~WorkflowSession() { static_cast<void>(close()); }

core::Result<std::unique_ptr<WorkflowSession>> WorkflowSession::create(const ToolRegistry& registry,
    std::shared_ptr<InvocationResourceProvider> resources, WorkflowEngineOptions options)
{
    try {
        return std::unique_ptr<WorkflowSession>{new WorkflowSession(std::make_unique<Impl>(registry, std::move(resources), options))};
    } catch (...) {
        return error(ErrorCode::ExecutionBoundaryFailure, "Could not create a workflow session.");
    }
}

core::Result<ConfigSnapshot> WorkflowSession::activate(const Solution& candidate)
{
    return activate(candidate, {});
}

core::Result<ConfigSnapshot> WorkflowSession::activate(const Solution& candidate,
    std::shared_ptr<InvocationResourceProvider> provider)
{
    try { return impl_->activate(candidate, std::move(provider)); }
    catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Could not prepare or activate the solution candidate.", candidate.id); }
}

core::Result<ConfigSnapshot> WorkflowSession::publish(const ConfigSnapshot& base, const Solution& draft,
    std::shared_ptr<InvocationResourceProvider> provider)
{
    try { return impl_->publish(base, draft, std::move(provider)); }
    catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Could not publish the solution draft.", draft.id); }
}

RunSubmission WorkflowSession::publish_and_submit(const ConfigSnapshot& base, const Solution& draft,
    const RunArguments& run, std::shared_ptr<InvocationResourceProvider> provider)
{
    try { return impl_->publish_and_submit(base, draft, run, std::move(provider)); }
    catch (...) {
        const auto workflow = std::visit([](const auto& target) { return target.workflow; }, run.target);
        return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Could not publish the draft for execution.", draft.id, workflow)};
    }
}

WorkflowSessionProgress WorkflowSession::inspect() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->progress;
}

core::Result<std::shared_ptr<const core::SharedData>> WorkflowSession::shared_preview() const
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active) {
        return error(ErrorCode::InvalidState, "Shared data preview requires an active solution session.");
    }
    return impl_->engine->shared_preview();
}

core::Result<std::shared_ptr<const core::CorrelationJoinAccess>> WorkflowSession::correlation_preview() const
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active) {
        return error(ErrorCode::InvalidState, "Correlation preview requires an active solution session.");
    }
    return impl_->engine->correlation_preview();
}

RunSubmission WorkflowSession::submit(const RunRequest& request)
{ return submit_guarded(request, nullptr); }

core::Result<SolutionRunReport> WorkflowSession::run_solution(const ConfigSnapshot& snapshot,
    std::span<const WorkflowRunInputs> inputs, RunTrigger trigger, std::stop_token stop, std::optional<CorrelationId> correlation)
{
    try { return impl_->run_solution(snapshot, inputs, std::move(trigger), stop, std::move(correlation)); }
    catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Solution execution failed unexpectedly.", snapshot.solution().id); }
}

RunSubmission WorkflowSession::submit_guarded(const RunRequest& request, const AdmissionGate* gate)
{
    const auto workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    try { return impl_->submit(request, workflow, gate); }
    catch (...) {
        return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Session run submission failed.", request.snapshot().solution().id, workflow)};
    }
}

Result<std::monostate> WorkflowSession::register_sdk_sender(const SnapshotIdentity& target,
    WorkflowId workflow, InputId input, SchemaId sender, const std::shared_ptr<TriggerControl>& subscription)
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active || !impl_->progress.active
        || impl_->progress.active->identity() != target) {
        return error(ErrorCode::ResultIdentityMismatch, "SDK sender refers to an inactive snapshot.", {}, workflow);
    }
    std::erase_if(impl_->sdk_senders, [&](const auto& item) {
        return std::get<0>(item.first) != target || item.second.expired();
    });
    auto& slot = impl_->sdk_senders[{target, workflow, std::move(input), std::move(sender)}];
    if (const auto active = slot.lock(); active && active->accepting()) {
        return error(ErrorCode::DuplicateIdentity, "SDK sender already has an armed subscription for this input.", {}, workflow);
    }
    slot = subscription;
    return std::monostate{};
}

core::Result<RunObservation> WorkflowSession::observe(std::size_t capacity)
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active) {
        return error(ErrorCode::InvalidState, "Observation requires an active workflow session.");
    }
    return impl_->engine->observe(capacity);
}

core::Result<std::optional<NodeResult>> WorkflowSession::lookup_result(const RunIdentity& run,
    const InvocationKey& invocation) const
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active) {
        return error(ErrorCode::InvalidState, "Session result lookup requires an active workflow session.", {}, run.workflow, invocation.node);
    }
    return impl_->engine->lookup_result(run, invocation);
}

core::Result<std::monostate> WorkflowSession::reset_node_state(const NodeTarget& target)
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->progress.status != WorkflowSessionStatus::Active) {
        return error(ErrorCode::InvalidState, "Node state reset requires an active workflow session.", {}, target.workflow, target.node);
    }
    return impl_->engine->reset_node_state(target);
}

core::Result<std::monostate> WorkflowSession::close()
{
    try { return impl_->close(); }
    catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Workflow session close failed."); }
}

} // namespace visionpro::workflow
