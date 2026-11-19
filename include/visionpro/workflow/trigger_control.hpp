#pragma once

#include <visionpro/workflow/workflow_session.hpp>

namespace visionpro::workflow {

enum class TriggerStopMode { Drain, Cancel };

struct TriggerReceipt {
    RunTrigger trigger;
    core::InputId frame_input;
    std::int64_t frame;
    std::optional<core::CorrelationId> correlation;
    RunSubmission submission;
};

// One subscription to the configured entry of one active solution session.
// Hosts capture this object in callbacks. A new start creates a distinct subscription.
class TriggerControl final {
public:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] static core::Result<std::shared_ptr<TriggerControl>> start(
        std::shared_ptr<WorkflowSession> session, core::WorkflowId workflow,
        Clock::time_point now = Clock::now());
    ~TriggerControl();
    TriggerControl(const TriggerControl&) = delete;
    TriggerControl& operator=(const TriggerControl&) = delete;

    // Without an explicit workpiece key the controlled source uses its ID and decimal frame number.
    [[nodiscard]] TriggerReceipt external(std::int64_t frame, std::optional<core::CorrelationId> correlation = {});
    // At most one frame per tick. A delayed host resumes its interval from now.
    [[nodiscard]] std::optional<TriggerReceipt> tick(Clock::time_point now);
    // Seal admission without inspecting accepted runs. A host can then wait on another thread.
    void disarm();
    // Disable admission first. The absolute deadline covers preparation and physical exit.
    // A later call may continue waiting or request cancellation of remaining work.
    [[nodiscard]] core::Result<RunWaitOutcome> stop(TriggerStopMode mode, Clock::time_point deadline);
private:
    struct Impl;
    explicit TriggerControl(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
