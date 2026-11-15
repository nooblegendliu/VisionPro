#include "roi.hpp"

#include "roi_mask.hpp"
#include "vision_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace visionpro::tools::vision {
namespace {
using namespace core;
using Matrix = std::array<double, 9>;
constexpr Matrix identity_matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

FieldId field(std::string_view name)
{ return std::get<FieldId>(SchemaIdFactory::parse<FieldId>(name)); }

Diagnostic locate(Diagnostic diagnostic, const RegionDefinition& region, const ToolValueTarget& binding,
    std::initializer_list<DiagnosticPathStep> path = {})
{
    diagnostic.location.region = region.id;
    if (const auto* parameter = std::get_if<ParameterId>(&binding)) { diagnostic.location.parameter = *parameter; }
    else { diagnostic.location.input = std::get<InputId>(binding); }
    diagnostic.location.path.insert(diagnostic.location.path.begin(), path.begin(), path.end());
    return diagnostic;
}

Matrix compose(const Matrix& outer, const Matrix& inner)
{
    auto result = identity_matrix;
    for (std::size_t row : {0u, 3u}) {
        result[row] = outer[row] * inner[0] + outer[row + 1] * inner[3];
        result[row + 1] = outer[row] * inner[1] + outer[row + 1] * inner[4];
        result[row + 2] = outer[row] * inner[2] + outer[row + 1] * inner[5] + outer[row + 2];
    }
    return result;
}

RegionPrimitive transform_primitive(RegionPrimitive primitive, const std::optional<Transform2D>& outer)
{
    if (!outer) { return primitive; }
    if (!primitive.transform) { primitive.transform = outer; }
    else {
        const auto& inner = *primitive.transform;
        primitive.transform = Transform2D{inner.from, outer->to, inner.from_unit, outer->to_unit,
            TransformModel::Affine, compose(outer->matrix, inner.matrix)};
    }
    return primitive;
}

Vector2D linear(const Matrix& matrix, Vector2D value)
{
    return {std::fma(matrix[0], value.x, matrix[1] * value.y),
        std::fma(matrix[3], value.x, matrix[4] * value.y)};
}

Result<Transform2D> inverse_linear(Transform2D transform)
{
    transform.matrix[2] = 0;
    transform.matrix[5] = 0;
    return inverse_transform(transform);
}

Vector2D unit_direction(double angle)
{
    // Preserve exact half-open edges for the cardinal rotations.
    constexpr double pi = std::numbers::pi;
    const auto reduced = std::abs(angle) <= 4 * pi ? std::remainder(angle, 2 * pi) : angle;
    if (reduced == 0) { return {1, 0}; }
    if (reduced == pi / 2) { return {0, 1}; }
    if (reduced == -pi / 2) { return {0, -1}; }
    if (std::abs(reduced) == pi) { return {-1, 0}; }
    return {std::cos(angle), std::sin(angle)};
}

Result<Vector2D> map_direction(double angle, const std::optional<Transform2D>& correction)
{
    auto direction = unit_direction(angle);
    if (!correction) { return direction; }
    direction = linear(correction->matrix, direction);
    const auto scale = (std::max)(std::abs(direction.x), std::abs(direction.y));
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || scale == 0) {
        return error(ErrorCode::InvalidTransform, "ROI correction does not yield a finite nonzero direction.");
    }
    direction = {direction.x / scale, direction.y / scale};
    const auto length = std::hypot(direction.x, direction.y);
    return Vector2D{direction.x / length, direction.y / length};
}

struct Rectangle {
    double left, top, right, bottom;

    bool contains(Vector2D point) const
    { return left <= point.x && point.x < right && top <= point.y && point.y < bottom; }
};

Rectangle dimensions(std::size_t width, std::size_t height)
{ return {0, 0, static_cast<double>(width), static_cast<double>(height)}; }

struct Extent {
    Rectangle box{};
    bool includes_right{false};
    bool includes_bottom{false};

