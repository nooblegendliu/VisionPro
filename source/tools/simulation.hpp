#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools {

[[nodiscard]] core::Result<core::ToolRegistry> make_simulation_registry();

} // namespace visionpro::tools
