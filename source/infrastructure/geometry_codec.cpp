#include "geometry_codec.hpp"

#include "resource_codec.hpp"
#include "value_codec.hpp"

#include <type_traits>

namespace visionpro::infrastructure::detail {
using namespace core;

template<> struct EnumNames<CoordinateKind> {
    static constexpr auto values = std::to_array<std::string_view>({"image", "physical", "local"});
};
template<> struct EnumNames<Handedness> {
    static constexpr auto values = std::to_array<std::string_view>({"right_handed", "left_handed"});
};
template<> struct EnumNames<TransformModel> {
    static constexpr auto values = std::to_array<std::string_view>({"similarity", "affine", "projective", "calibrated"});
};
template<> struct EnumNames<MaskSampling> {
    static constexpr auto values = std::to_array<std::string_view>({"none", "nearest", "linear"});
};
template<> struct EnumNames<MaskValueMode> {
    static constexpr auto values = std::to_array<std::string_view>({"nonzero", "at_least_threshold"});
};
template<> struct EnumNames<PixelDepth> {
    static constexpr auto values = std::to_array<std::string_view>({"uint8", "uint16", "float32", "float64"});
};
template<> struct EnumNames<ChannelOrder> {
    static constexpr auto values = std::to_array<std::string_view>({"mono", "rgb", "bgr", "rgba", "bgra"});
};
template<> struct EnumNames<RegionShapeKind> {
    static constexpr auto values = std::to_array<std::string_view>({
        "full_image", "empty", "axis_aligned_rect", "rotated_rect", "circle", "ring", "sector_ring", "polygon", "raster_mask"});
};
template<> struct EnumNames<RegionBoundaryPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"reject", "intersect", "fill"});
};
template<> struct EnumNames<RegionFillMode> {
    static constexpr auto values = std::to_array<std::string_view>({"constant", "replicate", "reflect"});
};
template<> struct EnumNames<EmptyRegionPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"error", "valid_empty_output"});
};

