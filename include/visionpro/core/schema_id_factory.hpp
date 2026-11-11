#pragma once

#include <visionpro/core/schema_id.hpp>
#include <visionpro/core/result.hpp>

#include <string>
#include <string_view>

namespace visionpro::core {

class SchemaIdFactory final {
public:
    // ASCII [A-Za-z_][A-Za-z0-9_.-]*, 1..128 bytes. No normalization or trimming.
    template <SchemaIdentity Id>
    [[nodiscard]] static Result<Id> parse(std::string_view text)
    {
        if (!valid_text(text)) {
            return Diagnostic{ErrorCode::InvalidSchemaId, Severity::Error, {},
                "Schema IDs require 1..128 ASCII characters matching [A-Za-z_][A-Za-z0-9_.-]*."};
        }
        return Id{std::string(text)};
    }

private:
    [[nodiscard]] static bool valid_text(std::string_view text) noexcept;
};

} // namespace visionpro::core
