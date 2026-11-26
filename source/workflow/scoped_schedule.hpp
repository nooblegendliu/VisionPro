#pragma once

#include "control_schedule.hpp"

#include <set>

namespace visionpro::workflow::detail {

// Node progress slots are stable across frames. The same static node may have
// a separate slot, control state and invocation in each loop iteration.
class ScopedSchedule final {
public:
    explicit ScopedSchedule(ControlSchedule root);
    [[nodiscard]] std::size_t enter(ControlSchedule body, ResolutionScope scope, std::size_t parent_frame);
    void retire(std::size_t frame);
    void release_inputs();
    [[nodiscard]] bool settled(std::size_t frame) const { return frames_.at(frame).schedule.settled(); }
    [[nodiscard]] std::span<const core::NodeId> nodes() const { return nodes_; }
    [[nodiscard]] std::size_t frame_of(std::size_t slot) const { return frame_of_.at(slot); }
    [[nodiscard]] const ResolutionScope& scope(std::size_t frame) const { return frames_.at(frame).scope; }
    [[nodiscard]] std::vector<ResolutionScope> ancestry(std::size_t frame) const;
    [[nodiscard]] std::optional<std::size_t> find(std::size_t frame, core::NodeId id) const;
    [[nodiscard]] std::vector<std::size_t> producers(std::size_t slot) const;
    [[nodiscard]] std::vector<RunControlLinkProgress> links() const;
    [[nodiscard]] bool has_ready() const;
    [[nodiscard]] std::optional<std::size_t> take_ready();
    void wait_for_data(std::size_t slot);
    void source_ready(std::size_t slot);
    [[nodiscard]] bool has_error_route(std::size_t slot) const;
    void complete(std::size_t slot, std::span<const core::ControlOutputId> outputs,
        std::optional<ControlCloseReason> failure = {}, bool close_branch = false);
    void stop(ControlCloseReason reason);
    [[nodiscard]] std::optional<SourceSelection::State> source_absence(std::size_t slot) const;
    [[nodiscard]] std::vector<std::pair<std::size_t, NodeSkipReason>> take_skipped();
private:
    struct Frame {
        ControlSchedule schedule;
        ResolutionScope scope;
        std::optional<std::size_t> parent;
        std::size_t offset;
    };
    std::size_t append(ControlSchedule schedule, ResolutionScope scope, std::optional<std::size_t> parent);
    std::vector<Frame> frames_;
    std::set<std::size_t> active_;
    std::vector<core::NodeId> nodes_;
    std::vector<std::size_t> frame_of_;
};

} // namespace visionpro::workflow::detail