    bool within(const ImageHandle& image) const
    {
        const auto domain = dimensions(image.width(), image.height());
        return box.left >= 0 && box.top >= 0
            && (box.right < domain.right || (box.right == domain.right && !includes_right))
            && (box.bottom < domain.bottom || (box.bottom == domain.bottom && !includes_bottom));
    }
};

Result<Extent> extent_of(Rectangle rectangle, const Matrix& matrix)
{
    const auto axis = [&](double x, double y, double translation) {
        const auto left = x >= 0 ? rectangle.left : rectangle.right;
        const auto right = x >= 0 ? rectangle.right : rectangle.left;
        const auto top = y >= 0 ? rectangle.top : rectangle.bottom;
        const auto bottom = y >= 0 ? rectangle.bottom : rectangle.top;
        return std::pair{translation + (x * left + y * top), translation + (x * right + y * bottom)};
    };
    const auto [left, right] = axis(matrix[0], matrix[1], matrix[2]);
    const auto [top, bottom] = axis(matrix[3], matrix[4], matrix[5]);
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom)) {
        return error(ErrorCode::InvalidTransform, "ROI correction produces nonfinite bounds.");
    }
    // An upper bound is open if maximizing it needs an excluded local upper edge.
    return Extent{{left, top, right, bottom}, matrix[0] <= 0 && matrix[1] <= 0,
        matrix[3] <= 0 && matrix[4] <= 0};
}

using Shape = std::variant<std::monostate, Rectangle, PreparedRoiMask>;

struct PlacedShape {
    Shape shape;
    Extent extent;
    Matrix inverse;
    Vector2D origin;

    bool contains(Vector2D point) const
    {
        const auto local = linear(inverse, {point.x - origin.x, point.y - origin.y});
        if (const auto* rectangle = std::get_if<Rectangle>(&shape)) { return rectangle->contains(local); }
        const auto* mask = std::get_if<PreparedRoiMask>(&shape);
        return mask && mask->contains(local);
    }
};

Result<PlacedShape> place_shape(Shape shape, Rectangle support, const Matrix& to_source,
    const RegionPrimitive& primitive)
{
    auto placement = primitive.transform.value_or(Transform2D{primitive.coordinate_frame, primitive.coordinate_frame,
        primitive.unit, primitive.unit});
    placement.model = TransformModel::Affine;
    placement.matrix = primitive.transform ? compose(primitive.transform->matrix, to_source) : to_source;
    auto inverse = inverse_linear(placement);
    if (auto* failure = std::get_if<Diagnostic>(&inverse)) { return std::move(*failure); }
    auto extent = extent_of(support, placement.matrix);
    if (auto* failure = std::get_if<Diagnostic>(&extent)) { return std::move(*failure); }
    return PlacedShape{std::move(shape), std::get<Extent>(extent), std::get<Transform2D>(inverse).matrix,
        {placement.matrix[2], placement.matrix[5]}};
}

