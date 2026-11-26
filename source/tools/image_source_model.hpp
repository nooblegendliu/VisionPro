#pragma once

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/tool_registry.hpp>

namespace visionpro::tools::image_source {
template<core::SchemaIdentity Id> Id key(std::string_view text)
{ return std::get<Id>(core::SchemaIdFactory::parse<Id>(text)); }

[[nodiscard]] core::ToolDescriptor descriptor();
}
