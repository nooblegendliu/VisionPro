#pragma once

#include <visionpro/core/image.hpp>

#include <opencv2/core.hpp>

#include <string_view>

namespace visionpro::tools::vision {

// Decodes fixed bytes at their native depth and channel count, ignoring EXIF orientation.
[[nodiscard]] core::Result<cv::Mat> decode_image_bytes(std::span<const std::byte> bytes);
// The bridge supports UInt8 Mono and BGR. Copies own their pixel storage.
[[nodiscard]] core::Result<cv::Mat> to_cv_mat(const core::ImageHandle& image);
// A Mat carries depth and channel count. The caller declares the channel order.
[[nodiscard]] core::Result<core::ImageHandle> copy_from_cv_mat(const cv::Mat& matrix,
    core::PixelFormat format, core::CoordinateSystemId coordinates);
// Replaces pixels at the prototype's dimensions, preserving space and acquisition metadata.
[[nodiscard]] core::Result<core::ImageFrame> copy_from_cv_mat(const cv::Mat& matrix,
    core::PixelFormat format, const core::ImageFrame& prototype);
[[nodiscard]] core::Diagnostic opencv_error(core::ErrorCode code, std::string_view operation,
    const cv::Exception& exception);

} // namespace visionpro::tools::vision
