#include "image_fixtures.hpp"
#include "vision/roi.hpp"
#include "vision/roi_mask.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::tools::vision;
using namespace visionpro::workflow;
namespace fs = std::filesystem;

// Grayscale PNG scanlines with filter 0, generated using struct and zlib.
// 3x2 [0,1,127;128,255,0].
constexpr unsigned char rule_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x1f, 0x39, 0xc6, 0x00, 0x00, 0x00,
    0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0xac, 0x67,
    0x68, 0xf8, 0xcf, 0x00, 0x00, 0x06, 0x07, 0x02, 0x00, 0x14, 0x67, 0x03,
    0xfc, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};
// 2x2 [1,0;0,1].
constexpr unsigned char diagonal_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x57, 0xdd, 0x52, 0xf8, 0x00, 0x00, 0x00,
    0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x64, 0x60, 0x60,
    0x60, 0x04, 0x00, 0x00, 0x0c, 0x00, 0x03, 0x03, 0x61, 0x9f, 0x7c, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
// 3x2 all zero.
constexpr unsigned char empty_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x00, 0xb8, 0x1f, 0x39, 0xc6, 0x00, 0x00, 0x00,
    0x0b, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x80, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x01, 0xb7, 0x58, 0x73, 0x95, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr auto pixel = MeasurementUnit::Pixel;

void check(bool passed, std::string_view message)
{
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> Diagnostic fails(Result<T> value, ErrorCode expected)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value); failure && failure->code == expected) { return *failure; }
    const auto* failure = std::get_if<Diagnostic>(&value);
    throw std::runtime_error("Expected " + std::string(error_code_name(expected))
        + (failure ? ", got " + std::string(error_code_name(failure->code)) + ": " + failure->message : ", got success"));
}

template<SchemaIdentity T> T key(std::string_view value)
{ return take(SchemaIdFactory::parse<T>(value)); }

template<UuidIdentity T> T id(char last = '1')
{
    std::string value = "00000000-0000-4000-8000-000000000001";
    value.back() = last;
    return take(IdFactory::parse<T>(value));
}

struct ScratchDirectory {
    ScratchDirectory() : path(fs::current_path() / ("s43-mask-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        check(fs::create_directory(path), "Could not create the mask fixture directory.");
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    const fs::path path;
};

struct Fixture {
    const ScratchDirectory scratch;
    const fs::path filename{u8"掩膜 图像.png"};
    const CoordinateSystemId frame = key<CoordinateSystemId>("target");
    const CoordinateSystemId mask_frame = key<CoordinateSystemId>("mask_local");
    const ResourceReference resource{id<ResourceId>(), ResourceKind::Mask, {}};
    const ToolValueTarget roi = key<ParameterId>("roi");

    void replace(std::span<const unsigned char> bytes) const
    {
        std::ofstream file(scratch.path / filename, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        file.close();
        check(static_cast<bool>(file), "Could not write the mask fixture.");
    }

    InvocationResourceLease acquire(std::span<const unsigned char> bytes, std::string_view slot_name = "mask",
        ResourceKind kind = ResourceKind::Mask) const
    {
        replace(bytes);
        const auto path = filename.generic_u8string();
        const ResourceDefinition file{resource.id, "Mask file", kind,
            ExternalFileResource{{PathBase::SolutionDirectory, {path.begin(), path.end()}, {}},
                ResourceReadPolicy::PerInvocation, {}, {}}};
        const Solution solution{id<SolutionId>(), "Mask fixture", 1, 1, {}, {file}, {}, {}, {}, {}};
        auto provider = take(FileResourceProvider::create(solution, scratch.path));
        const auto slot = key<SchemaId>(slot_name);
        const ToolResourceBinding binding{slot, {file.id, file.kind, {}}};
        const ToolDescriptor descriptor{key<ToolTypeId>("test.mask"), "Mask fixture", 1,
            {key<SchemaId>("test.mask"), 1, {}}, {}, {}, {}, {}, {}, {{slot, kind}}, {}, {}, {}, {}};
        const auto configuration = take(ToolExecutionConfiguration::create({id<SolutionSessionId>(),
            take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1)), id<WorkflowId>(), id<NodeId>(),
            descriptor, {}, {binding}}));
        return {slot, take(provider->acquire(configuration, binding))};
    }

    MaskConfig direct(std::size_t width, std::size_t height) const
    { return {resource, frame, width, height, mono8, {frame, width, height, {}, MaskSampling::None}, {}}; }

    MaskConfig translated(double x) const
    {
        const Transform2D mapping{mask_frame, frame, pixel, pixel, TransformModel::Affine,
            {1, 0, x, 0, 1, 0, 0, 0, 1}};
        return {resource, mask_frame, 2, 2, mono8, {frame, 3, 2, mapping, MaskSampling::Nearest}, {}};
    }

    RegionPrimitive primitive(RegionShape shape) const
    { return {frame, pixel, std::move(shape), 0, {}}; }

    ImageHandle image(std::size_t width = 3, std::size_t height = 2) const
    {
        return take(ImageHandle::copy({width, height, width, mono8},
            std::vector<std::byte>(width * height, std::byte{255}), frame));
    }
};

using Site = std::array<std::size_t, 2>;

void expect_members(const PreparedRoiMask& mask, std::initializer_list<Site> expected)
{
    std::vector<Site> actual;
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 0; x < 3; ++x) {
            if (mask.contains({static_cast<double>(x), static_cast<double>(y)})) { actual.push_back({x, y}); }
        }
    }
    check(std::ranges::equal(actual, expected), "The prepared mask must match the fixed member coordinates.");
}

