#include "control_schedule.hpp"

#include <visionpro/workflow/execution_plan.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>

namespace visionpro::workflow::detail {
namespace {

std::vector<core::NodeId> control_order(const ScopeDependencies& scope)
{
    struct Adjacency {
        std::size_t incoming{0};
        std::vector<core::NodeId> successors;
    };
    std::map<core::NodeId, Adjacency> graph;
    for (const auto& node : scope.reachable) { graph.try_emplace(node); }
    for (const auto& link : scope.control_links) {
        graph.at(link.source).successors.push_back(link.target);
        ++graph.at(link.target).incoming;
    }

    std::vector<core::NodeId> order;
    order.reserve(graph.size());
    for (const auto& node : scope.reachable) {
        if (graph.at(node).incoming == 0) { order.push_back(node); }
    }
    for (std::size_t index = 0; index < order.size(); ++index) {
        for (const auto& target : graph.at(order[index]).successors) {
            if (--graph.at(target).incoming == 0) { order.push_back(target); }
        }
    }
    if (order.size() != scope.reachable.size()) {
        throw std::logic_error("A validated control scope must be acyclic.");
    }
    return order;
}

} // namespace

ControlSchedule::ControlSchedule(const ExecutionPlan& plan)
    : ControlSchedule(plan.dependencies().scopes.front(),
        *find_workflow(plan.request().snapshot().solution(), plan.dependencies().scopes.front().workflow), plan.entries()) {}

ControlSchedule::ControlSchedule(const ScopeDependencies& scope, const Workflow& flow, std::span<const core::NodeId> entries)
    : nodes_(control_order(scope)), states_(nodes_.size()), unsettled_(nodes_.size())
{
    std::map<core::NodeId, std::size_t> indices;
    for (std::size_t index = 0; index < nodes_.size(); ++index) { indices.emplace(nodes_[index], index); }
    for (const auto& node : flow.nodes) {
        if (const auto found = indices.find(node.id); found != indices.end()) {
            states_[found->second].enabled = node.enabled;
            states_[found->second].error_output = node.execution_policy.error_output;
        }
    }
    // An entry grants a virtual arrival. Its actual incoming edges still
    // have to settle before it is considered for execution.
    for (const auto& entry : entries) { states_[indices.at(entry)].arrived = true; }

    routes_.reserve(scope.control_links.size());
    links_.reserve(scope.control_links.size());
    for (const auto& link : scope.control_links) {
        const auto target = indices.at(link.target);
        states_[indices.at(link.source)].outgoing.push_back(links_.size());
        ++states_[target].pending;
        routes_.push_back({target, link.output});
        links_.push_back({link.id});
    }
    for (const auto& binding : scope.data) {
        states_[indices.at(binding.consumer)].producers.push_back(indices.at(binding.producer));
    }
    for (std::size_t index = 0; index < states_.size(); ++index) {
        auto& producers = states_[index].producers;
        std::ranges::sort(producers);
        const auto duplicates = std::ranges::unique(producers);
        producers.erase(duplicates.begin(), duplicates.end());
        for (const auto producer : producers) { states_[producer].consumers.push_back(index); }
    }

    std::vector<std::size_t> determined;
    for (std::size_t index = 0; index < states_.size(); ++index) {
        if (states_[index].pending == 0) { determined.push_back(index); }
    }
    propagate(determined);
}

std::optional<std::size_t> ControlSchedule::take_ready() noexcept
{
    if (ready_.empty()) { return std::nullopt; }
    const auto index = ready_.front();
    ready_.pop_front();
    states_[index].phase = Phase::Dispatched;
    return index;
}

void ControlSchedule::wait_for_data(std::size_t index) noexcept
{
    if (!stopped_ && states_[index].phase == Phase::Dispatched) {
        states_[index].phase = Phase::WaitingData;
    }
}

void ControlSchedule::source_ready(std::size_t index)
{
    auto& source = states_[index];
    if (stopped_ || source.data_ready) { return; }
    source.data_ready = true;
    for (const auto consumer : source.consumers) {
        if (states_[consumer].phase == Phase::WaitingData) { enqueue(consumer); }
    }
}

bool ControlSchedule::has_error_route(std::size_t index) const
{
    const auto& source = states_[index];
    return source.error_output && std::ranges::any_of(source.outgoing, [&](std::size_t edge) {
        const auto& route = routes_[edge];
        return route.outlet == source.error_output && states_[route.target].enabled;
    });
}

void ControlSchedule::complete(std::size_t index, std::span<const core::ControlOutputId> selected,
    std::optional<ControlCloseReason> failure, bool close_branch)
{
    auto& source = states_[index];
    if (stopped_ || source.phase != Phase::Dispatched) { return; }
    source.phase = Phase::Completed;
    --unsettled_;

    std::vector<std::size_t> determined;
    for (const auto edge : source.outgoing) {
        const auto& outlet = routes_[edge].outlet;
        const bool arrives = failure ? (source.error_output && source.error_output == outlet)
            : (!close_branch && (!outlet || std::ranges::find(selected, *outlet) != selected.end()));
        settle_link(edge, arrives ? std::nullopt : std::optional{failure.value_or(ControlCloseReason::NotSelected)}, determined);
    }
    propagate(determined);
}

void ControlSchedule::stop(ControlCloseReason reason) noexcept
{
    stopped_ = true;
    ready_.clear();
    for (auto& link : links_) {
        if (link.status == ControlLinkStatus::Pending) {
            link.status = ControlLinkStatus::Closed;
            link.close_reason = reason;
        }
    }
}

std::optional<SourceSelection::State> ControlSchedule::source_absence(std::size_t index) const noexcept
{
    const auto& source = states_[index];
    if (!source.enabled) { return MissingReason::Disabled; }
    if (source.phase == Phase::Skipped) {
        if (source.upstream_failure) {
            return *source.upstream_failure == ControlCloseReason::TimedOut ? UpstreamFailure::TimedOut : UpstreamFailure::Failed;
        }
        return MissingReason::NotSelected;
    }
    return std::nullopt;
}

std::vector<std::pair<std::size_t, NodeSkipReason>> ControlSchedule::take_skipped()
{
    std::vector<std::pair<std::size_t, NodeSkipReason>> skipped;
    skipped.swap(skipped_);
    return skipped;
}

void ControlSchedule::enqueue(std::size_t index)
{
    ready_.push_back(index);
    states_[index].phase = Phase::Ready;
}

void ControlSchedule::settle_link(std::size_t edge, std::optional<ControlCloseReason> reason,
    std::vector<std::size_t>& determined)
{
    auto& link = links_[edge];
    if (link.status != ControlLinkStatus::Pending) { return; }
    link.status = reason ? ControlLinkStatus::Closed : ControlLinkStatus::Arrived;
    link.close_reason = reason;

    const auto target_index = routes_[edge].target;
    auto& target = states_[target_index];
    target.arrived = target.arrived || !reason;
    target.not_selected = target.not_selected || reason == ControlCloseReason::NotSelected;
    if (reason == ControlCloseReason::Failed || reason == ControlCloseReason::TimedOut) { target.upstream_failure = reason; }
    if (--target.pending == 0) { determined.push_back(target_index); }
}

void ControlSchedule::propagate(std::vector<std::size_t>& determined)
{
    // A worklist also handles long chains of closed edges without recursion.
    for (std::size_t cursor = 0; cursor < determined.size(); ++cursor) {
        const auto index = determined[cursor];
        auto& node = states_[index];
        if (node.enabled && node.arrived) {
            enqueue(index);
            continue;
        }

        auto reason = NodeSkipReason::Disabled;
        if (node.enabled) {
            reason = node.not_selected && !node.upstream_failure ? NodeSkipReason::NotSelected : NodeSkipReason::UpstreamClosed;
        }
        node.phase = Phase::Skipped;
        --unsettled_;
        skipped_.emplace_back(index, reason);
        source_ready(index);

        auto closure = ControlCloseReason::Disabled;
        if (node.enabled) {
            closure = node.upstream_failure.value_or(node.not_selected ? ControlCloseReason::NotSelected : ControlCloseReason::Disabled);
        }
        for (const auto edge : node.outgoing) { settle_link(edge, closure, determined); }
    }
}

} // namespace visionpro::workflow::detail
