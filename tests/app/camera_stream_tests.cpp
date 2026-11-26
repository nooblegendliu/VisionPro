#include "tool_catalog.hpp"
#include <visionpro/core/id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/camera_trigger.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/copy.hpp>
#include <visionpro/workflow/validation.hpp>
#include <algorithm>
#include <future>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void check(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
template<class T> T take(Result<T> value)
{
    if (auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}
void exercise()
{
    const auto registry = take(visionpro::app::make_tool_registry());
    const auto document = take(load_solution(VISIONPRO_CAMERA_DIRECTORY, registry));
    const auto& solution = document.solution();
    const auto& flow = solution.workflows.at(1);
    const auto& device = solution.resources.front();
    const auto& entry = *flow.trigger->camera;
    {
        auto modified = solution;
        auto& workflow = modified.workflows.at(1);
        auto& receiver = workflow.nodes.front();
        const auto receiver_id = receiver.id;
        const auto& acquisition = modified.workflows.front().nodes.front();
        receiver.resources = acquisition.resources;
        take(validate_camera_trigger_ownership(modified, workflow, registry.descriptors()));
        receiver = acquisition;
        receiver.id = receiver_id;
        const auto conflict = [&] {
            auto checked = validate_camera_trigger_ownership(modified, workflow, registry.descriptors());
            const auto* failure = std::get_if<Diagnostic>(&checked);
            return failure && failure->code == ErrorCode::InvalidResource && failure->location.workflow == workflow.id
                && failure->location.node == receiver_id && failure->location.resource == receiver.resources.front().resource.id;
        };
        check(conflict(), "A reachable acquisition cannot use the camera already owned by its workflow trigger.");
        auto alias = device;
        alias.id = take(IdFactory::generate<ResourceId>());
        auto& alias_device = std::get<DeviceResource>(alias.location);
        alias_device.configuration = take(camera_device_config("SIM-001", 2));
        modified.resources.push_back(alias);
        receiver.resources.front().resource.id = alias.id;
        check(conflict(), "A second catalog identity with different strobe settings still addresses the same physical camera.");
        auto prepare = take(WorkflowSession::create(registry));
        const auto activated = prepare->activate(modified);
        const auto* denied = std::get_if<Diagnostic>(&activated);
        check(denied && denied->code == ErrorCode::InvalidResource, "Planning rejects duplicate acquisition before the camera opens.");
        receiver.enabled = false;
        take(validate_camera_trigger_ownership(modified, workflow, registry.descriptors()));
        receiver.enabled = true;
        std::get<DeviceResource>(modified.resources.back().location).configuration = take(camera_device_config("SIM-OTHER"));
        take(validate_camera_trigger_ownership(modified, workflow, registry.descriptors()));
    }
    auto clone = take(clone_solution(solution, ExternalReferencePolicy::Reject));
    check(clone.solution.workflows.at(1).trigger->camera->device == clone.identities.resources.at(device.id),
        "An independent copy must remap the camera trigger resource.");
    std::shared_ptr<WorkflowSession> session = take(WorkflowSession::create(registry));
    take(activate_solution(*session, document));
    auto control = take(TriggerControl::start(session, flow.id));
    auto stream = take(CameraTriggerStream::open(solution, flow.id, control));
    auto simulator = simulated_camera();
    simulator->set_periodic_hardware_pulses(false);
    const auto capture = [&](const std::shared_ptr<CameraTriggerStream>& source) {
        auto waiting = std::async(std::launch::async, [source] { return source->next(); });
        check(simulator->wait_until_armed(Clock::now() + 2s), "A hardware grab must wait for its pulse.");
        return waiting;
    };
    const auto pulse = [&](const auto& source, std::optional<std::uint64_t> raw = {}) {
        auto waiting = capture(source);
        simulator->pulse(raw);
        return take(waiting.get());
    };
    const auto first = pulse(stream);
    const auto& first_run = std::get<RunHandle>(first.submission);
    take(first_run.wait(RunWaitTarget::Quiescence, Clock::now() + 2s));
    const auto second = pulse(stream);
    const auto& second_run = std::get<RunHandle>(second.submission);
    const auto& submitted = *second_run.request().input(entry.image_input)->value.as_image();
    check(second.camera && submitted.acquisition && submitted.acquisition->device_ticks == second.camera->device_ticks
        && submitted.acquisition->device_frame_number == second.camera->device_frame_number
        && submitted.acquisition->serial == "SIM-001", "Camera receipt and workflow input must carry the same capture metadata.");
    const auto read_pixels = [&](const RunHandle& run, std::byte expected) {
        const auto progress = take(run.wait(RunWaitTarget::Quiescence, Clock::now() + 2s)).progress;
        check(progress.status == ExecutionStatus::Succeeded && progress.call_order.size() == 1, "Exactly one successor should execute.");
        const auto& frame = *run.request().input(entry.image_input)->value.as_image();
        const auto bytes = frame.image.copy_pixels();
        check(bytes.size() == 4 && std::ranges::all_of(bytes, [expected](std::byte b) { return b == expected; }),
            "Published image must survive device buffer reuse and disconnect.");
    };
    read_pixels(first_run, std::byte{7});
    read_pixels(second_run, std::byte{31});
    auto interrupted = capture(stream);
    check(std::holds_alternative<Diagnostic>(open_camera(device)), "Outstanding grab retains its exclusive physical lease.");
    simulator->set_connected(false);
    const auto lost = interrupted.get();
    check(std::holds_alternative<Diagnostic>(lost) && std::get<Diagnostic>(lost).code == ErrorCode::ResourceNotFound,
        "A disconnect wakes the waiting capture with its diagnosed error.");
    take(stream->close());
    take(control->stop(TriggerStopMode::Drain, Clock::now() + 2s));
    simulator->set_connected(true);
    simulator->pulse();
    check(stream->statistics().accepted == 2 && std::holds_alternative<Diagnostic>(stream->next()),
        "Late signals on the retired subscription cannot start another run.");
    auto replacement = take(TriggerControl::start(session, flow.id));
    auto reopened = take(CameraTriggerStream::open(solution, flow.id, replacement));
    const auto restored = pulse(reopened);
    check(restored.correlation != first.correlation && reopened->statistics().accepted == 1, "Reconnect uses a new subscription.");
    read_pixels(first_run, std::byte{7});
    auto cancelled = capture(reopened);
    reopened->request_stop();
    const auto outcome = cancelled.get();
    check(std::holds_alternative<Diagnostic>(outcome) && std::get<Diagnostic>(outcome).code == ErrorCode::SourceCancelled,
        "Stop cancels a pending hardware wait.");
    simulator->fail_next_finish(-497);
    for (int call = 0; call < 2; ++call) {
        const auto closed = reopened->close();
        const auto* failure = std::get_if<Diagnostic>(&closed);
        check(failure && failure->code == ErrorCode::ExecutionBoundaryFailure && failure->location.resource == device.id
            && failure->external && failure->external->operation == "close" && failure->external->raw_code == -497
            && failure->external->mapping == ExternalCodeMapping::Unmapped, "Repeated close must return the original SDK status.");
    }
    take(replacement->stop(TriggerStopMode::Cancel, Clock::now() + 2s));
    check(reopened->statistics().received == 1 && reopened->statistics().accepted == 1, "Cancellation publishes no frame.");
    take(open_camera(device));

    struct Sample { std::uint64_t raw; bool newer; };
    const auto check_counters = [&](std::initializer_list<Sample> samples, std::uint64_t missing) {
        auto tracker = take(TriggerControl::start(session, flow.id));
        auto tracking = take(CameraTriggerStream::open(solution, flow.id, tracker));
        std::uint64_t accepted = 0;
        for (const auto& sample : samples) {
            const auto receipt = pulse(tracking, sample.raw);
            check(receipt.camera && receipt.camera->device_frame_number == sample.raw, "Receipt lost the raw device frame number.");
            if (sample.newer) {
                const auto* run = std::get_if<RunHandle>(&receipt.submission);
                check(run, "A forward device frame must start one run.");
                const auto progress = take(run->wait(RunWaitTarget::Quiescence, Clock::now() + 2s)).progress;
                check(progress.status == ExecutionStatus::Succeeded && progress.call_order.size() == 1, "Accepted frame lost its successor.");
                ++accepted;
            } else {
                const auto* rejection = std::get_if<RunRejected>(&receipt.submission);
                check(rejection && rejection->diagnostic.code == ErrorCode::ResultIdentityMismatch,
                    "A repeated or late frame must be rejected before submitting the workflow.");
            }
        }
        const auto stats = tracking->statistics();
        check(stats.received == samples.size() && stats.accepted == accepted && stats.rejected == samples.size() - accepted
            && stats.duplicates == stats.rejected && stats.missing == missing, "Raw frame ordering or gap counts are incorrect.");
        take(tracking->close());
        take(tracker->stop(TriggerStopMode::Drain, Clock::now() + 2s));
    };
    check_counters({{0xfffffffd, true}, {0xffffffff, true}, {0xffffffff, false}, {0, true},
        {0xfffffffe, false}, {1, true}, {0x80000001, false}, {3, true}}, 2);
    constexpr auto wide = std::uint64_t{1} << 32;
    check_counters({{wide + 10, true}, {wide + 12, true}, {wide + 11, false}, {wide + 13, true}}, 1);
}
} // namespace
int main()
{
    try { exercise(); return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
