#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/run_handle.hpp>

namespace visionpro::workflow {

class InvocationResourceProvider;

// Session-local submitter for acyclic workflows. One worker advances ready
// nodes while tools can complete asynchronously. Slots last through cleanup.
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
private:
    struct Impl;
    explicit WorkflowEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
