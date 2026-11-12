#pragma once

#include "json_codec.hpp"

#include <visionpro/core/value.hpp>

namespace visionpro::infrastructure::detail {

[[nodiscard]] Json encode_type(const core::DataType& type);
[[nodiscard]] core::DataType decode_type(const Reader& reader);
[[nodiscard]] Json encode_value(const core::ConfigValue& value);
[[nodiscard]] core::ConfigValue decode_value(const Reader& reader);
[[nodiscard]] Json encode_semantics(const core::ValueSemantics& semantics);
[[nodiscard]] core::ValueSemantics decode_semantics(const Reader& reader);
[[nodiscard]] Json encode_coordinate(const core::CoordinateFrameDefinition& frame);
[[nodiscard]] core::CoordinateFrameDefinition decode_coordinate(const Reader& reader);

} // namespace visionpro::infrastructure::detail
