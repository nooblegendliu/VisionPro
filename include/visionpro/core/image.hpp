#pragma once

#include <visionpro/core/result.hpp>
#include <visionpro/core/runtime_id.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace visionpro::core {

enum class PixelDepth { UInt8, UInt16, Float32, Float64 };
enum class ChannelOrder { Mono, RGB, BGR, RGBA, BGRA };
struct PixelFormat {
    PixelDepth depth;
    ChannelOrder order;
    bool operator==(const PixelFormat&) const = default;
};
struct ImageLayout {
    std::size_t width;
    std::size_t height;
    std::size_t row_stride;
    PixelFormat format;
    bool operator==(const ImageLayout&) const = default;
};
struct PixelRect { std::size_t x; std::size_t y; std::size_t width; std::size_t height; };

[[nodiscard]] Result<std::size_t> bytes_per_pixel(PixelFormat format);
[[nodiscard]] Result<std::size_t> required_image_bytes(const ImageLayout& layout);

// Interleaved rows, native-endian multi-byte samples. External adapters must
// explicitly convert other encodings. No writable aliases escape this owner.
class ImageHandle final {
public:
    ImageHandle(const ImageHandle&) noexcept = default;
    ImageHandle& operator=(const ImageHandle&) noexcept = default;
    ImageHandle(ImageHandle&& other) noexcept : view_(other.view_) {}
    ImageHandle& operator=(ImageHandle&& other) noexcept { view_ = other.view_; return *this; }

    // Copies while the caller's borrowed buffer is valid. Trailing bytes are ignored.
    [[nodiscard]] static Result<ImageHandle> copy(const ImageLayout& layout,
        std::span<const std::byte> pixels, CoordinateSystemId coordinates);
    [[nodiscard]] Result<ImageHandle> crop(PixelRect window, CoordinateSystemId view_coordinates) const;
    // The returned span borrows the storage; retain a handle throughout its use.
    [[nodiscard]] Result<std::span<const std::byte>> row(std::size_t y) const;
    [[nodiscard]] std::vector<std::byte> copy_pixels() const;
    [[nodiscard]] Result<ImageHandle> clone() const;

    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;
    [[nodiscard]] std::size_t row_stride() const noexcept;
    [[nodiscard]] PixelFormat format() const noexcept;
    [[nodiscard]] const CoordinateSystemId& coordinate_frame() const noexcept;
    [[nodiscard]] const CoordinateSystemId& root_frame() const noexcept;
    [[nodiscard]] std::size_t origin_x() const noexcept;
    [[nodiscard]] std::size_t origin_y() const noexcept;
    [[nodiscard]] bool shares_storage_with(const ImageHandle& other) const noexcept;
    // Identity of storage + view, not a potentially expensive pixel comparison.
    [[nodiscard]] bool operator==(const ImageHandle& other) const;

private:
    struct View;
    explicit ImageHandle(std::shared_ptr<const View> view) : view_(std::move(view)) {}
    std::shared_ptr<const View> view_;
};

struct ImageFrame {
    ImageHandle image;
    std::optional<FrameId> frame;
    std::optional<std::chrono::sys_time<std::chrono::nanoseconds>> captured_at;
    bool operator==(const ImageFrame&) const = default;
};

} // namespace visionpro::core
