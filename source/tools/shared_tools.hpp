#pragma once

#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools {

[[nodiscard]] core::DataType dimensions_type();
[[nodiscard]] core::DataType measurement_fragment_type();
[[nodiscard]] std::vector<core::ToolRegistration> shared_registrations();

} // namespace visionpro::tools
