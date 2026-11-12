#pragma once

#include <visionpro/core/tool_execution.hpp>
#include <visionpro/workflow/resolver.hpp>

namespace visionpro::workflow {

struct TargetResolution { BindingTarget target; BindingResolution resolution; };
struct PreparedInvocation {
    ConfigSnapshot snapshot;
    core::InvocationContext context;
    std::vector<TargetResolution> bindings;
};
struct PendingInvocation { std::vector<TargetResolution> bindings; };
struct UnavailableInvocation { std::vector<TargetResolution> bindings; };
struct InvocationFailure {
    core::Diagnostic diagnostic;
    std::vector<TargetResolution> bindings;
};
using InvocationPreparation = std::variant<PreparedInvocation, PendingInvocation,
    UnavailableInvocation, InvocationFailure>;

class InvocationResourceProvider {
public:
    virtual ~InvocationResourceProvider() = default;
    // Resource kinds available to the execution planner.
    [[nodiscard]] virtual std::span<const core::ResourceKind> kinds() const noexcept = 0;
    // Called once per configured slot after all bindings resolve.
    [[nodiscard]] virtual core::Result<std::shared_ptr<const core::ResourceLease>> acquire(
        const core::ToolExecutionConfiguration& configuration, const core::ToolResourceBinding& binding) = 0;
};

// Snapshot-to-core projection only. It owns the workflow snapshot, but creates no
// instance, reserves no run identity and never starts or schedules an operation.
class PreparedTool final {
public:
    [[nodiscard]] static core::Result<PreparedTool> create(const ConfigSnapshot& snapshot,
        core::WorkflowId workflow, core::NodeId node);
    [[nodiscard]] const ConfigSnapshot& snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const core::ToolExecutionConfiguration& configuration() const noexcept { return configuration_; }
    // Resolve data first. Leases are required/checked only when all targets are
    // ready, so waiting for producers never requires acquiring device access.
    // For a plan-backed tool, use a single empty root ResolutionScope. Public
    // workflow inputs and independent node inputs come from its fixed request.
    [[nodiscard]] InvocationPreparation prepare(const ResolutionContext& context, core::AttemptId attempt,
        const ResultStore& results, std::span<const core::InvocationResourceLease> leases = {}) const;
    [[nodiscard]] InvocationPreparation prepare(const ResolutionContext& context, core::AttemptId attempt,
        const ResultStore& results, InvocationResourceProvider& provider) const;
private:
    friend class ExecutionPlan;
    [[nodiscard]] static core::Result<PreparedTool> create(BindingResolver resolver,
        core::WorkflowId workflow, core::NodeId node);
    [[nodiscard]] InvocationPreparation prepare(const ResolutionContext& context, core::AttemptId attempt,
        const ResultStore& results, std::span<const core::InvocationResourceLease> leases,
        InvocationResourceProvider* provider) const;
    PreparedTool(ConfigSnapshot snapshot, BindingResolver resolver, core::ToolExecutionConfiguration configuration)
        : snapshot_(std::move(snapshot)), resolver_(std::move(resolver)), configuration_(std::move(configuration)) {}
    ConfigSnapshot snapshot_;
    BindingResolver resolver_;
    core::ToolExecutionConfiguration configuration_;
};

// Converts one matching tool completion, checking the snapshot's output contract.
// It does not commit the returned record or arbitrate retries/cancellation.
[[nodiscard]] core::Result<NodeResult> checked_node_result(const PreparedInvocation& invocation,
    const core::ToolCompletion& completion);

} // namespace visionpro::workflow
