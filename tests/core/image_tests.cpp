#include <visionpro/core/image.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace visionpro::core;

static_assert(!std::is_default_constructible_v<ImageHandle>);
static_assert(!std::is_constructible_v<ImageHandle, std::vector<std::byte>>);
static_assert(!std::is_constructible_v<ImageHandle, std::span<std::byte>>);
static_assert(std::is_same_v<decltype(std::declval<ImageHandle&>().row(0)),
    Result<std::span<const std::byte>>>);
static_assert(std::is_same_v<decltype(std::declval<ImageHandle&>().coordinate_frame()),
    const CoordinateSystemId&>);
static_assert(std::is_copy_constructible_v<ImageHandle> && std::is_copy_assignable_v<ImageHandle>);

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template <class T>
T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error("Invalid image fixture: " + error->message);
    }
    return std::get<T>(std::move(result));
}

template <class T>
void expect_error(const Result<T>& result, ErrorCode code, std::string_view description)
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error && error->code == code, description);
    if (error) {
        check(error->severity == Severity::Error && !error->message.empty(),
            "image failure has severity and an explanation");
    }
}

CoordinateSystemId coordinates(std::string_view name)
{ return take(SchemaIdFactory::parse<CoordinateSystemId>(name)); }

std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> result;
    for (const auto value : values) { result.push_back(static_cast<std::byte>(value)); }
    return result;
}

bool row_equals(const ImageHandle& image, std::size_t row, std::initializer_list<unsigned> expected)
{
    const auto values = take(image.row(row));
    return std::vector<std::byte>(values.begin(), values.end()) == bytes(expected);
}

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

void image_layout()
{
    constexpr std::array depths{PixelDepth::UInt8, PixelDepth::UInt16, PixelDepth::Float32, PixelDepth::Float64};
    constexpr std::array<std::size_t, 4> sample_bytes{1, 2, 4, 8};
    constexpr std::array orders{ChannelOrder::Mono, ChannelOrder::RGB, ChannelOrder::BGR,
        ChannelOrder::RGBA, ChannelOrder::BGRA};
    constexpr std::array<std::size_t, 5> channel_counts{1, 3, 3, 4, 4};
    for (std::size_t depth = 0; depth < depths.size(); ++depth) {
        for (std::size_t order = 0; order < orders.size(); ++order) {
            const PixelFormat format{depths[depth], orders[order]};
            const auto pixel_bytes = sample_bytes[depth] * channel_counts[order];
            check(take(bytes_per_pixel(format)) == pixel_bytes, "pixel size follows explicit depth and channel order");
            const auto stride = 2 * pixel_bytes + 3;
            const ImageLayout layout{2, 2, stride, format};
            const auto minimum = stride + 2 * pixel_bytes;
            check(take(required_image_bytes(layout)) == minimum, "minimum storage includes rows but not final padding");
            const std::vector<std::byte> source(minimum, std::byte{42});
            const auto image = take(ImageHandle::copy(layout, source, coordinates("format.frame")));
            check(image.format() == format && image.row_stride() == stride, "format and stride survive image creation");
            check(take(image.row(1)).size() == 2 * pixel_bytes, "row access exposes pixels without padding");
            check(image.copy_pixels().size() == 4 * pixel_bytes, "independent pixel copy is tightly packed");
        }
    }

    expect_error(bytes_per_pixel({static_cast<PixelDepth>(999), ChannelOrder::Mono}),
        ErrorCode::UnsupportedPixelFormat, "unknown depth is rejected");
    expect_error(bytes_per_pixel({PixelDepth::UInt8, static_cast<ChannelOrder>(999)}),
        ErrorCode::UnsupportedPixelFormat, "unknown channel order is rejected");
    expect_error(required_image_bytes({0, 1, 1, mono8}), ErrorCode::InvalidImage, "zero width is rejected");
    expect_error(required_image_bytes({1, 0, 1, mono8}), ErrorCode::InvalidImage, "zero height is rejected");
    expect_error(required_image_bytes({2, 1, 1, mono8}), ErrorCode::InvalidImage, "short stride is rejected");
    expect_error(required_image_bytes({1, 1, 0, mono8}), ErrorCode::InvalidImage, "zero stride is rejected");

    const auto maximum = std::numeric_limits<std::size_t>::max();
    expect_error(required_image_bytes({maximum, 1, maximum, {PixelDepth::UInt16, ChannelOrder::Mono}}),
        ErrorCode::SizeOverflow, "width times pixel size overflow is rejected before allocation");
    expect_error(required_image_bytes({1, 3, maximum, mono8}), ErrorCode::SizeOverflow,
        "preceding row offsets cannot overflow");
    expect_error(required_image_bytes({1, 2, maximum, mono8}), ErrorCode::SizeOverflow,
        "last row byte count cannot overflow the total buffer length");
    expect_error(required_image_bytes({1, maximum, 2, mono8}), ErrorCode::SizeOverflow,
        "large height cannot wrap row offsets");
    check(take(required_image_bytes({1, 1, maximum, mono8})) == 1,
        "one row does not require unused trailing stride bytes");
    const auto one_row = take(ImageHandle::copy({1, 1, maximum, mono8}, bytes({77}), coordinates("single")));
    check(row_equals(one_row, 0, {77}), "legal single row with large stride has bounded pixel access");
    check(take(one_row.clone()).row_stride() == 1, "single-row clone compacts unused stride");

    const ImageLayout padded_layout{2, 2, 4, mono8};
    expect_error(ImageHandle::copy(padded_layout, bytes({1, 2, 99, 99, 3}), coordinates("padded")),
        ErrorCode::InvalidImage, "buffer shorter than last complete row is rejected");
    expect_error(ImageHandle::copy({1, 1, 1, mono8}, {}, coordinates("empty")),
        ErrorCode::InvalidImage, "empty borrowed storage cannot satisfy a nonempty image");
    const auto exact = take(ImageHandle::copy(padded_layout, bytes({1, 2, 99, 99, 3, 4}), coordinates("padded")));
    check(row_equals(exact, 0, {1, 2}) && row_equals(exact, 1, {3, 4}),
        "final row without padding is readable");
    check(exact.copy_pixels() == bytes({1, 2, 3, 4}), "packed copy removes source padding");
    const auto trailing = take(ImageHandle::copy(padded_layout,
        bytes({1, 2, 99, 99, 3, 4, 88, 88, 55}), coordinates("padded")));
    check(trailing.copy_pixels() == exact.copy_pixels(), "trailing bytes are not exposed as pixels");
    expect_error(exact.row(2), ErrorCode::ImageBounds, "first row beyond image is rejected");
    expect_error(exact.row(maximum), ErrorCode::ImageBounds, "maximum row index is rejected before offset arithmetic");

    const auto rgb = take(ImageHandle::copy({1, 1, 3, {PixelDepth::UInt8, ChannelOrder::RGB}},
        bytes({255, 12, 7}), coordinates("color")));
    const auto bgr = take(ImageHandle::copy({1, 1, 3, {PixelDepth::UInt8, ChannelOrder::BGR}},
        bytes({255, 12, 7}), coordinates("color")));
    check(rgb.format() != bgr.format(), "RGB and BGR remain distinct despite equal channel counts");
    check(rgb.copy_pixels() == bgr.copy_pixels(), "image ownership does not silently reorder channels");
}

