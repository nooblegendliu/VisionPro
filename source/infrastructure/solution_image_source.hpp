#pragma once

#include <visionpro/workflow/model.hpp>

namespace visionpro::infrastructure::detail {

enum class LegacyImageSource { Local, Acquire, Delivered };

[[nodiscard]] std::optional<LegacyImageSource> legacy_image_source(const workflow::Node& node);
void migrate_image_source(workflow::Node& node, LegacyImageSource source);
void migrate_image_sources(workflow::Solution& solution);

} // namespace visionpro::infrastructure::detail
