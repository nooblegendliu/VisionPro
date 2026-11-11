#include <visionpro/core/image.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace visionpro::core {
namespace {

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

bool multiply_overflows(std::size_t a, std::size_t b) noexcept
{ return b != 0 && a > std::numeric_limits<std::size_t>::max() / b; }

} // namespace

Result<std::size_t> bytes_per_pixel(PixelFormat format)
{
    std::size_t depth = 0;
    switch (format.depth) {
    case PixelDepth::UInt8: depth = 1; break;
    case PixelDepth::UInt16: depth = 2; break;
    case PixelDepth::Float32: depth = 4; break;
    case PixelDepth::Float64: depth = 8; break;
    default: return error(ErrorCode::UnsupportedPixelFormat, "Unsupported image sample depth.");
    }
    std::size_t channels = 0;
    switch (format.order) {
    case ChannelOrder::Mono: channels = 1; break;
    case ChannelOrder::RGB:
    case ChannelOrder::BGR: channels = 3; break;
    case ChannelOrder::RGBA:
    case ChannelOrder::BGRA: channels = 4; break;
    default: return error(ErrorCode::UnsupportedPixelFormat, "Unsupported image channel order.");
    }
    return depth * channels;
}

Result<std::size_t> required_image_bytes(const ImageLayout& layout)
{
    auto pixel_size = bytes_per_pixel(layout.format);
    if (const auto* failure = std::get_if<Diagnostic>(&pixel_size)) { return *failure; }
    if (layout.width == 0 || layout.height == 0) {
        return error(ErrorCode::InvalidImage, "Image dimensions must both be positive.");
    }
    const auto bytes = std::get<std::size_t>(pixel_size);
    if (multiply_overflows(layout.width, bytes)) {
        return error(ErrorCode::SizeOverflow, "Image row byte count overflows size_t.");
    }
    const auto row_bytes = layout.width * bytes;
    if (layout.row_stride < row_bytes) {
        return error(ErrorCode::InvalidImage, "Row stride is smaller than the declared pixels.");
    }
    if (multiply_overflows(layout.height - 1, layout.row_stride)) {
        return error(ErrorCode::SizeOverflow, "Image buffer offset overflows size_t.");
    }
    const auto offset = (layout.height - 1) * layout.row_stride;
    if (offset > std::numeric_limits<std::size_t>::max() - row_bytes) {
        return error(ErrorCode::SizeOverflow, "Image buffer length overflows size_t.");
    }
    return offset + row_bytes;
}

struct ImageHandle::View {
    std::shared_ptr<const std::vector<std::byte>> storage;
    ImageLayout layout;
    std::size_t offset;
    std::size_t pixel_bytes;
    CoordinateSystemId coordinates;
    CoordinateSystemId root_coordinates;
    std::size_t x;
    std::size_t y;
};

Result<ImageHandle> ImageHandle::copy(const ImageLayout& layout,
    std::span<const std::byte> pixels, CoordinateSystemId coordinates)
{
    auto length = required_image_bytes(layout);
    if (const auto* failure = std::get_if<Diagnostic>(&length)) { return *failure; }
    const auto required = std::get<std::size_t>(length);
    if (pixels.size() < required) {
        return error(ErrorCode::InvalidImage, "Borrowed image buffer is shorter than the layout requires.");
    }
    // A fresh const-owned allocation prevents even retained external writable aliases.
    const auto selected = pixels.first(required);
    auto storage = std::make_shared<const std::vector<std::byte>>(selected.begin(), selected.end());
    const auto bytes = std::get<std::size_t>(bytes_per_pixel(layout.format));
    return ImageHandle{std::make_shared<const View>(View{
        std::move(storage), layout, 0, bytes, coordinates, coordinates, 0, 0})};
}

Result<ImageHandle> ImageHandle::crop(PixelRect window, CoordinateSystemId view_coordinates) const
{
    if (window.width == 0 || window.height == 0 || window.x >= width() || window.y >= height()
        || window.width > width() - window.x || window.height > height() - window.y) {
        return error(ErrorCode::ImageBounds, "Crop must be a nonempty half-open window inside the image.");
    }
    const auto root_x = view_->x + window.x;
    const auto root_y = view_->y + window.y;
    if (((window.x != 0 || window.y != 0) && view_coordinates == coordinate_frame())
        || ((root_x != 0 || root_y != 0) && view_coordinates == root_frame())) {
        return error(ErrorCode::CoordinateMismatch, "A translated crop requires its own coordinate identity.");
    }
    const auto offset = view_->offset + window.y * row_stride() + window.x * view_->pixel_bytes;
    return ImageHandle{std::make_shared<const View>(View{view_->storage,
        {window.width, window.height, row_stride(), format()}, offset, view_->pixel_bytes,
        std::move(view_coordinates), root_frame(), root_x, root_y})};
}

Result<std::span<const std::byte>> ImageHandle::row(std::size_t y) const
{
    if (y >= height()) { return error(ErrorCode::ImageBounds, "Image row is out of bounds."); }
    return std::span<const std::byte>(*view_->storage).subspan(
        view_->offset + y * row_stride(), width() * view_->pixel_bytes);
}

std::vector<std::byte> ImageHandle::copy_pixels() const
{
    const auto row_bytes = width() * view_->pixel_bytes;
    std::vector<std::byte> result(row_bytes * height());
    for (std::size_t y = 0; y < height(); ++y) {
        const auto source = std::span<const std::byte>(*view_->storage).subspan(view_->offset + y * row_stride(), row_bytes);
        std::ranges::copy(source, result.begin() + static_cast<std::ptrdiff_t>(y * row_bytes));
    }
    return result;
}

Result<ImageHandle> ImageHandle::clone() const
{
    const auto pixels = copy_pixels();
    const ImageLayout layout{width(), height(), width() * view_->pixel_bytes, format()};
    auto copied = copy(layout, pixels, coordinate_frame());
    if (auto* image = std::get_if<ImageHandle>(&copied)) {
        const auto& copy_view = *image->view_;
        image->view_ = std::make_shared<const View>(View{copy_view.storage, copy_view.layout, 0,
            view_->pixel_bytes, coordinate_frame(), root_frame(), origin_x(), origin_y()});
    }
    return copied;
}

std::size_t ImageHandle::width() const noexcept { return view_->layout.width; }
std::size_t ImageHandle::height() const noexcept { return view_->layout.height; }
std::size_t ImageHandle::row_stride() const noexcept { return view_->layout.row_stride; }
PixelFormat ImageHandle::format() const noexcept { return view_->layout.format; }
const CoordinateSystemId& ImageHandle::coordinate_frame() const noexcept { return view_->coordinates; }
const CoordinateSystemId& ImageHandle::root_frame() const noexcept { return view_->root_coordinates; }
std::size_t ImageHandle::origin_x() const noexcept { return view_->x; }
std::size_t ImageHandle::origin_y() const noexcept { return view_->y; }
bool ImageHandle::shares_storage_with(const ImageHandle& other) const noexcept { return view_->storage == other.view_->storage; }

bool ImageHandle::operator==(const ImageHandle& other) const
{
    return view_ == other.view_ || (shares_storage_with(other) && view_->offset == other.view_->offset
        && view_->layout == other.view_->layout && coordinate_frame() == other.coordinate_frame()
        && root_frame() == other.root_frame() && origin_x() == other.origin_x() && origin_y() == other.origin_y());
}

} // namespace visionpro::core
