#pragma once

#include "package_assets.hpp"
#include <visionpro/workflow/model.hpp>
#include <set>

namespace visionpro::infrastructure::detail {

[[nodiscard]] std::set<core::ResourceId> embedded_image_resources(const workflow::Solution& model);
// Stages immutable copies in the destination. The caller commits assets only after its manifest is published.
[[nodiscard]] core::Result<std::monostate> stage_image_resource(core::ResourceDefinition& resource,
    const ResourcePaths& from, const std::filesystem::path& destination, PackageAssets& transaction, bool relocate);

} // namespace visionpro::infrastructure::detail