namespace {

Json encode_vector(const Vector2D& point)
{
    return {{"x", point.x}, {"y", point.y}};
}

Vector2D decode_vector(const Reader& reader)
{
    reader.object({"x", "y"});
    return {reader.field("x").number(), reader.field("y").number()};
}

Json encode_domain(const TransformDomain& domain)
{
    return {{"minimum_x", domain.minimum_x}, {"minimum_y", domain.minimum_y},
        {"maximum_x", domain.maximum_x}, {"maximum_y", domain.maximum_y}};
}

TransformDomain decode_domain(const Reader& reader)
{
    reader.object({"minimum_x", "minimum_y", "maximum_x", "maximum_y"});
    return {reader.field("minimum_x").number(), reader.field("minimum_y").number(),
        reader.field("maximum_x").number(), reader.field("maximum_y").number()};
}

std::array<double, 9> decode_matrix(const Reader& reader)
{
    std::array<double, 9> matrix{};
    if (reader.size() != matrix.size()) { reader.fail(ErrorCode::InvalidTransform, "A transform requires nine matrix coefficients."); }
    for (std::size_t index = 0; index < matrix.size(); ++index) {
        matrix[index] = reader.at(index).number();
    }
    return matrix;
}

Json encode_format(const PixelFormat& format)
{
    return {{"depth", write_enum(format.depth)}, {"order", write_enum(format.order)}};
}

PixelFormat decode_format(const Reader& reader)
{
    reader.object({"depth", "order"});
    return {read_enum<PixelDepth>(reader.field("depth")), read_enum<ChannelOrder>(reader.field("order"))};
}

Json encode_mask_mapping(const MaskMapping& mapping)
{
    return {{"target_frame", write_id(mapping.target_frame)},
        {"target_width", std::to_string(mapping.target_width)}, {"target_height", std::to_string(mapping.target_height)},
        {"transform", write_optional(mapping.transform, encode_transform)}, {"sampling", write_enum(mapping.sampling)}};
}

MaskMapping decode_mask_mapping(const Reader& reader)
{
    reader.object({"target_frame", "target_width", "target_height", "transform", "sampling"});
    return {read_id<CoordinateSystemId>(reader.field("target_frame")), reader.field("target_width").size_value(),
        reader.field("target_height").size_value(), read_optional<Transform2D>(reader.field("transform"), decode_transform),
        read_enum<MaskSampling>(reader.field("sampling"))};
}

Json encode_mask_rule(const MaskValueRule& rule)
{
    return {{"mode", write_enum(rule.mode)}, {"threshold", rule.threshold}};
}

MaskValueRule decode_mask_rule(const Reader& reader)
{
    reader.object({"mode", "threshold"});
    return {read_enum<MaskValueMode>(reader.field("mode")), reader.field("threshold").number()};
}

Json encode_mask(const MaskConfig& mask)
{
    return {{"resource", encode_resource_reference(mask.resource)}, {"mask_frame", write_id(mask.mask_frame)},
        {"width", std::to_string(mask.width)}, {"height", std::to_string(mask.height)}, {"format", encode_format(mask.format)},
        {"mapping", encode_mask_mapping(mask.mapping)}, {"valid_pixels", encode_mask_rule(mask.valid_pixels)}};
}

MaskConfig decode_mask(const Reader& reader)
{
    reader.object({"resource", "mask_frame", "width", "height", "format", "mapping", "valid_pixels"});
    return {decode_resource_reference(reader.field("resource")), read_id<CoordinateSystemId>(reader.field("mask_frame")),
        reader.field("width").size_value(), reader.field("height").size_value(), decode_format(reader.field("format")),
        decode_mask_mapping(reader.field("mapping")), decode_mask_rule(reader.field("valid_pixels"))};
}

Json encode_shape(const RegionShape& shape)
{
    return std::visit([](const auto& value) -> Json {
        using Shape = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Shape, FullImageRegion>) {
            return {{"kind", write_enum(RegionShapeKind::FullImage)}};
        } else if constexpr (std::same_as<Shape, EmptyRegionShape>) {
            return {{"kind", write_enum(RegionShapeKind::Empty)}};
        } else if constexpr (std::same_as<Shape, AxisAlignedRect>) {
            return {{"kind", write_enum(RegionShapeKind::AxisAlignedRect)}, {"x", value.x}, {"y", value.y},
                {"width", value.width}, {"height", value.height}};
        } else if constexpr (std::same_as<Shape, RotatedRect>) {
            return {{"kind", write_enum(RegionShapeKind::RotatedRect)}, {"center_x", value.center_x}, {"center_y", value.center_y},
                {"width", value.width}, {"height", value.height}, {"angle_radians", value.angle_radians}};
        } else if constexpr (std::same_as<Shape, CircleRegion>) {
            return {{"kind", write_enum(RegionShapeKind::Circle)}, {"center_x", value.center_x}, {"center_y", value.center_y},
                {"radius", value.radius}};
        } else if constexpr (std::same_as<Shape, RingRegion>) {
            return {{"kind", write_enum(RegionShapeKind::Ring)}, {"center_x", value.center_x}, {"center_y", value.center_y},
                {"inner_radius", value.inner_radius}, {"outer_radius", value.outer_radius}};
        } else if constexpr (std::same_as<Shape, SectorRingRegion>) {
            return {{"kind", write_enum(RegionShapeKind::SectorRing)}, {"center_x", value.center_x}, {"center_y", value.center_y},
                {"inner_radius", value.inner_radius}, {"outer_radius", value.outer_radius},
                {"start_radians", value.start_radians}, {"sweep_radians", value.sweep_radians}};
        } else if constexpr (std::same_as<Shape, PolygonRegion>) {
            return {{"kind", write_enum(RegionShapeKind::Polygon)}, {"vertices", write_array(value.vertices, encode_vector)}};
        } else {
            static_assert(std::same_as<Shape, MaskConfig>);
            return {{"kind", write_enum(RegionShapeKind::RasterMask)}, {"mask", encode_mask(value)}};
        }
    }, shape);
}

