#include <visionpro/core/geometry.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace visionpro::core;
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template <class T> T value(Result<T> result)
{
    if (const auto* item = std::get_if<T>(&result)) { return *item; }
    throw std::runtime_error("Invalid fixture: " + std::get<Diagnostic>(result).message);
}
template <class T> void succeeds(const Result<T>& result, std::string_view message)
{ check(std::holds_alternative<T>(result), message); }
template <class T> Diagnostic fails(const Result<T>& result, ErrorCode code)
{
    const auto* invalid = std::get_if<Diagnostic>(&result);
    check(invalid != nullptr, "invalid input reports a diagnostic");
    if (!invalid) { throw std::runtime_error("Unexpected validation success"); }
    check(invalid->code == code && invalid->severity == Severity::Error && !invalid->message.empty(), "diagnostic identifies failure and explanation");
    return *invalid;
}
template <SchemaIdentity T> T key(std::string_view name) { return value(SchemaIdFactory::parse<T>(name)); }
template <UuidIdentity T> T id() { return value(IdFactory::parse<T>("10000000-0000-4000-8000-000000000001")); }
CoordinateSystemId camera() { return key<CoordinateSystemId>("camera"); }
CoordinateSystemId workpiece() { return key<CoordinateSystemId>("workpiece"); }
CoordinateFrameDefinition image_coordinates()
{ return {camera(), CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}; }
CoordinateFrameDefinition physical_coordinates()
{ return {workpiece(), CoordinateKind::Physical, MeasurementUnit::Millimetre, {12, 20}, {0, 1}, {-1, 0}, Handedness::RightHanded}; }
Transform2D correction()
{
    return {camera(), workpiece(), MeasurementUnit::Pixel, MeasurementUnit::Millimetre,
        TransformModel::Similarity, {0, -2, 10, 2, 0, 20, 0, 0, 1}, std::nullopt, std::nullopt};
}
RegionDefinition region(RegionShape shape)
{
    return {id<RegionId>(), camera(), MeasurementUnit::Pixel,
        RegionPrimitive{camera(), MeasurementUnit::Pixel, std::move(shape), 0, std::nullopt}, {},
        RegionBoundaryPolicy::Reject, std::nullopt, EmptyRegionPolicy::Error};
}
RegionCapabilities capabilities()
{
    return {{RegionShapeKind::FullImage, RegionShapeKind::Empty, RegionShapeKind::AxisAlignedRect,
        RegionShapeKind::RotatedRect, RegionShapeKind::Circle, RegionShapeKind::Ring,
        RegionShapeKind::SectorRing, RegionShapeKind::Polygon, RegionShapeKind::RasterMask},
        {RegionBoundaryPolicy::Reject, RegionBoundaryPolicy::Intersect, RegionBoundaryPolicy::Fill},
        GeometryMaskPolicy::Combine, true, true, true,
        {PixelDepth::UInt8, PixelDepth::UInt16, PixelDepth::Float32, PixelDepth::Float64}};
}
MaskConfig mask_config()
{
    return {{id<ResourceId>(), ResourceKind::Mask, 1}, camera(), 2, 2,
        {PixelDepth::UInt8, ChannelOrder::Mono}, {camera(), 2, 2, std::nullopt, MaskSampling::None}, {}};
}
ImageHandle pixels(CoordinateSystemId frame, PixelFormat format = {PixelDepth::UInt8, ChannelOrder::Mono})
{
    const auto stride = 2 * value(bytes_per_pixel(format));
    std::vector<std::byte> bytes(2 * stride, std::byte{127});
    return value(ImageHandle::copy({2, 2, stride, format}, bytes, std::move(frame)));
}

