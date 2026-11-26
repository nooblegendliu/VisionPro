#pragma once

#include <visionpro/core/image.hpp>

namespace visionpro::tools::image_source {
[[nodiscard]] core::Result<core::ImageHandle> convert(const core::ImageHandle& image, core::ChannelOrder order);
[[nodiscard]] core::Result<core::ImageHandle> stitch(const core::ImageHandle& image, const core::ImageHandle* previous,
    std::size_t start, std::size_t rows, const core::CoordinateSystemId& output_frame);
}
