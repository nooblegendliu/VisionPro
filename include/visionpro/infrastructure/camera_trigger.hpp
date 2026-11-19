#pragma once

#include <visionpro/workflow/trigger_control.hpp>
#include <stop_token>

namespace visionpro::infrastructure {

struct CameraStreamStatistics {
    std::uint64_t received{}, accepted{}, rejected{}, missing{}, duplicates{};
};

// One exclusive device lease per subscription. next and close run on host workers.
class CameraTriggerStream final {
public:
    static core::Result<std::shared_ptr<CameraTriggerStream>> open(const workflow::Solution& solution,
        core::WorkflowId workflow, std::shared_ptr<workflow::TriggerControl> control);
    ~CameraTriggerStream();
    core::Result<workflow::TriggerReceipt> next(std::stop_token stop = {});
    void request_stop();
    core::Result<std::monostate> close();
    [[nodiscard]] CameraStreamStatistics statistics() const;
private:
    struct Impl;
    explicit CameraTriggerStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::infrastructure
