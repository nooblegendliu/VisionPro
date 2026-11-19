#pragma once

#include <visionpro/core/tool_execution.hpp>

namespace visionpro::tools::vision {

struct RoiLayerTrace {
    core::RegionDefinition configured;
    std::optional<core::ToolInvocationIdentity> producer;
    std::optional<core::Transform2D> to_target;
    core::Vector2D direction;
    std::size_t member_count;
};

struct RoiEvaluation {
    core::RasterMask mask;
    std::size_t member_count;
    core::Vector2D direction;
    std::vector<RoiLayerTrace> layers;
    std::optional<core::ResourceReference> mask_resource;
    // Check this result when publishing Region2D. Some layer policies cannot
    // be expressed in a single RegionDefinition even when sampling succeeds.
    core::Result<core::RegionDefinition> effective_region;
};

// For a top-level Region2D (including Optional<Region2D> with a value), pass
// the validated invocation's frozen sources and leases for this binding.
[[nodiscard]] core::Result<RoiEvaluation> evaluate_roi(const core::ImageHandle& image,
    const core::RegionDefinition& region, const core::ToolValueTarget& binding,
    std::span<const core::ResolvedRegionInput> regions = {},
    std::span<const core::InvocationResourceLease> resources = {},
    const std::optional<core::Transform2D>& placement = {});

// Evaluate draft geometry with a separately resolved, self-contained configuration source.
[[nodiscard]] core::Result<RoiEvaluation> evaluate_roi_preview(const core::ImageHandle& image,
    const core::RegionDefinition& region, const core::ToolValueTarget& binding,
    const core::RegionDefinition* inherited_source,
    std::span<const core::InvocationResourceLease> resources = {});

} // namespace visionpro::tools::vision
