#include "scoped_schedule.hpp"

#include <visionpro/core/runtime_id_factory.hpp>

#include <algorithm>

namespace visionpro::workflow::detail {

ScopedSchedule::ScopedSchedule(ControlSchedule root)
{
    const auto id = std::get<core::ExecutionScopeId>(core::RuntimeIdFactory::from_value<core::ExecutionScopeId>(0));
    append(std::move(root), {id, {}, {}, {}}, {});
}

std::size_t ScopedSchedule::append(ControlSchedule schedule, ResolutionScope scope, std::optional<std::size_t> parent)
{
    const auto frame = frames_.size();
    const auto offset = nodes_.size();
    const auto members = schedule.nodes();
    nodes_.insert(nodes_.end(), members.begin(), members.end());
    frame_of_.insert(frame_of_.end(), members.size(), frame);
    frames_.push_back({std::move(schedule), std::move(scope), parent, offset});
    active_.insert(frame);
    return frame;
}

std::size_t ScopedSchedule::enter(ControlSchedule body, ResolutionScope scope, std::size_t parent_frame)
{ return append(std::move(body), std::move(scope), parent_frame); }

void ScopedSchedule::retire(std::size_t frame)
{
    frames_.at(frame).scope.inputs.clear();
    active_.erase(frame);
}

void ScopedSchedule::release_inputs()
{
    for (auto frame : active_) { frames_[frame].scope.inputs.clear(); }
}

std::vector<ResolutionScope> ScopedSchedule::ancestry(std::size_t frame) const
{
    std::vector<ResolutionScope> path;
    for (std::optional<std::size_t> at = frame; at; at = frames_[*at].parent) { path.push_back(frames_[*at].scope); }
    std::ranges::reverse(path);
    return path;
}

std::optional<std::size_t> ScopedSchedule::find(std::size_t frame, core::NodeId id) const
{
    const auto& region = frames_.at(frame);
    const auto members = region.schedule.nodes();
    const auto found = std::ranges::find(members, id);
    if (found == members.end()) { return std::nullopt; }
    return region.offset + static_cast<std::size_t>(found - members.begin());
}

std::vector<std::size_t> ScopedSchedule::producers(std::size_t slot) const
{
    const auto& frame = frames_[frame_of(slot)];
    std::vector<std::size_t> result;
    for (auto local : frame.schedule.producers(slot - frame.offset)) { result.push_back(frame.offset + local); }
    return result;
}

std::vector<RunControlLinkProgress> ScopedSchedule::links() const
{
    std::vector<RunControlLinkProgress> links;
    for (const auto& frame : frames_) {
        for (auto link : frame.schedule.links()) {
            if (frame.parent) { link.scope = frame.scope.id; }
            links.push_back(std::move(link));
        }
    }
    return links;
}

bool ScopedSchedule::has_ready() const
{ return std::ranges::any_of(active_, [&](auto i) { return frames_[i].schedule.has_ready(); }); }

std::optional<std::size_t> ScopedSchedule::take_ready()
{
    for (auto i : active_) {
        auto& frame = frames_[i];
        if (auto slot = frame.schedule.take_ready()) { return frame.offset + *slot; }
    }
    return std::nullopt;
}
void ScopedSchedule::wait_for_data(std::size_t slot)
{
    auto& frame = frames_[frame_of(slot)];
    frame.schedule.wait_for_data(slot - frame.offset);
}
void ScopedSchedule::source_ready(std::size_t slot)
{
    auto& frame = frames_[frame_of(slot)];
    frame.schedule.source_ready(slot - frame.offset);
}
bool ScopedSchedule::has_error_route(std::size_t slot) const
{
    const auto& frame = frames_[frame_of(slot)];
    return frame.schedule.has_error_route(slot - frame.offset);
}
void ScopedSchedule::complete(std::size_t slot, std::span<const core::ControlOutputId> outputs,
    std::optional<ControlCloseReason> failure)
{
    auto& frame = frames_[frame_of(slot)];
    frame.schedule.complete(slot - frame.offset, outputs, failure);
}
void ScopedSchedule::stop(ControlCloseReason reason)
{ for (auto i : active_) { frames_[i].schedule.stop(reason); } }

std::optional<SourceSelection::State> ScopedSchedule::source_absence(std::size_t slot) const
{
    const auto& frame = frames_[frame_of(slot)];
    return frame.schedule.source_absence(slot - frame.offset);
}
std::vector<std::pair<std::size_t, NodeSkipReason>> ScopedSchedule::take_skipped()
{
    std::vector<std::pair<std::size_t, NodeSkipReason>> result;
    for (auto i : active_) {
        auto& frame = frames_[i];
        for (auto [local, reason] : frame.schedule.take_skipped()) { result.emplace_back(frame.offset + local, reason); }
    }
    return result;
}

} // namespace visionpro::workflow::detail
