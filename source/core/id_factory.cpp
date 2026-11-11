#include <visionpro/core/id_factory.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <cstddef>
#include <string>

namespace visionpro::core {
namespace {

int hex_digit(char value) noexcept
{
    if (value >= '0' && value <= '9') { return value - '0'; }
    if (value >= 'a' && value <= 'f') { return value - 'a' + 10; }
    if (value >= 'A' && value <= 'F') { return value - 'A' + 10; }
    return -1;
}

Diagnostic invalid_format()
{
    return {ErrorCode::InvalidIdFormat, Severity::Error, {},
        "Expected a UUID in 8-4-4-4-12 hexadecimal form (36 characters)."};
}

} // namespace

Result<detail::UuidBytes> IdFactory::parse_bytes(std::string_view text)
{
    if (text.size() != 36) {
        return invalid_format();
    }

    detail::UuidBytes bytes{};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            if (text[offset++] != '-') {
                return invalid_format();
            }
        }
        const int high = hex_digit(text[offset++]);
        const int low = hex_digit(text[offset++]);
        if (high < 0 || low < 0) {
            return invalid_format();
        }
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }

    if ((bytes[6] & 0xf0) != 0x40) {
        return Diagnostic{ErrorCode::InvalidIdVersion, Severity::Error, {},
            "Persistent IDs require UUID version 4."};
    }
    if ((bytes[8] & 0xc0) != 0x80) {
        return Diagnostic{ErrorCode::InvalidIdVariant, Severity::Error, {},
            "Persistent IDs require UUID variant 10."};
    }
    return bytes;
}

Result<detail::UuidBytes> IdFactory::generate_bytes()
{
    detail::UuidBytes bytes{};
    const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        return Diagnostic{ErrorCode::IdGenerationFailed, Severity::Error, {},
            "BCryptGenRandom failed; NTSTATUS=" + std::to_string(status)};
    }

    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return bytes;
}

} // namespace visionpro::core
