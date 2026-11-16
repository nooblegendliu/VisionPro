#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::app {

[[nodiscard]] core::Result<core::ToolRegistry> make_tool_registry();

} // namespace visionpro::app
