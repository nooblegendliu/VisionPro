#include "vision/roi.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::tools::vision;

constexpr auto pixels = MeasurementUnit::Pixel;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
using Pixel = std::pair<std::size_t, std::size_t>;

void check(bool condition, std::string_view message)
{
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T>
T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<class T>
void rejects(const Result<T>& result, ErrorCode code, const RegionId& region)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure != nullptr, "Expected ROI failure " + std::string(error_code_name(code)) + ".");
    check(failure->code == code && failure->location.region == region && is_valid_diagnostic(*failure),
        "Unexpected ROI diagnostic: " + std::string(error_code_name(failure->code)) + ": " + failure->message);
}

template<SchemaIdentity Id> Id key(std::string_view text)
{ return take(SchemaIdFactory::parse<Id>(text)); }

template<UuidIdentity Id> Id id(char suffix = '1')
{
    std::string text{"10000000-0000-4000-8000-000000000001"};
    text.back() = suffix;
    return take(IdFactory::parse<Id>(text));
}

template<RuntimeIdentity Id> Id runtime(std::uint64_t value)
{ return take(RuntimeIdFactory::from_value<Id>(value)); }

ImageHandle image(std::size_t width, std::size_t height, const CoordinateSystemId& frame)
{
    const std::vector<std::byte> values(width * height, std::byte{73});
    return take(ImageHandle::copy({width, height, width, mono8}, values, frame));
}

RegionPrimitive primitive(const CoordinateSystemId& frame, RegionShape geometry,
    double angle = 0, std::optional<Transform2D> correction = {})
{ return {frame, pixels, std::move(geometry), angle, std::move(correction)}; }

RegionDefinition region(RegionId identity, const CoordinateSystemId& frame, RegionSource source,
    RegionBoundaryPolicy boundary = RegionBoundaryPolicy::Reject, EmptyRegionPolicy empty = EmptyRegionPolicy::Error)
{ return {identity, frame, pixels, std::move(source), {}, boundary, {}, empty}; }

Transform2D translation(const CoordinateSystemId& frame, double x, double y = 0)
{ return {frame, frame, pixels, pixels, TransformModel::Similarity, {1, 0, x, 0, 1, y, 0, 0, 1}}; }

InheritedRegion source_in(const CoordinateSystemId& frame, std::optional<Transform2D> correction = {})
{ return {id<WorkflowId>(), id<NodeId>(), key<OutputId>("effective_roi"), frame, pixels, 0, std::move(correction)}; }

ResolvedRegionInput freeze(const ToolValueTarget& binding, const InheritedRegion& reference, RegionDefinition value)
{
    const ToolInvocationIdentity producer{id<SolutionSessionId>(), runtime<ConfigSnapshotId>(1),
        {runtime<WorkflowRunId>(5), reference.node, runtime<ExecutionScopeId>(0), runtime<InvocationId>(2)}, runtime<AttemptId>(0)};
    return {binding, reference, std::move(value), producer, 0, {key<FieldId>("source")}};
}

RoiEvaluation inherited_roi(const ImageHandle& target, const RegionDefinition& configured,
    const ToolValueTarget& binding, const ResolvedRegionInput& frozen)
{ return take(evaluate_roi(target, configured, binding, std::span{&frozen, 1})); }

void expect_members(const RoiEvaluation& result, const ImageHandle& image, std::initializer_list<Pixel> expected)
{
    const auto& mask = result.mask.pixels;
    check(mask.format() == mono8 && mask.width() == image.width() && mask.height() == image.height(),
        "ROI mask is a same-sized Mono8 image.");
    check(mask.coordinate_frame() == image.coordinate_frame() && mask.root_frame() == image.root_frame()
        && mask.origin_x() == image.origin_x() && mask.origin_y() == image.origin_y(),
        "ROI mask retains the current coordinate frame and root offset.");
    check(!mask.shares_storage_with(image), "ROI mask pixels have independent storage.");
    take(validate_raster_mask(result.mask, image));
    check(result.mask.mapping.sampling == MaskSampling::None && !result.mask.mapping.transform
        && result.mask.valid_pixels == MaskValueRule{}, "An ROI mask is a direct, nonzero membership mask.");

    std::vector<Pixel> actual;
    for (std::size_t y = 0; y < mask.height(); ++y) {
        const auto row = take(mask.row(y));
        for (std::size_t x = 0; x < row.size(); ++x) {
            check(row[x] == std::byte{0} || row[x] == std::byte{255}, "Membership samples are 0 or 255.");
            if (row[x] != std::byte{0}) { actual.emplace_back(x, y); }
        }
    }
    check(result.member_count == expected.size() && actual == std::vector<Pixel>(expected),
        "ROI members differ from the hand-calculated sample centers.");
}

