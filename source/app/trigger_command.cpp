#include "trigger_command.hpp"
#include "run_summary.hpp"
#include <visionpro/infrastructure/camera_trigger.hpp>

namespace visionpro::app {
namespace {
using namespace core;
using namespace workflow;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

Diagnostic invalid(WorkflowId workflow, std::string message)
{
    DiagnosticLocation location;
    location.workflow = workflow;
    return {ErrorCode::InvalidRunRequest, Severity::Error, std::move(location), std::move(message)};
}

Result<json> event_report(const TriggerReceipt& event, TriggerStopMode stop, std::vector<Diagnostic>& failures)
{
    json result{{"mode", trigger_kind_name(event.trigger.kind)},
        {"source", event.trigger.source.text()}, {"frame_input", event.frame_input.text()},
        {"frame", std::to_string(event.frame)}, {"correlation", nullptr}};
    if (event.correlation) {
        result["correlation"] = {{"domain", event.correlation->domain().text()}, {"key", event.correlation->key()}};
    }
    if (const auto& camera = event.camera) {
        const auto number = [](const std::optional<std::uint64_t>& value) -> json {
            return value ? json(std::to_string(*value)) : json(nullptr);
        };
        result["camera"] = {{"device_frame_number", number(camera->device_frame_number)}, {"device_ticks", number(camera->device_ticks)},
            {"tick_frequency_hz", number(camera->tick_frequency_hz)}, {"timestamp_source", camera->timestamp_source},
            {"host_capture_us", camera->capture_duration.count()},
            {"device_duration", reported_duration(camera->device_duration, camera->device_duration_unit)}};
    }
    if (const auto* rejected = std::get_if<RunRejected>(&event.submission)) {
        result["admission"] = "rejected";
        result["diagnostics"] = diagnostics_json(std::span{&rejected->diagnostic, 1});
        failures.push_back(rejected->diagnostic);
    } else if (const auto* busy = std::get_if<RunBusy>(&event.submission)) {
        result["admission"] = "busy";
        result["active_run"] = std::to_string(busy->active.run.value());
        failures.push_back(invalid(busy->active.workflow, "The workflow is busy at frame " + std::to_string(event.frame)
            + " (active run " + std::to_string(busy->active.run.value()) + ")."));
    } else {
        const auto& handle = std::get<RunHandle>(event.submission);
        result["admission"] = handle.admission() == RunAdmission::Immediate ? "immediate" : "queued";
        auto inspected = handle.inspect();
        if (const auto* failure = std::get_if<Diagnostic>(&inspected)) { return *failure; }
        const auto& progress = std::get<RunProgress>(inspected);
        auto summary = summarize_run(handle, progress, {});
        if (const auto* failure = std::get_if<Diagnostic>(&summary)) { return *failure; }
        result["result"] = std::get<json>(std::move(summary));
        if (progress.status != ExecutionStatus::Succeeded
            && !(stop == TriggerStopMode::Cancel && progress.status == ExecutionStatus::Cancelled)) {
            if (progress.diagnostics.empty()) {
                failures.push_back(invalid(handle.identity().workflow, "The run for frame " + std::to_string(event.frame) + " did not succeed."));
            } else {
                failures.insert(failures.end(), progress.diagnostics.begin(), progress.diagnostics.end());
            }
        }
    }
    return result;
}
} // namespace

core::Result<TriggerReplay> replay_triggers(const infrastructure::SolutionDocument& document,
    const core::ToolRegistry& registry, core::WorkflowId workflow, const TriggerSequence& sequence)
{
    const auto* flow = find_workflow(document.solution(), workflow);
    if (!flow) { return invalid(workflow, "Unknown trigger workflow."); }
    const bool local = !flow->trigger && std::ranges::any_of(flow->nodes, [&](const Node& node) {
        const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
        const auto* tool = config ? registry.find_descriptor(config->type) : nullptr;
        if (!node.enabled || !tool || config->type.text() != "visionpro.image.source") { return false; }
        const auto descriptor = specialize_tool(*tool, node);
        return descriptor.inputs.empty() && descriptor.execution.effects != ToolEffects::External;
    });
    if (!flow->trigger && (!local || sequence.advances.empty())) { return invalid(workflow, "Configure a trigger or use --advance-ms for a local image source."); }
    if (static_cast<bool>(flow->trigger && flow->trigger->camera) != sequence.camera_frames.has_value()
        || (sequence.camera_frames && (!sequence.frames.empty() || !sequence.advances.empty()))) {
        return invalid(workflow, "A camera entry requires --camera-frames and obtains frame numbers from its camera.");
    }
    if ((local || flow->trigger->mode == TriggerMode::Continuous) && !sequence.frames.empty()) {
        return invalid(workflow, "Use --advance-ms for a continuous entry.");
    }
    if (!local && flow->trigger->mode == TriggerMode::External && !sequence.advances.empty()) {
        return invalid(workflow, "Use --frame for an external entry.");
    }
    auto created = WorkflowSession::create(registry);
    if (const auto* failure = std::get_if<Diagnostic>(&created)) { return *failure; }
    std::shared_ptr<WorkflowSession> session = std::get<std::unique_ptr<WorkflowSession>>(std::move(created));
    auto activated = infrastructure::activate_solution(*session, document);
    if (const auto* failure = std::get_if<Diagnostic>(&activated)) { return *failure; }
    Clock::time_point now{};
    const auto& snapshot = std::get<ConfigSnapshot>(activated);
    auto started = local ? TriggerControl::start_local(session, snapshot.identity(), workflow, 100, now)
        : TriggerControl::start(session, workflow, now);
    if (const auto* failure = std::get_if<Diagnostic>(&started)) { return *failure; }
    const auto& control = std::get<std::shared_ptr<TriggerControl>>(started);
    std::vector<TriggerReceipt> events;
    std::vector<Diagnostic> failures;
    std::optional<infrastructure::CameraStreamStatistics> camera_stats;
    bool camera_closed = false;
    if (sequence.camera_frames) {
        auto device = infrastructure::CameraTriggerStream::open(document.solution(), workflow, control);
        if (const auto* failure = std::get_if<Diagnostic>(&device)) { return *failure; }
        const auto& stream = std::get<std::shared_ptr<infrastructure::CameraTriggerStream>>(device);
        for (std::uint32_t n = 0; n < *sequence.camera_frames; ++n) {
            auto event = stream->next();
            if (auto* failure = std::get_if<Diagnostic>(&event)) { failures.push_back(std::move(*failure)); break; }
            events.push_back(std::get<TriggerReceipt>(std::move(event)));
        }
        auto closed = stream->close();
        camera_closed = std::holds_alternative<std::monostate>(closed);
        if (auto* failure = std::get_if<Diagnostic>(&closed)) { failures.push_back(std::move(*failure)); }
        camera_stats = stream->statistics();
    }
    for (const auto frame : sequence.frames) { events.push_back(control->external(frame)); }
    for (const auto advance : sequence.advances) {
        if (advance > std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now)) {
            return invalid(workflow, "Clock advances exceed the representable time range.");
        }
        now += advance;
        if (auto event = control->tick(now)) {
            if (local) {
                if (const auto* handle = std::get_if<RunHandle>(&event->submission)) {
                    auto done = handle->wait(RunWaitTarget::Quiescence, Clock::time_point::max());
                    if (const auto* bad = std::get_if<Diagnostic>(&done)) { return *bad; }
                }
            }
            events.push_back(std::move(*event));
        }
    }
    const auto mode = sequence.stop.value_or(TriggerStopMode::Drain);
    auto stopped = control->stop(mode, Clock::time_point::max());
    if (const auto* failure = std::get_if<Diagnostic>(&stopped)) { return *failure; }
    if (std::get<RunWaitOutcome>(stopped) != RunWaitOutcome::Satisfied) {
        return invalid(workflow, "Trigger stop did not reach quiescence.");
    }
    json reports = json::array();
    for (const auto& event : events) {
        auto report = event_report(event, mode, failures);
        if (const auto* failure = std::get_if<Diagnostic>(&report)) { return *failure; }
        reports.push_back(std::get<json>(std::move(report)));
    }
    json report{{"solution", document.solution().id.to_string()}, {"workflow", workflow.to_string()},
        {"session", snapshot.identity().session.to_string()}, {"snapshot", std::to_string(snapshot.identity().snapshot.value())},
        {"stop", mode == TriggerStopMode::Drain ? "drain" : "cancel"}, {"quiescent", true},
        {"events", std::move(reports)}, {"load_diagnostics", diagnostics_json(document.diagnostics())}};
    if (camera_stats) {
        report["camera"] = {{"received", camera_stats->received}, {"accepted", camera_stats->accepted},
            {"rejected", camera_stats->rejected}, {"duplicates", camera_stats->duplicates}, {"missing", camera_stats->missing},
            {"closed", camera_closed}};
    }
    return TriggerReplay{std::move(report), std::move(failures)};
}
} // namespace visionpro::app