RegionShape decode_shape(const Reader& reader)
{
    switch (read_enum<RegionShapeKind>(reader.field("kind"))) {
    case RegionShapeKind::FullImage:
        reader.object({"kind"});
        return FullImageRegion{};
    case RegionShapeKind::Empty:
        reader.object({"kind"});
        return EmptyRegionShape{};
    case RegionShapeKind::AxisAlignedRect:
        reader.object({"kind", "x", "y", "width", "height"});
        return AxisAlignedRect{reader.field("x").number(), reader.field("y").number(),
            reader.field("width").number(), reader.field("height").number()};
    case RegionShapeKind::RotatedRect:
        reader.object({"kind", "center_x", "center_y", "width", "height", "angle_radians"});
        return RotatedRect{reader.field("center_x").number(), reader.field("center_y").number(),
            reader.field("width").number(), reader.field("height").number(), reader.field("angle_radians").number()};
    case RegionShapeKind::Circle:
        reader.object({"kind", "center_x", "center_y", "radius"});
        return CircleRegion{reader.field("center_x").number(), reader.field("center_y").number(), reader.field("radius").number()};
    case RegionShapeKind::Ring:
        reader.object({"kind", "center_x", "center_y", "inner_radius", "outer_radius"});
        return RingRegion{reader.field("center_x").number(), reader.field("center_y").number(),
            reader.field("inner_radius").number(), reader.field("outer_radius").number()};
    case RegionShapeKind::SectorRing:
        reader.object({"kind", "center_x", "center_y", "inner_radius", "outer_radius", "start_radians", "sweep_radians"});
        return SectorRingRegion{reader.field("center_x").number(), reader.field("center_y").number(),
            reader.field("inner_radius").number(), reader.field("outer_radius").number(), reader.field("start_radians").number(),
            reader.field("sweep_radians").number()};
    case RegionShapeKind::Polygon:
        reader.object({"kind", "vertices"});
        return PolygonRegion{read_array<Vector2D>(reader.field("vertices"), decode_vector)};
    case RegionShapeKind::RasterMask:
        reader.object({"kind", "mask"});
        return decode_mask(reader.field("mask"));
    }
    reader.fail(ErrorCode::UnsupportedSolutionContent, "Unsupported ROI shape.");
}

Json encode_region_source(const RegionSource& source)
{
    return std::visit([](const auto& value) {
        Json result = {{"coordinate_frame", write_id(value.coordinate_frame)}, {"unit", write_enum(value.unit)},
            {"local_direction_radians", value.local_direction_radians}, {"transform", write_optional(value.transform, encode_transform)}};
        if constexpr (std::same_as<std::decay_t<decltype(value)>, RegionPrimitive>) {
            result["kind"] = "primitive";
            result["geometry"] = encode_shape(value.geometry);
        } else {
            result["kind"] = "inherited";
            result["workflow"] = write_id(value.workflow);
            result["node"] = write_id(value.node);
            result["output"] = write_id(value.output);
        }
        return result;
    }, source);
}

RegionSource decode_region_source(const Reader& reader)
{
    const auto kind = reader.field("kind").text();
    if (kind == "primitive") {
        reader.object({"kind", "coordinate_frame", "unit", "geometry", "local_direction_radians", "transform"});
        return RegionPrimitive{read_id<CoordinateSystemId>(reader.field("coordinate_frame")),
            read_enum<MeasurementUnit>(reader.field("unit")), decode_shape(reader.field("geometry")),
            reader.field("local_direction_radians").number(), read_optional<Transform2D>(reader.field("transform"), decode_transform)};
    }
    if (kind == "inherited") {
        reader.object({"kind", "workflow", "node", "output", "coordinate_frame", "unit", "local_direction_radians", "transform"});
        return InheritedRegion{read_id<WorkflowId>(reader.field("workflow")), read_id<NodeId>(reader.field("node")),
            read_id<OutputId>(reader.field("output")), read_id<CoordinateSystemId>(reader.field("coordinate_frame")),
            read_enum<MeasurementUnit>(reader.field("unit")), reader.field("local_direction_radians").number(),
            read_optional<Transform2D>(reader.field("transform"), decode_transform)};
    }
    reader.field("kind").fail(ErrorCode::UnsupportedSolutionContent, "Unsupported ROI source.");
}

Json encode_fill(const RegionFill& fill)
{
    return {{"mode", write_enum(fill.mode)}, {"value", fill.value}};
}

RegionFill decode_fill(const Reader& reader)
{
    reader.object({"mode", "value"});
    return {read_enum<RegionFillMode>(reader.field("mode")), reader.field("value").number()};
}

} // namespace

Json encode_point(const Point2D& point)
{
    return {{"coordinate_frame", write_id(point.coordinate_frame)}, {"unit", write_enum(point.unit)}, {"x", point.x}, {"y", point.y}};
}

Point2D decode_point(const Reader& reader)
{
    reader.object({"coordinate_frame", "unit", "x", "y"});
    return {read_id<CoordinateSystemId>(reader.field("coordinate_frame")), read_enum<MeasurementUnit>(reader.field("unit")),
        reader.field("x").number(), reader.field("y").number()};
}

Json encode_line(const Line2D& line)
{
    return {{"start", encode_point(line.start)}, {"end", encode_point(line.end)}};
}

