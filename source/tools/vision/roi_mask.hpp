#pragma once

#include <visionpro/core/tool_execution.hpp>

namespace visionpro::tools::vision {

struct PreparedRoiMask {
    // point is in the local target coordinates declared by MaskConfig.
    [[nodiscard]] bool contains(core::Vector2D point) const;

private:
    friend class RoiMaskResource;
    PreparedRoiMask(core::ImageHandle pixels, std::size_t target_width, std::size_t target_height,
        std::optional<core::Transform2D> inverse, core::MaskValueRule rule);

    core::ImageHandle pixels_;
    std::size_t target_width_;
    std::size_t target_height_;
    std::optional<core::Transform2D> inverse_;
    core::MaskValueRule rule_;
};

// One invocation owns the lease and can prepare several rules/mappings over the same decoded file.
class RoiMaskResource {
public:
    explicit RoiMaskResource(std::span<const core::InvocationResourceLease> leases);
    [[nodiscard]] core::Result<PreparedRoiMask> prepare(const core::MaskConfig& config);
    [[nodiscard]] core::Result<std::monostate> finish() const;
    [[nodiscard]] std::optional<core::ResourceReference> reference() const;

private:
    std::shared_ptr<const core::ResourceLease> lease_;
    std::optional<core::Diagnostic> slot_failure_;
    std::optional<core::ImageHandle> decoded_;
    bool used_{false};
};

} // namespace visionpro::tools::vision
