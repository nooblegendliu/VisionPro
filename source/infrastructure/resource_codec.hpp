#pragma once

#include "json_codec.hpp"

#include <visionpro/core/resource.hpp>

namespace visionpro::infrastructure::detail {

[[nodiscard]] Json encode_resource_reference(const core::ResourceReference& reference);
[[nodiscard]] core::ResourceReference decode_resource_reference(const Reader& reader);
[[nodiscard]] Json encode_resource(const core::ResourceDefinition& definition);
[[nodiscard]] core::ResourceDefinition decode_resource(const Reader& reader);

} // namespace visionpro::infrastructure::detail
