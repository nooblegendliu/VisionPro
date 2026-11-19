#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace visionpro::core {

// Validates Unicode scalar values in UTF-8, including an embedded U+0000.
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

[[nodiscard]] std::string encode_hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view text);

} // namespace visionpro::core
