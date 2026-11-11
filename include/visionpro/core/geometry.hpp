#pragma once

#include <visionpro/core/coordinate.hpp>
#include <visionpro/core/image.hpp>
#include <visionpro/core/resource_reference.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace visionpro::core {

// All orientations are radians, positive from the current frame's +x towards +y.
// Consequently positive image angles are clockwise; physical frames declare their own axes.
struct Point2D {
    CoordinateSystemId coordinate_frame;
    MeasurementUnit unit;
    double x;
    double y;
    bool operator==(const Point2D&) const = default;
};
struct Line2D {
    Point2D start;
    Point2D end;
    bool operator==(const Line2D&) const = default;
};
struct Circle2D {
    Point2D center;
    double radius;
    bool operator==(const Circle2D&) const = default;
};
struct Pose2D {
    Point2D position;
    double angle_radians;
    bool operator==(const Pose2D&) const = default;
};

struct TransformDomain {
    double minimum_x;
    double minimum_y;
    double maximum_x;
    double maximum_y;
    bool operator==(const TransformDomain&) const = default;
};
enum class TransformModel { Similarity, Affine, Projective, Calibrated };
struct Transform2D {
    CoordinateSystemId from;
    CoordinateSystemId to;
    MeasurementUnit from_unit;
    MeasurementUnit to_unit;
    TransformModel model{TransformModel::Affine};
    // Row-major homogeneous matrix, applied to a column vector (x,y,1).
    std::array<double, 9> matrix{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::optional<ResourceReference> calibration;
    std::optional<TransformDomain> valid_domain;
    bool operator==(const Transform2D&) const = default;
};

[[nodiscard]] Result<std::monostate> validate_point(const Point2D& point);
[[nodiscard]] Result<std::monostate> validate_line(const Line2D& line);
[[nodiscard]] Result<std::monostate> validate_circle(const Circle2D& circle);
[[nodiscard]] Result<std::monostate> validate_pose(const Pose2D& pose);
[[nodiscard]] Result<std::monostate> validate_transform(const Transform2D& transform);
// Checks declared connectivity and each source domain. It does not compose matrices or derive a chain domain.
[[nodiscard]] Result<std::monostate> validate_transform_chain(std::span<const Transform2D> transforms);
[[nodiscard]] Result<Point2D> map_point(const Transform2D& transform, const Point2D& point);
// Inverse domains are not guessed. Bounded/calibrated transforms require an explicit backend policy.
[[nodiscard]] Result<Transform2D> inverse_transform(const Transform2D& transform);

enum class MaskSampling { None, Nearest, Linear };
enum class MaskValueMode { NonZero, AtLeastThreshold };
struct MaskValueRule {
    MaskValueMode mode{MaskValueMode::NonZero};
    double threshold{0};
    bool operator==(const MaskValueRule&) const = default;
};
struct MaskMapping {
    CoordinateSystemId target_frame;
    std::size_t target_width;
    std::size_t target_height;
    std::optional<Transform2D> transform;
    MaskSampling sampling{MaskSampling::None};
    bool operator==(const MaskMapping&) const = default;
};
struct MaskConfig {
    ResourceReference resource;
    CoordinateSystemId mask_frame;
    std::size_t width;
    std::size_t height;
    PixelFormat format;
    MaskMapping mapping;
    MaskValueRule valid_pixels;
    bool operator==(const MaskConfig&) const = default;
};
struct RasterMask {
    ImageHandle pixels;
    MaskMapping mapping;
    MaskValueRule valid_pixels;
    bool operator==(const RasterMask&) const = default;
};

[[nodiscard]] Result<std::monostate> validate_mask_config(const MaskConfig& mask);
[[nodiscard]] Result<std::monostate> validate_raster_mask(const RasterMask& mask);
[[nodiscard]] Result<std::monostate> validate_raster_mask(const RasterMask& mask, const ImageHandle& target);

struct FullImageRegion { bool operator==(const FullImageRegion&) const = default; };
struct EmptyRegionShape { bool operator==(const EmptyRegionShape&) const = default; };
struct AxisAlignedRect {
    double x; double y; double width; double height;
    bool operator==(const AxisAlignedRect&) const = default;
};
struct RotatedRect {
    double center_x; double center_y; double width; double height; double angle_radians;
    bool operator==(const RotatedRect&) const = default;
};
struct CircleRegion {
    double center_x; double center_y; double radius;
    bool operator==(const CircleRegion&) const = default;
};
struct RingRegion {
    double center_x; double center_y; double inner_radius; double outer_radius;
    bool operator==(const RingRegion&) const = default;
};
struct SectorRingRegion {
    double center_x; double center_y; double inner_radius; double outer_radius;
    double start_radians; double sweep_radians;
    bool operator==(const SectorRingRegion&) const = default;
};
struct PolygonRegion {
    // Open vertex sequence: closing the last edge is implicit; no repeated closing vertex.
    std::vector<Vector2D> vertices;
    bool operator==(const PolygonRegion&) const = default;
};
using RegionShape = std::variant<FullImageRegion, EmptyRegionShape, AxisAlignedRect,
    RotatedRect, CircleRegion, RingRegion, SectorRingRegion, PolygonRegion, MaskConfig>;

struct RegionPrimitive {
    CoordinateSystemId coordinate_frame;
    MeasurementUnit unit;
    RegionShape geometry;
    double local_direction_radians{0};
    // Applies to both shape and its local direction, not just its axis-aligned bounds.
    std::optional<Transform2D> transform;
    bool operator==(const RegionPrimitive&) const = default;
};
struct InheritedRegion {
    WorkflowId workflow;
    NodeId node;
    OutputId output;
    CoordinateSystemId coordinate_frame;
    MeasurementUnit unit;
    double local_direction_radians{0};
    std::optional<Transform2D> transform;
    bool operator==(const InheritedRegion&) const = default;
};
using RegionSource = std::variant<RegionPrimitive, InheritedRegion>;
enum class RegionBoundaryPolicy { Reject, Intersect, Fill };
enum class RegionFillMode { Constant, Replicate, Reflect };
struct RegionFill {
    RegionFillMode mode;
    double value{0};
    bool operator==(const RegionFill&) const = default;
};
enum class EmptyRegionPolicy { Error, ValidEmptyOutput };
struct RegionDefinition {
    RegionId id;
    CoordinateSystemId coordinate_frame;
    MeasurementUnit unit;
    RegionSource source;
    std::vector<RegionSource> exclusions;
    RegionBoundaryPolicy boundary_policy{RegionBoundaryPolicy::Reject};
    std::optional<RegionFill> fill;
    EmptyRegionPolicy empty_policy{EmptyRegionPolicy::Error};
    bool operator==(const RegionDefinition&) const = default;
};

enum class RegionShapeKind { FullImage, Empty, AxisAlignedRect, RotatedRect, Circle, Ring, SectorRing, Polygon, RasterMask };
enum class GeometryMaskPolicy { Exclusive, Combine, RasterizeGeometry };
struct RegionCapabilities {
    std::vector<RegionShapeKind> shapes;
    std::vector<RegionBoundaryPolicy> boundary_policies;
    GeometryMaskPolicy geometry_mask_policy{GeometryMaskPolicy::Exclusive};
    bool inheritance{false};
    bool transforms{false};
    bool valid_empty_output{false};
    std::vector<PixelDepth> mask_depths;
    bool operator==(const RegionCapabilities&) const = default;
};

// These checks preserve symbolic shapes/corrections/exclusions. Pixel intersection, clipping and
// rasterization belong to algorithm adapters; inherited shape capabilities are checked after resolution.
[[nodiscard]] Result<std::monostate> validate_region(const RegionDefinition& region);
[[nodiscard]] Result<std::monostate> validate_region_capabilities(const RegionCapabilities& capabilities);
[[nodiscard]] Result<std::monostate> validate_region(const RegionDefinition& region, const RegionCapabilities& capabilities);
// Resolves declared frames/units only; inherited output existence and scope belong to S2.5.
[[nodiscard]] Result<std::monostate> validate_region_frames(const RegionDefinition& region,
    std::span<const CoordinateFrameDefinition> frames);

} // namespace visionpro::core
