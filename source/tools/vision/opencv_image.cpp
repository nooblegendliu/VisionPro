#include "opencv_image.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <opencv2/core/version.hpp>
#include <opencv2/imgcodecs.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace visionpro::tools::vision {
namespace {

using namespace core;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Result<int> cv_type(PixelFormat format)
{
    if (format == PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) { return CV_8UC1; }
    if (format == PixelFormat{PixelDepth::UInt8, ChannelOrder::BGR}) { return CV_8UC3; }
    return error(ErrorCode::UnsupportedPixelFormat, "OpenCV image bridge supports UInt8 Mono and UInt8 BGR.");
}

template<class Publish>
Result<ImageHandle> copy_mat(const cv::Mat& matrix, PixelFormat format, Publish publish)
{
    if (matrix.dims != 2 || matrix.cols <= 0 || matrix.rows <= 0 || !matrix.data) {
        return error(ErrorCode::InvalidImage, "An OpenCV image must be a nonempty two-dimensional matrix.");
    }
    const auto type = cv_type(format);
    if (const auto* failure = std::get_if<Diagnostic>(&type)) { return *failure; }
    if (matrix.type() != std::get<int>(type)) {
        return error(ErrorCode::UnsupportedPixelFormat, "OpenCV matrix depth or channel count differs from the declared format.");
    }

    const auto pixel_bytes = std::get<std::size_t>(bytes_per_pixel(format));
    if (matrix.step[1] != pixel_bytes) {
        return error(ErrorCode::InvalidImage, "OpenCV image pixels require adjacent interleaved samples.");
    }
    const ImageLayout source{static_cast<std::size_t>(matrix.cols), static_cast<std::size_t>(matrix.rows), matrix.step[0], format};
    const auto required = required_image_bytes(source);
    if (const auto* failure = std::get_if<Diagnostic>(&required)) { return *failure; }
    const auto required_bytes = std::get<std::size_t>(required);
    const auto first = reinterpret_cast<std::uintptr_t>(matrix.data);
    const auto end = reinterpret_cast<std::uintptr_t>(matrix.dataend);
    if (end < first || end - first < required_bytes) {
        return error(ErrorCode::InvalidImage, "OpenCV image storage is shorter than its declared layout.");
    }

    const auto source_bytes = std::as_bytes(std::span<const unsigned char>{matrix.data, required_bytes});
    const auto row_bytes = source.width * pixel_bytes;
    const ImageLayout packed{source.width, source.height, row_bytes, format};
    if (source.row_stride == row_bytes) { return publish(packed, source_bytes); }

    // The validated strided size bounds the packed allocation and every row copy.
    std::vector<std::byte> pixels(row_bytes * source.height);
    for (std::size_t y = 0; y < source.height; ++y) {
        const auto row = source_bytes.subspan(y * source.row_stride, row_bytes);
        std::memcpy(pixels.data() + y * row_bytes, row.data(), row.size());
    }
    return publish(packed, pixels);
}

} // namespace

core::Result<cv::Mat> decode_image_bytes(std::span<const std::byte> bytes)
{
    if (bytes.empty()) { return error(ErrorCode::InvalidImage, "Encoded image is empty."); }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return error(ErrorCode::SizeOverflow, "Encoded image exceeds OpenCV's input array size.");
    }
    std::vector<unsigned char> encoded(bytes.size());
    std::memcpy(encoded.data(), bytes.data(), bytes.size());
    try {
        auto pixels = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
        if (pixels.empty()) { return error(ErrorCode::InvalidImage, "The source bytes could not be decoded as an image."); }
        return pixels;
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::InvalidImage, "imdecode", failure);
    }
}

core::Result<cv::Mat> to_cv_mat(const core::ImageHandle& image)
{
    const auto type = cv_type(image.format());
    if (const auto* failure = std::get_if<Diagnostic>(&type)) { return *failure; }
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
    if (image.width() > limit || image.height() > limit) {
        return error(ErrorCode::SizeOverflow, "Image dimensions exceed the OpenCV matrix limits.");
    }

    try {
        cv::Mat matrix(static_cast<int>(image.height()), static_cast<int>(image.width()), std::get<int>(type));
        for (int y = 0; y < matrix.rows; ++y) {
            const auto row = std::get<std::span<const std::byte>>(image.row(static_cast<std::size_t>(y)));
            std::memcpy(matrix.ptr(y), row.data(), row.size());
        }
        return matrix;
    } catch (const cv::Exception& exception) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "Mat::create", exception);
    }
}

core::Result<core::ImageHandle> copy_from_cv_mat(const cv::Mat& matrix,
    core::PixelFormat format, core::CoordinateSystemId coordinates)
{
    return copy_mat(matrix, format, [&](const ImageLayout& layout, std::span<const std::byte> pixels) {
        return ImageHandle::copy(layout, pixels, std::move(coordinates));
    });
}

core::Result<core::ImageFrame> copy_from_cv_mat(const cv::Mat& matrix,
    core::PixelFormat format, const core::ImageFrame& prototype)
{
    auto copied = copy_mat(matrix, format, [&](const ImageLayout& layout, std::span<const std::byte> pixels) {
        return prototype.image.copy_with_pixels(layout, pixels);
    });
    if (const auto* failure = std::get_if<Diagnostic>(&copied)) { return *failure; }
    return ImageFrame{std::get<ImageHandle>(std::move(copied)), prototype.frame, prototype.captured_at};
}

core::Diagnostic opencv_error(core::ErrorCode code, std::string_view operation, const cv::Exception& exception)
{
    static const auto provider = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("opencv"));
    const auto name = std::string(operation);
    return {code, Severity::Error, {}, "OpenCV " + name + " failed.",
        ExternalDiagnostic{provider, name, ExternalStatusDomain::Call, exception.code,
            ExternalCodeMapping::Mapped, {}, {}, CV_VERSION}};
}

} // namespace visionpro::tools::vision