ImageHandle padded_fixture()
{
    return take(ImageHandle::copy({4, 3, 6, mono8},
        bytes({1, 2, 3, 4, 99, 98, 11, 12, 13, 14, 97, 96, 21, 22, 23, 24}), coordinates("root")));
}

void image_ownership()
{
    auto borrowed = bytes({1, 2, 3, 4});
    const auto first = take(ImageHandle::copy({2, 2, 2, mono8}, borrowed, coordinates("input")));
    borrowed[0] = std::byte{255};
    borrowed.clear();
    borrowed.shrink_to_fit();
    check(row_equals(first, 0, {1, 2}) && row_equals(first, 1, {3, 4}),
        "copy owns pixels after caller changes and releases borrowed storage");
    const auto second_branch = first;
    check(second_branch.shares_storage_with(first) && second_branch == first,
        "read-only branches can share a published image");
    auto changed = first.copy_pixels();
    changed[0] = std::byte{42};
    const auto output = take(ImageHandle::copy({2, 2, 2, mono8}, changed, coordinates("output")));
    check(row_equals(output, 0, {42, 2}), "independent output contains the requested edit");
    check(row_equals(first, 0, {1, 2}) && row_equals(second_branch, 0, {1, 2}),
        "creating edited output cannot contaminate either input reader");
    check(!output.shares_storage_with(first), "edited output has independent pixel ownership");
    const auto independent = take(first.clone());
    check(!independent.shares_storage_with(first) && independent != first,
        "explicit clone is a distinct owned image even when pixels match");
    check(independent.copy_pixels() == first.copy_pixels(), "clone preserves pixel values");
    check(independent.coordinate_frame() == first.coordinate_frame(), "clone preserves coordinate identity");

    const auto parent = padded_fixture();
    check(parent.origin_x() == 0 && parent.origin_y() == 0 && parent.root_frame() == coordinates("root"),
        "original image starts at its root coordinate origin");
    const auto view = take(parent.crop({1, 1, 3, 2}, coordinates("crop")));
    check(view.width() == 3 && view.height() == 2 && view.row_stride() == 6,
        "crop preserves original stride and exposes the requested dimensions");
    check(row_equals(view, 0, {12, 13, 14}) && row_equals(view, 1, {22, 23, 24}),
        "crop reads the correct parent pixel window");
    check(view.shares_storage_with(parent) && view != parent, "crop shares ownership but is a distinct view");
    check(view.coordinate_frame() == coordinates("crop") && view.root_frame() == coordinates("root")
        && view.origin_x() == 1 && view.origin_y() == 1, "crop explicitly records the root-coordinate translation");
    const auto nested = take(view.crop({1, 0, 1, 2}, coordinates("nested")));
    check(row_equals(nested, 0, {13}) && row_equals(nested, 1, {23}), "nested crop applies both pixel offsets");
    check(nested.root_frame() == coordinates("root") && nested.origin_x() == 2 && nested.origin_y() == 1,
        "nested crop accumulates its root translation");
    check(nested.row_stride() == 6 && nested.shares_storage_with(parent), "nested crop retains the original allocation");

    const auto nested_clone = take(nested.clone());
    check(nested_clone.row_stride() == 1 && !nested_clone.shares_storage_with(parent),
        "cloned crop has independent tightly packed storage");
    check(nested_clone.coordinate_frame() == nested.coordinate_frame()
        && nested_clone.root_frame() == nested.root_frame()
        && nested_clone.origin_x() == 2 && nested_clone.origin_y() == 1,
        "cloned crop retains the mapping into original root coordinates");
    const auto clone_crop = take(nested_clone.crop({0, 1, 1, 1}, coordinates("clone_crop")));
    check(row_equals(clone_crop, 0, {23}) && clone_crop.origin_x() == 2 && clone_crop.origin_y() == 2,
        "cropping independent clone uses local storage offsets and retained root mapping");

    const auto whole = take(parent.crop({0, 0, 4, 3}, parent.coordinate_frame()));
    check(whole == parent, "same storage and full zero-offset window has the same view identity");
    const auto maximum = std::numeric_limits<std::size_t>::max();
    for (const PixelRect invalid : std::array{
        PixelRect{0, 0, 0, 1}, PixelRect{0, 0, 1, 0}, PixelRect{4, 0, 1, 1},
        PixelRect{0, 3, 1, 1}, PixelRect{3, 0, 2, 1}, PixelRect{0, 2, 1, 2},
        PixelRect{maximum, 0, 1, 1}, PixelRect{0, maximum, 1, 1},
        PixelRect{1, 0, maximum, 1}, PixelRect{0, 1, 1, maximum}}) {
        expect_error(parent.crop(invalid, coordinates("invalid.crop")), ErrorCode::ImageBounds,
            "empty, out-of-bounds or overflowing crop is rejected");
    }
    expect_error(parent.crop({1, 0, 1, 1}, parent.coordinate_frame()), ErrorCode::CoordinateMismatch,
        "translated crop cannot claim the parent coordinate origin");
    expect_error(view.crop({1, 0, 1, 1}, view.coordinate_frame()), ErrorCode::CoordinateMismatch,
        "translated nested crop cannot reuse its immediate parent coordinates");
    expect_error(view.crop({0, 0, 1, 1}, parent.coordinate_frame()), ErrorCode::CoordinateMismatch,
        "nonzero-root crop cannot claim the root coordinate identity");

    std::optional<ImageHandle> surviving_view;
    {
        const auto temporary_parent = padded_fixture();
        const auto temporary_view = take(temporary_parent.crop({1, 1, 3, 2}, coordinates("temporary.crop")));
        surviving_view = take(temporary_view.crop({1, 0, 1, 2}, coordinates("survivor")));
    }
    check(row_equals(*surviving_view, 0, {13}) && row_equals(*surviving_view, 1, {23}),
        "child view keeps storage valid after both parent handles are destroyed");
    auto moved = *surviving_view;
    auto destination = std::move(moved);
    check(row_equals(destination, 1, {23}) && row_equals(moved, 1, {23}),
        "handle move preserves valid source and destination ownership");
    destination = std::move(moved);
    check(destination == moved, "handle move assignment preserves the immutable view");

    const auto source = take(IdFactory::parse<ResourceId>("00112233-4455-4677-8899-aabbccddeeff"));
    const auto captured = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{1234}};
    std::optional<ImageFrame> retained_frame;
    {
        const auto image = padded_fixture();
        const ImageFrame frame{image, FrameId{source, 7}, captured};
        retained_frame = frame;
        check(*retained_frame == frame, "ImageFrame copy preserves pixels and frame metadata");
    }
    check(retained_frame->frame == FrameId{source, 7} && retained_frame->captured_at == captured,
        "frame identity and capture timestamp survive source object destruction");
    check(row_equals(retained_frame->image, 2, {21, 22, 23, 24}),
        "ImageFrame owns a live read-only handle after the source frame is destroyed");
    const ImageFrame generated{first, std::nullopt, std::nullopt};
    check(!generated.frame && !generated.captured_at && row_equals(generated.image, 0, {1, 2}),
        "generated images may omit capture metadata without losing pixel ownership");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "image_layout") { image_layout(); }
        else if (group == "image_ownership") { image_ownership(); }
        else { return 2; }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 1;
    }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
