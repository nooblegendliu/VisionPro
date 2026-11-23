#pragma once

#include <visionpro/workflow/workflow_session.hpp>

#include <condition_variable>
#include <mutex>

namespace visionpro::workflow::detail {

// A level-triggered wakeup. Child handles retain the actual completion records.
class SolutionWake {
public:
    void signal()
    {
        const std::lock_guard lock(mutex_);
        ready_ = true;
        changed_.notify_one();
    }
    void wait()
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return ready_; });
        ready_ = false;
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ready_{};
};

using SubmitChild = std::function<RunSubmission(const RunRequest&)>;
[[nodiscard]] core::Result<SolutionRunReport> coordinate_solution(const ConfigSnapshot& snapshot,
    std::uint64_t sequence, std::span<const WorkflowRunInputs> inputs, RunTrigger trigger, std::stop_token stop,
    SolutionWake& wake, const SubmitChild& submit, std::optional<core::CorrelationId> correlation);

} // namespace visionpro::workflow::detail
