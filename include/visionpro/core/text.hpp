#pragma once

#include <string_view>

namespace visionpro::core {

// Validates Unicode scalar values in UTF-8, including an embedded U+0000.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

} // namespace visionpro::core
