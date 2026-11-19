#include <visionpro/infrastructure/camera.hpp>

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/text.hpp>

#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr std::string_view simulator_id = "visionpro.camera.simulated";
constexpr std::string_view mvs_id = "visionpro.camera.mvs";

template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Diagnostic simulated_error(std::int64_t native_code)
{
    ErrorCode code = ErrorCode::ExecutionBoundaryFailure;
    ExternalCodeMapping mapping = ExternalCodeMapping::Mapped;
    std::string message;
    switch (native_code) {
    case 1001: code = ErrorCode::ResourceNotFound; message = "Simulated camera is disconnected."; break;
    case 1002: code = ErrorCode::SourceTimedOut; message = "Timed out waiting for a simulated camera frame."; break;
    case 1003: code = ErrorCode::SourceCancelled; message = "Simulated camera capture was cancelled."; break;
    default: mapping = ExternalCodeMapping::Unmapped; message = "Unrecognized status from the simulated camera."; break;
    }
    return {code, Severity::Error, {}, std::move(message),
        ExternalDiagnostic{key<SchemaId>(simulator_id), "capture", ExternalStatusDomain::Call,
            native_code, mapping, {}, {}, "1"}};
}

const DataType& config_type()
{
    static const auto type = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.camera.device"), 1,
        {{key<FieldId>("serial"), DataType::string()}}));
    return type;
}

bool valid_serial(std::string_view serial)
{ return !serial.empty() && serial.find('\0') == std::string_view::npos && is_valid_utf8(serial); }

using Endpoint = std::pair<std::string, std::string>;
struct Reservations {
    std::mutex mutex;
    std::set<Endpoint> active;
};

Reservations& reservations()
{
    static Reservations table;
    return table;
}
} // namespace

struct SimulatedCamera::State {
    std::mutex capture_mutex;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::uint64_t sequence{};
    std::uint64_t generation{};
    bool connected{true};
    bool periodic{true};
    bool waiting{};
    bool pulse{};
    std::optional<std::uint64_t> pulse_number;
    std::optional<std::int64_t> failure;
    std::optional<std::int64_t> finish_failure;
    // The next capture overwrites this SDK-owned buffer in place.
    std::array<std::byte, 4> buffer{};
};

SimulatedCamera::SimulatedCamera() : state_(std::make_unique<State>()) {}
SimulatedCamera::~SimulatedCamera() = default;

void SimulatedCamera::set_connected(bool connected)
{
    std::lock_guard lock(state_->mutex);
    if (connected != state_->connected) {
        state_->connected = connected;
        ++state_->generation;
        state_->pulse = false;
        state_->changed.notify_all();
    }
}

void SimulatedCamera::set_periodic_hardware_pulses(bool enabled)
{
    std::lock_guard lock(state_->mutex);
    state_->periodic = enabled;
    state_->changed.notify_all();
}

void SimulatedCamera::pulse(std::optional<std::uint64_t> device_frame_number)
{
    std::lock_guard lock(state_->mutex);
    if (state_->connected && state_->waiting && !state_->pulse) {
        state_->pulse_number = device_frame_number;
        state_->pulse = true;
        state_->changed.notify_all();
    }
}

bool SimulatedCamera::wait_until_armed(Clock::time_point deadline)
{
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_until(lock, deadline, [&] { return state_->waiting && !state_->pulse; });
}

void SimulatedCamera::fail_next(std::int64_t native_code)
{
    std::lock_guard lock(state_->mutex);
    state_->failure = native_code;
    state_->changed.notify_all();
}

void SimulatedCamera::fail_next_finish(std::int64_t native_code)
{
    std::lock_guard lock(state_->mutex);
    state_->finish_failure = native_code;
}

Result<std::monostate> SimulatedCamera::finish()
{
    close();
    std::lock_guard lock(state_->mutex);
    if (const auto failure = std::exchange(state_->finish_failure, {})) {
        auto diagnostic = simulated_error(*failure);
        diagnostic.external->operation = "close";
        return diagnostic;
    }
    return std::monostate{};
}

void SimulatedCamera::close() noexcept
{
    std::lock_guard lock(state_->mutex);
    ++state_->generation;
    state_->pulse = false;
    state_->changed.notify_all();
}