Line2D decode_line(const Reader& reader)
{
    reader.object({"start", "end"});
    return {decode_point(reader.field("start")), decode_point(reader.field("end"))};
}

Json encode_circle(const Circle2D& circle)
{
    return {{"center", encode_point(circle.center)}, {"radius", circle.radius}};
}

Circle2D decode_circle(const Reader& reader)
{
    reader.object({"center", "radius"});
    return {decode_point(reader.field("center")), reader.field("radius").number()};
}

Json encode_pose(const Pose2D& pose)
{
    return {{"position", encode_point(pose.position)}, {"angle_radians", pose.angle_radians}};
}

Pose2D decode_pose(const Reader& reader)
{
    reader.object({"position", "angle_radians"});
    return {decode_point(reader.field("position")), reader.field("angle_radians").number()};
}

Json encode_transform(const Transform2D& transform)
{
    return {{"from", write_id(transform.from)}, {"to", write_id(transform.to)}, {"from_unit", write_enum(transform.from_unit)},
        {"to_unit", write_enum(transform.to_unit)}, {"model", write_enum(transform.model)}, {"matrix", transform.matrix},
        {"calibration", write_optional(transform.calibration, encode_resource_reference)},
        {"valid_domain", write_optional(transform.valid_domain, encode_domain)}};
}

Transform2D decode_transform(const Reader& reader)
{
    reader.object({"from", "to", "from_unit", "to_unit", "model", "matrix", "calibration", "valid_domain"});
    return {read_id<CoordinateSystemId>(reader.field("from")), read_id<CoordinateSystemId>(reader.field("to")),
        read_enum<MeasurementUnit>(reader.field("from_unit")), read_enum<MeasurementUnit>(reader.field("to_unit")),
        read_enum<TransformModel>(reader.field("model")), decode_matrix(reader.field("matrix")),
        read_optional<ResourceReference>(reader.field("calibration"), decode_resource_reference),
        read_optional<TransformDomain>(reader.field("valid_domain"), decode_domain)};
}

Json encode_region(const RegionDefinition& region)
{
    return {{"id", write_id(region.id)}, {"coordinate_frame", write_id(region.coordinate_frame)}, {"unit", write_enum(region.unit)},
        {"source", encode_region_source(region.source)}, {"exclusions", write_array(region.exclusions, encode_region_source)},
        {"boundary_policy", write_enum(region.boundary_policy)}, {"fill", write_optional(region.fill, encode_fill)},
        {"empty_policy", write_enum(region.empty_policy)}};
}

RegionDefinition decode_region(const Reader& reader)
{
    reader.object({"id", "coordinate_frame", "unit", "source", "exclusions", "boundary_policy", "fill", "empty_policy"});
    return {read_id<RegionId>(reader.field("id")), read_id<CoordinateSystemId>(reader.field("coordinate_frame")),
        read_enum<MeasurementUnit>(reader.field("unit")), decode_region_source(reader.field("source")),
        read_array<RegionSource>(reader.field("exclusions"), decode_region_source),
        read_enum<RegionBoundaryPolicy>(reader.field("boundary_policy")), read_optional<RegionFill>(reader.field("fill"), decode_fill),
        read_enum<EmptyRegionPolicy>(reader.field("empty_policy"))};
}

Json encode_coordinate(const CoordinateFrameDefinition& frame)
{
    return {{"id", write_id(frame.id)}, {"kind", write_enum(frame.kind)}, {"unit", write_enum(frame.unit)},
        {"origin", encode_vector(frame.origin)}, {"x_axis", encode_vector(frame.x_axis)}, {"y_axis", encode_vector(frame.y_axis)},
        {"handedness", write_enum(frame.handedness)}};
}

CoordinateFrameDefinition decode_coordinate(const Reader& reader)
{
    reader.object({"id", "kind", "unit", "origin", "x_axis", "y_axis", "handedness"});
    CoordinateFrameDefinition frame{read_id<CoordinateSystemId>(reader.field("id")), read_enum<CoordinateKind>(reader.field("kind")),
        read_enum<MeasurementUnit>(reader.field("unit")), decode_vector(reader.field("origin")), decode_vector(reader.field("x_axis")),
        decode_vector(reader.field("y_axis")), read_enum<Handedness>(reader.field("handedness"))};
    static_cast<void>(reader.checked(validate_coordinate_frame(frame)));
    return frame;
}

} // namespace visionpro::infrastructure::detail
