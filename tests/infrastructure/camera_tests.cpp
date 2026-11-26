#include "camera_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <algorithm>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void check(bool condition, const char* message)
{ if (!condition) { throw std::runtime_error(message); } }

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> Diagnostic rejects(Result<T> value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code, "Expected a specific camera diagnostic.");
    return *failure;
}

template<SchemaIdentity Id> Id key(std::string_view name) { return take(SchemaIdFactory::parse<Id>(name)); }
template<UuidIdentity Id> Id fresh() { return take(IdFactory::generate<Id>()); }
const auto coordinates = key<CoordinateSystemId>("camera_pixels");

ResourceDefinition device()
{
    return {fresh<ResourceId>(), "模拟相机一号", ResourceKind::Device,
        DeviceResource{key<SchemaId>("visionpro.camera.simulated"), take(camera_device_config("SIM-001"))}};
}

void pixels(const CameraFrame& frame, ResourceId source, std::uint64_t sequence, std::byte value)
{
    check(frame.image.frame == FrameId{source, sequence} && frame.device_frame_number == sequence,
        "Published identity and device frame number must use the captured source and frame.");
    check(frame.image.image.coordinate_frame() == coordinates && frame.image.image.copy_pixels() == std::vector<std::byte>(4,value),
        "The retained 2x2 Mono8 pixels changed after reuse of the device buffer.");
    check(frame.timestamp_source == "host_arrival" && frame.image.captured_at.has_value() && frame.device_ticks == sequence
        && frame.tick_frequency_hz == 100, "Device ticks and host arrival timestamps remain distinct.");
}

void camera_lease()
{
    const auto resource = device();
    auto alias = resource;
    alias.id = fresh<ResourceId>();
    auto camera = take(open_camera(resource));
    rejects(open_camera(alias), ErrorCode::InputUnavailable);
    const auto first = take(camera->capture(coordinates, CameraMode::Single, 100ms));
    const auto second = take(camera->capture(coordinates, CameraMode::Software, 100ms));
    pixels(first, resource.id, 1, std::byte{7});
    pixels(second, resource.id, 2, std::byte{31});
    check(!first.image.image.shares_storage_with(second.image.image), "SDK frame reuse did not produce independent owned bytes.");
    const auto third = take(camera->capture(coordinates, CameraMode::Continuous, 500ms));
    pixels(third, resource.id, 3, std::byte{7});

    const auto sim = simulated_camera();
    sim->set_periodic_hardware_pulses(false);
    auto capture = [&](std::stop_token token = {}) {
        return std::async(std::launch::async, [&, token] { return camera->capture(coordinates, CameraMode::Hardware, 2s, token); });
    };
    auto pending = capture();
    check(sim->wait_until_armed(Clock::now() + 1s), "Hardware trigger did not arm.");
    sim->set_connected(false);
    const auto disconnect = rejects(pending.get(), ErrorCode::ResourceNotFound);
    check(disconnect.external && disconnect.external->raw_code == 1001 && disconnect.external->mapping == ExternalCodeMapping::Mapped
        && disconnect.location.resource == resource.id, "Disconnect must preserve the device's raw status and resource ID.");
    sim->set_connected(true);
    pending = capture();
    check(sim->wait_until_armed(Clock::now() + 1s), "Reconnected camera did not arm.");
    sim->pulse();
    pixels(take(pending.get()), resource.id, 4, std::byte{31});

    std::stop_source stop;
    pending = capture(stop.get_token());
    check(sim->wait_until_armed(Clock::now() + 1s), "Pending acquisition did not arm before cancellation.");
    stop.request_stop();
    rejects(pending.get(), ErrorCode::SourceCancelled);
    sim->pulse(); // Late, after cancellation. Must not supply a frame to the next call.
    const auto timeout = rejects(camera->capture(coordinates, CameraMode::Hardware, 10ms), ErrorCode::SourceTimedOut);
    check(timeout.external && timeout.external->raw_code == 1002, "Frame wait timeout must retain its native code.");
    sim->fail_next(-493);
    const auto unknown = rejects(camera->capture(coordinates, CameraMode::Single, 100ms), ErrorCode::ExecutionBoundaryFailure);
    check(unknown.external && unknown.external->mapping == ExternalCodeMapping::Unmapped && unknown.external->raw_code == -493,
        "Unknown device errors must keep the original status and remain unmapped.");
    camera.reset();
    auto reopened = take(open_camera(alias));
    pixels(take(reopened->capture(coordinates, CameraMode::Single, 100ms)), alias.id, 5, std::byte{7});
    pixels(first, resource.id, 1, std::byte{7});
    sim->fail_next_finish(-494);
    const auto close_failure = rejects(reopened->finish(), ErrorCode::ExecutionBoundaryFailure);
    check(close_failure.external && close_failure.external->operation == "close" && close_failure.external->raw_code == -494,
        "An explicit close preserves the native shutdown status.");
    rejects(reopened->finish(), ErrorCode::ExecutionBoundaryFailure);
    rejects(reopened->capture(coordinates, CameraMode::Single, 100ms), ErrorCode::InvalidState);
    rejects(open_camera(resource), ErrorCode::InputUnavailable);
    reopened.reset();
    auto next = take(open_camera(resource));
    take(next->finish());
    take(next->finish());

    rejects(camera_device_config(std::string("bad\0serial", 10)), ErrorCode::InvalidResource);
    check(!take(camera_acquisition_settings(resource)), "A v1 camera must preserve per-node acquisition settings.");
    auto configured = device();
    auto& config = std::get<DeviceResource>(configured.location).configuration;
    config = take(camera_device_config("SIM-001", 2));
    check(!take(camera_acquisition_settings(configured)), "A v2 strobe resource also inherits per-node acquisition settings.");
    const CameraAcquisitionSettings settings{CameraMode::Software, 1200ms};
    config = take(camera_device_config("SIM-001", 2, settings));
    check(take(camera_acquisition_settings(configured)) == settings && take(camera_strobe_channels(configured)) == 2,
        "Global acquisition settings must round-trip without losing the strobe mapping.");
    rejects(camera_device_config("SIM-001", 0, CameraAcquisitionSettings{CameraMode::Single, 0ms}), ErrorCode::ConstraintViolation);
    auto wrong = device();
    auto& saved = std::get<DeviceResource>(wrong.location);
    const auto unsupported = take(DataType::record(key<SchemaId>("visionpro.camera.device"), 2, {{key<FieldId>("serial"), DataType::string()}}));
    saved.configuration = take(ConfigValue::record(unsupported, {{key<FieldId>("serial"), take(ConfigValue::string("SIM-001"))}}));
    rejects(open_camera(wrong), ErrorCode::IncompatibleSchemaVersion);
}

