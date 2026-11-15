#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

[[nodiscard]] core::ToolRegistration read_file_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
