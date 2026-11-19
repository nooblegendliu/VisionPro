#pragma once

#include "roi.hpp"
#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

struct TranslationMatch {
    double score;
    std::optional<core::Point2D> position;
    std::optional<core::Transform2D> transform;
};

[[nodiscard]] core::Result<TranslationMatch> locate_translation(const core::ImageHandle& scene,
    const core::ImageHandle& pattern, const core::Point2D& reference_origin, double minimum_score);
[[nodiscard]] core::Result<std::array<double, 3>> mean_bgr(const core::ImageHandle& image, const core::RasterMask& mask);

[[nodiscard]] core::ToolRegistration locate_registration(core::ToolDescriptor descriptor);
[[nodiscard]] core::ToolRegistration color_mean_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