Result<PlacedShape> prepare_shape(const RegionPrimitive& primitive, const ImageHandle& image, RoiMaskResource& masks)
{
    const auto& geometry = primitive.geometry;
    if (std::holds_alternative<EmptyRegionShape>(geometry)) { return PlacedShape{{}, {}, identity_matrix, {0, 0}}; }
    if (std::holds_alternative<FullImageRegion>(geometry)) {
        const auto rectangle = dimensions(image.width(), image.height());
        return place_shape(rectangle, rectangle, identity_matrix, primitive);
    }
    if (const auto* axis = std::get_if<AxisAlignedRect>(&geometry)) {
        const Rectangle rectangle{0, 0, axis->width, axis->height};
        const Matrix offset{1, 0, axis->x, 0, 1, axis->y, 0, 0, 1};
        return place_shape(rectangle, rectangle, offset, primitive);
    }
    if (const auto* rotated = std::get_if<RotatedRect>(&geometry)) {
        const Rectangle rectangle{-rotated->width / 2, -rotated->height / 2, rotated->width / 2, rotated->height / 2};
        const auto [cosine, sine] = unit_direction(rotated->angle_radians);
        const Matrix pose{cosine, -sine, rotated->center_x, sine, cosine, rotated->center_y, 0, 0, 1};
        return place_shape(rectangle, rectangle, pose, primitive);
    }
    if (const auto* mask = std::get_if<MaskConfig>(&geometry)) {
        const auto& mapping = mask->mapping;
        if (!primitive.transform && (mapping.target_frame != image.coordinate_frame()
            || mapping.target_width != image.width() || mapping.target_height != image.height())) {
            auto failure = error(ErrorCode::CoordinateMismatch, "Uncorrected ROI mask target must match the image frame and dimensions.");
            failure.location.resource = mask->resource.id;
            return failure;
        }
        auto prepared = masks.prepare(*mask);
        if (auto* failure = std::get_if<Diagnostic>(&prepared)) { return std::move(*failure); }
        return place_shape(std::get<PreparedRoiMask>(std::move(prepared)),
            dimensions(mapping.target_width, mapping.target_height), identity_matrix, primitive);
    }
    return error(ErrorCode::UnsupportedRegion, "ROI shape is outside the evaluator's supported geometry.");
}

Result<const ResolvedRegionInput*> frozen_source(const RegionDefinition& region, const ToolValueTarget& binding,
    std::span<const ResolvedRegionInput> regions)
{
    const auto* reference = std::get_if<InheritedRegion>(&region.source);
    if (!reference) { return nullptr; }
    const auto path = field("source");
    const ResolvedRegionInput* found = nullptr;
    for (const auto& candidate : regions) {
        if (candidate.target != binding || candidate.source_index != 0 || candidate.path.size() != 1
            || candidate.path.front() != DiagnosticPathStep{path}) { continue; }
        if (found || candidate.reference != *reference || candidate.producer.invocation.node != reference->node) {
            return error(ErrorCode::InvalidValue, "Inherited ROI requires one unambiguous frozen source at the binding address.");
        }
        found = &candidate;
    }
    if (!found) { return error(ErrorCode::MissingRequiredValue, "Inherited ROI has no frozen source at the binding address."); }
    if (found->value.coordinate_frame != reference->coordinate_frame || found->value.unit != reference->unit) {
        return error(ErrorCode::CoordinateMismatch, "Frozen ROI frame and unit must match the inherited reference before correction.");
    }
    if (!std::holds_alternative<RegionPrimitive>(found->value.source)) {
        return error(ErrorCode::UnsupportedRegion, "Frozen ROI must have a self-contained primary shape.");
    }
    return found;
}

struct Layer {
    RoiLayerTrace trace;
    std::vector<RegionPrimitive> exclusions;
    std::vector<PlacedShape> placed_exclusions;
};

Result<Layer> prepare_layer(const RegionDefinition& region, const ToolValueTarget& binding, const ImageHandle& image,
    std::optional<Transform2D> to_target, std::optional<ToolInvocationIdentity> producer,
    double direction_radians, const std::optional<Transform2D>& direction_correction, RoiMaskResource& masks)
{
    auto direction = map_direction(direction_radians, direction_correction);
    if (auto* failure = std::get_if<Diagnostic>(&direction)) { return locate(std::move(*failure), region, binding, {field("source")}); }
    Layer layer{{region, std::move(producer), to_target, std::get<Vector2D>(direction), 0}, {}, {}};
    for (std::size_t i = 0; i < region.exclusions.size(); ++i) {
        auto exclusion = transform_primitive(std::get<RegionPrimitive>(region.exclusions[i]), to_target);
        auto placed = prepare_shape(exclusion, image, masks);
        if (auto* failure = std::get_if<Diagnostic>(&placed)) {
            return locate(std::move(*failure), region, binding, {field("exclusions"), DiagnosticElementIndex{i}});
        }
        layer.exclusions.push_back(std::move(exclusion));
        layer.placed_exclusions.push_back(std::get<PlacedShape>(std::move(placed)));
    }
    return layer;
}