void expect_direction(Vector2D actual, Vector2D expected)
{
    constexpr double tolerance = 1e-9;
    check(std::abs(actual.x - expected.x) <= tolerance && std::abs(actual.y - expected.y) <= tolerance,
        "The corrected direction differs from the hand-calculated unit direction.");
}

void rotated_and_inherited_roi()
{
    const auto frame = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(9, 9, frame);
    const RotatedRect shape{4, 4, 4, 2, std::numbers::pi / 4};
    auto source = region(id<RegionId>('1'), frame, primitive(frame, shape));
    const auto rotated = take(evaluate_roi(target, source, binding));
    expect_members(rotated, target, {{3, 3}, {4, 3}, {3, 4}, {4, 4}, {5, 4}, {4, 5}, {5, 5}});
    check(take(rotated.effective_region) == source, "Local rotated ROI preserves its symbolic geometry.");

    const auto inherited = source_in(frame, translation(frame, 1));
    auto destination = region(id<RegionId>('2'), frame, inherited);
    destination.exclusions.emplace_back(primitive(frame, AxisAlignedRect{5, 4, 1, 1}));
    const auto frozen = freeze(binding, inherited, take(rotated.effective_region));
    const auto translated = inherited_roi(target, destination, binding, frozen);
    expect_members(translated, target, {{4, 3}, {5, 3}, {4, 4}, {6, 4}, {5, 5}, {6, 5}});
    check(translated.layers.size() == 2 && translated.layers[0].configured == frozen.value
        && translated.layers[0].producer == frozen.producer && translated.layers[0].to_target == inherited.transform
        && translated.layers[0].member_count == 7 && translated.layers[1].configured == destination
        && translated.layers[1].member_count == 6, "Trace records each layer and the frozen producer call.");

    source.exclusions.emplace_back(primitive(frame, AxisAlignedRect{3, 3, 1, 1}));
    const auto evaluated_source = take(evaluate_roi(target, source, binding));
    const auto with_source_exclusion = freeze(binding, inherited, take(evaluated_source.effective_region));
    const auto both = inherited_roi(target, destination, binding, with_source_exclusion);
    expect_members(both, target, {{5, 3}, {4, 4}, {6, 4}, {5, 5}, {6, 5}});
    const auto effective = take(both.effective_region);
    const auto* primary = std::get_if<RegionPrimitive>(&effective.source);
    check(primary && primary->geometry == RegionShape{shape} && primary->transform == inherited.transform
        && effective.id == destination.id && effective.exclusions.size() == 2,
        "Flattening preserves rotation, correction, both exclusions and the outer identity.");
    expect_members(take(evaluate_roi(target, effective, binding)), target, {{5, 3}, {4, 4}, {6, 4}, {5, 5}, {6, 5}});
    check(take(target.row(4))[4] == std::byte{73}, "ROI evaluation leaves the input pixels unchanged.");

    rejects(evaluate_roi(target, destination, binding), ErrorCode::MissingRequiredValue, destination.id);
    auto wrong = frozen;
    wrong.source_index = 1;
    rejects(evaluate_roi(target, destination, binding, std::span{&wrong, 1}), ErrorCode::MissingRequiredValue, destination.id);
    wrong = frozen;
    wrong.path = {key<FieldId>("exclusions"), DiagnosticElementIndex{0}};
    rejects(evaluate_roi(target, destination, binding, std::span{&wrong, 1}), ErrorCode::MissingRequiredValue, destination.id);
    wrong = frozen;
    wrong.target = key<ParameterId>("other_roi");
    rejects(evaluate_roi(target, destination, binding, std::span{&wrong, 1}), ErrorCode::MissingRequiredValue, destination.id);
    const std::array duplicate{frozen, frozen};
    rejects(evaluate_roi(target, destination, binding, duplicate), ErrorCode::InvalidValue, destination.id);
}

