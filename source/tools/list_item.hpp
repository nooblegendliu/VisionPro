#pragma once

#include <visionpro/core/tool_registry.hpp>

#include <string>
#include <string_view>

namespace visionpro::tools {

// Each registered specialization exposes an exact List<T> -> T contract.
[[nodiscard]] core::ToolRegistration list_item_registration(std::string_view id, std::string name,
    const core::DataType& element);

} // namespace visionpro::tools
