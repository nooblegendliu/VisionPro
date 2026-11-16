#pragma once

#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/model.hpp>

namespace visionpro::app {

[[nodiscard]] core::Result<core::ImageHandle> read_preview_image(const std::filesystem::path& absolute_file,
    const core::CoordinateSystemId& frame, bool grayscale);
[[nodiscard]] core::Result<core::ImageHandle> read_preview_resource(const workflow::Solution& solution,
    const core::ResourceReference& reference, const core::CoordinateSystemId& frame, bool grayscale,
    const std::optional<std::filesystem::path>& solution_directory,
    std::span<const infrastructure::NamedResourceRoot> roots = {});

struct RoiPreview {
    core::RasterMask mask;
    std::optional<core::RegionDefinition> effective_region;
    std::size_t member_count;
    std::optional<core::Diagnostic> outline_issue;
};

// Sample a draft against the given image. Known threshold ROI declarations can
// supply inherited geometry from the same configured image chain and scope.
[[nodiscard]] core::Result<RoiPreview> preview_roi(const core::ImageHandle& image, const workflow::Solution& solution,
    const core::WorkflowId& workflow, const core::NodeId& node, const core::ToolValueTarget& target,
    const core::RegionDefinition& region, const std::optional<std::filesystem::path>& solution_directory,
    std::span<const infrastructure::NamedResourceRoot> roots = {});

} // namespace visionpro::app