void direction_and_affine_correction()
{
    const auto native = key<CoordinateSystemId>("native");
    const auto source_frame = key<CoordinateSystemId>("source_frame");
    const auto camera = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(9, 9, camera);
    const Transform2D inner{native, source_frame, pixels, pixels, TransformModel::Similarity, {0, -1, 4, 1, 0, 1, 0, 0, 1}};
    const Transform2D outer{source_frame, camera, pixels, pixels, TransformModel::Similarity, {0, -1, 5, 1, 0, 0, 0, 0, 1}};
    const auto source = region(id<RegionId>('1'), source_frame, primitive(native, AxisAlignedRect{0, 0, 2, 2}, 0, inner));
    const auto inherited = source_in(source_frame, outer);
    const auto destination = region(id<RegionId>('2'), camera, inherited);
    const auto evaluated = inherited_roi(target, destination, binding, freeze(binding, inherited, source));
    expect_members(evaluated, target, {{3, 3}, {4, 3}, {3, 4}, {4, 4}});
    check(evaluated.layers.size() == 2, "The rotated source and consuming layer both have a direction.");
    expect_direction(evaluated.layers[0].direction, {-1, 0});
    expect_direction(evaluated.layers[1].direction, {0, 1});
    expect_direction(evaluated.direction, {0, 1});
    const auto effective = take(evaluated.effective_region);
    const auto roundtrip = take(evaluate_roi(target, effective, binding));
    expect_members(roundtrip, target, {{3, 3}, {4, 3}, {3, 4}, {4, 4}});
    expect_direction(roundtrip.direction, {0, 1});

    const Transform2D shear{camera, camera, pixels, pixels, TransformModel::Affine, {1, 1, 0, 0, 1, 0, 0, 0, 1}};
    const auto sheared = region(id<RegionId>(), camera, primitive(camera, AxisAlignedRect{1, 1, 2, 2}, std::numbers::pi / 2, shear));
    const auto affine = take(evaluate_roi(target, sheared, binding));
    expect_members(affine, target, {{2, 1}, {3, 1}, {3, 2}, {4, 2}});
    expect_direction(affine.direction, {std::numbers::sqrt2 / 2, std::numbers::sqrt2 / 2});

    const auto mm = MeasurementUnit::Millimetre;
    const Transform2D scale{native, camera, mm, pixels, TransformModel::Similarity, {2, 0, 0, 0, 2, 0, 0, 0, 1}};
    const RegionDefinition metric{id<RegionId>('1'), native, mm,
        RegionPrimitive{native, mm, AxisAlignedRect{0.5, 0.5, 1, 1}}, {}};
    auto scaled_reference = source_in(native, scale);
    scaled_reference.unit = mm;
    const auto scaled_roi = region(id<RegionId>('2'), camera, scaled_reference);
    const auto scaled = inherited_roi(target, scaled_roi, binding, freeze(binding, scaled_reference, metric));
    expect_members(scaled, target, {{1, 1}, {2, 1}, {1, 2}, {2, 2}});
    check(take(scaled.effective_region).coordinate_frame == camera, "Explicit correction maps a physical source to the target Pixel frame.");
}

