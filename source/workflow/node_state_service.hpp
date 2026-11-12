#pragma once

#include <visionpro/workflow/model.hpp>
#include <visionpro/core/tool_execution.hpp>

#include <map>
#include <mutex>

namespace visionpro::workflow::detail {

struct StateCell;

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

// The worker acquires state within its workflow slot. A reset is allowed only
// while the scheduler holds the admission lock and the workflow has no slot.
class NodeStateService final {
public:
    [[nodiscard]] core::Result<std::shared_ptr<InvocationStateLease>> acquire(
        const core::InvocationContext& context, const Node& node, bool isolated, core::ToolInstance& instance);
    [[nodiscard]] core::Result<std::monostate> reset(core::WorkflowId workflow, core::NodeId node);
    void clear() noexcept;

private:
    struct StateVersion {
        core::ToolExecutionConfiguration configuration;
        std::vector<Binding> bindings;
        std::uint64_t generation;
        std::shared_ptr<StateCell> session;
    };

    [[nodiscard]] core::Result<std::shared_ptr<StateCell>> select(
        const core::InvocationContext& context, const Node& node, bool isolated);

    std::mutex mutex_;
    std::map<std::pair<core::WorkflowId, core::NodeId>, StateVersion> versions_;
    std::uint64_t last_generation_{0};
};

} // namespace visionpro::workflow::detail
