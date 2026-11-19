#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
namespace vision = visionpro::tools::vision;

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
Diagnostic rejects(const Result<T>& result, ErrorCode expected)
{
    const auto* diagnostic = std::get_if<Diagnostic>(&result);
    check(diagnostic && diagnostic->code == expected, "Expected validation to report " + std::string(error_code_name(expected)));
    return *diagnostic;
}

template<class Range>
const auto& by_id(const Range& items, std::string_view id)
{
    const auto found = std::ranges::find(items, id, [](const auto& item) { return item.id.text(); });
    check(found != std::ranges::end(items), "Missing Schema member " + std::string(id));
    return *found;
}

void schemas()
{
    const auto tools = vision::tool_descriptors();
    for (const auto name : {"visionpro.calibration.fit", "visionpro.geometry.map_point", "visionpro.measure.distance"}) {
        by_id(tools, name);
    }
    const auto registry = take(ToolRegistry::create(vision::registrations()));
    for (const auto& tool : tools) {
        take(validate_descriptor(tool));
        check(tool.config_version == 1 && tool.schema.version == 1 && tool.schema.id.text() == tool.id.text(), "Tool Schema must be version 1.");
        take(registry.validate(tool));
    }

    const auto& reader = by_id(tools, "visionpro.image.read_file");
    const auto& decode = by_id(reader.parameters, "decode_mode");
    check(decode.type.kind() == TypeKind::Enum && decode.type.schema_version() == 1 && decode.type.enum_members().size() == 2,
        "Decode mode is a versioned Gray8/Native8 enumeration.");
    by_id(decode.type.enum_members(), "Gray8");
    by_id(decode.type.enum_members(), "Native8");
    const auto& origin = by_id(reader.parameters, "frame_origin");
    check(origin.type.kind() == TypeKind::Point2D && origin.semantics.unit == MeasurementUnit::Pixel
        && origin.binding.time == BindingTime::SnapshotBuild && !origin.subscribable, "The reader uses a fixed pixel origin.");
    check(reader.resources.size() == 1 && reader.resources.front().slot.text() == "source"
        && reader.resources.front().kind == ResourceKind::Image && reader.resources.front().required,
        "The reader requires the typed source slot.");
    check(by_id(reader.outputs, "image").type.kind() == TypeKind::Image, "The reader publishes an image.");

    const auto& threshold = by_id(tools, "visionpro.image.threshold");
    const auto& cutoff = by_id(threshold.parameters, "threshold");
    for (const auto value : {0, 255}) { take(validate_parameter_value(cutoff, ConfigValue::integer(value))); }
    for (const auto value : {-1, 256}) { rejects(validate_parameter_value(cutoff, ConfigValue::integer(value)), ErrorCode::ConstraintViolation); }
    const auto& mode = by_id(threshold.parameters, "mode").type;
    check(mode.kind() == TypeKind::Enum && mode.schema_version() == 1 && mode.enum_members().size() == 2,
        "Threshold mode is a versioned Binary/BinaryInverse enumeration.");
    by_id(mode.enum_members(), "Binary");
    by_id(mode.enum_members(), "BinaryInverse");
    const auto& required_roi = by_id(threshold.parameters, "roi");
    check(required_roi.required && !required_roi.default_value && required_roi.type == DataType::region2d(), "Threshold requires a Region2D.");
    const auto& effective_roi = by_id(threshold.outputs, "effective_roi");
    check(effective_roi.required && effective_roi.type == DataType::region2d()
        && by_id(threshold.outputs, "roi_mask").type == DataType::raster_mask(), "Threshold publishes a required Region2D and RasterMask.");

    const auto& blob = by_id(tools, "visionpro.image.blob");
    const auto& optional_roi = by_id(blob.parameters, "roi");
    check(take(validate_parameter_value(optional_roi, {})) == ConfigValue::none(DataType::region2d()), "Blob defaults to a typed None region.");
    take(validate_binding(optional_roi, BindingSourceKind::RunOutput, effective_roi.type, ConversionKind::WrapOptional));
    const auto& connectivity = by_id(blob.parameters, "connectivity");
    for (const auto value : {4, 8}) { take(validate_parameter_value(connectivity, ConfigValue::integer(value))); }
    rejects(validate_parameter_value(connectivity, ConfigValue::integer(5)), ErrorCode::ConstraintViolation);
    check(by_id(blob.outputs, "largest_area_pixels").type == DataType::optional(DataType::integer()), "An empty Blob result still produces a typed Optional.");
    const auto& components = by_id(blob.outputs, "components").type;
    check(components.kind() == TypeKind::List && components.element_type()->kind() == TypeKind::Record, "Blob components are a typed record list.");
    const auto& component = *components.element_type();
    const auto& bounds = by_id(component.fields(), "bounds").type;
    check(component.schema_version() == 1 && bounds.schema_version() == 1
        && by_id(component.fields(), "centroid").type == DataType::point2d() && by_id(bounds.fields(), "origin").type == DataType::point2d()
        && by_id(bounds.fields(), "width").type == DataType::integer() && by_id(bounds.fields(), "height").type == DataType::integer(),
        "Blob geometry uses versioned component and bounds records with typed coordinates.");

    const auto& judge = by_id(tools, "visionpro.judge.integer_range");
    const auto& input = by_id(judge.inputs, "value");
    const auto& area = by_id(component.fields(), "area_pixels");
    check(area.type == DataType::integer() && by_id(judge.outputs, "passed").type == DataType::boolean(), "Area is Int64 and judgement output is Bool.");
    take(validate_binding(input, BindingSourceKind::RunOutput, area.type, ConversionKind::Exact));

    for (auto* tool : {&threshold, &blob}) {
        check(tool->image_formats == std::vector{PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}}, "Processing tools accept Mono8 only.");
        check(tool->resources.size() == 1 && tool->resources.front().slot.text() == "mask"
            && tool->resources.front().kind == ResourceKind::Mask && !tool->resources.front().required, "The mask slot is optional and typed.");
    }
}

