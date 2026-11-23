#include "solution_run.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace visionpro::workflow::detail {
namespace {
using namespace core;

struct Participant {
    std::size_t waiting{};
    bool activated{};
    std::vector<std::size_t> outgoing;
    std::vector<const WorkflowPortBinding*> bindings;
    std::optional<std::size_t> run;
    std::optional<WorkflowPublicResult> result;
};

class Round {
public:
    Round(const ConfigSnapshot& capture, std::uint64_t sequence, RunTrigger source, std::optional<CorrelationId> key)
        : snapshot(capture), trigger(std::move(source)), correlation(std::move(key)), report{sequence, snapshot.identity()}
    {
        const auto& graph = *snapshot.solution().graph;
        for (auto id : graph.entries) {
            participants[id].activated = true;
            ready.push_back(id);
        }
        for (const auto& link : graph.links) {
            participants[link.source].outgoing.push_back(report.links.size());
            ++participants[link.target].waiting;
            report.links.push_back({link});
        }
        for (const auto& binding : graph.bindings) { participants.at(binding.target).bindings.push_back(&binding); }
        report.runs.reserve(participants.size());
        report.deliveries.reserve(graph.bindings.size());
    }

    Result<std::monostate> prepare_entries(std::span<const WorkflowRunInputs> inputs)
    {
        std::map<WorkflowId, const std::vector<RunInput>*> supplied;
        for (const auto& input : inputs) {
            if (!participants.contains(input.workflow) || !participants.at(input.workflow).activated
                || !supplied.emplace(input.workflow, &input.inputs).second) {
                return error(ErrorCode::InvalidRunRequest, input.workflow, "Supply input sets once, for graph entries only.");
            }
        }
        const std::vector<RunInput> empty;
        for (auto entry : snapshot.solution().graph->entries) {
            if (!find_workflow(snapshot.solution(), entry)->enabled) {
                if (supplied.contains(entry)) { return error(ErrorCode::InvalidRunRequest, entry, "Inputs target a disabled solution entry."); }
                continue;
            }
            auto request = RunRequest::create(snapshot, WorkflowTarget{entry},
                supplied.contains(entry) ? *supplied.at(entry) : empty, trigger, correlation);
            if (auto* failure = std::get_if<Diagnostic>(&request)) { return std::move(*failure); }
            entries.emplace(entry, std::get<RunRequest>(std::move(request)));
        }
        return std::monostate{};
    }

    SolutionRunReport run(std::stop_token stop, SolutionWake& wake, const SubmitChild& submit)
    {
        std::stop_callback notify(stop, [&] { wake.signal(); });
        bool cancellation_sent = false;
        while (settled < participants.size()) {
            if (stop.stop_requested() && !cancellation_sent) {
                cancellation_sent = true;
                if (report.status == ExecutionStatus::Succeeded) { report.status = ExecutionStatus::Cancelled; }
                for (auto id : active) { static_cast<void>(report.runs[*participants.at(id).run].cancel()); }
            }
            collect_completed();
            while (!ready.empty() && !(stop.stop_requested() && !cancellation_sent)) {
                const auto id = ready.front();
                ready.pop_front();
                auto& item = participants.at(id);
                if (report.status != ExecutionStatus::Succeeded || !item.activated) {
                    report.skipped.push_back(id);
                    settle(item, close_reason(report.status));
                } else if (!find_workflow(snapshot.solution(), id)->enabled) {
                    report.skipped.push_back(id);
                    settle(item, ControlCloseReason::Disabled);
                } else {
                    dispatch(id, item, submit, stop);
                }
            }
            if (settled < participants.size() && (cancellation_sent || !stop.stop_requested())) { wake.wait(); }
        }
        return std::move(report);
    }

private:
    static ControlCloseReason close_reason(ExecutionStatus status)
    {
        if (status == ExecutionStatus::Cancelled) { return ControlCloseReason::Cancelled; }
        if (status == ExecutionStatus::TimedOut) { return ControlCloseReason::TimedOut; }
        if (status == ExecutionStatus::Succeeded) { return ControlCloseReason::NotSelected; }
        return ControlCloseReason::Failed;
    }

    Diagnostic error(ErrorCode code, WorkflowId workflow, std::string message) const
    {
        DiagnosticLocation location;
        location.solution = snapshot.solution().id;
        location.workflow = workflow;
        return {code, Severity::Error, std::move(location), std::move(message)};
    }

    void fail(ExecutionStatus status, Diagnostic issue)
    {
        if (report.status == ExecutionStatus::Succeeded) { report.status = status; }
        report.diagnostics.push_back(std::move(issue));
    }

    void settle(Participant& item, std::optional<ControlCloseReason> reason)
    {
        ++settled;
        for (auto index : item.outgoing) {
            auto& edge = report.links[index];
            auto closed = reason;
            if (!closed && edge.link.output) {
                const auto& selected = item.result->selected_control_outputs;
                if (std::ranges::find(selected, *edge.link.output) == selected.end()) { closed = ControlCloseReason::NotSelected; }
            }
            edge.close_reason = closed;
            edge.status = closed ? ControlLinkStatus::Closed : ControlLinkStatus::Arrived;
            auto& target = participants.at(edge.link.target);
            target.activated |= !closed;
            if (--target.waiting == 0) { ready.push_back(edge.link.target); }
        }
    }

