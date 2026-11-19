#include <visionpro/core/text.hpp>

#include <cstddef>
#include <cstdint>

namespace visionpro::core {

std::string encode_hex(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}

std::optional<std::vector<std::uint8_t>> decode_hex(std::string_view text)
{
    if (text.size() % 2 != 0) { return {}; }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0) { return {}; }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

bool is_valid_utf8(std::string_view text) noexcept
{
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80) { continue; }
        unsigned count = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1; codepoint = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2; codepoint = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3; codepoint = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        if (text.size() - index < count) { return false; }
        for (unsigned continuation = 0; continuation < count; ++continuation) {
            const auto byte = static_cast<unsigned char>(text[index++]);
            if ((byte & 0xc0) != 0x80) { return false; }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff
            || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

} // namespace visionpro::core