RegionPrimitive pin_mask(RegionPrimitive primitive, const std::optional<ResourceReference>& resource)
{
    if (auto* mask = std::get_if<MaskConfig>(&primitive.geometry); mask && resource) { mask->resource = *resource; }
    return primitive;
}

Result<RegionDefinition> effective_region(const RegionDefinition& region, const ResolvedRegionInput* frozen,
    RegionPrimitive primary, const std::vector<Layer>& layers, const std::optional<ResourceReference>& mask)
{
    auto effective = region;
    if (frozen) {
        const auto& inherited = std::get<InheritedRegion>(region.source);
        if (frozen->value.boundary_policy == RegionBoundaryPolicy::Reject) { effective.boundary_policy = RegionBoundaryPolicy::Reject; }
        if (frozen->value.empty_policy == EmptyRegionPolicy::Error) {
            const auto has_exclusion = std::ranges::any_of(region.exclusions, [](const RegionSource& exclusion) {
                return !std::holds_alternative<EmptyRegionShape>(std::get<RegionPrimitive>(exclusion).geometry);
            });
            if (region.empty_policy == EmptyRegionPolicy::ValidEmptyOutput && has_exclusion) {
                return error(ErrorCode::UnsupportedRegion, "Independent inherited and outer empty policies cannot be published as one Region2D.");
            }
            effective.empty_policy = EmptyRegionPolicy::Error;
        }
        primary.local_direction_radians = inherited.local_direction_radians;
        const auto& native = std::get<RegionPrimitive>(frozen->value.source);
        if (native.transform) {
            auto inverse = inverse_linear(*native.transform);
            if (auto* failure = std::get_if<Diagnostic>(&inverse)) { return std::move(*failure); }
            auto relative = map_direction(inherited.local_direction_radians, std::get<Transform2D>(std::move(inverse)));
            if (auto* failure = std::get_if<Diagnostic>(&relative)) { return std::move(*failure); }
            const auto direction = std::get<Vector2D>(relative);
            primary.local_direction_radians = std::atan2(direction.y, direction.x);
        }
    }
    effective.source = pin_mask(std::move(primary), mask);
    effective.exclusions.clear();
    for (const auto& layer : layers) {
        for (const auto& exclusion : layer.exclusions) { effective.exclusions.emplace_back(pin_mask(exclusion, mask)); }
    }
    if (auto checked = validate_region_support(effective); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    return effective;
}
} // namespace