void boundaries_and_empty_roi()
{
    const auto frame = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(4, 3, frame);
    auto configured = region(id<RegionId>(), frame, primitive(frame, AxisAlignedRect{-1, 0, 3, 2}));
    rejects(evaluate_roi(target, configured, binding), ErrorCode::ImageBounds, configured.id);
    configured.exclusions.emplace_back(primitive(frame, AxisAlignedRect{-1, 0, 1, 2}));
    rejects(evaluate_roi(target, configured, binding), ErrorCode::ImageBounds, configured.id);
    configured.boundary_policy = RegionBoundaryPolicy::Intersect;
    expect_members(take(evaluate_roi(target, configured, binding)), target, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    configured.exclusions.emplace_back(primitive(frame, AxisAlignedRect{0, 0, 4, 3}));
    rejects(evaluate_roi(target, configured, binding), ErrorCode::EmptyRegion, configured.id);
    configured.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    expect_members(take(evaluate_roi(target, configured, binding)), target, {});

    const auto full = region(id<RegionId>(), frame, primitive(frame, AxisAlignedRect{0, 0, 4, 3}));
    expect_members(take(evaluate_roi(target, full, binding)), target,
        {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {1, 1}, {2, 1}, {3, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}});
    const auto half_open = region(id<RegionId>(), frame, primitive(frame, AxisAlignedRect{1, 1, 2, 1}));
    expect_members(take(evaluate_roi(target, half_open, binding)), target, {{1, 1}, {2, 1}});

    const Transform2D reflection{frame, frame, pixels, pixels, TransformModel::Affine, {-1, 0, 4, 0, 1, 0, 0, 0, 1}};
    const auto closed_far_edge = region(id<RegionId>(), frame, primitive(frame, AxisAlignedRect{0, 0, 4, 1}, 0, reflection));
    rejects(evaluate_roi(target, closed_far_edge, binding), ErrorCode::ImageBounds, closed_far_edge.id);

    const auto tall_target = image(4, 5, frame);
    const RotatedRect tangent{4 - std::numbers::sqrt2, 2.25, 2, 2, std::numbers::pi / 4};
    auto tilted = region(id<RegionId>(), frame, primitive(frame, tangent));
    expect_members(take(evaluate_roi(tall_target, tilted, binding)), tall_target, {{2, 2}, {3, 2}, {2, 3}, {3, 3}});
    std::get<RotatedRect>(std::get<RegionPrimitive>(tilted.source).geometry).center_x += 1e-7;
    rejects(evaluate_roi(tall_target, tilted, binding), ErrorCode::ImageBounds, tilted.id);

    const auto tiny = region(id<RegionId>(), frame, primitive(frame, AxisAlignedRect{0.1, 0.1, 0.2, 0.2}));
    rejects(evaluate_roi(target, tiny, binding), ErrorCode::EmptyRegion, tiny.id);
    const auto empty = region(id<RegionId>(), frame, primitive(frame, EmptyRegionShape{}),
        RegionBoundaryPolicy::Reject, EmptyRegionPolicy::ValidEmptyOutput);
    expect_members(take(evaluate_roi(target, empty, binding)), target, {});

    const auto crop_frame = key<CoordinateSystemId>("crop_frame");
    const auto crop = take(target.crop({1, 1, 2, 2}, crop_frame));
    const auto cropped_full = region(id<RegionId>(), crop_frame, primitive(crop_frame, FullImageRegion{}));
    expect_members(take(evaluate_roi(crop, cropped_full, binding)), crop, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    rejects(evaluate_roi(target, cropped_full, binding), ErrorCode::CoordinateMismatch, cropped_full.id);
}

void inherited_layer_policies()
{
    const auto frame = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(4, 3, frame);
    const auto clipped = region(id<RegionId>('1'), frame, primitive(frame, AxisAlignedRect{-1, 0, 3, 2}), RegionBoundaryPolicy::Intersect);
    const auto previous = take(evaluate_roi(target, clipped, binding));
    expect_members(previous, target, {{0, 0}, {1, 0}, {0, 1}, {1, 1}});
    const auto moved = source_in(frame, translation(frame, 1));
    const auto new_target = region(id<RegionId>('2'), frame, moved);
    expect_members(inherited_roi(target, new_target, binding, freeze(binding, moved, take(previous.effective_region))), target,
        {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {2, 1}});

    auto source = region(id<RegionId>('1'), frame, primitive(frame, AxisAlignedRect{0, 0, 2, 2}));
    const auto outside = source_in(frame, translation(frame, -1));
    const auto intersecting = region(id<RegionId>('2'), frame, outside, RegionBoundaryPolicy::Intersect);
    const auto reject_layer = freeze(binding, outside, source);
    rejects(evaluate_roi(target, intersecting, binding, std::span{&reject_layer, 1}), ErrorCode::ImageBounds, source.id);

    source.boundary_policy = RegionBoundaryPolicy::Intersect;
    const auto off_image = source_in(frame, translation(frame, 10));
    const auto permissive = region(id<RegionId>('2'), frame, off_image, RegionBoundaryPolicy::Intersect, EmptyRegionPolicy::ValidEmptyOutput);
    const auto error_layer = freeze(binding, off_image, source);
    rejects(evaluate_roi(target, permissive, binding, std::span{&error_layer, 1}), ErrorCode::EmptyRegion, source.id);

    const auto inherited = source_in(frame);
    auto unflattenable = region(id<RegionId>('2'), frame, inherited, RegionBoundaryPolicy::Intersect, EmptyRegionPolicy::ValidEmptyOutput);
    unflattenable.exclusions.emplace_back(primitive(frame, FullImageRegion{}));
    const auto distinct_empty_rules = inherited_roi(target, unflattenable, binding, freeze(binding, inherited, source));
    expect_members(distinct_empty_rules, target, {});
    check(distinct_empty_rules.layers.size() == 2 && distinct_empty_rules.layers[0].member_count == 4
        && distinct_empty_rules.layers[1].member_count == 0, "Source and outer scope retain independent empty policies.");
    rejects(distinct_empty_rules.effective_region, ErrorCode::UnsupportedRegion, unflattenable.id);

    auto unsupported_exclusion = region(id<RegionId>('2'), frame, primitive(frame, FullImageRegion{}));
    unsupported_exclusion.exclusions.emplace_back(inherited);
    rejects(evaluate_roi(target, unsupported_exclusion, binding), ErrorCode::UnsupportedRegion, unsupported_exclusion.id);
}

void curved_bounds_and_inheritance()
{
    const auto frame = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(7, 5, frame);
    constexpr auto pi = std::numbers::pi;
    const auto quarter = region(id<RegionId>(), frame, primitive(frame, SectorRingRegion{0, 0, 0, 2, 0, pi/2}));
    expect_members(take(evaluate_roi(target, quarter, binding)), target,
        {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}, {0, 2}});

    auto source = region(id<RegionId>('1'), frame, primitive(frame, SectorRingRegion{2, 2, 1, 2, pi/4, -pi/2}));
    source.exclusions.push_back(primitive(frame, CircleRegion{3, 2, 0.25}));
    const auto resolved = take(evaluate_roi(target, source, binding));
    expect_members(resolved, target, {{3, 1}, {4, 2}, {3, 3}});
    const auto reference = source_in(frame, translation(frame, 1));
    auto outer = region(id<RegionId>('2'), frame, reference);
    outer.exclusions.push_back(primitive(frame, PolygonRegion{{{4, 1}, {4.25, 1}, {4, 1.25}}}));
    const auto inherited = inherited_roi(target, outer, binding, freeze(binding, reference, take(resolved.effective_region)));
    expect_members(inherited, target, {{5, 2}, {4, 3}});
    expect_members(take(evaluate_roi(target, take(inherited.effective_region), binding)), target, {{5, 2}, {4, 3}});

    const Transform2D shear{frame, frame, pixels, pixels, TransformModel::Affine, {1, 1, 1.5, 0, 1, 2, 0, 0, 1}};
    const auto ellipse = region(id<RegionId>(), frame, primitive(frame, CircleRegion{0, 0, 1}, 0, shear));
    // Its transformed square support extends to x=-0.5, but the actual ellipse fits.
    expect_members(take(evaluate_roi(target, ellipse, binding)), target, {{1, 2}, {2, 2}});
    const auto tangent = region(id<RegionId>(), frame, primitive(frame, CircleRegion{6, 2, 1}));
    rejects(evaluate_roi(target, tangent, binding), ErrorCode::ImageBounds, tangent.id);
}

void closed_shape_roundoff()
{
    const auto frame = key<CoordinateSystemId>("camera");
    const ToolValueTarget binding{key<ParameterId>("roi")};
    const auto target = image(7, 6, frame);
    constexpr double pi = std::numbers::pi;
    SectorRingRegion crossing{3, 3, 1, 2.25, 2 * pi - pi / 6, pi / 6 + std::atan2(1.0, 2.0)};
    const auto evaluate = [&](RegionShape shape) {
        return evaluate_roi(target, region(id<RegionId>(), frame, primitive(frame, std::move(shape))), binding);
    };
    expect_members(take(evaluate(crossing)), target, {{5, 2}, {4, 3}, {5, 3}, {5, 4}});
    crossing.sweep_radians -= 1e-8;
    expect_members(take(evaluate(crossing)), target, {{5, 2}, {4, 3}, {5, 3}});

    // The continuous sector is tangent to x=0 at its end, with no part left of the image.
    SectorRingRegion tangent{0, 2, 0, 2, 2 * pi - pi / 9, pi / 9 + pi / 2};
    expect_members(take(evaluate(tangent)), target, {{0, 2}, {1, 2}, {2, 2}, {0, 3}, {1, 3}, {0, 4}});
    tangent.sweep_radians += 1e-8;
    rejects(evaluate(tangent), ErrorCode::ImageBounds, id<RegionId>());

    PolygonRegion triangle{{{0.1, 0.4}, {2.8, 2.2}, {0.1, 2.2}}};
    expect_members(take(evaluate(triangle)), target, {{1, 1}, {1, 2}, {2, 2}});
    triangle.vertices[1].y += 1e-8;
    expect_members(take(evaluate(triangle)), target, {{1, 2}, {2, 2}});
}
} // namespace

int main()
{
    try {
        rotated_and_inherited_roi();
        direction_and_affine_correction();
        boundaries_and_empty_roi();
        inherited_layer_policies();
        curved_bounds_and_inheritance();
        closed_shape_roundoff();
        std::cout << "V07-V09 ROI geometry, inherited layers and boundary policies passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
