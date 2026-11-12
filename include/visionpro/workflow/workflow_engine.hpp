#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/run_handle.hpp>
#include <visionpro/workflow/run_observation.hpp>

#include <cstddef>

namespace visionpro::workflow {

class InvocationResourceProvider;

struct WorkflowEngineOptions {
    // Count of quiescent runs indexed by the session. Zero keeps only active
    // runs in the index. RunHandles and records own their data independently.
    std::size_t completed_run_limit{16};
};

// Session-local submitter for acyclic workflows and independent tool calls.
// One worker advances ready nodes. Workflow slots last through cleanup.
class WorkflowEngine final : public RunSubmitter {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<WorkflowEngine>> create(const core::ToolRegistry& registry,
        std::shared_ptr<InvocationResourceProvider> resources = {}, WorkflowEngineOptions options = {});
    // Drains accepted work and joins the worker. RunHandles retain their records.
    ~WorkflowEngine() override;
    WorkflowEngine(const WorkflowEngine&) = delete;
    WorkflowEngine& operator=(const WorkflowEngine&) = delete;
    [[nodiscard]] core::SolutionSessionId session() const noexcept;
    [[nodiscard]] RunSubmission submit(const RunRequest& request) override;
    // Restore the initialized session state's configured value while the
    // workflow is idle. The state definition must allow explicit reset.
    [[nodiscard]] core::Result<std::monostate> reset_node_state(const NodeTarget& target);
    // Looks up an exact active or retained run. Evicted run IDs report ResultRetired.
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup_result(
        const RunIdentity& run, const core::InvocationKey& invocation) const;
    [[nodiscard]] core::Result<RunObservation> observe(std::size_t capacity = 32);
    // Control-plane operation. Stops admission and observations, drains
    // accepted calls through physical exit, and releases session-owned state.
    [[nodiscard]] core::Result<std::monostate> close();
private:
    struct Impl;
    explicit WorkflowEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
