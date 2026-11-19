#pragma once

#include <visionpro/core/value.hpp>

namespace visionpro::core {

inline constexpr std::string_view class_map_schema = "visionpro.image.class_map.result";

struct ClassMapEntry {
    std::uint8_t id;
    std::string name;
    std::int64_t count;
    bool operator==(const ClassMapEntry&) const = default;
};

struct ClassMap {
    ImageFrame pixels;
    std::vector<ClassMapEntry> classes;
};

[[nodiscard]] const DataType& class_map_type();
// Immutable result records contain a Mono8 class-ID raster and the exact
// histogram for its declared classes. Display colors belong to the UI.
[[nodiscard]] Result<DataValue> make_class_map(const ClassMap& map);
[[nodiscard]] Result<ClassMap> read_class_map(const DataValue& value);

} // namespace visionpro::core
