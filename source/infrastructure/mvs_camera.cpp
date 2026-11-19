#include <visionpro/infrastructure/mvs_camera.hpp>
#include <visionpro/infrastructure/camera.hpp>

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/text.hpp>

#ifdef VISIONPRO_WITH_MVS
#include "mvs_frame.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#endif

namespace visionpro::infrastructure {
using namespace core;

#ifdef VISIONPRO_WITH_MVS
namespace {
using Check = Result<std::monostate>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

std::string sdk_version()
{
    const auto value = MV_CC_GetSDKVersion();
    return std::to_string(value >> 24) + "." + std::to_string((value >> 16) & 255)
        + "." + std::to_string((value >> 8) & 255) + "." + std::to_string(value & 255);
}

} // namespace

namespace mvs_detail {
Diagnostic sdk_error(ErrorCode fallback, std::string operation, int status)
{
    static const auto provider = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("hikrobot.mvs"));
    auto code = fallback;
    auto mapping = ExternalCodeMapping::Mapped;
    switch (static_cast<std::uint32_t>(status)) {
    case MV_E_SUPPORT: code = ErrorCode::MissingCapability; break;
    case MV_E_PARAMETER: code = ErrorCode::ConstraintViolation; break;
    case MV_E_BUFOVER: code = ErrorCode::QueueFull; break;
    case MV_E_NODATA:
    case MV_E_GC_TIMEOUT: break; // A disconnected device can also end its frame wait with NODATA.
    case MV_E_ACCESS_DENIED:
    case MV_E_GC_ACCESS:
    case MV_E_BUSY:
    case MV_E_NETER:
    case MV_E_HANDLE: code = ErrorCode::InvalidResource; break;
    default: code = ErrorCode::ExecutionBoundaryFailure; mapping = ExternalCodeMapping::Unmapped; break;
    }
    return {code, Severity::Error, {}, "MVS call failed: " + operation,
        ExternalDiagnostic{provider, std::move(operation), ExternalStatusDomain::Call,
            static_cast<std::uint32_t>(status), mapping, {}, {}, sdk_version()}};
}
} // namespace mvs_detail

namespace {
using mvs_detail::sdk_error;

struct Runtime {
    // Device list pointers are owned by MVS and invalidated by another enumeration.
    std::mutex enumeration;
    bool initialized{};
    ~Runtime() { if (initialized) { MV_CC_Finalize(); } }
};

Result<std::shared_ptr<Runtime>> runtime()
{
    static std::mutex init;
    static std::shared_ptr<Runtime> active;
    std::lock_guard lock(init);
    if (!active) {
        auto next = std::make_shared<Runtime>();
        const auto code = MV_CC_Initialize();
        if (code != MV_OK) { return sdk_error(ErrorCode::MissingCapability, "MV_CC_Initialize", code); }
        next->initialized = true;
        active = std::move(next);
    }
    return active;
}

template<std::size_t Size>
std::string field(const unsigned char (&bytes)[Size])
{
    const auto end = std::find(bytes, bytes + Size, 0);
    return {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes)};
}

std::optional<MvsCameraInfo> describe(const MV_CC_DEVICE_INFO& device)
{
    switch (device.nTLayerType) {
    case MV_GIGE_DEVICE:
    case MV_GENTL_GIGE_DEVICE:
    case MV_VIR_GIGE_DEVICE: {
        const auto& info = device.SpecialInfo.stGigEInfo;
        return MvsCameraInfo{field(info.chSerialNumber), field(info.chModelName), field(info.chDeviceVersion),
            device.nTLayerType == MV_VIR_GIGE_DEVICE ? "VirtualGigE" : "GigE"};
    }
    case MV_USB_DEVICE:
    case MV_VIR_USB_DEVICE: {
        const auto& info = device.SpecialInfo.stUsb3VInfo;
        return MvsCameraInfo{field(info.chSerialNumber), field(info.chModelName), field(info.chDeviceVersion),
            device.nTLayerType == MV_VIR_USB_DEVICE ? "VirtualUSB" : "USB3"};
    }
    default: return {};
    }
}

