#include <visionpro/workflow/run_handle.hpp>
#include <visionpro/workflow/result_store.hpp>
#include <visionpro/core/text.hpp>

#include <algorithm>

namespace visionpro::workflow {
namespace {
using core::Diagnostic;
using core::ErrorCode;
using core::Result;

Diagnostic failure(const RunRequest& request, ErrorCode code, std::string message)
{
    core::DiagnosticLocation location;
    location.solution = request.snapshot().solution().id;
    location.workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    if (const auto* node = std::get_if<NodeTarget>(&request.target())) { location.node = node->node; }
    return {code, core::Severity::Error, std::move(location), std::move(message)};
}

bool terminal(ExecutionStatus status)
{
    return status == ExecutionStatus::Succeeded || status == ExecutionStatus::Failed
        || status == ExecutionStatus::Cancelled || status == ExecutionStatus::TimedOut;
}

Result<RunProgress> checked(const RunRequest& request, RunProgress progress)
{
    if ((!terminal(progress.status) && progress.status != ExecutionStatus::Pending && progress.status != ExecutionStatus::Running)
        || (progress.quiescent && !terminal(progress.status))) {
        return failure(request, ErrorCode::InvalidRunHandle, "Invalid run status or quiescence before logical completion.");
    }
    bool has_error = false;
    for (const auto& diagnostic : progress.diagnostics) {
        if ((diagnostic.severity != core::Severity::Info && diagnostic.severity != core::Severity::Warning
             && diagnostic.severity != core::Severity::Error) || diagnostic.message.empty()
            || !core::is_valid_utf8(diagnostic.message)) {
            return failure(request, ErrorCode::InvalidRunHandle, "Invalid run diagnostic.");
        }
        has_error = has_error || diagnostic.severity == core::Severity::Error;
    }
    if ((progress.status == ExecutionStatus::Failed && !has_error)
        || (progress.status == ExecutionStatus::Succeeded && has_error)) {
        return failure(request, ErrorCode::InvalidRunHandle, "Run status and technical diagnostics disagree.");
    }
    return progress;
}

template<class T, class F> Result<T> boundary(const RunRequest& request, F&& action)
{
    try { return action(); }
    catch (...) {
        // Do not expose backend exception text, which may contain external data.
        return failure(request, ErrorCode::ExecutionBoundaryFailure, "Run backend raised an unexpected exception.");
    }
}
} // namespace

RunControl::RunControl(const RunRequest& request, core::WorkflowRunId run)
    : request_(request), identity_{request.snapshot().identity(),
        std::visit([](const auto& target) { return target.workflow; }, request.target()), run} {}

Result<RunHandle> RunHandle::from_accepted(std::shared_ptr<RunControl> control)
{
    if (!control) {
        return Diagnostic{ErrorCode::InvalidRunHandle, core::Severity::Error, {}, "Accepted run requires a non-null backend."};
    }
    return RunHandle(std::move(control));
}

Result<RunProgress> RunHandle::inspect() const
{
    return boundary<RunProgress>(request(), [&]() -> Result<RunProgress> {
        auto progress = control_->inspect();
        if (const auto* error = std::get_if<Diagnostic>(&progress)) { return *error; }
        return checked(request(), std::get<RunProgress>(std::move(progress)));
    });
}

Result<RunWaitResult> RunHandle::wait(RunWaitTarget target, std::chrono::steady_clock::time_point deadline) const
{
    if (target != RunWaitTarget::Completion && target != RunWaitTarget::Quiescence) {
        return failure(request(), ErrorCode::InvalidRunHandle, "Unknown wait target.");
    }
    return boundary<RunWaitResult>(request(), [&]() -> Result<RunWaitResult> {
        auto waited = control_->wait(target, deadline);
        if (const auto* error = std::get_if<Diagnostic>(&waited)) { return *error; }
        auto result = std::get<RunWaitResult>(std::move(waited));
        auto progress = checked(request(), result.progress);
        if (const auto* error = std::get_if<Diagnostic>(&progress)) { return *error; }
        const bool ready = target == RunWaitTarget::Completion ? terminal(result.progress.status) : result.progress.quiescent;
        if ((result.outcome != RunWaitOutcome::Satisfied && result.outcome != RunWaitOutcome::DeadlineExpired)
            || ((result.outcome == RunWaitOutcome::Satisfied) != ready)) {
            return failure(request(), ErrorCode::InvalidRunHandle, "Wait outcome contradicts its returned progress snapshot.");
        }
        return result;
    });
}

Result<RunCancelDisposition> RunHandle::cancel() const
{
    return boundary<RunCancelDisposition>(request(), [&]() -> Result<RunCancelDisposition> {
        auto result = control_->cancel();
        if (const auto* value = std::get_if<RunCancelDisposition>(&result);
            value && *value != RunCancelDisposition::Requested && *value != RunCancelDisposition::AlreadyRequested
                && *value != RunCancelDisposition::AlreadyCompleted) {
            return failure(request(), ErrorCode::InvalidRunHandle, "Unknown cancellation disposition.");
        }
        return result;
    });
}

Result<std::optional<NodeResult>> RunHandle::lookup(const core::InvocationKey& invocation) const
{
    const auto* workflow = find_workflow(request().snapshot().solution(), identity().workflow);
    const auto* target = std::get_if<NodeTarget>(&request().target());
    bool in_target = !target || invocation.node == target->node;
    if (target && !in_target) {
        const auto* node = find_node(*workflow, target->node);
        if (node && std::holds_alternative<GroupConfiguration>(node->configuration)) {
            const auto nested = descendants(*workflow, target->node);
            if (const auto* ids = std::get_if<std::vector<core::NodeId>>(&nested)) {
                in_target = std::ranges::find(*ids, invocation.node) != ids->end();
            }
        }
    }
    if (invocation.workflow_run != identity().run || !workflow || !find_node(*workflow, invocation.node) || !in_target) {
        return failure(request(), ErrorCode::ResultIdentityMismatch, "Result query is outside this run target.");
    }
    return boundary<std::optional<NodeResult>>(request(), [&]() -> Result<std::optional<NodeResult>> {
        auto queried = control_->lookup(invocation);
        if (const auto* error = std::get_if<Diagnostic>(&queried)) { return *error; }
        auto result = std::get<std::optional<NodeResult>>(std::move(queried));
        if (!result) { return result; }
        if (result->data().identity.run != identity() || result->data().identity.invocation != invocation) {
            return failure(request(), ErrorCode::ResultIdentityMismatch, "Backend returned a result from another run or invocation.");
        }
        // Reuse S2.6 output/domain validation without modifying backend history.
        ResultStore contract(identity().snapshot.session);
        const auto valid = contract.insert(request().snapshot(), *result);
        if (const auto* error = std::get_if<Diagnostic>(&valid)) { return *error; }
        return result;
    });
}

} // namespace visionpro::workflow
