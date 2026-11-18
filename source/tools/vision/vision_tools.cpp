#include "vision_tools.hpp"
#include "blob.hpp"
#include "integer_range.hpp"
#include "read_file.hpp"
#include "threshold.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <array>

namespace visionpro::tools::vision {
namespace {
using namespace core;
using Check = Result<std::monostate>;

constexpr std::uint32_t schema_version = 1;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr ValueSemantics pixels{MeasurementUnit::Pixel, {}};

template<SchemaIdentity Id>
Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

ToolDescriptor descriptor(std::string_view id, std::string name)
{
    return {key<ToolTypeId>(id), std::move(name), 1, {key<SchemaId>(id), schema_version, {}},
        {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

ParameterDescriptor fixed_parameter(std::string_view id, DataType type, ValueSemantics semantics = {})
{
    ParameterDescriptor result{key<ParameterId>(id), std::string(id), std::move(type), true, {}, {}, {}, semantics};
    result.binding.time = BindingTime::SnapshotBuild;
    return result;
}

ParameterDescriptor subscribable_parameter(std::string_view id, DataType type, ValueSemantics semantics = {})
{
    ParameterDescriptor result{key<ParameterId>(id), std::string(id), std::move(type), true, {}, {}, {}, semantics};
    result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
    result.subscribable = true;
    return result;
}

InputDescriptor input(std::string_view id, DataType type, std::vector<BindingSourceKind> sources, ValueSemantics semantics = {})
{
    InputDescriptor result{key<InputId>(id), std::string(id), std::move(type), true, {}, {}, {}, semantics};
    result.binding.allowed_sources = std::move(sources);
    return result;
}

OutputDescriptor output(std::string_view id, DataType type, ValueSemantics semantics = {})
{ return {key<OutputId>(id), std::string(id), std::move(type), semantics}; }

const RegionCapabilities& region_capabilities()
{
    static const RegionCapabilities capabilities{
        {RegionShapeKind::FullImage, RegionShapeKind::Empty, RegionShapeKind::AxisAlignedRect,
            RegionShapeKind::RotatedRect, RegionShapeKind::RasterMask},
        {RegionBoundaryPolicy::Reject, RegionBoundaryPolicy::Intersect},
        GeometryMaskPolicy::Combine, true, true, true, {PixelDepth::UInt8}};
    return capabilities;
}

ToolDescriptor read_file_descriptor()
{
    auto tool = descriptor("visionpro.image.read_file", "Read image file");
    const auto mode = std::get<DataType>(DataType::enumeration(key<SchemaId>("visionpro.image.read_file.decode_mode"),
        schema_version, {{key<EnumMemberId>("Gray8"), "Gray8"}, {key<EnumMemberId>("Native8"), "Native8"}}));
    tool.parameters = {fixed_parameter("decode_mode", mode), fixed_parameter("frame_origin", DataType::point2d(), pixels)};
    tool.resources = {{key<SchemaId>("source"), ResourceKind::Image}};
    tool.outputs = {output("image", DataType::image(), pixels)};
    tool.image_formats = {mono8, {PixelDepth::UInt8, ChannelOrder::BGR}};
    return tool;
}

ToolDescriptor threshold_descriptor()
{
    auto tool = descriptor("visionpro.image.threshold", "Threshold image");
    const auto mode = std::get<DataType>(DataType::enumeration(key<SchemaId>("visionpro.image.threshold.mode"), schema_version,
        {{key<EnumMemberId>("Binary"), "Binary"}, {key<EnumMemberId>("BinaryInverse"), "Binary inverse"}}));
    auto threshold = subscribable_parameter("threshold", DataType::integer());
    threshold.constraints.integer_range = IntegerRange{0, 255};
    tool.parameters = {subscribable_parameter("roi", DataType::region2d(), pixels),
        std::move(threshold), fixed_parameter("mode", mode)};
    tool.inputs = {input("image", DataType::image(), {BindingSourceKind::RunOutput, BindingSourceKind::PublicInput}, pixels)};
    tool.outputs = {output("binary", DataType::image(), pixels), output("effective_roi", DataType::region2d(), pixels),
        output("roi_mask", DataType::raster_mask(), pixels)};
    tool.resources = {{key<SchemaId>("mask"), ResourceKind::Mask, false}};
    tool.region_capabilities = region_capabilities();
    tool.image_formats = {mono8};
    return tool;
}

} // namespace

core::DataType blob_component_type()
{
    const auto integer = DataType::integer();
    const auto point = DataType::point2d();
    const auto bounds = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.image.blob.bounds"), schema_version,
        {{key<FieldId>("origin"), point}, {key<FieldId>("width"), integer}, {key<FieldId>("height"), integer}}));
    return std::get<DataType>(DataType::record(key<SchemaId>("visionpro.image.blob.component"), schema_version,
        {{key<FieldId>("area_pixels"), integer}, {key<FieldId>("centroid"), point}, {key<FieldId>("bounds"), bounds}}));
}

namespace {
ToolDescriptor blob_descriptor()
{
    auto tool = descriptor("visionpro.image.blob", "Measure blobs");
    const auto integer = DataType::integer();
    const auto region = DataType::region2d();
    auto roi = subscribable_parameter("roi", DataType::optional(region), pixels);
    roi.required = false;
    roi.default_value = ConfigValue::none(region);
    auto connectivity = fixed_parameter("connectivity", integer);
    connectivity.constraints.allowed_values = {ConfigValue::integer(4), ConfigValue::integer(8)};
    tool.parameters = {std::move(connectivity), std::move(roi)};
    tool.inputs = {input("binary", DataType::image(), {BindingSourceKind::RunOutput, BindingSourceKind::PublicInput}, pixels)};
    tool.outputs = {output("count", integer), output("components", DataType::list(blob_component_type())),
        output("total_area_pixels", integer), output("largest_area_pixels", DataType::optional(integer))};
    tool.resources = {{key<SchemaId>("mask"), ResourceKind::Mask, false}};
    tool.region_capabilities = region_capabilities();
    tool.image_formats = {mono8};
    return tool;
}

ToolDescriptor integer_range_descriptor()
{
    auto tool = descriptor("visionpro.judge.integer_range", "Judge integer range");
    const auto integer = DataType::integer();
    tool.parameters = {fixed_parameter("minimum", integer), fixed_parameter("maximum", integer)};
    tool.inputs = {input("value", integer, {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput})};
    tool.outputs = {output("passed", DataType::boolean())};
    return tool;
}

bool supports_transform(const std::optional<Transform2D>& transform)
{
    return !transform || ((transform->model == TransformModel::Similarity || transform->model == TransformModel::Affine)
        && !transform->calibration && !transform->valid_domain);
}
} // namespace

std::span<const core::ToolDescriptor> tool_descriptors()
{
    static const std::array descriptors{read_file_descriptor(), threshold_descriptor(), blob_descriptor(), integer_range_descriptor()};
    return descriptors;
}

std::vector<core::ToolRegistration> registrations()
{
    return {read_file_registration(read_file_descriptor()), threshold_registration(threshold_descriptor()),
        blob_registration(blob_descriptor()), integer_range_registration(integer_range_descriptor())};
}

core::Result<std::monostate> validate_file_frame_origin(const core::Point2D& origin)
{
    DiagnosticLocation location;
    location.parameter = key<ParameterId>("frame_origin");
    if (auto checked = validate_point(origin); auto* failure = std::get_if<Diagnostic>(&checked)) {
        failure->location = location;
        return checked;
    }
    if (origin.unit != MeasurementUnit::Pixel) {
        return Diagnostic{ErrorCode::CoordinateMismatch, Severity::Error, location, "File image origin requires a Pixel coordinate frame."};
    }
    if (origin.x != 0 || origin.y != 0) {
        return Diagnostic{ErrorCode::ConstraintViolation, Severity::Error, location, "File image origin must be (0, 0)."};
    }
    return std::monostate{};
}

core::Result<std::monostate> validate_region_support(const core::RegionDefinition& region)
{
    if (auto checked = validate_region(region, region_capabilities()); std::holds_alternative<Diagnostic>(checked)) {
        return checked;
    }
    DiagnosticLocation location;
    location.region = region.id;
    const auto unsupported = [&](std::string message) -> Check {
        return Diagnostic{ErrorCode::UnsupportedRegion, Severity::Error, location, std::move(message)};
    };
    const auto validate_source = [&](const RegionSource& source) -> Check {
        const auto& correction = std::visit([](const auto& item) -> const auto& { return item.transform; }, source);
        if (!supports_transform(correction)) {
            return unsupported("ROI correction requires an unbounded, uncalibrated Similarity or Affine transform.");
        }
        const auto* primitive = std::get_if<RegionPrimitive>(&source);
        const auto* mask = primitive ? std::get_if<MaskConfig>(&primitive->geometry) : nullptr;
        if (mask && (mask->mapping.sampling == MaskSampling::Linear || !supports_transform(mask->mapping.transform))) {
            return unsupported("ROI masks require direct mapping or Nearest sampling with an unbounded, uncalibrated Similarity or Affine transform.");
        }
        return std::monostate{};
    };
    if (auto checked = validate_source(region.source); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    for (const auto& exclusion : region.exclusions) {
        if (std::holds_alternative<InheritedRegion>(exclusion)) {
            return unsupported("ROI exclusions require a local region primitive.");
        }
        if (auto checked = validate_source(exclusion); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    return std::monostate{};
}

} // namespace visionpro::tools::vision