// Copy device information while the process-wide enumeration lock is held.
Result<std::vector<MV_CC_DEVICE_INFO>> enumerate(Runtime& sdk)
{
    std::lock_guard lock(sdk.enumeration);
    MV_CC_DEVICE_INFO_LIST list{};
    const auto status = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &list);
    if (status != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_EnumDevices", status); }
    if (list.nDeviceNum > MV_MAX_DEVICE_NUM) { return error(ErrorCode::InvalidResource, "MVS returned an invalid device count."); }
    std::vector<MV_CC_DEVICE_INFO> records;
    for (unsigned i = 0; i < list.nDeviceNum; ++i) {
        if (!list.pDeviceInfo[i]) { return error(ErrorCode::InvalidResource, "MVS returned an empty device record."); }
        records.push_back(*list.pDeviceInfo[i]);
    }
    return records;
}

Result<MV_CC_DEVICE_INFO> select(Runtime& sdk, std::string_view serial)
{
    auto listed = enumerate(sdk);
    if (const auto* failure = std::get_if<Diagnostic>(&listed)) { return *failure; }
    std::optional<MV_CC_DEVICE_INFO> match;
    for (const auto& device : std::get<std::vector<MV_CC_DEVICE_INFO>>(listed)) {
        const auto item = describe(device);
        if (!item || item->serial != serial) { continue; }
        if (match) { return error(ErrorCode::InvalidResource, "Multiple MVS devices report the configured serial."); }
        match = device;
    }
    if (!match) { return error(ErrorCode::ResourceNotFound, "The MVS camera with the configured serial was not found."); }
    return *match;
}

Check set_enum(void* handle, const char* feature, const char* value)
{
    // Fixed features can be readable but not writable on a particular camera.
    MVCC_ENUMVALUE current{};
    MVCC_ENUMENTRY entry{};
    if (MV_CC_GetEnumValue(handle, feature, &current) == MV_OK) {
        entry.nValue = current.nCurValue;
        if (MV_CC_GetEnumEntrySymbolic(handle, feature, &entry) == MV_OK &&
            std::string_view(entry.chSymbolic, std::find(std::begin(entry.chSymbolic),
                std::end(entry.chSymbolic), '\0') - std::begin(entry.chSymbolic)) == value) { return std::monostate{}; }
    }
    const int code = MV_CC_SetEnumValueByString(handle, feature, value);
    if (code != MV_OK) {
        return sdk_error(ErrorCode::MissingCapability, std::string("MV_CC_SetEnumValueByString(") + feature + "=" + value + ")", code);
    }
    return std::monostate{};
}

struct BorrowedFrame {
    void* handle;
    MV_FRAME_OUT frame{};
    bool acquired{};
    int release() noexcept {
        if (!acquired) { return MV_OK; }
        acquired = false;
        return MV_CC_FreeImageBuffer(handle, &frame);
    }
    ~BorrowedFrame() { release(); }
};
} // namespace