core::Result<RoiEvaluation> evaluate_roi(const core::ImageHandle& image, const core::RegionDefinition& region,
    const core::ToolValueTarget& binding, std::span<const core::ResolvedRegionInput> regions,
    std::span<const core::InvocationResourceLease> resources)
{
    if (auto valid = validate_region_support(region); auto* failure = std::get_if<Diagnostic>(&valid)) {
        return locate(std::move(*failure), region, binding);
    }
    if (region.coordinate_frame != image.coordinate_frame() || region.unit != MeasurementUnit::Pixel) {
        return locate(error(ErrorCode::CoordinateMismatch, "ROI must map to the input image's current Pixel coordinate frame."), region, binding);
    }
    auto selected = frozen_source(region, binding, regions);
    if (auto* failure = std::get_if<Diagnostic>(&selected)) { return locate(std::move(*failure), region, binding, {field("source")}); }
    const auto* frozen = std::get<const ResolvedRegionInput*>(selected);
    const auto& base = frozen ? frozen->value : region;
    if (frozen) {
        if (auto valid = validate_region_support(base); auto* failure = std::get_if<Diagnostic>(&valid)) {
            return locate(std::move(*failure), base, binding);
        }
    }

    const auto* inherited = std::get_if<InheritedRegion>(&region.source);
    const auto correction = inherited ? inherited->transform : std::nullopt;
    auto primary = transform_primitive(std::get<RegionPrimitive>(base.source), correction);
    RoiMaskResource masks(resources);
    auto prepared = prepare_shape(primary, image, masks);
    if (auto* failure = std::get_if<Diagnostic>(&prepared)) { return locate(std::move(*failure), base, binding, {field("source")}); }
    const auto& inclusion = std::get<PlacedShape>(prepared);

    std::vector<Layer> layers;
    auto source = prepare_layer(base, binding, image, correction, frozen ? std::optional{frozen->producer} : std::nullopt,
        primary.local_direction_radians, primary.transform, masks);
    if (auto* failure = std::get_if<Diagnostic>(&source)) { return std::move(*failure); }
    layers.push_back(std::get<Layer>(std::move(source)));
    if (inherited) {
        auto outer = prepare_layer(region, binding, image, {}, {}, inherited->local_direction_radians, correction, masks);
        if (auto* failure = std::get_if<Diagnostic>(&outer)) { return std::move(*failure); }
        layers.push_back(std::get<Layer>(std::move(outer)));
    }
    if (auto checked = masks.finish(); auto* failure = std::get_if<Diagnostic>(&checked)) {
        return locate(std::move(*failure), region, binding);
    }
    for (const auto& layer : layers) {
        if (layer.trace.configured.boundary_policy == RegionBoundaryPolicy::Reject && !inclusion.extent.within(image)) {
            return locate(error(ErrorCode::ImageBounds, "The complete ROI inclusion exceeds the image sampling domain."),
                layer.trace.configured, binding, {field("source")});
        }
    }

    const ImageLayout layout{image.width(), image.height(), image.width(), mono8};
    auto length = required_image_bytes(layout);
    if (auto* failure = std::get_if<Diagnostic>(&length)) { return locate(std::move(*failure), region, binding); }
    std::vector<std::byte> pixels(std::get<std::size_t>(length));
    for (std::size_t y = 0; y < layout.height; ++y) {
        for (std::size_t x = 0; x < layout.width; ++x) {
            const Vector2D point{static_cast<double>(x), static_cast<double>(y)};
            if (!inclusion.contains(point)) { continue; }
            bool member = true;
            for (auto& layer : layers) {
                if (std::ranges::any_of(layer.placed_exclusions, [&](const PlacedShape& shape) { return shape.contains(point); })) {
                    member = false;
                    break;
                }
                ++layer.trace.member_count;
            }
            if (member) { pixels[y * layout.row_stride + x] = std::byte{255}; }
        }
    }
    for (const auto& layer : layers) {
        if (layer.trace.member_count == 0 && layer.trace.configured.empty_policy == EmptyRegionPolicy::Error) {
            return locate(error(ErrorCode::EmptyRegion, "ROI has no members after clipping and exclusions."), layer.trace.configured, binding);
        }
    }

    auto copied = image.copy_with_pixels(layout, pixels);
    if (auto* failure = std::get_if<Diagnostic>(&copied)) { return locate(std::move(*failure), region, binding); }
    const auto mask_resource = masks.reference();
    auto effective = effective_region(region, frozen, std::move(primary), layers, mask_resource);
    if (auto* failure = std::get_if<Diagnostic>(&effective)) { *failure = locate(std::move(*failure), region, binding); }
    const auto count = layers.back().trace.member_count;
    const auto direction = layers.back().trace.direction;
    std::vector<RoiLayerTrace> trace;
    trace.reserve(layers.size());
    for (auto& layer : layers) { trace.push_back(std::move(layer.trace)); }
    return RoiEvaluation{{std::get<ImageHandle>(std::move(copied)),
        {image.coordinate_frame(), layout.width, layout.height, {}, MaskSampling::None}, {}},
        count, direction, std::move(trace), mask_resource, std::move(effective)};
}

} // namespace visionpro::tools::vision
