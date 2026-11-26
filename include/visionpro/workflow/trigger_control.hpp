#pragma once

#include <visionpro/workflow/workflow_session.hpp>

namespace visionpro::workflow {

enum class TriggerStopMode { Drain, Cancel };

struct CameraEventInfo {
    std::optional<std::uint64_t> device_frame_number;
    std::optional<std::uint64_t> device_ticks;
    std::optional<std::uint64_t> tick_frequency_hz;
    std::string timestamp_source;
    std::chrono::microseconds capture_duration;
    std::optional<double> device_duration;
    core::TimingUnit device_duration_unit;
};
struct TriggerReceipt {
    RunTrigger trigger;
    core::InputId frame_input;
    std::int64_t frame;
    std::optional<core::CorrelationId> correlation;
    RunSubmission submission;
    std::optional<CameraEventInfo> camera{};
};

// One subscription to the configured entry of one active solution session.
// Hosts capture this object in callbacks. A new start creates a distinct subscription.
class TriggerControl final {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] static core::Result<std::shared_ptr<TriggerControl>> start(
        std::shared_ptr<WorkflowSession> session, core::WorkflowId workflow,
        Clock::time_point now = Clock::now());
    // SDK delivery numbers increase within one sender subscription. Rejected deliveries may be retried.
    [[nodiscard]] static core::Result<std::shared_ptr<TriggerControl>> start_sdk(
        std::shared_ptr<WorkflowSession> session, const SnapshotIdentity& target, core::WorkflowId workflow,
        core::InputId image_input, core::SchemaId sender);
    // A local subscription requires no synthetic public frame-number input in the workflow.
    // Zero interval selects directory change events, a positive interval selects periodic ticks.
    [[nodiscard]] static core::Result<std::shared_ptr<TriggerControl>> start_local(
        std::shared_ptr<WorkflowSession> session, const SnapshotIdentity& target, core::WorkflowId workflow,
        std::uint32_t interval_ms = 0, Clock::time_point now = Clock::now());
    [[nodiscard]] TriggerReceipt sdk_frame(std::int64_t delivery, const core::ImageFrame& image,
        std::optional<core::CorrelationId> correlation = {}, std::span<const RunInput> inputs = {});
    // Borrows pixels only until this call returns. Padding and row stride are validated before admission.
    [[nodiscard]] TriggerReceipt sdk_frame(std::int64_t delivery, const core::ImageLayout& layout,
        std::span<const std::byte> pixels, core::CoordinateSystemId coordinates,
        std::optional<core::FrameId> frame = {},
        std::optional<std::chrono::sys_time<std::chrono::nanoseconds>> captured_at = {},
        std::optional<core::CorrelationId> correlation = {}, std::span<const RunInput> inputs = {});
    [[nodiscard]] bool accepting() const;
    [[nodiscard]] TriggerReceipt directory_changed(std::int64_t batch, std::vector<DirectoryChange> changes);
    ~TriggerControl();
    TriggerControl(const TriggerControl&) = delete;
    TriggerControl& operator=(const TriggerControl&) = delete;

    // Without an explicit workpiece key the controlled source uses its ID and decimal frame number.
    [[nodiscard]] TriggerReceipt external(std::int64_t frame, std::optional<core::CorrelationId> correlation = {});
    [[nodiscard]] TriggerReceipt camera_frame(std::int64_t number, const core::ImageFrame& image, core::CorrelationId correlation);
    [[nodiscard]] bool matches_camera(core::WorkflowId workflow, const WorkflowTrigger& entry,
        const core::ResourceDefinition& device) const;
    // At most one frame per tick. A delayed host resumes its interval from now.
    [[nodiscard]] std::optional<TriggerReceipt> tick(Clock::time_point now);
    // Seal admission without inspecting accepted runs. A host can then wait on another thread.
    void disarm();
    // Disable admission first. The absolute deadline covers preparation and physical exit.
    // A later call may continue waiting or request cancellation of remaining work.
    [[nodiscard]] core::Result<RunWaitOutcome> stop(TriggerStopMode mode, Clock::time_point deadline);
private:
    static core::Result<std::shared_ptr<TriggerControl>> start_image_entry(std::shared_ptr<WorkflowSession> session,
        const SnapshotIdentity& target, core::WorkflowId workflow, core::SchemaId source,
        std::optional<core::InputId> image_input, std::uint32_t interval_ms, Clock::time_point now);
    struct Impl;
    explicit TriggerControl(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