namespace mvs_detail {

Result<CameraFrame> copy_frame(const CameraCaptureRequest& request, const MV_FRAME_OUT& frame,
    std::optional<std::uint64_t> tick_frequency)
{
    const auto& info = frame.stFrameInfo;
    const auto width = info.nExtendWidth ? info.nExtendWidth : info.nWidth;
    const auto height = info.nExtendHeight ? info.nExtendHeight : info.nHeight;
    if (!frame.pBufAddr || !width || !height || info.nLostPacket || info.nExtraType != MV_FRAME_EXTRA_NO_INFO) {
        return error(ErrorCode::InvalidImage, "MVS delivered an incomplete or multipart image.");
    }
    PixelFormat format{PixelDepth::UInt8, ChannelOrder::Mono};
    if (info.enPixelType == PixelType_Gvsp_BGR8_Packed) { format.order = ChannelOrder::BGR; }
    else if (info.enPixelType != PixelType_Gvsp_Mono8) {
        return error(ErrorCode::UnsupportedPixelFormat, "MVS capture supports native Mono8 or packed BGR8 pixels.");
    }
    const ImageLayout layout{width, height, static_cast<std::size_t>(width) * (format.order == ChannelOrder::Mono ? 1u : 3u), format};
    const auto needed = required_image_bytes(layout);
    if (const auto* failure = std::get_if<Diagnostic>(&needed)) { return *failure; }
    const auto bytes = std::get<std::size_t>(needed);
    const auto length = info.nFrameLenEx ? info.nFrameLenEx : info.nFrameLen;
    if (length != bytes) { return error(ErrorCode::InvalidImage, "MVS frame length differs from the supported packed image layout."); }
    const auto received = std::chrono::system_clock::now();
    auto image = ImageHandle::copy(layout, std::as_bytes(std::span(frame.pBufAddr, bytes)), request.coordinates);
    if (const auto* failure = std::get_if<Diagnostic>(&image)) { return *failure; }
    // A stream reopen may reset the raw 32-bit device counter. Publish a separate application sequence.
    static std::mutex sequence_mutex;
    static std::uint64_t sequence{};
    std::lock_guard lock(sequence_mutex);
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        return error(ErrorCode::IdentityExhausted, "MVS application frame sequence exhausted.");
    }
    const auto ticks = (static_cast<std::uint64_t>(info.nDevTimeStampHigh) << 32) | info.nDevTimeStampLow;
    return CameraFrame{{std::get<ImageHandle>(std::move(image)), FrameId{request.source, ++sequence}, received},
        "host_receive_utc", ticks ? std::optional{ticks} : std::nullopt, tick_frequency, {}, TimingUnit::Unknown, info.nFrameNum};
}
} // namespace mvs_detail

namespace {
class MvsCamera final : public CameraDevice {
public:
    explicit MvsCamera(std::string_view serial) : serial_(serial) {}
    ~MvsCamera() override { close(); }

    void close() noexcept override
    {
        close_generation_.fetch_add(1);
        std::lock_guard lock(mutex_);
        shutdown();
    }

    Check finish() override
    {
        close_generation_.fetch_add(1);
        std::lock_guard lock(mutex_);
        shutdown();
        if (close_failure_.first != MV_OK) {
            return sdk_error(ErrorCode::InvalidResource, close_failure_.second, close_failure_.first);
        }
        return std::monostate{};
    }

