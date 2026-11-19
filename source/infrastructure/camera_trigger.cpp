#include <visionpro/infrastructure/camera_trigger.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <limits>
#include <mutex>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace workflow;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

std::optional<std::uint64_t> forward_distance(std::uint64_t previous, std::uint64_t now)
{
    if (previous <= UINT32_MAX && now <= UINT32_MAX) {
        // A 32-bit serial number is unambiguously newer only in the next half
        // of the ring. This applies on both sides of zero, even when now > previous.
        const auto delta = static_cast<std::uint32_t>(now - previous);
        if (delta == 0 || delta >= (std::uint32_t{1} << 31)) { return {}; }
        return delta;
    }
    if (now <= previous) { return {}; }
    return now - previous;
}
}

struct CameraTriggerStream::Impl {
    WorkflowTrigger entry;
    CorrelationDomainId domain;
    CameraStreamId stream;
    std::shared_ptr<TriggerControl> control;
    std::shared_ptr<CameraLease> device;
    std::stop_source stop{};
    std::mutex reader{}, counts{};
    CameraStreamStatistics statistics{};
    std::optional<std::uint64_t> previous_device_frame{};
    std::optional<Diagnostic> close_failure{};
};

CameraTriggerStream::CameraTriggerStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CameraTriggerStream::~CameraTriggerStream() { (void)close(); }

Result<std::shared_ptr<CameraTriggerStream>> CameraTriggerStream::open(const Solution& solution, WorkflowId id,
    std::shared_ptr<TriggerControl> control)
{
    const auto* flow = find_workflow(solution, id);
    if (!control || !flow || !flow->trigger || !flow->trigger->camera) {
        return error(ErrorCode::InvalidRunRequest, "Opening a camera stream requires an armed camera entry.");
    }
    const auto& entry = *flow->trigger;
    const auto resource = std::ranges::find(solution.resources, entry.camera->device, &ResourceDefinition::id);
    if (resource == solution.resources.end()) { return error(ErrorCode::ResourceNotFound, "Camera resource is absent."); }
    if (!control->matches_camera(id, entry, *resource)) { return error(ErrorCode::ResultIdentityMismatch, "Camera source differs from the armed entry."); }
    auto domain = SchemaIdFactory::parse<CorrelationDomainId>(entry.source.text());
    if (auto* failure = std::get_if<Diagnostic>(&domain)) { return *failure; }
    auto stream = IdFactory::generate<CameraStreamId>();
    if (auto* failure = std::get_if<Diagnostic>(&stream)) { return *failure; }
    auto lease = open_camera(*resource);
    if (auto* failure = std::get_if<Diagnostic>(&lease)) { return *failure; }
    std::unique_ptr<Impl> impl(new Impl{entry, std::get<CorrelationDomainId>(domain), std::get<CameraStreamId>(stream),
        std::move(control), std::get<std::shared_ptr<CameraLease>>(std::move(lease))});
    return std::shared_ptr<CameraTriggerStream>(new CameraTriggerStream(std::move(impl)));
}

Result<TriggerReceipt> CameraTriggerStream::next(std::stop_token stop)
{
    auto& s = *impl_;
    std::stop_callback cancel(stop, [this] { request_stop(); });
    const std::lock_guard read(s.reader);
    if (s.stop.stop_requested()) { return error(ErrorCode::SourceCancelled, "Camera subscription has stopped."); }
    const auto& binding = *s.entry.camera;
    const auto started = std::chrono::steady_clock::now();
    auto capture = s.device->capture(binding.coordinate_frame, binding.mode, std::chrono::milliseconds(binding.timeout_ms), s.stop.get_token());
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
    if (auto* failure = std::get_if<Diagnostic>(&capture)) { request_stop(); return *failure; }
    const auto& frame = std::get<CameraFrame>(capture);
    if (!frame.image.frame || frame.image.frame->sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || frame.image.frame->source != binding.device || frame.image.image.coordinate_frame() != binding.coordinate_frame) {
        request_stop();
        return error(ErrorCode::ResultIdentityMismatch, "Camera returned a frame inconsistent with its resource or coordinates.");
    }
    const auto sequence = static_cast<std::int64_t>(frame.image.frame->sequence);
    auto key = RuntimeIdFactory::correlation(s.domain, s.stream.to_string() + "/" + std::to_string(sequence));
    if (auto* failure = std::get_if<Diagnostic>(&key)) { request_stop(); return *failure; }
    const auto correlation = std::get<CorrelationId>(key);
    bool duplicate = false;
    {
        const std::lock_guard count(s.counts);
        ++s.statistics.received;
        if (binding.mode != CameraMode::Single && frame.device_frame_number && s.previous_device_frame) {
            const auto distance = forward_distance(*s.previous_device_frame, *frame.device_frame_number);
            duplicate = !distance.has_value();
            if (distance) { s.statistics.missing += *distance - 1; }
            else { ++s.statistics.duplicates; }
        }
        if (!duplicate) { s.previous_device_frame = frame.device_frame_number; }
    }
    auto receipt = duplicate
        ? TriggerReceipt{{TriggerKind::External, s.entry.source}, s.entry.frame_input, sequence, correlation,
            RunRejected{error(ErrorCode::ResultIdentityMismatch, "Repeated or out-of-order device frame in the same camera stream.")}}
        : s.control->camera_frame(sequence, frame.image, correlation);
    receipt.camera = CameraEventInfo{frame.device_frame_number, frame.device_ticks, frame.tick_frequency_hz,
        frame.timestamp_source, duration, frame.device_duration, frame.device_duration_unit};
    {
        const std::lock_guard count(s.counts);
        if (std::holds_alternative<RunHandle>(receipt.submission)) { ++s.statistics.accepted; }
        else { ++s.statistics.rejected; }
    }
    return receipt;
}

void CameraTriggerStream::request_stop()
{
    impl_->control->disarm();
    impl_->stop.request_stop();
}

Result<std::monostate> CameraTriggerStream::close()
{
    request_stop();
    const std::lock_guard read(impl_->reader);
    if (impl_->device) {
        auto result = impl_->device->finish();
        if (auto* failure = std::get_if<Diagnostic>(&result)) { impl_->close_failure = std::move(*failure); }
        impl_->device.reset();
    }
    if (impl_->close_failure) { return *impl_->close_failure; }
    return std::monostate{};
}

CameraStreamStatistics CameraTriggerStream::statistics() const
{
    const std::lock_guard count(impl_->counts);
    return impl_->statistics;
}
} // namespace visionpro::infrastructure
