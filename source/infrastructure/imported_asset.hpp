#pragma once

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/resource.hpp>

namespace visionpro::infrastructure::detail {

inline std::string imported_filename(core::ResourceId id, const core::ManagedResource& resource)
{
    auto digest = resource.sha256;
    for (auto& c : digest) { if (c >= 'A' && c <= 'F') { c = static_cast<char>(c + ('a' - 'A')); } }
    return id.to_string() + "-" + std::to_string(resource.revision) + "-" + digest + ".bin";
}

// Imported immutable versions are already staged. A normal save still verifies
// their bytes under its package lock before first referencing them in the manifest.
inline bool is_imported_asset(core::ResourceId id, const core::ManagedResource& resource)
{
    const std::string_view path = resource.path.path;
    constexpr std::string_view prefix = "assets-";
    constexpr auto slash = prefix.size() + 36;
    return resource.path.base == core::PathBase::SolutionDirectory && !resource.path.named_root
        && path.starts_with(prefix) && path.size() > slash && path[slash] == '/'
        && std::holds_alternative<core::MessageId>(core::IdFactory::parse<core::MessageId>(path.substr(prefix.size(), 36)))
        && path.substr(slash + 1) == imported_filename(id, resource);
}

} // namespace visionpro::infrastructure::detail
