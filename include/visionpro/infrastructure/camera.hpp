#pragma once

#include <visionpro/core/camera_mode.hpp>
#include <visionpro/core/tool_execution.hpp>

#include <stop_token>

namespace visionpro::infrastructure {

struct CameraCaptureControls {
    std::optional<double> exposure_us{};
    std::optional<double> gain{};
    bool operator==(const CameraCaptureControls&) const = default;
};

struct CameraCaptureRequest {
    core::ResourceId source;
    core::CoordinateSystemId coordinates;
    core::CameraMode mode;
    std::chrono::milliseconds timeout;
    CameraCaptureControls controls{};
    unsigned strobe_channels{};
};

struct CameraFrame {
    core::ImageFrame image;
    std::string timestamp_source;
    std::optional<std::uint64_t> device_ticks;
    std::optional<std::uint64_t> tick_frequency_hz;
    std::optional<double> device_duration{};
    core::TimingUnit device_duration_unit{core::TimingUnit::Unknown};
    std::optional<std::uint64_t> device_frame_number{};
    std::optional<std::uint64_t> lost_packets{};
    std::optional<std::uint64_t> lost_frames{};
    std::optional<std::uint64_t> acquire_failures{};
    // Only a backend with a verified strobe mapping supplies these in light-channel order.
    std::vector<core::ImageFrame> strobe_images{};
};

class CameraDevice {
public:
    virtual ~CameraDevice() = default;
    // Return only after SDK buffers have been released and the published image owns its data.
    [[nodiscard]] virtual core::Result<CameraFrame> capture(const CameraCaptureRequest& request, std::stop_token stop) = 0;
    [[nodiscard]] virtual core::Result<std::monostate> finish() { close(); return std::monostate{}; }
    virtual void close() noexcept = 0;
};

// A persistent simulated physical device. Hardware mode normally receives a
// periodic pulse. Tests may disable that source and drive individual pulses.
class SimulatedCamera final : public CameraDevice {
public:
    SimulatedCamera();
    ~SimulatedCamera() override;
    SimulatedCamera(const SimulatedCamera&) = delete;
    SimulatedCamera& operator=(const SimulatedCamera&) = delete;
    void set_connected(bool connected);
    void set_periodic_hardware_pulses(bool enabled);
    void pulse(std::optional<std::uint64_t> device_frame_number = {});
    [[nodiscard]] bool wait_until_armed(std::chrono::steady_clock::time_point deadline);
    void fail_next(std::int64_t native_code);
    void fail_next_finish(std::int64_t native_code);
    [[nodiscard]] core::Result<CameraFrame> capture(const CameraCaptureRequest& request, std::stop_token stop) override;
    [[nodiscard]] core::Result<std::monostate> finish() override;
    void close() noexcept override;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// Returns the same SIM-001 endpoint used by the device resource provider.
[[nodiscard]] std::shared_ptr<SimulatedCamera> simulated_camera();
struct CameraAcquisitionSettings {
    core::CameraMode mode{core::CameraMode::Single};
    std::chrono::milliseconds timeout{1000};
    bool operator==(const CameraAcquisitionSettings&) const = default;
};
[[nodiscard]] core::Result<core::ConfigValue> camera_device_config(std::string_view serial, unsigned strobe_channels = 0,
    std::optional<CameraAcquisitionSettings> acquisition = {});
[[nodiscard]] core::Result<std::string> camera_serial(const core::ResourceDefinition& device);
[[nodiscard]] core::Result<unsigned> camera_strobe_channels(const core::ResourceDefinition& device);
[[nodiscard]] core::Result<std::optional<CameraAcquisitionSettings>> camera_acquisition_settings(const core::ResourceDefinition& device);

class CameraLease final : public core::ResourceLease {
public:
    ~CameraLease() override;
    CameraLease(const CameraLease&) = delete;
    CameraLease& operator=(const CameraLease&) = delete;
    [[nodiscard]] const std::string& serial() const noexcept;
    [[nodiscard]] const std::optional<CameraAcquisitionSettings>& acquisition_settings() const noexcept;
    // Idempotent, including a previously reported failure. The physical endpoint
    // stays reserved while this lease is owned; its backend can no longer capture.
    [[nodiscard]] core::Result<std::monostate> finish() const;
    [[nodiscard]] core::Result<CameraFrame> capture(const core::CoordinateSystemId& coordinates,
        core::CameraMode mode, std::chrono::milliseconds timeout, std::stop_token stop = {},
        CameraCaptureControls controls = {}) const;
private:
    friend core::Result<std::shared_ptr<CameraLease>> open_camera(const core::ResourceDefinition&);
    struct Connection;
    CameraLease(core::ResourceReference ref, std::unique_ptr<Connection> connection);
    std::unique_ptr<Connection> connection_;
};

// Validates the saved device configuration and reserves its adapter+serial.
// The backend opens on first capture. Concurrent leases for aliases of the same
// endpoint are rejected until physical exit and close release the reservation.
[[nodiscard]] core::Result<std::shared_ptr<CameraLease>> open_camera(const core::ResourceDefinition& device);
[[nodiscard]] core::Result<std::shared_ptr<CameraDevice>> open_mvs_camera(std::string_view serial);

} // namespace visionpro::infrastructure
