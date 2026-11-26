#include "image_source_pixels.hpp"

#include <algorithm>
#include <limits>

namespace visionpro::tools::image_source {
namespace {
using namespace core;
Diagnostic issue(ErrorCode code, std::string text) { return {code, Severity::Error, {}, std::move(text)}; }
bool supported(ChannelOrder order) { return order == ChannelOrder::Mono || order == ChannelOrder::RGB || order == ChannelOrder::BGR; }
}

Result<ImageHandle> convert(const ImageHandle& image, ChannelOrder order)
{
    const auto in = image.format();
    if (in.depth != PixelDepth::UInt8 || !supported(in.order) || !supported(order)) {
        return issue(ErrorCode::UnsupportedPixelFormat, "Image source requires UInt8 Mono, RGB or BGR.");
    }
    if (in.order == order) { return image; }
    const std::size_t source_channels = in.order == ChannelOrder::Mono ? 1 : 3;
    const std::size_t channels = order == ChannelOrder::Mono ? 1 : 3;
    if (image.width() > std::numeric_limits<std::size_t>::max() / channels) {
        return issue(ErrorCode::SizeOverflow, "Converted image row exceeds addressable size.");
    }
    const ImageLayout layout{image.width(), image.height(), image.width() * channels, {PixelDepth::UInt8, order}};
    auto count = required_image_bytes(layout);
    if (const auto* bad = std::get_if<Diagnostic>(&count)) { return *bad; }
    std::vector<std::byte> pixels(std::get<std::size_t>(count));
    for (std::size_t y = 0; y < image.height(); ++y) {
        const auto bytes = std::get<std::span<const std::byte>>(image.row(y));
        auto* row = pixels.data() + layout.row_stride * y;
        for (std::size_t x = 0; x < image.width(); ++x) {
            const auto* source = bytes.data() + x * source_channels;
            auto* target = row + x * channels;
            if (source_channels == 1) { std::fill_n(target, channels, *source); continue; }
            if (channels == 3) { target[0] = source[2]; target[1] = source[1]; target[2] = source[0]; continue; }
            const auto red = std::to_integer<unsigned>(source[in.order == ChannelOrder::RGB ? 0 : 2]);
            const auto green = std::to_integer<unsigned>(source[1]);
            const auto blue = std::to_integer<unsigned>(source[in.order == ChannelOrder::RGB ? 2 : 0]);
            target[0] = static_cast<std::byte>((4899 * red + 9617 * green + 1868 * blue + 8192) >> 14);
        }
    }
    return image.copy_with_pixels(layout, pixels);
}

Result<ImageHandle> stitch(const ImageHandle& image, const ImageHandle* previous, std::size_t start,
    std::size_t rows, const CoordinateSystemId& output_frame)
{
    if (output_frame == image.coordinate_frame()) {
        return issue(ErrorCode::CoordinateMismatch, "Select a separate pixel coordinate system for the stitched image.");
    }
    if (previous && (previous->format() != image.format() || previous->width() != image.width()
        || previous->coordinate_frame() != image.coordinate_frame())) { previous = nullptr; }
    if (previous && (start > previous->height() || rows > previous->height() - start)) {
        return issue(ErrorCode::ImageBounds, "Stitch start plus height exceeds the previous image.");
    }
    if (rows > std::numeric_limits<std::size_t>::max() - image.height()) {
        return issue(ErrorCode::SizeOverflow, "Stitched image is too tall.");
    }
    const auto first = std::get<std::span<const std::byte>>(image.row(0));
    const ImageLayout layout{image.width(), image.height() + rows, first.size(), image.format()};
    auto count = required_image_bytes(layout);
    if (const auto* bad = std::get_if<Diagnostic>(&count)) { return *bad; }
    std::vector<std::byte> output(std::get<std::size_t>(count));
    auto copy_rows = [&](const ImageHandle& source, std::size_t from, std::size_t to, std::size_t length) {
        for (std::size_t y = 0; y < length; ++y) {
            const auto data = std::get<std::span<const std::byte>>(source.row(from + y));
            std::ranges::copy(data, output.data() + (to + y) * layout.row_stride);
        }
    };
    if (previous) { copy_rows(*previous, start, 0, rows); }
    copy_rows(image, 0, rows, image.height());
    return ImageHandle::copy(layout, output, output_frame);
}
}
