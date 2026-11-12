#pragma once

#include "json_codec.hpp"

#include <visionpro/core/geometry.hpp>

namespace visionpro::infrastructure::detail {

[[nodiscard]] Json encode_point(const core::Point2D& point);
[[nodiscard]] core::Point2D decode_point(const Reader& reader);
[[nodiscard]] Json encode_line(const core::Line2D& line);
[[nodiscard]] core::Line2D decode_line(const Reader& reader);
[[nodiscard]] Json encode_circle(const core::Circle2D& circle);
[[nodiscard]] core::Circle2D decode_circle(const Reader& reader);
[[nodiscard]] Json encode_pose(const core::Pose2D& pose);
[[nodiscard]] core::Pose2D decode_pose(const Reader& reader);
[[nodiscard]] Json encode_transform(const core::Transform2D& transform);
[[nodiscard]] core::Transform2D decode_transform(const Reader& reader);
[[nodiscard]] Json encode_region(const core::RegionDefinition& region);
[[nodiscard]] core::RegionDefinition decode_region(const Reader& reader);

} // namespace visionpro::infrastructure::detail
