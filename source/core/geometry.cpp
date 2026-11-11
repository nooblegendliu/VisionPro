#include <visionpro/core/geometry.hpp>

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace visionpro::core {
namespace {

using Check = Result<std::monostate>;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
bool failed(const Check& check) { return std::holds_alternative<Diagnostic>(check); }
bool finite(double value) { return std::isfinite(value); }
bool finite(Vector2D value) { return finite(value.x) && finite(value.y); }
bool spatial(MeasurementUnit unit)
{ return unit == MeasurementUnit::Pixel || unit == MeasurementUnit::Millimetre || unit == MeasurementUnit::Dimensionless; }
bool near(double a, double b)
{ return finite(a) && finite(b) && std::abs(a - b) <= 1e-10 * std::max({1.0, std::abs(a), std::abs(b)}); }
double determinant(const std::array<double, 9>& m)
{ return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]); }
bool depth_valid(PixelDepth depth)
{
    switch (depth) {
    case PixelDepth::UInt8: case PixelDepth::UInt16: case PixelDepth::Float32: case PixelDepth::Float64: return true;
    }
    return false;
}
bool boundary_valid(RegionBoundaryPolicy policy)
{ return policy == RegionBoundaryPolicy::Reject || policy == RegionBoundaryPolicy::Intersect || policy == RegionBoundaryPolicy::Fill; }
bool shape_kind_valid(RegionShapeKind kind)
{
    switch (kind) {
    case RegionShapeKind::FullImage: case RegionShapeKind::Empty: case RegionShapeKind::AxisAlignedRect:
    case RegionShapeKind::RotatedRect: case RegionShapeKind::Circle: case RegionShapeKind::Ring:
    case RegionShapeKind::SectorRing: case RegionShapeKind::Polygon: case RegionShapeKind::RasterMask: return true;
    }
    return false;
}
template <class T> bool contains(const std::vector<T>& values, T value)
{ return std::find(values.begin(), values.end(), value) != values.end(); }
template <class T> bool duplicate(const std::vector<T>& values)
{ return std::set<T>(values.begin(), values.end()).size() != values.size(); }

Check mask_rule(PixelFormat format, const MaskValueRule& rule)
{
    if (format.order != ChannelOrder::Mono || !depth_valid(format.depth)) {
        return error(ErrorCode::InvalidMask, "Masks require an explicitly supported single-channel pixel format.");
    }
    if (!finite(rule.threshold)) { return error(ErrorCode::InvalidMask, "Mask threshold must be finite."); }
    if (rule.mode == MaskValueMode::NonZero) {
        if (rule.threshold != 0) { return error(ErrorCode::InvalidMask, "Nonzero masks cannot also declare a threshold."); }
    } else if (rule.mode == MaskValueMode::AtLeastThreshold) {
        if ((format.depth == PixelDepth::UInt8 && (rule.threshold < 0 || rule.threshold > 255))
            || (format.depth == PixelDepth::UInt16 && (rule.threshold < 0 || rule.threshold > 65535))
            || (format.depth == PixelDepth::Float32 && std::abs(rule.threshold) > std::numeric_limits<float>::max())) {
            return error(ErrorCode::InvalidMask, "Mask threshold is outside its pixel sample domain.");
        }
    } else { return error(ErrorCode::InvalidMask, "Unknown mask validity rule."); }
    return std::monostate{};
}

