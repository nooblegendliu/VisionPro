#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools {
[[nodiscard]] std::vector<core::ToolRegistration> camera_registrations();
}