void geometry()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    auto frame = image_coordinates();
    succeeds(validate_coordinate_frame(frame), "image coordinates use pixel centers, x right and y down");
    succeeds(validate_coordinate_frame(physical_coordinates()), "physical frame has explicitly declared origin/axes and units");
    frame.handedness = Handedness::RightHanded;
    fails(validate_coordinate_frame(frame), ErrorCode::InvalidCoordinate);
    frame = image_coordinates(); frame.origin.x = 0.5;
    fails(validate_coordinate_frame(frame), ErrorCode::InvalidCoordinate);
    frame = physical_coordinates(); frame.unit = MeasurementUnit::Pixel;
    fails(validate_coordinate_frame(frame), ErrorCode::InvalidCoordinate);
    frame = physical_coordinates(); frame.x_axis = {1e200, 1e200};
    fails(validate_coordinate_frame(frame), ErrorCode::InvalidCoordinate);
    frame = physical_coordinates(); frame.y_axis.x = nan;
    fails(validate_coordinate_frame(frame), ErrorCode::InvalidCoordinate);

    const Point2D p{camera(), MeasurementUnit::Pixel, 1, 2};
    succeeds(validate_point(p), "finite point preserves declared pixel coordinates");
    auto invalid_point = p; invalid_point.x = nan;
    fails(validate_point(invalid_point), ErrorCode::InvalidGeometry);
    invalid_point = p; invalid_point.unit = MeasurementUnit::Unspecified;
    fails(validate_point(invalid_point), ErrorCode::InvalidGeometry);
    succeeds(validate_line({p, {camera(), MeasurementUnit::Pixel, 3, 4}}), "nondegenerate line is valid");
    fails(validate_line({p, p}), ErrorCode::InvalidGeometry);
    fails(validate_line({p, {workpiece(), MeasurementUnit::Pixel, 3, 4}}), ErrorCode::CoordinateMismatch);
    fails(validate_line({p, {camera(), MeasurementUnit::Millimetre, 3, 4}}), ErrorCode::CoordinateMismatch);
    succeeds(validate_circle({p, 2}), "circle center carries coordinates/units");
    fails(validate_circle({p, 0}), ErrorCode::InvalidGeometry);
    fails(validate_circle({p, infinity}), ErrorCode::InvalidGeometry);
    succeeds(validate_pose({p, std::numbers::pi / 2}), "pose orientation is explicit radians");
    fails(validate_pose({p, nan}), ErrorCode::InvalidGeometry);

    auto transform = correction();
    succeeds(validate_transform(transform), "similarity mapping is validated in its declared direction");
    const auto mapped = value(map_point(transform, p));
    check(mapped == Point2D{workpiece(), MeasurementUnit::Millimetre, 6, 22}, "mapping applies scale/rotation/translation with destination units");
    const auto inverse = value(inverse_transform(transform));
    check(value(map_point(inverse, mapped)) == p, "explicit inverse restores source point");
    const std::array chain{transform, inverse};
    succeeds(validate_transform_chain(chain), "composable endpoints and units are accepted");
    const std::array broken_chain{transform, transform};
    fails(validate_transform_chain(broken_chain), ErrorCode::CoordinateMismatch);
    fails(map_point(transform, mapped), ErrorCode::CoordinateMismatch);
    transform.matrix[0] = infinity;
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform = correction(); transform.matrix[1] = 0;
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform = correction(); transform.matrix[1] = -3;
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform = correction(); transform.matrix = {1e-8, 0, 0, 0, 2e-8, 0, 0, 0, 1};
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform.matrix = {1e-8, 6e-9, 0, 0, 8e-9, 0, 0, 0, 1};
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform.model = TransformModel::Affine;
    succeeds(validate_transform(transform), "affine permits independent axis scales");
    transform = correction(); transform.valid_domain = TransformDomain{0, 0, 5, 5};
    succeeds(map_point(transform, p), "mapping checks explicit source domain");
    fails(map_point(transform, {camera(), MeasurementUnit::Pixel, 6, 1}), ErrorCode::InvalidTransform);
    fails(inverse_transform(transform), ErrorCode::UnsupportedConversion);
    transform.valid_domain->maximum_x = -1;
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform = correction(); transform.model = TransformModel::Projective;
    transform.matrix = {1, 0, 0, 0, 1, 0, 1, 0, -1};
    fails(map_point(transform, p), ErrorCode::InvalidTransform);
    succeeds(map_point(transform, {camera(), MeasurementUnit::Pixel, 2, 2}), "projective denominator is checked per point");
    transform = correction(); transform.model = TransformModel::Calibrated;
    transform.matrix = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    transform.calibration = ResourceReference{id<ResourceId>(), ResourceKind::Calibration, 1};
    transform.valid_domain = TransformDomain{0, 0, 5, 5};
    succeeds(validate_transform(transform), "calibrated mapping is a pinned resource/domain declaration");
    fails(map_point(transform, p), ErrorCode::UnsupportedConversion);
    fails(inverse_transform(transform), ErrorCode::UnsupportedConversion);
    transform.calibration->revision.reset();
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
    transform = correction(); transform.from_unit = MeasurementUnit::Second;
    fails(validate_transform(transform), ErrorCode::InvalidTransform);
}