Check mask_mapping(const CoordinateSystemId& source, std::size_t width, std::size_t height,
    const MaskMapping& mapping)
{
    if (width == 0 || height == 0 || mapping.target_width == 0 || mapping.target_height == 0) {
        return error(ErrorCode::InvalidMask, "Mask and target dimensions must be nonzero.");
    }
    if (mapping.sampling != MaskSampling::None && mapping.sampling != MaskSampling::Nearest && mapping.sampling != MaskSampling::Linear) {
        return error(ErrorCode::InvalidMask, "Unknown mask resampling policy.");
    }
    if (!mapping.transform) {
        if (source != mapping.target_frame || width != mapping.target_width || height != mapping.target_height
            || mapping.sampling != MaskSampling::None) {
            return error(ErrorCode::CoordinateMismatch, "Direct masks require identical coordinates/dimensions and no resampling.");
        }
    } else {
        if (auto checked = validate_transform(*mapping.transform); failed(checked)) { return checked; }
        if (mapping.transform->from != source || mapping.transform->to != mapping.target_frame
            || mapping.transform->from_unit != MeasurementUnit::Pixel || mapping.transform->to_unit != MeasurementUnit::Pixel) {
            return error(ErrorCode::CoordinateMismatch, "Mask transform endpoints must match source/target pixel coordinates.");
        }
        if (mapping.sampling == MaskSampling::None) {
            return error(ErrorCode::InvalidMask, "Transformed masks require an explicit resampling policy.");
        }
    }
    return std::monostate{};
}

double cross(Vector2D a, Vector2D b, Vector2D c)
{ return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
bool on_segment(Vector2D a, Vector2D b, Vector2D p)
{ return p.x >= std::min(a.x, b.x) && p.x <= std::max(a.x, b.x) && p.y >= std::min(a.y, b.y) && p.y <= std::max(a.y, b.y); }
bool segments_intersect(Vector2D a, Vector2D b, Vector2D c, Vector2D d)
{
    const auto c1 = cross(a, b, c), c2 = cross(a, b, d), c3 = cross(c, d, a), c4 = cross(c, d, b);
    if (!finite(c1) || !finite(c2) || !finite(c3) || !finite(c4)) { return true; }
    return ((c1 > 0) != (c2 > 0) && (c3 > 0) != (c4 > 0) && c1 != 0 && c2 != 0 && c3 != 0 && c4 != 0)
        || (c1 == 0 && on_segment(a, b, c)) || (c2 == 0 && on_segment(a, b, d))
        || (c3 == 0 && on_segment(c, d, a)) || (c4 == 0 && on_segment(c, d, b));
}

Check polygon(const PolygonRegion& polygon)
{
    const auto& vertices = polygon.vertices;
    if (vertices.size() < 3 || !std::all_of(vertices.begin(), vertices.end(), [](auto p) { return finite(p); })) {
        return error(ErrorCode::InvalidGeometry, "A polygon requires at least three finite vertices.");
    }
    double area = 0;
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto next = (i + 1) % vertices.size();
        if (vertices[i] == vertices[next]) { return error(ErrorCode::InvalidGeometry, "A polygon cannot repeat adjacent/closing vertices."); }
        area += vertices[i].x * vertices[next].y - vertices[next].x * vertices[i].y;
        for (std::size_t j = i + 1; j < vertices.size(); ++j) {
            const auto after = (j + 1) % vertices.size();
            if (j == next || after == i) { continue; }
            if (segments_intersect(vertices[i], vertices[next], vertices[j], vertices[after])) {
                return error(ErrorCode::InvalidGeometry, "Polygon edges must not self-intersect.");
            }
        }
    }
    if (!finite(area) || area == 0) { return error(ErrorCode::InvalidGeometry, "Polygon area must be finite and nonzero."); }
    return std::monostate{};
}

