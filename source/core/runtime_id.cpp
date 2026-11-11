#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/text.hpp>

#include <string>
#include <utility>

namespace visionpro::core {

Result<CorrelationId> RuntimeIdFactory::correlation(CorrelationDomainId domain, std::string_view key)
{
    if (key.empty() || key.size() > 256 || key.find('\0') != std::string_view::npos || !is_valid_utf8(key)) {
        return Diagnostic{ErrorCode::InvalidCorrelationId, Severity::Error, {},
            "Correlation keys require 1..256 bytes of valid UTF-8 without embedded NUL."};
    }
    return CorrelationId{std::move(domain), std::string(key)};
}

} // namespace visionpro::core