void regions()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto caps = capabilities();
    const std::vector<RegionShape> shapes{FullImageRegion{}, AxisAlignedRect{-4, 2, 10, 5},
        RotatedRect{5, 5, 10, 3, 0.7}, CircleRegion{5, 5, 4}, RingRegion{5, 5, 1, 4},
        SectorRingRegion{5, 5, 1, 4, 0, -std::numbers::pi}, PolygonRegion{{{0, 0}, {4, 0}, {3, 2}, {0, 4}}}};
    for (const auto& shape : shapes) {
        const auto item = region(shape);
        succeeds(validate_region(item, caps), "supported region preserves original shape and boundary declaration");
    }
    const std::vector<RegionShape> invalid_shapes{AxisAlignedRect{0, 0, 0, 1}, RotatedRect{0, 0, 1, 1, nan},
        CircleRegion{0, 0, -1}, RingRegion{0, 0, 2, 1}, SectorRingRegion{0, 0, 1, 2, 0, 0},
        PolygonRegion{{{0, 0}, {1, 1}, {2, 2}}}, PolygonRegion{{{0, 0}, {2, 2}, {0, 2}, {2, 0}}},
        PolygonRegion{{{0, 0}, {2, 0}, {2, 2}, {0, 0}}}};
    for (const auto& shape : invalid_shapes) {
        const auto item = region(shape);
        check(fails(validate_region(item), ErrorCode::InvalidGeometry).location.region == item.id, "shape error identifies the Region");
    }
    auto item = region(EmptyRegionShape{});
    fails(validate_region(item), ErrorCode::EmptyRegion);
    item.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    succeeds(validate_region(item, caps), "empty geometry requires an explicit policy and tool capability");
    auto narrow = caps; narrow.valid_empty_output = false;
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    item = region(RotatedRect{2, 2, 4, 2, 0.75});
    narrow = caps; narrow.shapes = {RegionShapeKind::AxisAlignedRect};
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    check(std::get<RotatedRect>(std::get<RegionPrimitive>(item.source).geometry).angle_radians == 0.75, "unsupported rotations are not converted to axis-aligned bounds");
    item.boundary_policy = RegionBoundaryPolicy::Fill;
    fails(validate_region(item), ErrorCode::InvalidGeometry);
    item.fill = RegionFill{RegionFillMode::Constant, 128};
    succeeds(validate_region(item, caps), "Fill explicitly retains fill mode and sample value");
    narrow = caps; narrow.boundary_policies = {RegionBoundaryPolicy::Reject};
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    item.fill->value = nan;
    fails(validate_region(item), ErrorCode::InvalidGeometry);
    item.fill = RegionFill{RegionFillMode::Reflect, 0};
    succeeds(validate_region(item), "reflect filling has no competing constant value");
    item.boundary_policy = RegionBoundaryPolicy::Intersect; item.fill.reset();
    succeeds(validate_region(item), "intersection is a declaration without destructive clipping");

    item.coordinate_frame = workpiece(); item.unit = MeasurementUnit::Millimetre;
    item.source = InheritedRegion{id<WorkflowId>(), id<NodeId>(), key<OutputId>("region"),
        camera(), MeasurementUnit::Pixel, 0.3, correction()};
    item.exclusions.push_back(RegionPrimitive{camera(), MeasurementUnit::Pixel, CircleRegion{5, 5, 1}, 0.1, correction()});
    succeeds(validate_region(item, caps), "inheritance, correction, local direction and exclusions coexist");
    narrow = caps; narrow.inheritance = false;
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    narrow = caps; narrow.transforms = false;
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    const std::array frames{image_coordinates(), physical_coordinates()};
    succeeds(validate_region_frames(item, frames), "frame catalogue checks referenced units and coordinate declarations");
    fails(validate_region_frames(item, std::span<const CoordinateFrameDefinition>{frames.data(), 1}), ErrorCode::CoordinateMismatch);
    auto wrong_frames = frames; wrong_frames[1].unit = MeasurementUnit::Pixel;
    fails(validate_region_frames(item, wrong_frames), ErrorCode::InvalidCoordinate);
    wrong_frames[1].kind = CoordinateKind::Local;
    fails(validate_region_frames(item, wrong_frames), ErrorCode::CoordinateMismatch);
    const std::array duplicated_frames{image_coordinates(), image_coordinates(), physical_coordinates()};
    fails(validate_region_frames(item, duplicated_frames), ErrorCode::InvalidCoordinate);
    std::get<InheritedRegion>(item.source).transform.reset();
    fails(validate_region(item), ErrorCode::CoordinateMismatch);

    item = region(PolygonRegion{{{0, 0}, {2, 0}, {0, 2}}});
    auto copied = item;
    std::get<PolygonRegion>(std::get<RegionPrimitive>(copied.source).geometry).vertices[0].x = 1;
    check(item != copied && std::get<PolygonRegion>(std::get<RegionPrimitive>(item.source).geometry).vertices[0].x == 0, "configuration copies do not share mutable shape storage");
    narrow = caps; narrow.shapes.push_back(narrow.shapes.front());
    fails(validate_region_capabilities(narrow), ErrorCode::UnsupportedRegion);
    narrow = caps; narrow.mask_depths.clear();
    fails(validate_region_capabilities(narrow), ErrorCode::UnsupportedRegion);
    narrow = caps; narrow.boundary_policies.clear();
    fails(validate_region_capabilities(narrow), ErrorCode::UnsupportedRegion);
    narrow = caps; narrow.geometry_mask_policy = static_cast<GeometryMaskPolicy>(100);
    fails(validate_region_capabilities(narrow), ErrorCode::UnsupportedRegion);
    narrow = caps; narrow.shapes = {static_cast<RegionShapeKind>(100)};
    fails(validate_region_capabilities(narrow), ErrorCode::UnsupportedRegion);

    item = region(AxisAlignedRect{0, 0, 2, 2});
    item.exclusions.push_back(RegionPrimitive{camera(), MeasurementUnit::Pixel, mask_config(), 0, std::nullopt});
    succeeds(validate_region(item, caps), "declared composition permits geometric region and mask exclusion");
    narrow = caps; narrow.geometry_mask_policy = GeometryMaskPolicy::Exclusive;
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
    narrow.geometry_mask_policy = GeometryMaskPolicy::RasterizeGeometry;
    succeeds(validate_region(item, narrow), "rasterization is an explicit capability declaration");
    narrow.mask_depths = {PixelDepth::Float32};
    fails(validate_region(item, narrow), ErrorCode::UnsupportedRegion);
}

