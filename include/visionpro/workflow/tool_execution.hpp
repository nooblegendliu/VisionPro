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
    [[nodiscard]] InvocationPreparation prepare(const ResolutionContext& context, core::AttemptId attempt,
        const ResultStore& results, std::span<const core::InvocationResourceLease> leases = {}) const;
private:
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