template<UuidIdentity T>
T id() { return take(IdFactory::parse<T>("10000000-0000-4000-8000-000000000001")); }

void adapter_limits()
{
    const auto frame = take(SchemaIdFactory::parse<CoordinateSystemId>("camera"));
    const auto pixels = MeasurementUnit::Pixel;
    take(vision::validate_file_frame_origin({frame, pixels, 0, 0}));
    const auto invalid_origin = rejects(vision::validate_file_frame_origin({frame, pixels, 1, 0}), ErrorCode::ConstraintViolation);
    check(invalid_origin.location.parameter && invalid_origin.location.parameter->text() == "frame_origin", "File origin failure locates the parameter.");
    rejects(vision::validate_file_frame_origin({frame, MeasurementUnit::Millimetre, 0, 0}), ErrorCode::CoordinateMismatch);

    const Transform2D correction{frame, frame, pixels, pixels};
    const RegionDefinition rectangle{id<RegionId>(), frame, pixels, RegionPrimitive{frame, pixels, RotatedRect{2, 2, 3, 2, 0.5}, 0, correction}, {}};
    const auto unsupported = [&](const RegionDefinition& region) {
        const auto failure = rejects(vision::validate_region_support(region), ErrorCode::UnsupportedRegion);
        check(failure.location.region == region.id, "Unsupported geometry locates its region.");
    };
    take(vision::validate_region_support(rectangle));
    auto changed = rectangle;
    auto& source = std::get<RegionPrimitive>(changed.source);
    source.transform->model = TransformModel::Projective;
    unsupported(changed);
    source.transform = correction;
    source.transform->valid_domain = TransformDomain{0, 0, 3, 3};
    unsupported(changed);
    source.transform = correction;
    source.geometry = CircleRegion{2, 2, 1};
    take(vision::validate_region_support(changed));

    changed = rectangle;
    changed.boundary_policy = RegionBoundaryPolicy::Fill;
    changed.fill = RegionFill{RegionFillMode::Constant, 0};
    unsupported(changed);

    const InheritedRegion inherited{id<WorkflowId>(), id<NodeId>(), take(SchemaIdFactory::parse<OutputId>("effective_roi")), frame, pixels};
    auto inherited_region = rectangle;
    inherited_region.source = inherited;
    take(vision::validate_region_support(inherited_region));
    inherited_region.exclusions.push_back(inherited);
    unsupported(inherited_region);

    const ResourceReference resource{id<ResourceId>(), ResourceKind::Mask, {}};
    MaskConfig mask{resource, frame, 4, 4, {PixelDepth::UInt8, ChannelOrder::Mono}, {frame, 4, 4, correction, MaskSampling::Nearest}, {}};
    changed = rectangle;
    changed.exclusions.emplace_back(RegionPrimitive{frame, pixels, mask});
    take(vision::validate_region_support(changed));
    auto& mask_region = std::get<MaskConfig>(std::get<RegionPrimitive>(changed.exclusions.front()).geometry);
    mask_region.mapping.sampling = MaskSampling::Linear;
    unsupported(changed);
    mask_region.mapping.sampling = MaskSampling::Nearest;
    mask_region.format.depth = PixelDepth::UInt16;
    unsupported(changed);
}
} // namespace

int main()
{
    try {
        schemas();
        adapter_limits();
        std::cout << "Vision Schemas and symbolic adapter limits passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
