#include "observation_hub.hpp"

#include <condition_variable>
#include <limits>
#include <utility>

namespace visionpro::workflow {
namespace detail {

class ObservationQueue final {
public:
    explicit ObservationQueue(std::size_t capacity) : events_(capacity) {}

    [[nodiscard]] RunObservationRead wait(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock(mutex_);
        ready_.wait_until(lock, deadline, [this] { return size_ != 0 || closed_; });
        if (size_ == 0) { return {{}, dropped_, closed_}; }
        auto event = std::exchange(events_[head_], std::nullopt);
        head_ = after(head_);
        --size_;
        return {std::move(event), dropped_, false};
    }

    [[nodiscard]] bool push(const RunObservationEvent& event) noexcept
    {
        {
            const std::lock_guard lock(mutex_);
            if (closed_) { return false; }
            if (size_ == events_.size()) {
                head_ = after(head_);
                if (dropped_ != std::numeric_limits<std::uint64_t>::max()) { ++dropped_; }
            } else {
                ++size_;
            }
            events_[tail_] = event;
            tail_ = after(tail_);
        }
        ready_.notify_one();
        return true;
    }

    [[nodiscard]] bool closed() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return closed_;
    }

    void close() noexcept
    {
        std::vector<std::optional<RunObservationEvent>> discarded;
        {
            const std::lock_guard lock(mutex_);
            if (closed_) { return; }
            closed_ = true;
            size_ = 0;
            discarded.swap(events_);
        }
        ready_.notify_all();
    }
private:
    [[nodiscard]] std::size_t after(std::size_t index) const noexcept
    { return index + 1 == events_.size() ? 0 : index + 1; }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<std::optional<RunObservationEvent>> events_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
    std::uint64_t dropped_{0};
    bool closed_{false};
};

core::Result<RunObservation> ObservationHub::subscribe(std::size_t capacity)
{
    if (capacity == 0) {
        return core::Diagnostic{core::ErrorCode::InvalidValue, core::Severity::Error, {},
            "A run observer requires a positive queue capacity."};
    }
    try {
        auto queue = std::make_shared<ObservationQueue>(capacity);
        const std::lock_guard lock(mutex_);
        if (closed_) {
            return core::Diagnostic{core::ErrorCode::InvalidState, core::Severity::Error, {},
                "The engine has closed run observation delivery."};
        }
        std::erase_if(subscribers_, [](const auto& subscriber) {
            const auto existing = subscriber.lock();
            return !existing || existing->closed();
        });
        subscribers_.emplace_back(queue);
        return RunObservation{std::move(queue)};
    } catch (...) {
        return core::Diagnostic{core::ErrorCode::ExecutionBoundaryFailure, core::Severity::Error, {},
            "Could not allocate a run observation queue."};
    }
}

void ObservationHub::publish(const RunIdentity& run, RunObservationKind kind, ExecutionStatus status,
    std::optional<ResultIdentity> result) noexcept
{
    const std::lock_guard lock(mutex_);
    if (closed_) { return; }
    if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        close_locked();
        return;
    }
    const RunObservationEvent event{++sequence_, run, kind, status, std::move(result)};
    std::erase_if(subscribers_, [&](const auto& subscriber) {
        const auto queue = subscriber.lock();
        return !queue || !queue->push(event);
    });
}

void ObservationHub::close() noexcept
{
    const std::lock_guard lock(mutex_);
    if (!closed_) { close_locked(); }
}

void ObservationHub::close_locked() noexcept
{
    closed_ = true;
    for (const auto& subscriber : subscribers_) {
        if (const auto queue = subscriber.lock()) { queue->close(); }
    }
    std::vector<std::weak_ptr<ObservationQueue>>{}.swap(subscribers_);
}

} // namespace detail

RunObservation::RunObservation(std::shared_ptr<detail::ObservationQueue> queue) noexcept : queue_(std::move(queue)) {}

RunObservation::RunObservation(RunObservation&& other) noexcept = default;

RunObservation& RunObservation::operator=(RunObservation&& other) noexcept
{
    if (this != &other) {
        unsubscribe();
        queue_ = std::move(other.queue_);
    }
    return *this;
}

RunObservation::~RunObservation() { unsubscribe(); }

RunObservationRead RunObservation::wait(std::chrono::steady_clock::time_point deadline) const
{
    if (!queue_) { return {{}, 0, true}; }
    return queue_->wait(deadline);
}

void RunObservation::unsubscribe() noexcept
{
    if (queue_) { queue_->close(); }
}

} // namespace visionpro::workflow
