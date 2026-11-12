#pragma once

#include <visionpro/workflow/run.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace visionpro::workflow {

enum class RunObservationKind { ResultPublished, RunCompleted, RunQuiescent };

struct RunObservationEvent {
    std::uint64_t sequence;
    RunIdentity run;
    RunObservationKind kind;
    ExecutionStatus status;
    std::optional<ResultIdentity> result;
};

struct RunObservationRead {
    std::optional<RunObservationEvent> event;
    std::uint64_t dropped; // Cumulative count of events evicted on overflow.
    bool closed;
};

namespace detail {
class ObservationHub;
class ObservationQueue;
}

// An optional observer with a bounded queue. Its events identify results
// and run transitions. Clients query the result separately when needed.
class RunObservation final {
public:
    RunObservation(const RunObservation&) = delete;
    RunObservation& operator=(const RunObservation&) = delete;
    RunObservation(RunObservation&& other) noexcept;
    RunObservation& operator=(RunObservation&& other) noexcept;
    ~RunObservation();

    // The deadline can expire while the observer is still open. A read
    // with no event and closed == false indicates that expiry.
    [[nodiscard]] RunObservationRead wait(std::chrono::steady_clock::time_point deadline) const;
    // Discard queued events and wake waiting consumers. Already-read
    // values remain valid. This may run concurrently with wait().
    void unsubscribe() noexcept;
private:
    friend class detail::ObservationHub;
    explicit RunObservation(std::shared_ptr<detail::ObservationQueue> queue) noexcept;
    std::shared_ptr<detail::ObservationQueue> queue_;
};

} // namespace visionpro::workflow
