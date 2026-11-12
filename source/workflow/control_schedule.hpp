#pragma once

#include <visionpro/workflow/run_handle.hpp>

#include <deque>

namespace visionpro::workflow {
class ExecutionPlan;

namespace detail {

// Root-scope scheduling state for one validated plan. EngineRun serializes
// mutations with run progress and result acceptance.
class ControlSchedule final {
public:
    explicit ControlSchedule(const ExecutionPlan& plan);

    [[nodiscard]] std::span<const core::NodeId> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::span<const RunControlLinkProgress> links() const noexcept { return links_; }
    [[nodiscard]] std::span<const std::size_t> producers(std::size_t index) const noexcept
    { return states_[index].producers; }
    [[nodiscard]] bool has_ready() const noexcept { return !ready_.empty(); }
    [[nodiscard]] std::optional<std::size_t> take_ready() noexcept;
    void wait_for_data(std::size_t index) noexcept;
    void source_ready(std::size_t index);
    void complete(std::size_t index, std::span<const core::ControlOutputId> selected);
    void stop(ControlCloseReason reason) noexcept;
    [[nodiscard]] std::optional<MissingReason> source_absence(std::size_t index) const noexcept;
    [[nodiscard]] std::vector<std::pair<std::size_t, NodeSkipReason>> take_skipped();

private:
    enum class Phase { WaitingControl, Ready, Dispatched, WaitingData, Completed, Skipped };
    struct NodeState {
        Phase phase{Phase::WaitingControl};
        bool enabled{true};
        bool arrived{false};
        bool not_selected{false};
        bool data_ready{false};
        std::size_t pending{0};
        std::vector<std::size_t> outgoing;
        std::vector<std::size_t> producers;
        std::vector<std::size_t> consumers;
    };
    struct LinkRoute {
        std::size_t target;
        std::optional<core::ControlOutputId> outlet;
    };

    void enqueue(std::size_t index);
    void settle_link(std::size_t edge, std::optional<ControlCloseReason> reason, std::vector<std::size_t>& determined);
    void propagate(std::vector<std::size_t>& determined);

    std::vector<core::NodeId> nodes_;
    std::vector<NodeState> states_;
    std::vector<LinkRoute> routes_;
    std::vector<RunControlLinkProgress> links_;
    std::deque<std::size_t> ready_;
    std::vector<std::pair<std::size_t, NodeSkipReason>> skipped_;
    bool stopped_{false};
};

} // namespace detail
} // namespace visionpro::workflow