Check shape(const RegionShape& geometry)
{
    return std::visit([](const auto& item) -> Check {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, FullImageRegion> || std::same_as<T, EmptyRegionShape>) {
            return std::monostate{};
        } else if constexpr (std::same_as<T, AxisAlignedRect>) {
            if (!finite(item.x) || !finite(item.y) || !finite(item.width) || !finite(item.height)
                || item.width <= 0 || item.height <= 0 || !finite(item.x + item.width) || !finite(item.y + item.height)) {
                return error(ErrorCode::InvalidGeometry, "Rectangle origin/extents must be finite with positive size.");
            }
            return std::monostate{};
        } else if constexpr (std::same_as<T, RotatedRect>) {
            if (!finite(item.center_x) || !finite(item.center_y) || !finite(item.width) || !finite(item.height)
                || !finite(item.angle_radians) || item.width <= 0 || item.height <= 0) {
                return error(ErrorCode::InvalidGeometry, "Rotated rectangle requires finite center, angle and positive dimensions.");
            }
            return std::monostate{};
        } else if constexpr (std::same_as<T, CircleRegion>) {
            if (!finite(item.center_x) || !finite(item.center_y) || !finite(item.radius) || item.radius <= 0) {
                return error(ErrorCode::InvalidGeometry, "Circle requires a finite center and positive finite radius.");
            }
            return std::monostate{};
        } else if constexpr (std::same_as<T, RingRegion> || std::same_as<T, SectorRingRegion>) {
            if (!finite(item.center_x) || !finite(item.center_y) || !finite(item.inner_radius) || !finite(item.outer_radius)
                || item.inner_radius < 0 || item.outer_radius <= item.inner_radius) {
                return error(ErrorCode::InvalidGeometry, "Ring radii must be finite with 0 <= inner < outer.");
            }
            if constexpr (std::same_as<T, SectorRingRegion>) {
                if (!finite(item.start_radians) || !finite(item.sweep_radians) || item.sweep_radians == 0
                    || std::abs(item.sweep_radians) > 2 * std::numbers::pi) {
                    return error(ErrorCode::InvalidGeometry, "Sector sweep must be finite, nonzero, and no greater than one turn.");
                }
            }
            return std::monostate{};
        } else if constexpr (std::same_as<T, PolygonRegion>) {
            return polygon(item);
        } else {
            static_assert(std::same_as<T, MaskConfig>);
            return validate_mask_config(item);
        }
    }, geometry);
}

template <class Source>
Check source_frame(const Source& source, const RegionDefinition& region)
{
    if (!spatial(source.unit) || !finite(source.local_direction_radians)) {
        return error(ErrorCode::InvalidGeometry, "Region source requires spatial units and a finite local direction in radians.");
    }
    if (source.transform) {
        if (auto checked = validate_transform(*source.transform); failed(checked)) { return checked; }
        if (source.transform->from != source.coordinate_frame || source.transform->from_unit != source.unit
            || source.transform->to != region.coordinate_frame || source.transform->to_unit != region.unit) {
            return error(ErrorCode::CoordinateMismatch, "Region correction must connect source coordinates/units to the region coordinates/units.");
        }
    } else if (source.coordinate_frame != region.coordinate_frame || source.unit != region.unit) {
        return error(ErrorCode::CoordinateMismatch, "Different region coordinate systems/units require an explicit correction.");
    }
    return std::monostate{};
}

Check source_check(const RegionSource& source, const RegionDefinition& region)
{
    return std::visit([&](const auto& item) -> Check {
        if (auto checked = source_frame(item, region); failed(checked)) { return checked; }
        if constexpr (std::same_as<std::decay_t<decltype(item)>, RegionPrimitive>) {
            if (auto checked = shape(item.geometry); failed(checked)) { return checked; }
            if (std::holds_alternative<FullImageRegion>(item.geometry) && item.unit != MeasurementUnit::Pixel) {
                return error(ErrorCode::CoordinateMismatch, "Full-image regions are defined in pixel coordinates before correction.");
            }
            if (const auto* mask = std::get_if<MaskConfig>(&item.geometry)) {
                if (item.coordinate_frame != mask->mapping.target_frame || item.unit != MeasurementUnit::Pixel) {
                    return error(ErrorCode::CoordinateMismatch, "A mask region uses its explicitly mapped target pixel coordinates.");
                }
            }
        }
        return std::monostate{};
    }, source);
}

