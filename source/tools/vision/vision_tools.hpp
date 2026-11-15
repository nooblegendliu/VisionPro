#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

[[nodiscard]] std::span<const core::ToolDescriptor> tool_descriptors();
[[nodiscard]] std::vector<core::ToolRegistration> registrations();

// Workflow validation checks that the frame is declared by the solution.
[[nodiscard]] core::Result<std::monostate> validate_file_frame_origin(const core::Point2D& origin);
// Check one symbolic layer. Only the evaluated target must be a Pixel frame.
[[nodiscard]] core::Result<std::monostate> validate_region_support(const core::RegionDefinition& region);

} // namespace visionpro::tools::vision
