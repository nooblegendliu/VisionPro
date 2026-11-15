#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::vision {

[[nodiscard]] core::ToolRegistration blob_registration(core::ToolDescriptor descriptor);

} // namespace visionpro::tools::vision