RegionShapeKind kind(const RegionShape& geometry)
{ return static_cast<RegionShapeKind>(geometry.index()); }

Check declared_frame(const CoordinateSystemId& id, MeasurementUnit unit, std::span<const CoordinateFrameDefinition> frames)
{
    const auto found = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) { return frame.id == id; });
    if (found == frames.end() || found->unit != unit) {
        return error(ErrorCode::CoordinateMismatch, "Coordinate reference is absent or declares a different unit.");
    }
    return std::monostate{};
}

} // namespace

Check validate_coordinate_frame(const CoordinateFrameDefinition& frame)
{
    if (!spatial(frame.unit) || !finite(frame.origin) || !finite(frame.x_axis) || !finite(frame.y_axis)) {
        return error(ErrorCode::InvalidCoordinate, "Coordinate frames require finite origins/axes and spatial units.");
    }
    const double xnorm = frame.x_axis.x * frame.x_axis.x + frame.x_axis.y * frame.x_axis.y;
    const double ynorm = frame.y_axis.x * frame.y_axis.x + frame.y_axis.y * frame.y_axis.y;
    const double dot = frame.x_axis.x * frame.y_axis.x + frame.x_axis.y * frame.y_axis.y;
    const double det = frame.x_axis.x * frame.y_axis.y - frame.x_axis.y * frame.y_axis.x;
    if (!near(xnorm, 1) || !near(ynorm, 1) || !near(dot, 0)) {
        return error(ErrorCode::InvalidCoordinate, "Coordinate axes must be orthonormal; scale belongs in units/transforms.");
    }
    if ((frame.handedness != Handedness::LeftHanded && frame.handedness != Handedness::RightHanded)
        || (frame.handedness == Handedness::RightHanded ? det <= 0 : det >= 0)) {
        return error(ErrorCode::InvalidCoordinate, "Coordinate handedness must agree with the declared axes.");
    }
    switch (frame.kind) {
    case CoordinateKind::Image:
        if (frame.unit != MeasurementUnit::Pixel || frame.origin != Vector2D{0, 0}
            || frame.x_axis != Vector2D{1, 0} || frame.y_axis != Vector2D{0, -1}
            || frame.handedness != Handedness::LeftHanded) {
            return error(ErrorCode::InvalidCoordinate, "Image frame origin is the top-left pixel center, x right and y down.");
        }
        break;
    case CoordinateKind::Physical:
        if (frame.unit != MeasurementUnit::Millimetre) { return error(ErrorCode::InvalidCoordinate, "Physical frames require declared physical length units."); }
        break;
    case CoordinateKind::Local: break;
    default: return error(ErrorCode::InvalidCoordinate, "Unknown coordinate kind.");
    }
    return std::monostate{};
}

Check validate_point(const Point2D& point)
{
    if (!spatial(point.unit) || !finite(point.x) || !finite(point.y)) {
        return error(ErrorCode::InvalidGeometry, "Points require finite coordinates and explicit spatial units.");
    }
    return std::monostate{};
}
Check validate_line(const Line2D& line)
{
    if (auto checked = validate_point(line.start); failed(checked)) { return checked; }
    if (auto checked = validate_point(line.end); failed(checked)) { return checked; }
    if (line.start.coordinate_frame != line.end.coordinate_frame || line.start.unit != line.end.unit) {
        return error(ErrorCode::CoordinateMismatch, "Line endpoints must use the same coordinate system and units.");
    }
    if (line.start.x == line.end.x && line.start.y == line.end.y) { return error(ErrorCode::InvalidGeometry, "Line endpoints must be distinct."); }
    return std::monostate{};
}
Check validate_circle(const Circle2D& circle)
{
    if (auto checked = validate_point(circle.center); failed(checked)) { return checked; }
    if (!finite(circle.radius) || circle.radius <= 0) { return error(ErrorCode::InvalidGeometry, "Circle radius must be finite and positive."); }
    return std::monostate{};
}
Check validate_pose(const Pose2D& pose)
{
    if (auto checked = validate_point(pose.position); failed(checked)) { return checked; }
    if (!finite(pose.angle_radians)) { return error(ErrorCode::InvalidGeometry, "Pose orientation must be a finite angle in radians."); }
    return std::monostate{};
}

