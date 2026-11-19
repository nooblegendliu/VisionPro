#pragma once

#include <visionpro/core/class_map.hpp>

namespace visionpro::core::detail {
// Run once before publishing an immutable DataValue, including records built
// directly through the general Record API.
[[nodiscard]] Result<std::monostate> validate_class_map_record(const DataValue& value);
}