void mask_rules(const Fixture& fixture)
{
    const std::array leases{fixture.acquire(rule_png)};
    fixture.replace(empty_png); // The existing invocation lease still owns rule_png.
    RoiMaskResource masks(leases);
    check(!masks.reference(), "Unconsumed masks have no resource trace.");
    auto config = fixture.direct(3, 2);
    expect_members(take(masks.prepare(config)), {{1, 0}, {2, 0}, {0, 1}, {1, 1}});
    config.valid_pixels = {MaskValueMode::AtLeastThreshold, 128};
    expect_members(take(masks.prepare(config)), {{0, 1}, {1, 1}});
    check(masks.reference() == fixture.resource, "Repeated uses trace the single fixed lease.");
    take(masks.finish());
}

void nearest(const Fixture& fixture)
{
    const std::array leases{fixture.acquire(diagonal_png)};
    RoiMaskResource masks(leases);
    expect_members(take(masks.prepare(fixture.translated(1.0))), {{1, 0}, {2, 1}});
    expect_members(take(masks.prepare(fixture.translated(0.5))), {{0, 0}, {1, 1}});
    auto config = fixture.translated(1.0);
    config.valid_pixels = {MaskValueMode::AtLeastThreshold, 0};
    expect_members(take(masks.prepare(config)), {{1, 0}, {2, 0}, {1, 1}, {2, 1}});
    config.mapping.sampling = MaskSampling::Linear;
    fails(masks.prepare(config), ErrorCode::UnsupportedRegion);
    take(masks.finish());
}

