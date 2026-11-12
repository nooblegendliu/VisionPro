#include <visionpro/workflow/workflow_session.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <limits>
#include <mutex>
#include <set>

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

    Result<ConfigSnapshot> activate(const Solution& candidate)
    {
        {
            const std::lock_guard lock(mutex);
            auto begun = begin_operation(SessionOperationKind::Activate);
            if (auto* failure = std::get_if<Diagnostic>(&begun)) { return std::move(*failure); }
        }
        const FinishOperation finish{*this};
        auto validated = validate_candidate(candidate, registry);
        if (auto* failure = std::get_if<Diagnostic>(&validated)) { return std::move(*failure); }
        auto created = WorkflowEngine::create(registry, resources, options);
        if (auto* failure = std::get_if<Diagnostic>(&created)) { return std::move(*failure); }
        auto replacement = std::get<std::unique_ptr<WorkflowEngine>>(std::move(created));
        const auto version = std::get<ConfigSnapshotId>(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
        auto captured = ConfigSnapshot::capture(candidate, registry.descriptors(), {replacement->session(), version});
        if (auto* failure = std::get_if<Diagnostic>(&captured)) { return std::move(*failure); }
        const auto snapshot = std::get<ConfigSnapshot>(std::move(captured));

        {
            const std::lock_guard lock(mutex);
            progress.status = WorkflowSessionStatus::Switching;
        }
        auto drained = drain_engine();
        if (auto* failure = std::get_if<Diagnostic>(&drained)) { return std::move(*failure); }
        {
            const std::lock_guard lock(mutex);
            engine = std::move(replacement);
            progress.active = snapshot;
            progress.status = WorkflowSessionStatus::Active;
        }
        return snapshot;
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
        resources.reset();
        {
            const std::lock_guard lock(mutex);
            progress.active.reset();
            progress.status = WorkflowSessionStatus::Closed;
        }
        return std::monostate{};
    }

    const ToolRegistry registry;
    std::shared_ptr<InvocationResourceProvider> resources;
    const WorkflowEngineOptions options;
    mutable std::mutex mutex;
    WorkflowSessionProgress progress;
    std::unique_ptr<WorkflowEngine> engine;

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
        if (progress.status == WorkflowSessionStatus::Closed) {
            return error(ErrorCode::InvalidState, "The workflow session has been closed.");
        }
        if (last_operation == std::numeric_limits<std::uint64_t>::max()) {
            return error(ErrorCode::IdentityExhausted, "Session management operation identities are exhausted.");
        }
        progress.operation = SessionOperation{{++last_operation}, kind};
        return std::monostate{};
    }

    Result<std::monostate> drain_engine()
    {
        // Only the reserved management operation can change the engine while
        // the session is Switching or Closing. Waiting never holds this lock.
        if (engine) {
            auto closed = engine->close();
            if (auto* failure = std::get_if<Diagnostic>(&closed)) { return std::move(*failure); }
            engine.reset();
        }
        return std::monostate{};
    }

    std::uint64_t last_operation{0};
};

WorkflowSession::WorkflowSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkflowSession::~WorkflowSession() = default;

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
    try { return impl_->activate(candidate); }
    catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Could not prepare or activate the solution candidate.", candidate.id); }
}

WorkflowSessionProgress WorkflowSession::inspect() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->progress;
}

RunSubmission WorkflowSession::submit(const RunRequest& request)
{
    const auto workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    const auto solution = request.snapshot().solution().id;
    try {
        const std::lock_guard lock(impl_->mutex);
        if (impl_->progress.status != WorkflowSessionStatus::Active) {
            return RunRejected{error(ErrorCode::InvalidRunRequest, "Run submission requires an active workflow session.", solution, workflow)};
        }
        if (!request.snapshot().same_capture(*impl_->progress.active)) {
            return RunRejected{error(ErrorCode::ResultIdentityMismatch, "Run request does not belong to the active configuration capture.", solution, workflow)};
        }
        return impl_->engine->submit(request);
    } catch (...) {
        return RunRejected{error(ErrorCode::ExecutionBoundaryFailure, "Session run submission failed.", solution, workflow)};
    }
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
