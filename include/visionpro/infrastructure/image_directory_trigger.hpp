#pragma once

#include <visionpro/workflow/trigger_control.hpp>

namespace visionpro::infrastructure {

// Poll from a host worker. Two matching scans coalesce a change batch.
class ImageDirectoryTrigger final {
public:
    [[nodiscard]] static bool configured(const workflow::Workflow& flow);
    [[nodiscard]] static core::Result<std::shared_ptr<ImageDirectoryTrigger>> open(
        std::shared_ptr<workflow::WorkflowSession> session, core::WorkflowId workflow);
    [[nodiscard]] core::Result<std::optional<workflow::TriggerReceipt>> poll();
    [[nodiscard]] const std::shared_ptr<workflow::TriggerControl>& control() const;
    ~ImageDirectoryTrigger();
private:
    struct Impl;
    explicit ImageDirectoryTrigger(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::infrastructure