    Result<RunRequest> request_for(WorkflowId id, const Participant& item) const
    {
        if (const auto entry = entries.find(id); entry != entries.end()) { return entry->second; }
        std::vector<RunInput> inputs;
        for (const auto* binding : item.bindings) {
            const auto& producer = participants.at(binding->source);
            if (producer.result) {
                const auto& outputs = producer.result->outputs;
                const auto slot = std::ranges::find(outputs, binding->output, &OutputSlot::id);
                if (slot != outputs.end()) {
                    if (const auto* value = std::get_if<DataValue>(&slot->content)) {
                        inputs.push_back({binding->input, *value});
                        continue;
                    }
                    if (const auto* invalid = std::get_if<InvalidOutput>(&slot->content)) { return invalid->diagnostic; }
                }
            }
            auto issue = error(ErrorCode::InputUnavailable, id, "Mapped workflow input has no value from this solution run.");
            issue.location.input = binding->input;
            issue.location.output = binding->output;
            return issue;
        }
        return RunRequest::create(snapshot, WorkflowTarget{id}, inputs, trigger, correlation);
    }

    void dispatch(WorkflowId id, Participant& item, const SubmitChild& submit, std::stop_token stop)
    {
        auto request = request_for(id, item);
        if (auto* issue = std::get_if<Diagnostic>(&request)) {
            fail(ExecutionStatus::Failed, std::move(*issue));
        } else {
            auto submission = submit(std::get<RunRequest>(request));
            if (auto* handle = std::get_if<RunHandle>(&submission)) {
                item.run = report.runs.size();
                report.runs.push_back(*handle);
                active.insert(id);
                for (const auto* binding : item.bindings) {
                    const auto& source = report.runs[*participants.at(binding->source).run];
                    report.deliveries.push_back({source.identity(), binding->output, handle->identity(), binding->input});
                }
                return;
            }
            if (auto* rejected = std::get_if<RunRejected>(&submission)) {
                fail(stop.stop_requested() ? ExecutionStatus::Cancelled : ExecutionStatus::Failed, std::move(rejected->diagnostic));
            } else {
                fail(ExecutionStatus::Failed, error(ErrorCode::RunQueueFull, id, "Target workflow is busy."));
            }
        }
        report.skipped.push_back(id);
        settle(item, close_reason(report.status));
    }

    void collect_completed()
    {
        for (auto it = active.begin(); it != active.end(); ) {
            auto& item = participants.at(*it);
            const auto& handle = report.runs[*item.run];
            auto progress = handle.inspect();
            auto status = ExecutionStatus::Failed;
            if (auto* issue = std::get_if<Diagnostic>(&progress)) {
                fail(status, std::move(*issue));
            } else {
                auto& child = std::get<RunProgress>(progress);
                status = child.status;
                if (status == ExecutionStatus::Running || status == ExecutionStatus::Pending) { ++it; continue; }
                if (status == ExecutionStatus::Succeeded) {
                    auto result = handle.public_result();
                    if (auto* projection_error = std::get_if<Diagnostic>(&result)) {
                        status = ExecutionStatus::Failed;
                        fail(status, std::move(*projection_error));
                    } else {
                        item.result = std::get<WorkflowPublicResult>(std::move(result));
                    }
                } else {
                    if (report.status == ExecutionStatus::Succeeded) { report.status = status; }
                    report.diagnostics.insert(report.diagnostics.end(), child.diagnostics.begin(), child.diagnostics.end());
                }
            }
            settle(item, status == ExecutionStatus::Succeeded ? std::nullopt : std::optional{close_reason(status)});
            it = active.erase(it);
        }
    }

    const ConfigSnapshot& snapshot;
    RunTrigger trigger;
    std::optional<CorrelationId> correlation;
    SolutionRunReport report;
    std::map<WorkflowId, Participant> participants;
    std::map<WorkflowId, RunRequest> entries;
    std::deque<WorkflowId> ready;
    std::set<WorkflowId> active;
    std::size_t settled{};
};
} // namespace

core::Result<SolutionRunReport> coordinate_solution(const ConfigSnapshot& snapshot, std::uint64_t sequence,
    std::span<const WorkflowRunInputs> inputs, RunTrigger trigger, std::stop_token stop, SolutionWake& wake,
    const SubmitChild& submit, std::optional<core::CorrelationId> correlation)
{
    Round round(snapshot, sequence, std::move(trigger), std::move(correlation));
    auto checked = round.prepare_entries(inputs);
    if (auto* failure = std::get_if<core::Diagnostic>(&checked)) { return std::move(*failure); }
    return round.run(stop, wake, submit);
}
} // namespace visionpro::workflow::detail

namespace visionpro::workflow {
core::Result<std::monostate> validate_solution_inputs(const ConfigSnapshot& snapshot,
    std::span<const WorkflowRunInputs> inputs, RunTrigger trigger)
{
    if (!snapshot.solution().graph) {
        return core::Diagnostic{core::ErrorCode::InvalidRunRequest, core::Severity::Error, {}, "No explicit solution graph is configured."};
    }
    auto graph = validate_solution_graph(snapshot.solution());
    if (auto* failure = std::get_if<core::Diagnostic>(&graph)) { return std::move(*failure); }
    detail::Round preview(snapshot, 0, std::move(trigger), {});
    return preview.prepare_entries(inputs);
}
} // namespace visionpro::workflow