    Result<CameraFrame> capture(const CameraCaptureRequest& request, std::stop_token stop) override
    {
        const auto generation = close_generation_.load();
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock) { return error(ErrorCode::InvalidState, "An MVS operation already owns this camera."); }
        if (close_failure_.first != MV_OK) {
            return sdk_error(ErrorCode::InvalidResource, close_failure_.second, close_failure_.first);
        }
        if (request.timeout <= 0ms || request.timeout > std::chrono::milliseconds(std::numeric_limits<int>::max())) {
            return error(ErrorCode::ConstraintViolation, "A positive bounded MVS acquisition timeout is required.");
        }
        const auto deadline = Clock::now() + request.timeout;
        const auto cancelled = [&] { return stop.stop_requested() || generation != close_generation_.load(); };
        if (cancelled()) { return error(ErrorCode::SourceCancelled, "MVS acquisition cancelled."); }
        if (handle_ && mode_ != request.mode) {
            if (auto closed = shutdown(); closed.first != MV_OK) {
                return sdk_error(ErrorCode::InvalidResource, closed.second, closed.first);
            }
        }
        if (!handle_) {
            auto started = start(request.mode);
            if (auto* failure = std::get_if<Diagnostic>(&started)) { shutdown(); return *failure; }
        }
        auto frame = receive(request, deadline, cancelled);
        if (cancelled()) { frame = error(ErrorCode::SourceCancelled, "MVS acquisition cancelled."); }
        if (std::holds_alternative<Diagnostic>(frame) || request.mode == CameraMode::Single) {
            const auto closed = shutdown();
            if (closed.first != MV_OK && !std::holds_alternative<Diagnostic>(frame)) {
                return sdk_error(ErrorCode::InvalidResource, closed.second, closed.first);
            }
        }
        return frame;
    }

private:
    Check start(CameraMode mode)
    {
        auto acquired = runtime();
        if (const auto* failure = std::get_if<Diagnostic>(&acquired)) { return *failure; }
        runtime_ = std::get<std::shared_ptr<Runtime>>(std::move(acquired));
        auto chosen = select(*runtime_, serial_);
        if (const auto* failure = std::get_if<Diagnostic>(&chosen)) { return *failure; }
        int code = MV_CC_CreateHandle(&handle_, &std::get<MV_CC_DEVICE_INFO>(chosen));
        if (code != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_CreateHandle", code); }
        code = MV_CC_OpenDevice(handle_, MV_ACCESS_Exclusive, 0);
        if (code != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_OpenDevice", code); }
        open_ = true;
        const bool triggered = mode == CameraMode::Software || mode == CameraMode::Hardware;
        auto setting = set_enum(handle_, "AcquisitionMode", mode == CameraMode::Single ? "SingleFrame" : "Continuous");
        if (auto* failure = std::get_if<Diagnostic>(&setting)) { return *failure; }
        if (triggered) {
            setting = set_enum(handle_, "TriggerSelector", "FrameStart");
            if (auto* failure = std::get_if<Diagnostic>(&setting)) { return *failure; }
        }
        setting = set_enum(handle_, "TriggerMode", triggered ? "On" : "Off");
        if (auto* failure = std::get_if<Diagnostic>(&setting)) { return *failure; }
        if (triggered) {
            setting = set_enum(handle_, "TriggerSource", mode == CameraMode::Hardware ? "Line0" : "Software");
            if (auto* failure = std::get_if<Diagnostic>(&setting)) { return *failure; }
        }
        MVCC_ENUMVALUE format{};
        code = MV_CC_GetEnumValue(handle_, "PixelFormat", &format);
        if (code != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_GetEnumValue(PixelFormat)", code); }
        if (format.nCurValue != PixelType_Gvsp_Mono8 && format.nCurValue != PixelType_Gvsp_BGR8_Packed) {
            return error(ErrorCode::UnsupportedPixelFormat, "Configured camera format must be Mono8 or BGR8Packed.");
        }
        MVCC_INTVALUE_EX frequency{};
        if (MV_CC_GetIntValueEx(handle_, "GevTimestampTickFrequency", &frequency) == MV_OK && frequency.nCurValue > 0) {
            frequency_ = static_cast<std::uint64_t>(frequency.nCurValue);
        }
        code = MV_CC_StartGrabbing(handle_);
        if (code != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_StartGrabbing", code); }
        grabbing_ = true;
        mode_ = mode;
        return std::monostate{};
    }

    template<class Cancelled>
    Result<CameraFrame> receive(const CameraCaptureRequest& request, Clock::time_point deadline, Cancelled cancelled)
    {
        if (cancelled()) { return error(ErrorCode::SourceCancelled, "MVS acquisition cancelled before trigger."); }
        if (Clock::now() >= deadline) { return error(ErrorCode::SourceTimedOut, "MVS acquisition timed out before retrieving a frame."); }
        if (request.mode == CameraMode::Software) {
            const int code = MV_CC_SetCommandValue(handle_, "TriggerSoftware");
            if (code != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_SetCommandValue(TriggerSoftware)", code); }
        }
        BorrowedFrame buffer{handle_};
        int last = MV_OK;
        while (!cancelled()) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining <= 0ms) {
                if (last != MV_OK) { return sdk_error(ErrorCode::SourceTimedOut, "MV_CC_GetImageBuffer", last); }
                return error(ErrorCode::SourceTimedOut, "MVS frame wait timed out.");
            }
            const auto wait = static_cast<unsigned>(std::min(remaining, 25ms).count());
            last = MV_CC_GetImageBuffer(handle_, &buffer.frame, wait);
            if (last == MV_OK) {
                buffer.acquired = true;
                Result<CameraFrame> frame = Clock::now() <= deadline
                    ? mvs_detail::copy_frame(request, buffer.frame, frequency_)
                    : Result<CameraFrame>{error(ErrorCode::SourceTimedOut, "MVS delivered a frame after the request deadline.")};
                const int freed = buffer.release();
                if (freed != MV_OK) { return sdk_error(ErrorCode::InvalidResource, "MV_CC_FreeImageBuffer", freed); }
                return frame;
            }
            if (static_cast<unsigned>(last) != MV_E_NODATA && static_cast<unsigned>(last) != MV_E_GC_TIMEOUT) {
                return sdk_error(ErrorCode::InvalidResource, "MV_CC_GetImageBuffer", last);
            }
            if (!MV_CC_IsDeviceConnected(handle_)) {
                auto failure = sdk_error(ErrorCode::ResourceNotFound, "MV_CC_GetImageBuffer", last);
                failure.message = "MVS reports that the camera disconnected while waiting for a frame.";
                return failure;
            }
        }
        return error(ErrorCode::SourceCancelled, "MVS frame wait cancelled.");
    }

    std::pair<int, const char*> shutdown() noexcept
    {
        std::pair<int, const char*> first{MV_OK, ""};
        const auto record = [&](int result, const char* operation) {
            if (first.first == MV_OK && result != MV_OK) { first = {result, operation}; }
        };
        if (grabbing_) { record(MV_CC_StopGrabbing(handle_), "MV_CC_StopGrabbing"); }
        if (open_) { record(MV_CC_CloseDevice(handle_), "MV_CC_CloseDevice"); }
        if (handle_) { record(MV_CC_DestroyHandle(handle_), "MV_CC_DestroyHandle"); }
        handle_ = nullptr;
        open_ = grabbing_ = false;
        frequency_.reset();
        runtime_.reset();
        if (close_failure_.first == MV_OK && first.first != MV_OK) { close_failure_ = first; }
        return first;
    }

    std::string serial_;
    std::shared_ptr<Runtime> runtime_;
    std::mutex mutex_;
    std::atomic<std::uint64_t> close_generation_{};
    void* handle_{};
    bool open_{}, grabbing_{};
    CameraMode mode_{CameraMode::Single};
    std::optional<std::uint64_t> frequency_;
    // Capture failure can initiate teardown before the owner explicitly finishes the lease.
    std::pair<int, const char*> close_failure_{MV_OK, ""};
};
} // namespace

Result<MvsCameraInventory> query_mvs_cameras()
{
    auto sdk = runtime();
    if (const auto* failure = std::get_if<Diagnostic>(&sdk)) { return *failure; }
    auto list = enumerate(*std::get<std::shared_ptr<Runtime>>(sdk));
    if (const auto* failure = std::get_if<Diagnostic>(&list)) { return *failure; }
    MvsCameraInventory result{sdk_version(), {}};
    for (const auto& device : std::get<std::vector<MV_CC_DEVICE_INFO>>(list)) {
        if (auto item = describe(device)) { result.devices.push_back(std::move(*item)); }
    }
    return result;
}

Result<std::shared_ptr<CameraDevice>> open_mvs_camera(std::string_view serial)
{
    if (serial.empty() || serial.find('\0') != serial.npos || !is_valid_utf8(serial)) {
        return error(ErrorCode::InvalidResource, "MVS requires a nonempty UTF-8 serial without embedded NUL bytes.");
    }
    return std::shared_ptr<CameraDevice>{std::make_shared<MvsCamera>(serial)};
}
#else
Result<MvsCameraInventory> query_mvs_cameras()
{ return Diagnostic{ErrorCode::MissingCapability, Severity::Error, {}, "MVS camera support is disabled in this build."}; }

Result<std::shared_ptr<CameraDevice>> open_mvs_camera(std::string_view)
{ return Diagnostic{ErrorCode::MissingCapability, Severity::Error, {}, "MVS camera support is disabled in this build."}; }
#endif
} // namespace visionpro::infrastructure
