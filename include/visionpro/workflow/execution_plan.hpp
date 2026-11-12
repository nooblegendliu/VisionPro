#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/tool_execution.hpp>

namespace visionpro::workflow {

// Capabilities of the caller consuming the plan. The default profile covers
// linear synchronous calls with invocation-lifetime instances.
struct ExecutionCapabilities {
    bool asynchronous{false};
    bool control_graph{false};
    bool node_state{false};
    std::vector<core::ResourceKind> resource_leases;
};

// A preflight product holding the request, its exact validation index and the
// fixed tool configurations. Creating a plan does not call any factory or tool.
class ExecutionPlan final {
public:
    ExecutionPlan(const ExecutionPlan&) noexcept = default;
    ExecutionPlan& operator=(const ExecutionPlan&) noexcept = default;
    [[nodiscard]] static core::Result<ExecutionPlan> create(const RunRequest& request,
        const core::ToolRegistry& registry, const ExecutionCapabilities& capabilities = {});
    [[nodiscard]] const RunRequest& request() const noexcept;
    [[nodiscard]] const ExecutionCapabilities& capabilities() const noexcept;
    [[nodiscard]] std::span<const core::NodeId> entries() const noexcept;
    [[nodiscard]] const ValidationIndex& dependencies() const noexcept;
    [[nodiscard]] const PreparedTool* tool(const core::NodeId& node) const noexcept;
    [[nodiscard]] core::Result<std::shared_ptr<core::ToolInstance>> create_instance(const core::NodeId& node) const;
private:
    struct Storage;
    explicit ExecutionPlan(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
};

} // namespace visionpro::workflow
