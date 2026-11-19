#pragma once

#include <visionpro/core/camera_mode.hpp>
#include <visionpro/core/tool_execution.hpp>

#include <stop_token>

namespace visionpro::infrastructure {

struct CameraCaptureRequest {
    core::ResourceId source;
    core::CoordinateSystemId coordinates;
    core::CameraMode mode;
    std::chrono::milliseconds timeout;
};

struct CameraFrame {
    core::ImageFrame image;
    std::string timestamp_source;
    std::optional<std::uint64_t> device_ticks;
    std::optional<std::uint64_t> tick_frequency_hz;
    std::optional<double> device_duration{};
    core::TimingUnit device_duration_unit{core::TimingUnit::Unknown};
    std::optional<std::uint64_t> device_frame_number{};
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
[[nodiscard]] core::Result<core::ConfigValue> camera_device_config(std::string_view serial);
[[nodiscard]] core::Result<std::string> camera_serial(const core::ResourceDefinition& device);

class CameraLease final : public core::ResourceLease {
public:
    ~CameraLease() override;
    CameraLease(const CameraLease&) = delete;
    CameraLease& operator=(const CameraLease&) = delete;
    [[nodiscard]] const std::string& serial() const noexcept;
    // Idempotent, including a previously reported failure. The physical endpoint
    // stays reserved while this lease is owned; its backend can no longer capture.
    [[nodiscard]] core::Result<std::monostate> finish() const;
    [[nodiscard]] core::Result<CameraFrame> capture(const core::CoordinateSystemId& coordinates,
        core::CameraMode mode, std::chrono::milliseconds timeout, std::stop_token stop = {}) const;
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
