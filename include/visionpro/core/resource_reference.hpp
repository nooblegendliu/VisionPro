#pragma once

#include <visionpro/core/result.hpp>

#include <cstdint>
#include <optional>

namespace visionpro::core {

enum class ResourceKind { Image, Mask, Template, Calibration, Device, DirectorySequence, OutputPath, Binary };

struct ResourceReference {
    ResourceId id;
    ResourceKind kind;
    std::optional<std::uint64_t> revision;
    bool operator==(const ResourceReference&) const = default;
};

[[nodiscard]] Result<std::monostate> validate_resource_reference(const ResourceReference& reference);

} // namespace visionpro::core
