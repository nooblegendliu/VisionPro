#include <visionpro/core/text.hpp>

#include <cstddef>
#include <cstdint>

namespace visionpro::core {

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
