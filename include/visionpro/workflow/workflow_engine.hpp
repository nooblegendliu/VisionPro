#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/run_handle.hpp>

namespace visionpro::workflow {

class InvocationResourceProvider;

// Session-local submitter for acyclic workflows and independent tool calls.
// One worker advances ready nodes. Workflow slots last through cleanup.
class WorkflowEngine final : public RunSubmitter {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<WorkflowEngine>> create(const core::ToolRegistry& registry,
        std::shared_ptr<InvocationResourceProvider> resources = {});
    // Drains accepted work and joins the worker. RunHandles retain their records.
    ~WorkflowEngine() override;
    WorkflowEngine(const WorkflowEngine&) = delete;
    WorkflowEngine& operator=(const WorkflowEngine&) = delete;
    [[nodiscard]] core::SolutionSessionId session() const noexcept;
    [[nodiscard]] RunSubmission submit(const RunRequest& request) override;
    // Restore the initialized session state's configured value while the
    // workflow is idle. The state definition must allow explicit reset.
    [[nodiscard]] core::Result<std::monostate> reset_node_state(const NodeTarget& target);
private:
    struct Impl;
    explicit WorkflowEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
