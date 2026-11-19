#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

struct CalibrationFit {
    core::Transform2D transform;
    double rms_error_mm;
};

// Version 1 stores calibration observations in a pinned UTF-8 JSON resource.
[[nodiscard]] core::Result<CalibrationFit> fit_calibration(std::span<const std::byte> bytes);
[[nodiscard]] core::Result<core::Point2D> pixel_to_mm(const core::Transform2D& transform, const core::Point2D& point);
[[nodiscard]] core::Result<double> distance_mm(const core::Point2D& first, const core::Point2D& second);

[[nodiscard]] core::ToolRegistration fit_registration(core::ToolDescriptor descriptor);
[[nodiscard]] core::ToolRegistration map_point_registration(core::ToolDescriptor descriptor);
[[nodiscard]] core::ToolRegistration distance_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
