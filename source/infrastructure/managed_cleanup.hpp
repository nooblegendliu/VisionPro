#pragma once

#include <visionpro/infrastructure/solution_file.hpp>

namespace visionpro::infrastructure::detail {

[[nodiscard]] core::Result<std::monostate> verify_managed_resource(const core::ManagedResource& resource,
    const std::filesystem::path& directory);

// The caller holds the package save lock while checking the current manifest
// and removing old versions.
[[nodiscard]] SolutionCleanupReport cleanup_managed_versions(const workflow::Solution& previous,
    const workflow::Solution& current, const std::filesystem::path& directory,
    std::span<const NamedResourceRoot> roots);

} // namespace visionpro::infrastructure::detail
