#pragma once

#include <visionpro/workflow/run_observation.hpp>

#include <cstddef>
#include <mutex>
#include <vector>

namespace visionpro::workflow::detail {

// One hub per engine session. The scheduler publishes only value metadata.
// Consumers wait on their own queues, without taking the hub mutex.
class ObservationHub final {
public:
    [[nodiscard]] core::Result<RunObservation> subscribe(std::size_t capacity);
    void publish(const RunIdentity& run, RunObservationKind kind, ExecutionStatus status,
        std::optional<ResultIdentity> result = {}) noexcept;
    void close() noexcept;
private:
    void close_locked() noexcept;

    std::mutex mutex_;
    std::vector<std::weak_ptr<ObservationQueue>> subscribers_;
    std::uint64_t sequence_{0};
    bool closed_{false};
};

} // namespace visionpro::workflow::detail
