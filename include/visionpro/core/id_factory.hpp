#pragma once

#include <visionpro/core/id.hpp>
#include <visionpro/core/result.hpp>

#include <string_view>
#include <utility>

namespace visionpro::core {

class IdFactory final {
public:
    // Exactly 8-4-4-4-12 hex digits, either case; UUIDv4 and variant 10 only.
    template <UuidIdentity Id>
    [[nodiscard]] static Result<Id> parse(std::string_view text)
    {
        auto result = parse_bytes(text);
        if (const auto* bytes = std::get_if<detail::UuidBytes>(&result)) {
            return Id{*bytes};
        }
        return std::get<Diagnostic>(std::move(result));
    }

    template <UuidIdentity Id>
    [[nodiscard]] static Result<Id> generate()
    {
        auto result = generate_bytes();
        if (const auto* bytes = std::get_if<detail::UuidBytes>(&result)) {
            return Id{*bytes};
        }
        return std::get<Diagnostic>(std::move(result));
    }

private:
    [[nodiscard]] static Result<detail::UuidBytes> parse_bytes(std::string_view text);
    [[nodiscard]] static Result<detail::UuidBytes> generate_bytes();
};

} // namespace visionpro::core
