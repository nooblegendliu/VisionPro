#include <visionpro/core/schema_id_factory.hpp>

namespace visionpro::core {

bool SchemaIdFactory::valid_text(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 128) {
        return false;
    }
    const auto is_letter = [](char value) {
        return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
    };
    if (!is_letter(text.front())) {
        return false;
    }
    for (const char value : text) {
        if (!is_letter(value) && !(value >= '0' && value <= '9') && value != '.' && value != '-') {
            return false;
        }
    }
    return true;
}

} // namespace visionpro::core