Result<CameraFrame> SimulatedCamera::capture(const CameraCaptureRequest& request, std::stop_token stop)
{
    std::unique_lock capture_lock(state_->capture_mutex, std::try_to_lock);
    if (!capture_lock) { return error(ErrorCode::InputUnavailable, "Simulated camera already has a pending capture."); }
    std::unique_lock lock(state_->mutex);
    if (!state_->connected) { return simulated_error(1001); }
    const auto generation = state_->generation;
    const auto start = Clock::now();
    const auto deadline = start + request.timeout;
    std::optional<std::uint64_t> device_frame_number;
    // Single/software capture is immediate. The simulated continuous clock
    // and periodic hardware pulse each produce one frame after 10 ms.
    if (request.mode == CameraMode::Continuous || request.mode == CameraMode::Hardware) {
        state_->waiting = request.mode == CameraMode::Hardware;
        state_->changed.notify_all();
        while (!stop.stop_requested() && !state_->failure && generation == state_->generation && !state_->pulse) {
            const bool timed = request.mode == CameraMode::Continuous || state_->periodic;
            const auto ready_at = timed ? start + 10ms : Clock::time_point::max();
            const auto now = Clock::now();
            if (ready_at <= deadline && now >= ready_at) { break; }
            if (now >= deadline) { state_->waiting = false; return simulated_error(1002); }
            const bool periodic = state_->periodic;
            state_->changed.wait_until(lock, stop, std::min(deadline, ready_at), [&] {
                return generation != state_->generation || state_->failure || state_->pulse || state_->periodic != periodic;
            });
        }
        state_->waiting = false;
        if (state_->pulse) { device_frame_number = state_->pulse_number; }
        state_->pulse = false;
    }
    if (stop.stop_requested()) { return simulated_error(1003); }
    if (state_->generation != generation || !state_->connected) { return simulated_error(1001); }
    if (state_->failure) {
        const auto code = *std::exchange(state_->failure, {});
        return simulated_error(code);
    }
    if (state_->sequence == std::numeric_limits<std::uint64_t>::max()) {
        return error(ErrorCode::IdentityExhausted, "Simulated camera frame sequence exhausted.");
    }
    const auto sequence = ++state_->sequence;
    state_->buffer.fill(sequence % 2 ? std::byte{7} : std::byte{31});
    auto image = ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, state_->buffer, request.coordinates);
    if (auto* failure = std::get_if<Diagnostic>(&image)) { return std::move(*failure); }
    // One tick per simulated frame. Host arrival is tracked independently.
    return CameraFrame{{std::get<ImageHandle>(std::move(image)), FrameId{request.source, sequence}, std::chrono::system_clock::now()},
        "host_arrival", sequence, 100, 100.0, TimingUnit::Microseconds, device_frame_number.value_or(sequence)};
}

std::shared_ptr<SimulatedCamera> simulated_camera()
{
    static auto camera = std::make_shared<SimulatedCamera>();
    return camera;
}

Result<ConfigValue> camera_device_config(std::string_view serial)
{
    if (!valid_serial(serial)) { return error(ErrorCode::InvalidResource, "Camera serial must be nonempty UTF-8 text without embedded nulls."); }
    return ConfigValue::record(config_type(), {{key<FieldId>("serial"), std::get<ConfigValue>(ConfigValue::string(serial))}});
}

Result<std::string> camera_serial(const ResourceDefinition& resource)
{
    const auto* device = std::get_if<DeviceResource>(&resource.location);
    if (resource.kind != ResourceKind::Device || !device) { return error(ErrorCode::ResourceKindMismatch, "Expected a camera device resource."); }
    if (device->adapter.text() != simulator_id && device->adapter.text() != mvs_id) {
        return error(ErrorCode::MissingCapability, "No adapter was registered for this device type.");
    }
    if (device->configuration.type() != config_type()) {
        return error(ErrorCode::IncompatibleSchemaVersion, "Expected visionpro.camera.device version 1 with a String serial field.");
    }
    const auto& serial = *device->configuration.field(key<FieldId>("serial"))->as_string();
    if (!valid_serial(serial)) { return error(ErrorCode::InvalidResource, "Saved camera serial is empty or contains an embedded null."); }
    return serial;
}

struct CameraLease::Connection {
    Endpoint endpoint;
    std::shared_ptr<CameraDevice> device;
    bool reserved{};
    std::mutex capture_mutex;
    std::optional<Result<std::monostate>> finished;
    ~Connection()
    {
        if (!reserved) { return; }
        if (device) {
            if (!finished || std::holds_alternative<Diagnostic>(*finished)) { device->close(); }
            device.reset();
        }
        auto& table = reservations();
        std::lock_guard lock(table.mutex);
        table.active.erase(endpoint);
    }
};

