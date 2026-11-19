#pragma once

#include <visionpro/core/class_map.hpp>
#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

[[nodiscard]] core::Result<core::DataValue> classify_intensity(const core::ImageFrame& source,
    std::span<const std::int64_t> thresholds, std::span<const std::string> class_names);
[[nodiscard]] core::ToolDescriptor class_map_descriptor();
[[nodiscard]] core::ToolRegistration class_map_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