void masks()
{
    static_assert(std::is_same_v<decltype(std::declval<ImageHandle>().row(0)), Result<std::span<const std::byte>>>);
    auto config = mask_config();
    succeeds(validate_mask_config(config), "mask configuration contains only resource and mapping declarations");
    config.resource.kind = ResourceKind::Image;
    fails(validate_mask_config(config), ErrorCode::InvalidMask);
    config = mask_config(); config.mapping.target_width = 3;
    fails(validate_mask_config(config), ErrorCode::CoordinateMismatch);
    config = mask_config(); config.format.order = ChannelOrder::RGB;
    fails(validate_mask_config(config), ErrorCode::InvalidMask);
    config = mask_config(); config.valid_pixels = {MaskValueMode::AtLeastThreshold, 256};
    fails(validate_mask_config(config), ErrorCode::InvalidMask);
    config.valid_pixels.threshold = 128;
    succeeds(validate_mask_config(config), "threshold rule is explicit and inside the sample domain");
    config.valid_pixels.mode = MaskValueMode::NonZero;
    fails(validate_mask_config(config), ErrorCode::InvalidMask);
    config.valid_pixels = {MaskValueMode::AtLeastThreshold, std::numeric_limits<double>::quiet_NaN()};
    fails(validate_mask_config(config), ErrorCode::InvalidMask);
    config = mask_config(); config.format.depth = static_cast<PixelDepth>(100);
    fails(validate_mask_config(config), ErrorCode::InvalidMask);

    RasterMask mask = [] {
        auto temporary_image = pixels(camera());
        return RasterMask{temporary_image, {camera(), 2, 2, std::nullopt, MaskSampling::None}, {}};
    }();
    check(value(mask.pixels.row(1))[1] == std::byte{127}, "raster mask owns pixels after its source handle is destroyed");
    succeeds(validate_raster_mask(mask, pixels(camera())), "mask explicitly matches target coordinates and dimensions");
    fails(validate_raster_mask(mask, pixels(workpiece())), ErrorCode::CoordinateMismatch);
    mask.mapping.target_width = 3;
    fails(validate_raster_mask(mask), ErrorCode::CoordinateMismatch);
    mask.mapping.target_width = 2; mask.mapping.target_frame = workpiece();
    fails(validate_raster_mask(mask), ErrorCode::CoordinateMismatch);
    auto transform = correction(); transform.to_unit = MeasurementUnit::Pixel;
    mask.mapping.transform = transform;
    fails(validate_raster_mask(mask), ErrorCode::InvalidMask);
    mask.mapping.sampling = MaskSampling::Nearest;
    succeeds(validate_raster_mask(mask, pixels(workpiece())), "cross-frame masks require explicit mapping and sampling");
    mask.mapping.transform->from_unit = MeasurementUnit::Millimetre;
    fails(validate_raster_mask(mask), ErrorCode::CoordinateMismatch);
    mask.mapping.transform = transform; mask.mapping.transform->matrix[0] = std::numeric_limits<double>::quiet_NaN();
    fails(validate_raster_mask(mask), ErrorCode::InvalidTransform);
    RasterMask rgb{pixels(camera(), {PixelDepth::UInt8, ChannelOrder::RGB}), {camera(), 2, 2, std::nullopt, MaskSampling::None}, {}};
    fails(validate_raster_mask(rgb), ErrorCode::InvalidMask);
    for (const auto depth : {PixelDepth::UInt8, PixelDepth::UInt16, PixelDepth::Float32, PixelDepth::Float64}) {
        RasterMask typed{pixels(camera(), {depth, ChannelOrder::Mono}), {camera(), 2, 2, std::nullopt, MaskSampling::None}, {}};
        succeeds(validate_raster_mask(typed), "supported single-channel depth has explicit nonzero validity");
    }
    const auto cropped_frame = key<CoordinateSystemId>("crop");
    auto full = pixels(camera());
    auto cropped = value(full.crop({1, 0, 1, 2}, cropped_frame));
    RasterMask cropped_mask{cropped, {camera(), 2, 2, std::nullopt, MaskSampling::None}, {}};
    fails(validate_raster_mask(cropped_mask), ErrorCode::CoordinateMismatch);
    check(cropped_mask.pixels.origin_x() == 1 && cropped_mask.pixels.root_frame() == camera(), "mask retains crop/root mapping without implicit alignment");
}
} // namespace

int main(int argc, char** argv)
{
    const std::string group = argc == 2 ? argv[1] : "all";
    try {
        if (group == "all" || group == "geometry") { geometry(); }
        if (group == "all" || group == "regions") { regions(); }
        if (group == "all" || group == "masks") { masks(); }
        if (checks == 0) { throw std::runtime_error("Unknown geometry test group"); }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n"; return 1;
    }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
