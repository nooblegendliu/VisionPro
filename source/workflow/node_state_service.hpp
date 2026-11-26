#pragma once

#include <visionpro/workflow/model.hpp>
#include <visionpro/core/tool_execution.hpp>

#include <map>
#include <mutex>

namespace visionpro::workflow { class ConfigSnapshot; class ExecutionPlan; }

namespace visionpro::workflow::detail {

struct StateCell;
struct StateGeneration;
using RunStateCells = std::map<core::NodeId, std::shared_ptr<StateCell>>;
using RunStateBindings = std::map<core::NodeId, std::shared_ptr<StateGeneration>>;
using StateCatalog = std::map<std::pair<core::WorkflowId, core::NodeId>, std::shared_ptr<StateGeneration>>;
[[nodiscard]] std::optional<std::uint64_t> serialized_state_generation(
    const RunStateBindings& bindings, core::NodeId node) noexcept;

class InvocationStateLease final : public core::ToolStateAccess {
public:
    InvocationStateLease(core::ToolInvocationIdentity identity, std::shared_ptr<StateCell> cell);

    const core::NodeStateKey& key() const noexcept override { return key_; }
    const core::ToolInvocationIdentity& identity() const noexcept override { return identity_; }
    [[nodiscard]] core::Result<core::DataValue> read() const override;
    [[nodiscard]] core::Result<std::monostate> write(const core::DataValue& value) override;

    // Called by the scheduler under the same arbitration lock as result publication.
    void settle(bool succeeded) noexcept;

private:
    enum class Access { Open, Sealed, Closed };

    void seal() noexcept override;
    void finish() noexcept override;

    const core::ToolInvocationIdentity identity_;
    const core::NodeStateKey key_;
    mutable std::mutex mutex_;
    std::shared_ptr<StateCell> cell_;
    std::optional<core::DataValue> staged_;
    Access access_{Access::Open};
    bool settled_{false};
};

// Admission binds generations in publication order. Workers initialize only
// reached nodes, against the exact generation retained by their request.
class NodeStateService final {
public:
    [[nodiscard]] core::Result<std::monostate> prepare_solution(const ConfigSnapshot& snapshot);
    [[nodiscard]] core::Result<RunStateBindings> bind(const ExecutionPlan& plan, StateCatalog* retired = nullptr);
    [[nodiscard]] StateCatalog capture();
    [[nodiscard]] core::Result<std::shared_ptr<InvocationStateLease>> acquire(
        const core::InvocationContext& context, core::ToolInstance& instance,
        const RunStateBindings& bindings, RunStateCells& run_states);
    [[nodiscard]] core::Result<std::monostate> reset(core::WorkflowId workflow, core::NodeId node);
    void clear() noexcept;

private:
    std::mutex mutex_;
    StateCatalog versions_;
    StateCatalog debug_versions_;
    std::optional<core::ConfigSnapshotId> prepared_solution_;
    std::uint64_t last_generation_{0};
};

} // namespace visionpro::workflow::detail