void workflow_capture()
{
    const auto resource = device();
    const auto registry = take(ToolRegistry::create(visionpro::tools::camera_registrations()));
    const auto type = key<ToolTypeId>("visionpro.camera.acquire");
    const auto* descriptor = registry.find_descriptor(type);
    const auto mode = std::ranges::find(descriptor->parameters, key<ParameterId>("mode"), &ParameterDescriptor::id);
    const auto snapshot_mode = take(ConfigValue::enumeration(mode->type, key<EnumMemberId>("Hardware")));
    const auto fid = fresh<WorkflowId>();
    Node acquire{fresh<NodeId>(), "Acquire", ToolConfiguration{type, 1},
        {{key<ParameterId>("mode"), {snapshot_mode}},
         {key<ParameterId>("timeout_ms"), {ConfigValue::integer(2000)}},
         {key<ParameterId>("frame_origin"), {take(ConfigValue::point2d({coordinates, MeasurementUnit::Pixel, 0, 0}))}}},
        {{key<SchemaId>("camera"), {resource.id, ResourceKind::Device, {}}}}};
    const auto image = key<OutputId>("image");
    Node forward{fresh<NodeId>(), "Forward", ToolConfiguration{key<ToolTypeId>("visionpro.camera.frame"), 1},
        {{key<InputId>("image"), {OutputReference{fid, acquire.id, image, {}}}}}, {}};
    Workflow flow{fid, "Capture", {key<SchemaId>("capture"),1}, {acquire, forward},
        {{fresh<ControlLinkId>(), acquire.id, {}, forward.id}}, {}, {acquire.id}, {}};
    Solution solution{fresh<SolutionId>(), "Camera fixture",1,1, {flow}, {resource}, {}, {},
        {{coordinates,CoordinateKind::Image,MeasurementUnit::Pixel,{0,0},{1,0},{0,-1},Handedness::LeftHanded}}, {}};
    const auto files = take(FileResourceProvider::create(solution, {}));
    auto session = take(WorkflowSession::create(registry, files));
    const auto active = take(session->activate(solution));
    const auto run = [&] {
        auto request = take(RunRequest::create(active, WorkflowTarget{fid}, {}, {TriggerKind::Manual, key<SchemaId>("test")}));
        auto submitted = session->submit(request);
        if (const auto* refusal = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(refusal->diagnostic.message); }
        return std::get<RunHandle>(std::move(submitted));
    };
    const auto wait = [&](const RunHandle& h) {
        const auto result = take(h.wait(RunWaitTarget::Quiescence, Clock::now()+3s));
        check(result.outcome == RunWaitOutcome::Satisfied, "Run did not reach physical exit.");
        return result.progress;
    };
    const auto sim = simulated_camera();
    sim->set_periodic_hardware_pulses(false);
    const auto cancelled = run();
    check(sim->wait_until_armed(Clock::now()+1s), "Capture node did not arm its camera.");
    rejects(open_camera(resource), ErrorCode::InputUnavailable);
    take(cancelled.cancel());
    const auto stopped = wait(cancelled);
    const auto skipped = std::ranges::find(stopped.nodes, forward.id, &RunNodeProgress::node);
    check(stopped.status == ExecutionStatus::Cancelled && skipped != stopped.nodes.end() && !skipped->invocation,
        "Cancellation must release the camera without dispatching a successor.");
    sim->pulse();
    const auto completed = run();
    check(sim->wait_until_armed(Clock::now()+1s), "A fresh run could not reacquire the camera after exit.");
    sim->pulse();
    const auto finished = wait(completed);
    check(finished.status == ExecutionStatus::Succeeded && finished.nodes.size()==2, "Camera workflow failed.");
    auto record = [&](NodeId node) {
        const auto progress = std::ranges::find(finished.nodes, node, &RunNodeProgress::node);
        return take(completed.lookup(progress->invocation.value())).value();
    };
    const auto captured = record(acquire.id);
    const auto passed = record(forward.id);
    const auto value = [&](const NodeResult& n, std::string_view name) -> const DataValue& {
        return std::get<DataValue>(std::ranges::find(n.data().outputs, key<OutputId>(name), &OutputSlot::id)->content);
    };
    const auto& first = *value(captured, "image").as_image();
    const auto& second = *value(passed, "image").as_image();
    check(first.frame == second.frame && first.image.shares_storage_with(second.image)
        && *value(captured,"frame_number").as_int64() == 1 && *value(passed,"frame_number").as_int64() == 1,
        "The receiving node must forward one owned capture with the same frame number.");
    check(std::ranges::any_of(captured.data().timings, [](const auto& t) {
        return t.source == TimingSource::AdapterReported && t.unit == TimingUnit::Microseconds && t.value == 100.0;
    }), "The acquired frame must expose its device-reported duration separately.");
    check(take(open_camera(resource)) != nullptr, "Physical exit releases the exclusive lease.");
    sim->fail_next_finish(-495);
    const auto close_error = run();
    check(sim->wait_until_armed(Clock::now()+1s), "The final capture did not arm.");
    sim->pulse();
    const auto failed = wait(close_error);
    const auto successor = std::ranges::find(failed.nodes, forward.id, &RunNodeProgress::node);
    check(failed.status == ExecutionStatus::Failed && !successor->invocation
        && std::ranges::any_of(failed.diagnostics, [](const auto& d) {
            return d.external && d.external->raw_code == -495 && d.external->operation == "close";
        }), "Acquisition must report close failure before publishing a frame and invoking the successor.");
}
}

int main(int argc, char** argv)
{
    try {
        if (argc != 2) { throw std::runtime_error("Expected camera test group."); }
        if (std::string_view(argv[1]) == "lease") { camera_lease(); }
        else if (std::string_view(argv[1]) == "workflow") { workflow_capture(); }
        else { throw std::runtime_error("Unknown camera test group."); }
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
