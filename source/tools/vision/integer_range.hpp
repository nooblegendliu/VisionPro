#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

[[nodiscard]] core::ToolDescriptor integer_range_descriptor();
[[nodiscard]] core::ToolRegistration integer_range_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