Check validate_transform(const Transform2D& transform)
{
    if (!spatial(transform.from_unit) || !spatial(transform.to_unit)
        || (transform.from == transform.to && transform.from_unit != transform.to_unit)) {
        return error(ErrorCode::InvalidTransform, "Transform endpoints require consistent explicit spatial units.");
    }
    if (!std::all_of(transform.matrix.begin(), transform.matrix.end(), [](double x) { return finite(x); })) {
        return error(ErrorCode::InvalidTransform, "Transform matrix coefficients must be finite.");
    }
    if (transform.valid_domain) {
        const auto& d = *transform.valid_domain;
        if (!finite(d.minimum_x) || !finite(d.minimum_y) || !finite(d.maximum_x) || !finite(d.maximum_y)
            || d.minimum_x >= d.maximum_x || d.minimum_y >= d.maximum_y) {
            return error(ErrorCode::InvalidTransform, "Transform domain must have finite, ordered bounds in source coordinates.");
        }
    }
    if (transform.model == TransformModel::Calibrated) {
        if (!transform.calibration || transform.calibration->kind != ResourceKind::Calibration
            || !transform.calibration->revision || !transform.valid_domain
            || transform.matrix != std::array<double, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1}) {
            return error(ErrorCode::InvalidTransform, "Calibrated mappings require a pinned calibration reference, valid domain, and no competing matrix mapping.");
        }
        return validate_resource_reference(*transform.calibration);
    }
    if (transform.calibration) { return error(ErrorCode::InvalidTransform, "Matrix transforms cannot also declare a calibration mapping."); }
    const auto& m = transform.matrix;
    const auto det = determinant(m);
    if (!finite(det) || det == 0) { return error(ErrorCode::InvalidTransform, "Transform matrix must have a finite nonzero determinant."); }
    switch (transform.model) {
    case TransformModel::Similarity:
    case TransformModel::Affine:
        if (m[6] != 0 || m[7] != 0 || m[8] != 1) { return error(ErrorCode::InvalidTransform, "Affine/similarity matrices require a normalized affine last row."); }
        if (transform.model == TransformModel::Similarity) {
            const auto a = std::hypot(m[0], m[3]);
            const auto b = std::hypot(m[1], m[4]);
            if (!finite(a) || !finite(b) || a <= 0 || b <= 0
                || std::abs(a - b) > 1e-10 * std::max(a, b)
                || !near((m[0] / a) * (m[1] / b) + (m[3] / a) * (m[4] / b), 0)) {
                return error(ErrorCode::InvalidTransform, "Similarity linear axes must be orthogonal and equally scaled.");
            }
        }
        break;
    case TransformModel::Projective: break;
    default: return error(ErrorCode::InvalidTransform, "Unknown transform model.");
    }
    return std::monostate{};
}

Check validate_transform_chain(std::span<const Transform2D> transforms)
{
    for (std::size_t index = 0; index < transforms.size(); ++index) {
        if (auto checked = validate_transform(transforms[index]); failed(checked)) { return checked; }
        if (index != 0 && (transforms[index - 1].to != transforms[index].from
            || transforms[index - 1].to_unit != transforms[index].from_unit)) {
            return error(ErrorCode::CoordinateMismatch, "Transform composition requires matching intermediate frames and units.");
        }
    }
    return std::monostate{};
}