void resource_failures(const Fixture& fixture)
{
    auto config = fixture.direct(3, 2);
    const auto where = fixture.resource.id;
    const auto check_resource = [&](auto result, ErrorCode code) {
        check(fails(std::move(result), code).location.resource == where, "Mask errors locate the responsible resource.");
    };
    RoiMaskResource none({});
    check_resource(none.prepare(config), ErrorCode::MissingRequiredValue);
    take(none.finish());

    const std::array leases{fixture.acquire(rule_png)};
    RoiMaskResource masks(leases);
    check_resource(masks.finish(), ErrorCode::InvalidResource);
    config.resource.revision = 7;
    check_resource(masks.prepare(config), ErrorCode::ResourceVersionMismatch);
    config.resource = fixture.resource;
    config.mapping.target_width = 4;
    check_resource(masks.prepare(config), ErrorCode::CoordinateMismatch);
    config = fixture.direct(2, 2);
    check_resource(masks.prepare(config), ErrorCode::InvalidMask); // Decoded file is 3x2.
    config = fixture.direct(3, 2);
    config.resource.id = id<ResourceId>('2');
    check(fails(masks.prepare(config), ErrorCode::InvalidResource).location.resource == config.resource.id,
        "A different resource identity cannot use the existing mask slot.");
    config = fixture.direct(3, 2);
    config.mask_frame = fixture.mask_frame;
    check_resource(masks.prepare(config), ErrorCode::CoordinateMismatch);

    const std::array duplicate{leases.front(), leases.front()};
    check_resource(RoiMaskResource(duplicate).finish(), ErrorCode::InvalidResource);
    const std::array wrong_slot{InvocationResourceLease{key<SchemaId>("spare"), leases.front().lease}};
    check_resource(RoiMaskResource(wrong_slot).finish(), ErrorCode::InvalidResource);
    const std::array wrong_kind{fixture.acquire(rule_png, "mask", ResourceKind::Image)};
    check_resource(RoiMaskResource(wrong_kind).prepare(fixture.direct(3, 2)), ErrorCode::ResourceKindMismatch);
    const std::array other_kind{fixture.acquire(rule_png, "source", ResourceKind::Image)};
    take(RoiMaskResource(other_kind).finish());

    constexpr unsigned char corrupt[]{1, 2, 3};
    const std::array broken{fixture.acquire(corrupt)};
    check_resource(RoiMaskResource(broken).prepare(fixture.direct(3, 2)), ErrorCode::InvalidMask);
    const std::array color{fixture.acquire(visionpro::test_fixtures::bmp)};
    check_resource(RoiMaskResource(color).prepare(fixture.direct(2, 2)), ErrorCode::UnsupportedPixelFormat);
}

void evaluate_with_lease(const Fixture& fixture)
{
    const auto target = fixture.image();
    const std::array leases{fixture.acquire(rule_png)};
    auto exclusion = fixture.direct(3, 2);
    exclusion.valid_pixels = {MaskValueMode::AtLeastThreshold, 128};
    RegionDefinition region{id<RegionId>(), fixture.frame, pixel, fixture.primitive(FullImageRegion{}),
        {fixture.primitive(exclusion)}};
    const auto evaluated = take(evaluate_roi(target, region, fixture.roi, {}, leases));
    constexpr unsigned char expected[]{255, 255, 255, 0, 0, 255};
    check(evaluated.member_count == 4 && evaluated.mask_resource == fixture.resource
        && std::ranges::equal(evaluated.mask.pixels.copy_pixels(), std::as_bytes(std::span{expected})),
        "Mask exclusions subtract the fixed pixels and retain the used resource reference.");
    check(fails(evaluate_roi(fixture.image(4, 2), region, fixture.roi, {}, leases), ErrorCode::CoordinateMismatch)
        .location.region == region.id, "A mask mapped to the wrong target size locates its ROI.");

    const std::array empty{fixture.acquire(empty_png)};
    region.source = fixture.primitive(fixture.direct(3, 2));
    region.exclusions.clear();
    fails(evaluate_roi(target, region, fixture.roi, {}, empty), ErrorCode::EmptyRegion);
    region.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    const auto cleared = take(evaluate_roi(target, region, fixture.roi, {}, empty));
    check(cleared.member_count == 0 && cleared.mask.pixels.width() == 3 && cleared.mask.pixels.height() == 2
        && std::ranges::all_of(cleared.mask.pixels.copy_pixels(), [](auto value) { return value == std::byte{}; }),
        "An allowed empty mask still produces a complete zero-member image.");

    const std::array mapped{fixture.acquire(diagonal_png)};
    auto corrected = fixture.primitive(fixture.translated(1));
    corrected.transform = Transform2D{fixture.frame, fixture.frame, pixel, pixel, TransformModel::Affine,
        {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    region.source = corrected;
    const auto positioned = take(evaluate_roi(fixture.image(4, 2), region, fixture.roi, {}, mapped));
    constexpr unsigned char shifted[]{0, 0, 255, 0, 0, 0, 0, 255};
    check(positioned.member_count == 2 && positioned.mask_resource == fixture.resource
        && std::ranges::equal(positioned.mask.pixels.copy_pixels(), std::as_bytes(std::span{shifted})),
        "The ROI correction places the result of the local Nearest mapping in the final image.");
}

} // namespace

int main()
{
    try {
        const Fixture fixture;
        mask_rules(fixture);
        nearest(fixture);
        resource_failures(fixture);
        evaluate_with_lease(fixture);
        std::cout << "Mask rules, nearest mapping, fixed file leases and ROI composition passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