CameraLease::CameraLease(ResourceReference ref, std::unique_ptr<Connection> connection)
    : ResourceLease(std::move(ref)), connection_(std::move(connection)) {}
CameraLease::~CameraLease() = default;
const std::string& CameraLease::serial() const noexcept { return connection_->endpoint.second; }

Result<std::monostate> CameraLease::finish() const
{
    std::lock_guard lock(connection_->capture_mutex);
    if (!connection_->finished) {
        try { connection_->finished = connection_->device->finish(); }
        catch (...) { connection_->finished = error(ErrorCode::ExecutionBoundaryFailure, "Camera adapter failed while closing."); }
        if (auto* failure = std::get_if<Diagnostic>(&*connection_->finished)) { failure->location.resource = reference().id; }
    }
    return *connection_->finished;
}

Result<std::shared_ptr<CameraLease>> open_camera(const ResourceDefinition& resource)
{
    auto invalid = [&](Diagnostic failure) { failure.location.resource = resource.id; return failure; };
    auto serial = camera_serial(resource);
    if (auto* failure = std::get_if<Diagnostic>(&serial)) { return invalid(*failure); }
    auto c = std::make_unique<CameraLease::Connection>();
    c->endpoint = {std::string(std::get<DeviceResource>(resource.location).adapter.text()), std::get<std::string>(std::move(serial))};
    {
        auto& table = reservations();
        std::lock_guard lock(table.mutex);
        if (!table.active.insert(c->endpoint).second) { return invalid(error(ErrorCode::InputUnavailable, "Camera is already reserved by another acquisition.")); }
        c->reserved = true;
    }
    if (c->endpoint.first == simulator_id) {
        if (c->endpoint.second != "SIM-001") { return invalid(simulated_error(1001)); }
        c->device = simulated_camera();
    } else {
        auto opened = open_mvs_camera(c->endpoint.second);
        if (auto* failure = std::get_if<Diagnostic>(&opened)) { return invalid(*failure); }
        c->device = std::get<std::shared_ptr<CameraDevice>>(std::move(opened));
    }
    return std::shared_ptr<CameraLease>(new CameraLease({resource.id, ResourceKind::Device, {}}, std::move(c)));
}

Result<CameraFrame> CameraLease::capture(const CoordinateSystemId& coordinates, CameraMode mode,
    std::chrono::milliseconds timeout, std::stop_token stop) const
{
    const auto invalid = [&](Diagnostic failure) { failure.location.resource = reference().id; return failure; };
    if (mode < CameraMode::Single || mode > CameraMode::Hardware) { return invalid(error(ErrorCode::InvalidEnumValue, "Unknown camera acquisition mode.")); }
    if (timeout < 1ms || timeout > 60s) { return invalid(error(ErrorCode::ConstraintViolation, "Camera timeout must be between 1 and 60000 ms.")); }
    std::unique_lock lock(connection_->capture_mutex, std::try_to_lock);
    if (!lock) { return invalid(error(ErrorCode::InputUnavailable, "Another capture is active on this camera lease.")); }
    if (connection_->finished) { return invalid(error(ErrorCode::InvalidState, "Camera lease has already been closed.")); }
    if (stop.stop_requested()) { return invalid(error(ErrorCode::SourceCancelled, "Camera capture cancelled before acquisition.")); }
    try {
        auto captured = connection_->device->capture({reference().id, coordinates, mode, timeout}, stop);
        if (auto* failure = std::get_if<Diagnostic>(&captured)) { return invalid(std::move(*failure)); }
        if (stop.stop_requested()) { return invalid(error(ErrorCode::SourceCancelled, "Camera capture cancelled before publication.")); }
        auto& frame = std::get<CameraFrame>(captured);
        if (!frame.image.frame || frame.image.frame->source != reference().id || frame.image.image.coordinate_frame() != coordinates
            || frame.timestamp_source.empty() || frame.tick_frequency_hz == 0) {
            return invalid(error(ErrorCode::InvalidImage, "Camera returned inconsistent frame identity, coordinates or time metadata."));
        }
        return captured;
    } catch (...) {
        return invalid(error(ErrorCode::ExecutionBoundaryFailure, "Camera adapter failed during capture."));
    }
}
} // namespace visionpro::infrastructure
