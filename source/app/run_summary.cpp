#include "run_summary.hpp"
#include <visionpro/core/class_map.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace visionpro::app {
namespace {
using namespace core;
using namespace workflow;
using nlohmann::json;

template<class... F> struct Overload : F... { using F::operator()...; };

template<class Range, class Convert> json array_json(const Range& values, Convert convert)
{
    auto result = json::array();
    for (const auto& value : values) { result.push_back(convert(value)); }
    return result;
}

template<class Id> std::string id_text(const Id& id)
{
    if constexpr (UuidIdentity<Id>) { return id.to_string(); }
    else if constexpr (RuntimeIdentity<Id>) { return std::to_string(id.value()); }
    else { return std::string(id.text()); }
}

template<class Id> json optional_id(const std::optional<Id>& id)
{ return id ? json(id_text(*id)) : json(nullptr); }

std::string_view name(ExecutionStatus status)
{
    switch (status) {
    case ExecutionStatus::Pending: return "Pending";
    case ExecutionStatus::Running: return "Running";
    case ExecutionStatus::Succeeded: return "Succeeded";
    case ExecutionStatus::Failed: return "Failed";
    case ExecutionStatus::Skipped: return "Skipped";
    case ExecutionStatus::Cancelled: return "Cancelled";
    case ExecutionStatus::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

std::string_view name(NodeSkipReason reason)
{
    switch (reason) {
    case NodeSkipReason::Disabled: return "Disabled";
    case NodeSkipReason::UpstreamClosed: return "UpstreamClosed";
    case NodeSkipReason::FailFast: return "FailFast";
    case NodeSkipReason::RunCancelled: return "RunCancelled";
    case NodeSkipReason::RunTimedOut: return "RunTimedOut";
    case NodeSkipReason::NotSelected: return "NotSelected";
    }
    return "Unknown";
}

std::string_view name(Judgement judgement)
{
    switch (judgement) {
    case Judgement::NotEvaluated: return "NotEvaluated";
    case Judgement::OK: return "OK";
    case Judgement::NG: return "NG";
    }
    return "Unknown";
}

std::string_view name(ControlLinkStatus status)
{
    switch (status) {
    case ControlLinkStatus::Pending: return "Pending";
    case ControlLinkStatus::Arrived: return "Arrived";
    case ControlLinkStatus::Closed: return "Closed";
    }
    return "Unknown";
}

std::string_view name(ControlCloseReason reason)
{
    switch (reason) {
    case ControlCloseReason::NotSelected: return "NotSelected";
    case ControlCloseReason::Disabled: return "Disabled";
    case ControlCloseReason::Failed: return "Failed";
    case ControlCloseReason::Cancelled: return "Cancelled";
    case ControlCloseReason::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

std::string_view name(MissingReason reason)
{
    switch (reason) {
    case MissingReason::NotSelected: return "NotSelected";
    case MissingReason::Disabled: return "Disabled";
    case MissingReason::ToolDidNotProduce: return "ToolDidNotProduce";
    case MissingReason::ConditionNotMet: return "ConditionNotMet";
    }
    return "Unknown";
}

std::string_view name(OutputAbsenceReason reason)
{
    switch (reason) {
    case OutputAbsenceReason::NotSelected: return "NotSelected";
    case OutputAbsenceReason::Disabled: return "Disabled";
    case OutputAbsenceReason::ToolDidNotProduce: return "ToolDidNotProduce";
    case OutputAbsenceReason::Failed: return "Failed";
    case OutputAbsenceReason::Cancelled: return "Cancelled";
    case OutputAbsenceReason::TimedOut: return "TimedOut";
    case OutputAbsenceReason::ConditionNotMet: return "ConditionNotMet";
    }
    return "Unknown";
}

std::string_view name(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Image: return "Image";
    case ResourceKind::Mask: return "Mask";
    case ResourceKind::Template: return "Template";
    case ResourceKind::Calibration: return "Calibration";
    case ResourceKind::Device: return "Device";
    case ResourceKind::DirectorySequence: return "DirectorySequence";
    case ResourceKind::OutputPath: return "OutputPath";
    case ResourceKind::Binary: return "Binary";
    }
    return "Unknown";
}

std::string_view name(PixelDepth depth)
{
    switch (depth) {
    case PixelDepth::UInt8: return "UInt8";
    case PixelDepth::UInt16: return "UInt16";
    case PixelDepth::Float32: return "Float32";
    case PixelDepth::Float64: return "Float64";
    }
    return "Unknown";
}

std::string_view name(ChannelOrder order)
{
    switch (order) {
    case ChannelOrder::Mono: return "Mono";
    case ChannelOrder::RGB: return "RGB";
    case ChannelOrder::BGR: return "BGR";
    case ChannelOrder::RGBA: return "RGBA";
    case ChannelOrder::BGRA: return "BGRA";
    }
    return "Unknown";
}

std::string_view name(MeasurementUnit unit)
{
    switch (unit) {
    case MeasurementUnit::Unspecified: return "Unspecified";
    case MeasurementUnit::Dimensionless: return "Dimensionless";
    case MeasurementUnit::Pixel: return "Pixel";
    case MeasurementUnit::Millimetre: return "Millimetre";
    case MeasurementUnit::Second: return "Second";
    case MeasurementUnit::Radian: return "Radian";
    case MeasurementUnit::Degree: return "Degree";
    }
    return "Unknown";
}

std::string_view name(TransformModel model)
{
    switch (model) {
    case TransformModel::Similarity: return "Similarity";
    case TransformModel::Affine: return "Affine";
    case TransformModel::Projective: return "Projective";
    case TransformModel::Calibrated: return "Calibrated";
    }
    return "Unknown";
}

std::string_view name(RegionBoundaryPolicy policy)
{
    switch (policy) {
    case RegionBoundaryPolicy::Reject: return "Reject";
    case RegionBoundaryPolicy::Intersect: return "Intersect";
    case RegionBoundaryPolicy::Fill: return "Fill";
    }
    return "Unknown";
}

std::string_view name(EmptyRegionPolicy policy)
{
    switch (policy) {
    case EmptyRegionPolicy::Error: return "Error";
    case EmptyRegionPolicy::ValidEmptyOutput: return "ValidEmptyOutput";
    }
    return "Unknown";
}

std::string_view name(RegionFillMode mode)
{
    switch (mode) {
    case RegionFillMode::Constant: return "Constant";
    case RegionFillMode::Replicate: return "Replicate";
    case RegionFillMode::Reflect: return "Reflect";
    }
    return "Unknown";
}

std::string_view name(MaskSampling sampling)
{
    switch (sampling) {
    case MaskSampling::None: return "None";
    case MaskSampling::Nearest: return "Nearest";
    case MaskSampling::Linear: return "Linear";
    }
    return "Unknown";
}

std::string_view name(MaskValueMode mode)
{
    switch (mode) {
    case MaskValueMode::NonZero: return "NonZero";
    case MaskValueMode::AtLeastThreshold: return "AtLeastThreshold";
    }
    return "Unknown";
}

std::string_view name(SystemField field)
{
    switch (field) {
    case SystemField::ExecutionStatus: return "ExecutionStatus";
    case SystemField::Judgement: return "Judgement";
    case SystemField::Diagnostics: return "Diagnostics";
    case SystemField::Elapsed: return "Elapsed";
    }
    return "Unknown";
}

std::string_view name(Severity severity)
{
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
    }
    return "Unknown";
}

template<class Enum> json optional_name(const std::optional<Enum>& value)
{ return value ? json(name(*value)) : json(nullptr); }

json diagnostic_path_json(std::span<const DiagnosticPathStep> path)
{
    return array_json(path, [](const DiagnosticPathStep& step) {
        return std::visit(Overload{
            [](const FieldId& field) { return json{{"field", id_text(field)}}; },
            [](DiagnosticElementIndex index) { return json{{"index", index.value}}; }}, step);
    });
}

json diagnostic_json(const Diagnostic& diagnostic)
{
    const auto& source = diagnostic.location;
    auto location = json::object();
    const auto locate = [&](std::string_view key, const auto& id) {
        if (id) { location[std::string(key)] = id_text(*id); }
    };
    locate("solution", source.solution);
    locate("workflow", source.workflow);
    locate("node", source.node);
    locate("tool", source.tool);
    locate("schema", source.schema);
    locate("input", source.input);
    locate("output", source.output);
    locate("parameter", source.parameter);
    locate("resource", source.resource);
    locate("variable", source.variable);
    locate("queue", source.queue);
    locate("region", source.region);
    locate("control_link", source.control_link);
    if (const auto& call = source.invocation) {
        location["call"] = {{"run", id_text(call->workflow_run)}, {"node", id_text(call->node)},
            {"scope", id_text(call->scope)}, {"invocation", id_text(call->invocation)}};
    }
    if (!source.path.empty()) { location["path"] = diagnostic_path_json(source.path); }
    json result{{"code", error_code_name(diagnostic.code)}, {"severity", name(diagnostic.severity)},
        {"message", diagnostic.message}, {"location", std::move(location)}};
    if (const auto& key = diagnostic.operation_key) {
        const auto& call = key->key;
        result["operation_key"] = {{"session", id_text(key->session)}, {"run", id_text(call.workflow_run)},
            {"node", id_text(call.node)}, {"scope", id_text(call.scope)}, {"invocation", id_text(call.invocation)}};
    }
    if (const auto& external = diagnostic.external) {
        result["external"] = {{"provider", id_text(external->provider)}, {"operation", external->operation},
            {"domain", static_cast<int>(external->domain)}, {"raw_code", std::to_string(external->raw_code)},
            {"mapped", external->mapping == ExternalCodeMapping::Mapped}, {"provider_domain", optional_id(external->provider_domain)},
            {"description", external->description ? json(*external->description) : json(nullptr)},
            {"backend_version", external->backend_version ? json(*external->backend_version) : json(nullptr)}};
    }
    return result;
}

void add_run_identity(json& object, const RunIdentity& run)
{
    object["session"] = id_text(run.snapshot.session);
    object["snapshot"] = id_text(run.snapshot.snapshot);
    object["workflow"] = id_text(run.workflow);
    object["run"] = id_text(run.run);
}

json selection_json(const FieldSelection& field) { return {{"field", id_text(field.field)}}; }
json selection_json(ElementSelection element) { return {{"index", element.index}}; }

json selection_json(const DynamicElementSelection& element)
{
    auto source = std::visit(Overload{
        [](const IndexOutputReference& ref) {
            const auto* port = std::get_if<OutputId>(&ref.output);
            return json{{"kind", "result"}, {"workflow", id_text(ref.workflow)}, {"node", id_text(ref.node)},
                {"output_kind", port ? "port" : "system"},
                {"output", port ? id_text(*port) : std::string(name(std::get<SystemField>(ref.output)))}};
        },
        [](const IndexInputReference& ref) {
            return json{{"kind", "public_input"}, {"workflow", id_text(ref.workflow)}, {"group", optional_id(ref.group)},
                {"input", id_text(ref.input)}};
        },
        [](const IndexGlobalReference& ref) {
            return json{{"kind", "global"}, {"variable", id_text(ref.variable)},
                {"required_revision", ref.required_revision ? json(std::to_string(*ref.required_revision)) : json(nullptr)}};
        }}, element.source);
    const auto& path = std::visit([](const auto& ref) -> const FixedSelector& { return ref.selector; }, element.source);
    source["selector"] = array_json(path, [](const FixedSelectionStep& step) {
        return std::visit([](const auto& part) { return selection_json(part); }, step);
    });
    return {{"index_source", std::move(source)}};
}

json selector_json(const Selector& selector)
{
    return array_json(selector, [](const SelectionStep& step) {
        return std::visit([](const auto& part) { return selection_json(part); }, step);
    });
}

json resource_json(const ResourceReference& reference)
{
    return {{"resource", id_text(reference.id)}, {"kind", name(reference.kind)},
        {"revision", reference.revision ? json(std::to_string(*reference.revision)) : json(nullptr)}};
}

json transform_json(const std::optional<Transform2D>& transform)
{
    if (!transform) { return nullptr; }
    const auto& value = *transform;
    json result{{"from", id_text(value.from)}, {"to", id_text(value.to)},
        {"from_unit", name(value.from_unit)}, {"to_unit", name(value.to_unit)}, {"model", name(value.model)}, {"matrix", value.matrix},
        {"calibration", value.calibration ? resource_json(*value.calibration) : json(nullptr)}, {"valid_domain", nullptr}};
    if (const auto& domain = value.valid_domain) {
        result["valid_domain"] = {{"minimum_x", domain->minimum_x}, {"maximum_x", domain->maximum_x},
            {"minimum_y", domain->minimum_y}, {"maximum_y", domain->maximum_y}};
    }
    return result;
}

json mask_mapping_json(const MaskMapping& mapping)
{
    return {{"target_frame", id_text(mapping.target_frame)}, {"target_width", mapping.target_width},
        {"target_height", mapping.target_height}, {"sampling", name(mapping.sampling)}, {"transform", transform_json(mapping.transform)}};
}

json mask_rule_json(const MaskValueRule& rule)
{ return {{"mode", name(rule.mode)}, {"threshold", rule.threshold}}; }

json shape_json(const RegionShape& shape)
{
    return std::visit(Overload{
        [](FullImageRegion) { return json{{"kind", "FullImage"}}; },
        [](EmptyRegionShape) { return json{{"kind", "Empty"}}; },
        [](const AxisAlignedRect& rectangle) {
            return json{{"kind", "AxisAlignedRect"}, {"x", rectangle.x}, {"y", rectangle.y},
                {"width", rectangle.width}, {"height", rectangle.height}};
        },
        [](const RotatedRect& rectangle) {
            return json{{"kind", "RotatedRect"}, {"center_x", rectangle.center_x}, {"center_y", rectangle.center_y},
                {"width", rectangle.width}, {"height", rectangle.height}, {"angle_radians", rectangle.angle_radians}};
        },
        [](const CircleRegion& circle) {
            return json{{"kind", "Circle"}, {"center_x", circle.center_x}, {"center_y", circle.center_y}, {"radius", circle.radius}};
        },
        [](const RingRegion& ring) {
            return json{{"kind", "Ring"}, {"center_x", ring.center_x}, {"center_y", ring.center_y},
                {"inner_radius", ring.inner_radius}, {"outer_radius", ring.outer_radius}};
        },
        [](const SectorRingRegion& sector) {
            return json{{"kind", "SectorRing"}, {"center_x", sector.center_x}, {"center_y", sector.center_y},
                {"inner_radius", sector.inner_radius}, {"outer_radius", sector.outer_radius},
                {"start_radians", sector.start_radians}, {"sweep_radians", sector.sweep_radians}};
        },
        [](const PolygonRegion& polygon) {
            return json{{"kind", "Polygon"}, {"vertices", array_json(polygon.vertices, [](const Vector2D& point) {
                return json{{"x", point.x}, {"y", point.y}};
            })}};
        },
        [](const MaskConfig& mask) {
            return json{{"kind", "RasterMask"}, {"resource", resource_json(mask.resource)}, {"mask_frame", id_text(mask.mask_frame)},
                {"width", mask.width}, {"height", mask.height}, {"depth", name(mask.format.depth)},
                {"channel_order", name(mask.format.order)}, {"mapping", mask_mapping_json(mask.mapping)},
                {"valid_pixels", mask_rule_json(mask.valid_pixels)}};
        }}, shape);
}

json region_source_json(const RegionSource& source)
{
    return std::visit([](const auto& value) {
        json result{{"coordinate_frame", id_text(value.coordinate_frame)}, {"unit", name(value.unit)},
            {"local_direction_radians", value.local_direction_radians}, {"transform", transform_json(value.transform)}};
        if constexpr (std::same_as<std::decay_t<decltype(value)>, RegionPrimitive>) {
            result["kind"] = "primitive";
            result["geometry"] = shape_json(value.geometry);
        } else {
            result["kind"] = "inherited";
            result["workflow"] = id_text(value.workflow);
            result["node"] = id_text(value.node);
            result["output"] = id_text(value.output);
        }
        return result;
    }, source);
}

json region_json(const RegionDefinition& region)
{
    return {{"id", id_text(region.id)}, {"coordinate_frame", id_text(region.coordinate_frame)}, {"unit", name(region.unit)},
        {"source", region_source_json(region.source)}, {"exclusions", array_json(region.exclusions, region_source_json)},
        {"boundary_policy", name(region.boundary_policy)}, {"empty_policy", name(region.empty_policy)},
        {"fill", region.fill ? json{{"mode", name(region.fill->mode)}, {"value", region.fill->value}} : json(nullptr)}};
}

json update_origin_json(const UpdateOrigin& origin)
{
    return std::visit(Overload{
        [](const SessionInvocationKey& key) {
            return json{{"session", id_text(key.session)}, {"run", id_text(key.key.workflow_run)},
                {"node", id_text(key.key.node)}, {"scope", id_text(key.key.scope)}, {"invocation", id_text(key.key.invocation)}};
        },
        [](const ExternalUpdateOrigin& update) {
            return json{{"session", id_text(update.session)}, {"source", id_text(update.source)}};
        }}, origin);
}

json result_trace_json(const ResultTrace& trace)
{
    const auto* port = std::get_if<OutputId>(&trace.reference.output);
    json result{{"kind", "result"}, {"node", id_text(trace.reference.node)},
        {"output", port ? id_text(*port) : std::string(name(std::get<SystemField>(trace.reference.output)))},
        {"output_kind", port ? "port" : "system"}, {"selector", selector_json(trace.reference.selector)},
        {"scope", id_text(trace.scope)}, {"invocation", optional_id(trace.invocation)}, {"attempt", optional_id(trace.attempt)}};
    add_run_identity(result, trace.run);
    return result;
}

json source_json(const SourceTrace& source)
{
    auto value = std::visit(Overload{
        [](ConstantTrace constant) { return json{{"kind", "constant"}, {"defaulted", constant.defaulted}}; },
        [](const ResultTrace& trace) { return result_trace_json(trace); },
        [](const PublicInputTrace& trace) {
            json result{{"kind", "public_input"}, {"input", id_text(trace.reference.input)}, {"scope", id_text(trace.scope)},
                {"group", optional_id(trace.reference.group)}, {"selector", selector_json(trace.reference.selector)},
                {"origin", trace.origin == InputOrigin::Provided ? "Provided" : "Default"}};
            add_run_identity(result, trace.run);
            return result;
        },
        [](const IndependentInputTrace& trace) {
            json result{{"kind", "independent_input"}, {"input", id_text(trace.input)},
                {"origin", trace.origin == InputOrigin::Provided ? "Provided" : "Default"}};
            add_run_identity(result, trace.run);
            return result;
        },
        [](const GlobalReadTrace& trace) {
            return json{{"kind", "global"}, {"variable", id_text(trace.variable)},
                {"revision", std::to_string(trace.revision)}, {"selector", selector_json(trace.selector)},
                {"update_origin", update_origin_json(trace.update_origin)}};
        },
        [](const ResourceReference& resource) {
            auto result = resource_json(resource);
            result["resource_kind"] = result["kind"];
            result["kind"] = "resource";
            return result;
        }}, source.origin);
    value["index"] = source.source_index;
    if (source.selector_step) { value["selector_step"] = *source.selector_step; }
    if (source.index_value) { value["index_value"] = std::to_string(*source.index_value); }
    value["accepted_absence"] = optional_name(source.accepted_absence);
    return value;
}

json binding_json(const RunBindingTrace& binding)
{
    json result{{"target", std::visit([](const auto& id) { return id_text(id); }, binding.target)},
        {"target_kind", std::holds_alternative<InputId>(binding.target) ? "input" : "parameter"},
        {"sources", array_json(binding.sources, source_json)}};
    if (!binding.regions.empty()) {
        result["regions"] = array_json(binding.regions, [](const ResolvedRegionReference& region) {
            const auto& identity = region.identity;
            json producer{{"node", id_text(identity.invocation.node)}, {"scope", id_text(identity.invocation.scope)},
                {"invocation", id_text(identity.invocation.invocation)}, {"attempt", id_text(identity.attempt)},
                {"output", id_text(region.reference.output)}};
            add_run_identity(producer, identity.run);
            return json{{"source_index", region.source_index}, {"path", diagnostic_path_json(region.path)},
                {"region", id_text(region.value.id)}, {"reference", region_source_json(region.reference)},
                {"producer", std::move(producer)}};
        });
    }
    return result;
}

json image_layout_json(const ImageHandle& image)
{
    return {{"width", image.width()}, {"height", image.height()},
        {"depth", name(image.format().depth)}, {"channel_order", name(image.format().order)},
        {"current_frame", id_text(image.coordinate_frame())}, {"root_frame", id_text(image.root_frame())},
        {"origin", {{"x", image.origin_x()}, {"y", image.origin_y()}}}};
}

using UInt8SampleCounts = std::array<std::size_t, 256>;

std::optional<UInt8SampleCounts> uint8_sample_counts(const ImageHandle& image)
{
    if (image.format().depth != PixelDepth::UInt8) { return std::nullopt; }
    UInt8SampleCounts counts{};
    for (std::size_t y = 0; y < image.height(); ++y) {
        const auto row = std::get<std::span<const std::byte>>(image.row(y));
        for (const auto sample : row) {
            ++counts[std::to_integer<unsigned>(sample)];
        }
    }
    return counts;
}

json sample_summary_json(const UInt8SampleCounts& counts)
{
    // Positive image dimensions guarantee at least one sample.
    std::size_t minimum = 0;
    std::size_t maximum = counts.size() - 1;
    while (counts[minimum] == 0) { ++minimum; }
    while (counts[maximum] == 0) { --maximum; }
    return {{"minimum", minimum}, {"maximum", maximum},
        {"nonzero_count", std::accumulate(counts.begin() + 1, counts.end(), std::size_t{0})}};
}

json image_json(const ImageFrame& frame)
{
    auto result = image_layout_json(frame.image);
    if (const auto counts = uint8_sample_counts(frame.image)) { result["samples"] = sample_summary_json(*counts); }
    result["frame"] = frame.frame ? json{{"source", id_text(frame.frame->source)},
        {"sequence", std::to_string(frame.frame->sequence)}} : json(nullptr);
    result["captured_at_unix_ns"] = frame.captured_at ? json(std::to_string(frame.captured_at->time_since_epoch().count())) : json(nullptr);
    return result;
}

json raster_mask_json(const RasterMask& mask)
{
    auto pixels = image_layout_json(mask.pixels);
    const auto counts = uint8_sample_counts(mask.pixels);
    if (counts) { pixels["samples"] = sample_summary_json(*counts); }
    json result{{"pixels", std::move(pixels)}, {"mapping", mask_mapping_json(mask.mapping)},
        {"valid_pixels", mask_rule_json(mask.valid_pixels)}};
    // Only direct sampling makes source pixel counts the target membership counts.
    if (counts && mask.mapping.sampling == MaskSampling::None && !mask.mapping.transform) {
        // Valid UInt8 masks are single-channel and thresholds are in [0, 255].
        const auto first = mask.valid_pixels.mode == MaskValueMode::NonZero
            ? 1 : static_cast<int>(std::ceil(mask.valid_pixels.threshold));
        result["member_count"] = std::accumulate(counts->begin() + first, counts->end(), std::size_t{0});
    }
    return result;
}

void add_small_label_raster(json& summary, const ImageHandle& pixels)
{
    // Keep CLI reports bounded while exposing the exact IDs for small inspection maps.
    constexpr std::size_t maximum_labels = 256;
    if (pixels.width() > maximum_labels / pixels.height()) { return; }
    auto labels = json::array();
    for (std::size_t y = 0; y < pixels.height(); ++y) {
        labels.push_back(array_json(std::get<std::span<const std::byte>>(pixels.row(y)),
            [](std::byte id) { return std::to_integer<std::uint8_t>(id); }));
    }
    summary["labels"] = std::move(labels);
}

json data_json(const DataValue& data)
{
    if (const auto* flag = data.as_bool()) { return *flag; }
    if (const auto* number = data.as_int64()) { return std::to_string(*number); }
    if (const auto* number = data.as_float64()) { return *number; }
    if (const auto* value = data.as_string()) { return *value; }
    if (const auto* member = data.as_enum()) {
        return {{"schema", id_text(*data.type().schema_id())}, {"version", data.type().schema_version()},
            {"member", id_text(*member)}};
    }
    if (const auto* list = data.elements()) { return array_json(*list, data_json); }
    if (const auto* fields = data.fields()) {
        auto result = json::object();
        const bool class_map = data.type() == class_map_type();
        for (const auto& field : *fields) {
            auto& member = result[id_text(field.id)];
            member = data_json(field.value);
            if (class_map && field.id.text() == "pixels") { add_small_label_raster(member, field.value.as_image()->image); }
        }
        return result;
    }
    if (data.type().kind() == TypeKind::Optional) {
        const auto* value = data.optional_value();
        return value ? data_json(*value) : json(nullptr);
    }
    if (const auto* image = data.as_image()) { return image_json(*image); }
    if (const auto* point = data.as_point2d()) {
        return {{"coordinate_frame", id_text(point->coordinate_frame)}, {"unit", name(point->unit)}, {"x", point->x}, {"y", point->y}};
    }
    if (const auto* transform = data.as_transform2d()) { return transform_json(*transform); }
    if (const auto* resource = data.as_resource_reference()) { return resource_json(*resource); }
    if (const auto* region = data.as_region2d()) { return region_json(*region); }
    if (const auto* mask = data.as_raster_mask()) { return raster_mask_json(*mask); }
    throw std::logic_error("The catalog emitted an output type without a CLI summary.");
}

json output_json(const OutputSlot& slot)
{
    json value = std::visit(Overload{
        [](const DataValue& data) { return json{{"status", "Produced"}, {"value", data_json(data)}}; },
        [](NotProduced missing) { return json{{"status", "NotProduced"}, {"reason", name(missing.reason)}}; },
        [](const InvalidOutput& invalid) { return json{{"status", "Invalid"}, {"diagnostic", diagnostic_json(invalid.diagnostic)}}; }}, slot.content);
    value["id"] = id_text(slot.id);
    return value;
}

json node_json(const RunNodeProgress& node, const std::optional<NodeResult>& result)
{
    json trace{{"id", id_text(node.node)}, {"status", name(node.status)}, {"skip_reason", optional_name(node.skip_reason)},
        {"invocation", nullptr}, {"scope", optional_id(node.scope)}, {"attempt", nullptr}, {"judgement", nullptr},
        {"selected_outputs", json::array()}, {"outputs", json::array()}, {"diagnostics", json::array()},
        {"bindings", array_json(node.bindings, binding_json)}, {"resources", array_json(node.resources, [](const RunResourceTrace& resource) {
            auto value = resource_json(resource.reference);
            value["slot"] = id_text(resource.slot);
            return value;
        })}};
    if (const auto& call = node.invocation) {
        trace["invocation"] = id_text(call->invocation);
        trace["scope"] = id_text(call->scope);
    }
    trace["error_handled"] = node.error_handled;
    trace["attempts"] = array_json(node.attempts, [](const RunAttempt& attempt) {
        return json{{"attempt", id_text(attempt.attempt)}, {"status", name(attempt.status)},
            {"diagnostics", diagnostics_json(attempt.diagnostics)}};
    });
    if (result) {
        const auto& data = result->data();
        trace["attempt"] = id_text(data.identity.attempt);
        trace["judgement"] = name(data.judgement);
        trace["selected_outputs"] = array_json(data.selected_control_outputs, id_text<ControlOutputId>);
        trace["outputs"] = array_json(data.outputs, output_json);
        trace["diagnostics"] = diagnostics_json(data.diagnostics);
        for (const auto& timing : data.timings) {
            if (timing.source == TimingSource::AdapterReported && timing.scope == TimingScope::ToolModule) {
                trace["adapter_duration"] = reported_duration(timing.value, timing.unit);
                trace["host_elapsed_ns"] = data.elapsed ? json(std::to_string(data.elapsed->count())) : json(nullptr);
            }
        }
    }
    return trace;
}
} // namespace

nlohmann::json diagnostics_json(std::span<const core::Diagnostic> diagnostics)
{ return array_json(diagnostics, diagnostic_json); }

nlohmann::json reported_duration(std::optional<double> duration, core::TimingUnit unit)
{
    const char* label = "unknown";
    switch (unit) {
    case core::TimingUnit::Nanoseconds: label = "ns"; break;
    case core::TimingUnit::Microseconds: label = "us"; break;
    case core::TimingUnit::Milliseconds: label = "ms"; break;
    case core::TimingUnit::Seconds: label = "s"; break;
    case core::TimingUnit::Unknown: break;
    }
    return {{"value", duration ? json(*duration) : json(nullptr)}, {"unit", label}};
}

core::Result<nlohmann::json> summarize_run(const workflow::RunHandle& handle,
    const workflow::RunProgress& progress, std::span<const core::Diagnostic> load_diagnostics)
{
    const auto& solution = handle.request().snapshot().solution();
    auto nodes = json::array();
    for (const auto& node : progress.nodes) {
        std::optional<NodeResult> result;
        if (node.invocation) {
            auto found = handle.lookup(*node.invocation);
            if (auto* failure = std::get_if<Diagnostic>(&found)) { return std::move(*failure); }
            result = std::get<std::optional<NodeResult>>(std::move(found));
        }
        nodes.push_back(node_json(node, result));
    }
    json summary{{"solution", id_text(solution.id)}, {"revision", std::to_string(solution.revision)},
        {"status", name(progress.status)}, {"quiescent", progress.quiescent},
        {"load_diagnostics", diagnostics_json(load_diagnostics)}, {"diagnostics", diagnostics_json(progress.diagnostics)},
        {"nodes", std::move(nodes)}, {"call_order", array_json(progress.call_order, id_text<NodeId>)},
        {"control_links", array_json(progress.control_links, [](const RunControlLinkProgress& link) {
            json edge{{"id", id_text(link.link)}, {"status", name(link.status)}, {"close_reason", optional_name(link.close_reason)}};
            if (link.scope) { edge["scope"] = id_text(*link.scope); }
            return edge;
        })}};
    const auto export_trace = [](const RunExportTrace& item) {
        const auto& id = item.producer;
        return json{{"output", id_text(item.output)}, {"producer", result_trace_json(
            {item.source, id.run, id.invocation.scope, id.invocation.invocation, id.attempt})}};
    };
    summary["iterations"] = array_json(progress.iterations, [&](const RunLoopIteration& iteration) {
        return json{{"scope", id_text(iteration.scope)}, {"index", std::to_string(iteration.index)},
            {"loop_node", id_text(iteration.loop.node)}, {"parent_scope", id_text(iteration.loop.scope)},
            {"loop_invocation", id_text(iteration.loop.invocation)},
            {"exports", array_json(iteration.exports, export_trace)}};
    });
    summary["groups"] = array_json(progress.groups, [&](const RunGroupCall& call) {
        return json{{"scope", id_text(call.scope)}, {"group_node", id_text(call.group.node)},
            {"parent_scope", id_text(call.group.scope)}, {"group_invocation", id_text(call.group.invocation)},
            {"exports", array_json(call.exports, export_trace)}};
    });
    add_run_identity(summary, handle.identity());
    const auto& request = handle.request();
    const auto& trigger = request.trigger();
    if (trigger.kind == TriggerKind::External || trigger.kind == TriggerKind::Continuous) {
        summary["trigger"] = {{"kind", trigger.kind == TriggerKind::External ? "external" : "continuous"},
            {"source", id_text(trigger.source)}, {"correlation", nullptr}};
        if (const auto& correlation = request.correlation()) {
            summary["trigger"]["correlation"] = {{"domain", id_text(correlation->domain())}, {"key", correlation->key()}};
        }
        summary["inputs"] = array_json(request.inputs(), [](const FixedRunInput& input) {
            return json{{"id", id_text(input.id)}, {"origin", input.origin == InputOrigin::Provided ? "provided" : "default"},
                {"value", data_json(input.value)}};
        });
    }
    return summary;
}

} // namespace visionpro::app
