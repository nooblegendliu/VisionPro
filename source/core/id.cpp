#include <visionpro/core/id.hpp>

#include <cstddef>

namespace visionpro::core::detail {

std::string format_uuid(const UuidBytes& bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text(36, '-');
    std::size_t offset = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            ++offset;
        }
        text[offset++] = digits[bytes[index] >> 4];
        text[offset++] = digits[bytes[index] & 0x0f];
    }
    return text;
}

} // namespace visionpro::core::detail