Result<Point2D> map_point(const Transform2D& transform, const Point2D& point)
{
    if (auto checked = validate_transform(transform); const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    if (auto checked = validate_point(point); const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    if (transform.from != point.coordinate_frame || transform.from_unit != point.unit) {
        return error(ErrorCode::CoordinateMismatch, "Point does not belong to the transform's source frame/units.");
    }
    if (transform.model == TransformModel::Calibrated) {
        return error(ErrorCode::UnsupportedConversion, "Calibrated mapping execution requires its explicit backend; it is not an identity matrix.");
    }
    if (transform.valid_domain) {
        const auto& d = *transform.valid_domain;
        if (point.x < d.minimum_x || point.x > d.maximum_x || point.y < d.minimum_y || point.y > d.maximum_y) {
            return error(ErrorCode::InvalidTransform, "Point is outside the transform's valid source domain.");
        }
    }
    const auto& m = transform.matrix;
    const double w = m[6] * point.x + m[7] * point.y + m[8];
    if (!finite(w) || w == 0) { return error(ErrorCode::InvalidTransform, "Projected point has an invalid homogeneous denominator."); }
    const double x = (m[0] * point.x + m[1] * point.y + m[2]) / w;
    const double y = (m[3] * point.x + m[4] * point.y + m[5]) / w;
    if (!finite(x) || !finite(y)) { return error(ErrorCode::InvalidTransform, "Transformed coordinates are not finite."); }
    return Point2D{transform.to, transform.to_unit, x, y};
}

Result<Transform2D> inverse_transform(const Transform2D& transform)
{
    if (auto checked = validate_transform(transform); const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    if (transform.model == TransformModel::Calibrated || transform.valid_domain) {
        return error(ErrorCode::UnsupportedConversion, "Bounded/calibrated inverse mappings need an explicit inverse-domain/backend policy.");
    }
    const auto& m = transform.matrix;
    const double d = determinant(m);
    Transform2D inverse{transform.to, transform.from, transform.to_unit, transform.from_unit, transform.model};
    inverse.matrix = {(m[4]*m[8]-m[5]*m[7])/d, (m[2]*m[7]-m[1]*m[8])/d, (m[1]*m[5]-m[2]*m[4])/d,
        (m[5]*m[6]-m[3]*m[8])/d, (m[0]*m[8]-m[2]*m[6])/d, (m[2]*m[3]-m[0]*m[5])/d,
        (m[3]*m[7]-m[4]*m[6])/d, (m[1]*m[6]-m[0]*m[7])/d, (m[0]*m[4]-m[1]*m[3])/d};
    if (auto checked = validate_transform(inverse); const auto* invalid = std::get_if<Diagnostic>(&checked)) { return *invalid; }
    return inverse;
}

Check validate_mask_config(const MaskConfig& mask)
{
    if (auto checked = validate_resource_reference(mask.resource); failed(checked)) { return checked; }
    if (mask.resource.kind != ResourceKind::Mask) { return error(ErrorCode::InvalidMask, "Mask configuration requires an explicit mask resource."); }
    if (auto checked = mask_rule(mask.format, mask.valid_pixels); failed(checked)) { return checked; }
    const auto pixel_size = bytes_per_pixel(mask.format);
    const auto* bytes = std::get_if<std::size_t>(&pixel_size);
    if (!bytes || mask.width > std::numeric_limits<std::size_t>::max() / *bytes) {
        return error(ErrorCode::InvalidMask, "Mask layout size overflows the addressable domain.");
    }
    if (const auto required = required_image_bytes({mask.width, mask.height, mask.width * *bytes, mask.format});
        const auto* invalid = std::get_if<Diagnostic>(&required)) { return *invalid; }
    return mask_mapping(mask.mask_frame, mask.width, mask.height, mask.mapping);
}

Check validate_raster_mask(const RasterMask& mask)
{
    if (auto checked = mask_rule(mask.pixels.format(), mask.valid_pixels); failed(checked)) { return checked; }
    return mask_mapping(mask.pixels.coordinate_frame(), mask.pixels.width(), mask.pixels.height(), mask.mapping);
}
Check validate_raster_mask(const RasterMask& mask, const ImageHandle& target)
{
    if (auto checked = validate_raster_mask(mask); failed(checked)) { return checked; }
    if (target.coordinate_frame() != mask.mapping.target_frame || target.width() != mask.mapping.target_width
        || target.height() != mask.mapping.target_height) {
        return error(ErrorCode::CoordinateMismatch, "Mask mapping does not match this target image's coordinates and dimensions.");
    }
    return std::monostate{};
}

Check validate_region(const RegionDefinition& region)
{
    const auto contextualize = [&](Check checked) {
        if (auto* invalid = std::get_if<Diagnostic>(&checked)) { invalid->location.region = region.id; }
        return checked;
    };
    if (!spatial(region.unit) || !boundary_valid(region.boundary_policy)
        || (region.empty_policy != EmptyRegionPolicy::Error && region.empty_policy != EmptyRegionPolicy::ValidEmptyOutput)) {
        return contextualize(error(ErrorCode::InvalidGeometry, "Region requires known spatial units, boundary and empty policies."));
    }
    if ((region.boundary_policy == RegionBoundaryPolicy::Fill) != region.fill.has_value()) {
        return contextualize(error(ErrorCode::InvalidGeometry, "Only Fill boundaries require a fill specification."));
    }
    if (region.fill && (!finite(region.fill->value)
        || (region.fill->mode != RegionFillMode::Constant && region.fill->mode != RegionFillMode::Replicate && region.fill->mode != RegionFillMode::Reflect)
        || (region.fill->mode != RegionFillMode::Constant && region.fill->value != 0))) {
        return contextualize(error(ErrorCode::InvalidGeometry, "Fill mode/value must be finite and unambiguous."));
    }
    if (auto checked = source_check(region.source, region); failed(checked)) { return contextualize(std::move(checked)); }
    if (const auto* local = std::get_if<RegionPrimitive>(&region.source)) {
        if (std::holds_alternative<EmptyRegionShape>(local->geometry) && region.empty_policy == EmptyRegionPolicy::Error) {
            return contextualize(error(ErrorCode::EmptyRegion, "Explicit empty region is rejected by the declared policy."));
        }
    }
    for (const auto& exclusion : region.exclusions) {
        if (auto checked = source_check(exclusion, region); failed(checked)) { return contextualize(std::move(checked)); }
    }
    return std::monostate{};
}

Check validate_region_capabilities(const RegionCapabilities& capabilities)
{
    if (capabilities.shapes.empty() || capabilities.boundary_policies.empty() || duplicate(capabilities.shapes)
        || duplicate(capabilities.boundary_policies) || duplicate(capabilities.mask_depths)
        || !std::all_of(capabilities.shapes.begin(), capabilities.shapes.end(), shape_kind_valid)
        || !std::all_of(capabilities.boundary_policies.begin(), capabilities.boundary_policies.end(), boundary_valid)
        || !std::all_of(capabilities.mask_depths.begin(), capabilities.mask_depths.end(), depth_valid)) {
        return error(ErrorCode::UnsupportedRegion, "Region capabilities require nonempty, unique, recognized shapes and boundaries; mask depths must be known and unique.");
    }
    if (contains(capabilities.shapes, RegionShapeKind::RasterMask) && capabilities.mask_depths.empty()) {
        return error(ErrorCode::UnsupportedRegion, "Mask-capable tools must declare their accepted pixel depths.");
    }
    if (capabilities.geometry_mask_policy != GeometryMaskPolicy::Exclusive
        && capabilities.geometry_mask_policy != GeometryMaskPolicy::Combine && capabilities.geometry_mask_policy != GeometryMaskPolicy::RasterizeGeometry) {
        return error(ErrorCode::UnsupportedRegion, "Unknown geometry/mask composition capability.");
    }
    return std::monostate{};
}

Check validate_region(const RegionDefinition& region, const RegionCapabilities& capabilities)
{
    if (auto checked = validate_region(region); failed(checked)) { return checked; }
    if (auto checked = validate_region_capabilities(capabilities); failed(checked)) { return checked; }
    const auto reject = [&](std::string message) -> Check {
        auto invalid = error(ErrorCode::UnsupportedRegion, std::move(message)); invalid.location.region = region.id; return invalid;
    };
    if (!contains(capabilities.boundary_policies, region.boundary_policy)
        || (region.empty_policy == EmptyRegionPolicy::ValidEmptyOutput && !capabilities.valid_empty_output)) {
        return reject("Tool does not support the declared boundary/empty-region policy.");
    }
    bool masks = false, geometric = false;
    const auto source_capability = [&](const RegionSource& source) -> Check {
        return std::visit([&](const auto& item) -> Check {
            if (item.transform && !capabilities.transforms) { return reject("Tool does not support corrected regions."); }
            if constexpr (std::same_as<std::decay_t<decltype(item)>, InheritedRegion>) {
                if (!capabilities.inheritance) { return reject("Tool does not support inherited regions."); }
            } else {
                if (!contains(capabilities.shapes, kind(item.geometry))) { return reject("Tool does not support this shape; it cannot be replaced by a bounding box."); }
                if (const auto* mask = std::get_if<MaskConfig>(&item.geometry)) {
                    masks = true;
                    if (!contains(capabilities.mask_depths, mask->format.depth)) { return reject("Tool does not support the mask's declared depth."); }
                } else if (!std::holds_alternative<EmptyRegionShape>(item.geometry)) { geometric = true; }
            }
            return std::monostate{};
        }, source);
    };
    if (auto checked = source_capability(region.source); failed(checked)) { return checked; }
    for (const auto& exclusion : region.exclusions) {
        if (auto checked = source_capability(exclusion); failed(checked)) { return checked; }
    }
    if (masks && geometric && capabilities.geometry_mask_policy == GeometryMaskPolicy::Exclusive) {
        return reject("Tool requires exclusive geometry or mask input; explicit composition/rasterization support is absent.");
    }
    return std::monostate{};
}

Check validate_region_frames(const RegionDefinition& region, std::span<const CoordinateFrameDefinition> frames)
{
    if (auto checked = validate_region(region); failed(checked)) { return checked; }
    std::set<CoordinateSystemId> ids;
    for (const auto& frame : frames) {
        if (auto checked = validate_coordinate_frame(frame); failed(checked)) { return checked; }
        if (!ids.insert(frame.id).second) { return error(ErrorCode::InvalidCoordinate, "Coordinate catalogue repeats an identity."); }
    }
    if (auto checked = declared_frame(region.coordinate_frame, region.unit, frames); failed(checked)) { return checked; }
    const auto inspect = [&](const RegionSource& source) -> Check {
        return std::visit([&](const auto& item) -> Check {
            if (auto checked = declared_frame(item.coordinate_frame, item.unit, frames); failed(checked)) { return checked; }
            if constexpr (std::same_as<std::decay_t<decltype(item)>, RegionPrimitive>) {
                if (const auto* mask = std::get_if<MaskConfig>(&item.geometry)) {
                    if (auto checked = declared_frame(mask->mask_frame, MeasurementUnit::Pixel, frames); failed(checked)) { return checked; }
                    if (auto checked = declared_frame(mask->mapping.target_frame, MeasurementUnit::Pixel, frames); failed(checked)) { return checked; }
                }
            }
            return std::monostate{};
        }, source);
    };
    if (auto checked = inspect(region.source); failed(checked)) { return checked; }
    for (const auto& exclusion : region.exclusions) {
        if (auto checked = inspect(exclusion); failed(checked)) { return checked; }
    }
    return std::monostate{};
}

} // namespace visionpro::core
