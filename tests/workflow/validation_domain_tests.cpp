#include <visionpro/workflow/validation.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
template <class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
template <class T> bool ok(const Result<T>& result) { return std::holds_alternative<T>(result); }
template <SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template <UuidIdentity T> T uuid(unsigned value)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
CoordinateSystemId camera() { return key<CoordinateSystemId>("camera"); }
CoordinateSystemId physical() { return key<CoordinateSystemId>("workpiece"); }
RegionDefinition local_region()
{
    return {uuid<RegionId>(1), camera(), MeasurementUnit::Pixel,
        RegionPrimitive{camera(), MeasurementUnit::Pixel, AxisAlignedRect{0, 0, 8, 8}}, {}};
}
RegionCapabilities capabilities()
{
    return {{RegionShapeKind::AxisAlignedRect, RegionShapeKind::RasterMask}, {RegionBoundaryPolicy::Reject},
        GeometryMaskPolicy::Combine, true, true, false, {PixelDepth::UInt8}};
}
InputDescriptor domain_input()
{
    InputDescriptor result{key<InputId>("domain"), "Domain", DataType::region2d(), false,
        take(ConfigValue::region2d(local_region()))};
    result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput};
    return result;
}
ToolDescriptor domain_tool(std::string_view id, bool consumer)
{
    ToolDescriptor result{key<ToolTypeId>(id), "Domain tool", 1,
        {key<SchemaId>(std::string(id) + ".schema"), 1, {}, true}, {}, {},
        {{key<OutputId>("roi"), "ROI", DataType::region2d(), {MeasurementUnit::Pixel, camera()}}}};
    if (consumer) { result.inputs.push_back(domain_input()); }
    result.region_capabilities = capabilities();
    return result;
}
struct Fixture {
    Solution solution;
    std::vector<ToolDescriptor> tools;
    Workflow& flow() { return solution.workflows.front(); }
    Node& producer() { return flow().nodes[0]; }
    Node& consumer() { return flow().nodes[1]; }
    InputDescriptor& input() { return tools[1].inputs.front(); }
    void bind(ConfigValue value)
    {
        input().type = value.type(); input().required = true; input().default_value.reset();
        consumer().bindings = {{key<InputId>("domain"), {std::move(value)}}};
    }
    InheritedRegion inherited(unsigned source = 1)
    { return {flow().id, uuid<NodeId>(source), key<OutputId>("roi"), camera(), MeasurementUnit::Pixel}; }
    RegionDefinition inherited_region(unsigned source = 1)
    { auto result = local_region(); result.source = inherited(source); return result; }
    Result<ValidationIndex> validate() const { return validate_solution(solution, tools); }
};
Fixture fixture()
{
    Fixture result{{uuid<SolutionId>(1), "Domain validation", 1, 1,
        {{uuid<WorkflowId>(1), "Workflow", {key<SchemaId>("workflow.public"), 1, {}, {}, {}}}}},
        {domain_tool("test.source", false), domain_tool("test.consumer", true)}};
    result.flow().nodes = {
        {uuid<NodeId>(1), "Source", ToolConfiguration{result.tools[0].id, 1}},
        {uuid<NodeId>(2), "Consumer", ToolConfiguration{result.tools[1].id, 1}}};
    result.flow().entries = {uuid<NodeId>(1)};
    result.flow().control_links = {{uuid<ControlLinkId>(1), uuid<NodeId>(1), {}, uuid<NodeId>(2)}};
    result.solution.coordinates = {
        {camera(), CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded},
        {physical(), CoordinateKind::Physical, MeasurementUnit::Millimetre, {0, 0}, {1, 0}, {0, 1}, Handedness::RightHanded}};
    return result;
}
Diagnostic rejected(const Result<ValidationIndex>& result, ErrorCode code, std::string_view description,
    unsigned node = 2)
{
    const auto* invalid = std::get_if<Diagnostic>(&result);
    check(invalid && invalid->code == code, description);
    if (!invalid) { throw std::runtime_error("Unexpected validation success"); }
    check(invalid->location.node == uuid<NodeId>(node), "domain rejection identifies the affected node");
    check(invalid->severity == Severity::Error && !invalid->message.empty(), "domain rejection explains the error");
    return *invalid;
}
ResourceDefinition resource(unsigned id, ResourceKind kind)
{
    return {uuid<ResourceId>(id), "Resource", kind,
        ManagedResource{{PathBase::SolutionDirectory, "resources/fixture.bin", {}}, 1, std::string(64, 'a')}};
}
ConfigValue nested(ConfigValue value)
{
    const auto list = take(ConfigValue::list(DataType::optional(value.type()), {ConfigValue::some(std::move(value))}));
    const auto type = take(DataType::record(key<SchemaId>("nested.domain"), 1,
        {{key<FieldId>("payload"), list.type()}}));
    return take(ConfigValue::record(type, {{key<FieldId>("payload"), list}}));
}
bool nested_path(const Diagnostic& diagnostic)
{
    return diagnostic.location.path.size() >= 2
        && diagnostic.location.path[0] == DiagnosticPathStep{key<FieldId>("payload")}
        && diagnostic.location.path[1] == DiagnosticPathStep{DiagnosticElementIndex{0}};
}

void inherited_regions()
{
    auto valid = fixture(); valid.bind(take(ConfigValue::region2d(valid.inherited_region())));
    auto checked = valid.validate();
    check(ok(checked), "inherited ROI follows valid control order");
    if (ok(checked)) {
        const auto& index = std::get<ValidationIndex>(checked);
        check(index.scopes.size() == 1 && index.scopes[0].data.size() == 1
            && index.scopes[0].data[0].inherited_region
            && index.scopes[0].data[0].producer == uuid<NodeId>(1)
            && index.scopes[0].data[0].consumer == uuid<NodeId>(2),
            "ROI hidden reference contributes an explicit derived dependency");
    }
    auto changed = valid;
    changed.tools[0].outputs[0].type = DataType::integer();
    rejected(changed.validate(), ErrorCode::TypeMismatch, "inheritance requires a Region2D output");
    changed = valid;
    changed.tools[0].outputs[0].semantics = {MeasurementUnit::Millimetre, physical()};
    rejected(changed.validate(), ErrorCode::CoordinateMismatch, "inheritance must match output coordinates before correction");
    changed = valid;
    changed.tools[0].outputs[0].semantics = {};
    rejected(changed.validate(), ErrorCode::CoordinateMismatch, "inheritance cannot guess missing output coordinates");

    changed = fixture(); changed.tools[0].inputs.push_back(domain_input());
    changed.producer().bindings = {{key<InputId>("domain"), {take(ConfigValue::region2d(changed.inherited_region(2)))}}};
    auto inverse = changed.validate();
    check(!ok(inverse), "hidden ROI reverse dependency is rejected");
    if (const auto* error = std::get_if<Diagnostic>(&inverse)) {
        check(error->location.node == uuid<NodeId>(1) && error->location.input == key<InputId>("domain"),
            "hidden reverse dependency locates its binding");
    }
    changed.flow().control_links.clear(); changed.flow().entries.push_back(uuid<NodeId>(2));
    changed.bind(take(ConfigValue::region2d(changed.inherited_region(1))));
    check(!ok(changed.validate()), "two constant ROI definitions cannot hide a data dependency cycle");

    auto excluded = fixture(); auto region = local_region(); region.exclusions.push_back(excluded.inherited());
    excluded.bind(take(ConfigValue::region2d(region)));
    auto exclusion_result = excluded.validate();
    check(ok(exclusion_result), "inherited exclusion is a legal ROI data dependency");
    if (ok(exclusion_result)) {
        check(std::get<ValidationIndex>(exclusion_result).scopes[0].data.size() == 1,
            "exclusion inheritance participates in the dependency index");
    }
    std::get<InheritedRegion>(region.exclusions[0]).output = key<OutputId>("missing");
    excluded.bind(take(ConfigValue::region2d(region)));
    const auto exclusion_error = excluded.validate();
    check(!ok(exclusion_error), "exclusion cannot reference an undeclared output");
    if (const auto* error = std::get_if<Diagnostic>(&exclusion_error)) {
        check(error->location.path.size() >= 2 && error->location.path[0] == DiagnosticPathStep{key<FieldId>("exclusions")}
            && error->location.path[1] == DiagnosticPathStep{DiagnosticElementIndex{0}},
            "exclusion error identifies its index");
    }

    changed = valid; changed.tools[1].region_capabilities->inheritance = false;
    rejected(changed.validate(), ErrorCode::UnsupportedRegion, "undeclared ROI inheritance capability is rejected");
    changed = fixture(); changed.tools[1].region_capabilities->shapes = {RegionShapeKind::RasterMask};
    rejected(changed.validate(), ErrorCode::UnsupportedRegion, "default ROI must satisfy declared tool shape capabilities");
    changed = valid; changed.tools[1].region_capabilities.reset();
    check(ok(changed.validate()), "absent ROI capabilities does not invent a universal rejection policy");

    changed = valid; changed.input().binding.allowed_sources = {BindingSourceKind::Constant};
    rejected(changed.validate(), ErrorCode::DisallowedBindingSource, "constant wrapper cannot bypass RunOutput source permission");
    changed = valid; changed.input().binding.time = BindingTime::SnapshotBuild;
    changed.input().binding.allowed_sources = {BindingSourceKind::Constant};
    rejected(changed.validate(), ErrorCode::DisallowedBindingSource, "snapshot-time constant cannot carry runtime ROI inheritance");
    changed = valid; changed.producer().enabled = false;
    rejected(changed.validate(), ErrorCode::InvalidBinding, "hidden ROI producer cannot be disabled without declared absence handling");
    changed.bind(ConfigValue::some(take(ConfigValue::region2d(changed.inherited_region()))));
    changed.consumer().bindings[0].availability.accepted_absence = {MissingReason::Disabled};
    check(ok(changed.validate()), "Optional inherited ROI explicitly accepts a disabled producer");
    changed = valid;
    changed.input().required = false;
    changed.input().default_value = take(ConfigValue::region2d(changed.inherited_region()));
    changed.consumer().bindings.clear();
    check(ok(changed.validate()), "unconfigured default ROI still validates and indexes runtime inheritance");
    changed.input().binding.allowed_sources = {BindingSourceKind::Constant};
    rejected(changed.validate(), ErrorCode::DisallowedBindingSource, "default ROI cannot bypass runtime source permission");
    changed = fixture();
    ParameterDescriptor structural{key<ParameterId>("shape"), "Shape", DataType::region2d()};
    structural.structural = true; structural.binding.time = BindingTime::SnapshotBuild;
    changed.tools[1].parameters.push_back(structural);
    changed.consumer().bindings = {{key<ParameterId>("shape"), {take(ConfigValue::region2d(changed.inherited_region()))}}};
    rejected(changed.validate(), ErrorCode::DisallowedBindingSource, "structural parameter cannot conceal an invocation dependency");
}

void nested_values_and_frames()
{
    auto valid = fixture(); valid.bind(nested(take(ConfigValue::region2d(valid.inherited_region()))));
    auto checked = valid.validate();
    check(ok(checked), "Record/List/Optional nesting preserves valid ROI inheritance");
    if (ok(checked)) {
        check(std::get<ValidationIndex>(checked).scopes[0].data.size() == 1,
            "nested ROI reference still contributes a data dependency");
    }
    auto changed = valid; changed.tools[0].outputs[0].type = DataType::integer();
    const auto wrong_type = rejected(changed.validate(), ErrorCode::TypeMismatch, "nested ROI output type is validated");
    check(nested_path(wrong_type), "nested ROI diagnostic keeps record field and list index");

    auto references = fixture();
    references.bind(nested(take(ConfigValue::resource_reference({uuid<ResourceId>(8), ResourceKind::Template, 1}))));
    const auto missing = rejected(references.validate(), ErrorCode::ResourceNotFound, "nested resource cannot bypass directory validation");
    check(nested_path(missing) && missing.location.resource == uuid<ResourceId>(8),
        "nested resource diagnostic retains path and identity");
    references.solution.resources.push_back(resource(8, ResourceKind::Template));
    check(ok(references.validate()), "adding the exact resource identity makes nested reference valid");

    auto point = fixture();
    point.bind(nested(take(ConfigValue::point2d({camera(), MeasurementUnit::Millimetre, 1, 2}))));
    const auto wrong_unit = rejected(point.validate(), ErrorCode::CoordinateMismatch, "nested geometry unit must match its frame catalog");
    check(nested_path(wrong_unit), "nested frame mismatch keeps its value path");
    point.bind(take(ConfigValue::point2d({key<CoordinateSystemId>("missing"), MeasurementUnit::Pixel, 1, 2})));
    rejected(point.validate(), ErrorCode::InvalidCoordinate, "point references require declared frames");
    point.bind(ConfigValue::some(take(ConfigValue::point2d({camera(), MeasurementUnit::Pixel, 1, 2}))));
    point.input().semantics = {MeasurementUnit::Pixel, camera()};
    check(ok(point.validate()), "Optional point retains matching coordinate semantics");
    point.input().semantics = {MeasurementUnit::Millimetre, physical()};
    rejected(point.validate(), ErrorCode::CoordinateMismatch, "Optional geometry does not bypass target coordinate compatibility");
}

void masks_and_calibration()
{
    auto mask_fixture = fixture();
    auto region = local_region();
    std::get<RegionPrimitive>(region.source).geometry = MaskConfig{
        {uuid<ResourceId>(10), ResourceKind::Mask, 1}, camera(), 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono},
        {camera(), 2, 2, {}, MaskSampling::None}, {}};
    mask_fixture.bind(take(ConfigValue::region2d(region)));
    const auto missing_mask = rejected(mask_fixture.validate(), ErrorCode::ResourceNotFound, "mask ROI resource must exist in the catalog");
    check(missing_mask.location.resource == uuid<ResourceId>(10) && missing_mask.location.region == region.id,
        "missing mask identifies both resource and region");
    mask_fixture.solution.resources.push_back(resource(10, ResourceKind::Mask));
    check(ok(mask_fixture.validate()), "valid mask configuration is checked without loading pixel bytes");

    Transform2D transform{camera(), physical(), MeasurementUnit::Pixel, MeasurementUnit::Millimetre,
        TransformModel::Calibrated};
    transform.calibration = ResourceReference{uuid<ResourceId>(11), ResourceKind::Calibration, 1};
    transform.valid_domain = TransformDomain{0, 0, 10, 10};
    auto calibration = fixture(); calibration.bind(nested(take(ConfigValue::transform2d(transform))));
    const auto missing_calibration = rejected(calibration.validate(), ErrorCode::ResourceNotFound, "nested calibration reference must resolve in catalog");
    check(nested_path(missing_calibration) && missing_calibration.location.resource == uuid<ResourceId>(11),
        "calibration diagnostic keeps the enclosing value path");
    calibration.solution.resources.push_back(resource(11, ResourceKind::Calibration));
    check(ok(calibration.validate()), "pinned calibration reference with declared endpoint frames is valid");
    transform.to = key<CoordinateSystemId>("unknown");
    calibration.bind(take(ConfigValue::transform2d(transform)));
    rejected(calibration.validate(), ErrorCode::InvalidCoordinate, "calibration target frame cannot be undeclared");

    auto corrected = fixture(); region = local_region();
    region.coordinate_frame = physical(); region.unit = MeasurementUnit::Millimetre;
    transform.to = physical();
    std::get<RegionPrimitive>(region.source).transform = transform;
    corrected.bind(take(ConfigValue::region2d(region)));
    rejected(corrected.validate(), ErrorCode::ResourceNotFound, "ROI correction calibration is checked recursively");
    corrected.solution.resources.push_back(resource(11, ResourceKind::Calibration));
    check(ok(corrected.validate()), "ROI explicit correction preserves source and target frame semantics");

    const auto target_frame = key<CoordinateSystemId>("mask_target");
    mask_fixture.solution.coordinates.push_back({target_frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    region = local_region(); region.coordinate_frame = target_frame;
    auto& primitive = std::get<RegionPrimitive>(region.source); primitive.coordinate_frame = target_frame;
    transform.to = target_frame; transform.to_unit = MeasurementUnit::Pixel;
    primitive.geometry = MaskConfig{{uuid<ResourceId>(10), ResourceKind::Mask, 1}, camera(), 2, 2,
        {PixelDepth::UInt8, ChannelOrder::Mono}, {target_frame, 2, 2, transform, MaskSampling::Nearest}, {}};
    mask_fixture.bind(take(ConfigValue::region2d(region)));
    rejected(mask_fixture.validate(), ErrorCode::ResourceNotFound, "mask mapping calibration cannot hide a missing resource");
    mask_fixture.solution.resources.push_back(resource(11, ResourceKind::Calibration));
    check(ok(mask_fixture.validate()), "mask mapping validates both pixel frames and pinned calibration");
}

void resource_slots()
{
    auto valid = fixture();
    valid.tools[1].resources = {{key<SchemaId>("template"), ResourceKind::Template, true}};
    rejected(valid.validate(), ErrorCode::MissingRequiredValue, "required resource slot must be configured");
    valid.solution.resources.push_back(resource(20, ResourceKind::Template));
    valid.consumer().resources = {{key<SchemaId>("template"), {uuid<ResourceId>(20), ResourceKind::Template, 1}}};
    check(ok(valid.validate()), "matching declared resource slot is valid");
    auto changed = valid; changed.consumer().resources[0].slot = key<SchemaId>("unknown");
    rejected(changed.validate(), ErrorCode::UnknownTarget, "unknown resource slot is rejected");
    changed = valid; changed.consumer().resources[0].resource.kind = ResourceKind::Mask;
    rejected(changed.validate(), ErrorCode::ResourceKindMismatch, "slot requires its declared resource kind");
    changed = valid; changed.consumer().resources[0].resource.revision = 2;
    rejected(changed.validate(), ErrorCode::ResourceVersionMismatch, "slot cannot silently use another resource revision");
    changed = valid; changed.solution.resources.clear();
    rejected(changed.validate(), ErrorCode::ResourceNotFound, "slot identity must resolve in the catalog");
    changed = valid; changed.tools[1].resources[0].required = false; changed.consumer().resources.clear();
    check(ok(changed.validate()), "optional resource slot may remain unconfigured");
}

NodeStateDefinition state(ConfigValue value)
{
    return {value.type(), value, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit,
        StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
}
void non_invocation_configuration()
{
    auto configured = fixture();
    configured.tools[1].state = state(nested(take(ConfigValue::resource_reference(
        {uuid<ResourceId>(40), ResourceKind::Template, 1}))));
    auto checked = configured.validate();
    const auto* error = std::get_if<Diagnostic>(&checked);
    check(error && error->code == ErrorCode::ResourceNotFound, "state initial value validates nested catalog references");
    check(error && error->location.tool == configured.tools[1].id && nested_path(*error),
        "state initial reference diagnostic identifies tool and nested value path");
    configured.solution.resources.push_back(resource(40, ResourceKind::Template));
    check(ok(configured.validate()), "state initial resource reference resolves without runtime services");
    configured.tools[1].state = state(nested(take(ConfigValue::region2d(configured.inherited_region()))));
    checked = configured.validate(); error = std::get_if<Diagnostic>(&checked);
    check(error && error->code == ErrorCode::DisallowedBindingSource,
        "state initialization cannot inherit invocation-specific ROI output");

    configured = fixture();
    configured.solution.resources.push_back({uuid<ResourceId>(41), "Device", ResourceKind::Device,
        DeviceResource{key<SchemaId>("test.adapter"), nested(take(ConfigValue::resource_reference(
            {uuid<ResourceId>(40), ResourceKind::Template, 1})))}});
    checked = configured.validate(); error = std::get_if<Diagnostic>(&checked);
    check(error && error->code == ErrorCode::ResourceNotFound, "device configuration validates nested resource references");
    check(error && nested_path(*error), "device configuration reference diagnostic preserves nesting");
    configured.solution.resources.push_back(resource(40, ResourceKind::Template));
    check(ok(configured.validate()), "device configuration can reference a declared resource without connecting a device");
    std::get<DeviceResource>(configured.solution.resources[0].location).configuration =
        nested(take(ConfigValue::region2d(configured.inherited_region())));
    checked = configured.validate(); error = std::get_if<Diagnostic>(&checked);
    check(error && error->code == ErrorCode::DisallowedBindingSource,
        "device setup cannot read invocation-specific ROI output");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--self-test-failure") {
            check(false, "intentional test-runner failure");
        } else if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "validation_domains")) {
            inherited_regions(); nested_values_and_frames(); masks_and_calibration(); resource_slots();
            non_invocation_configuration();
        } else { throw std::runtime_error("Unknown validation-domain test selection"); }
    } catch (const std::exception& error) {
        ++failures; std::cerr << "EXCEPTION: " << error.what() << '\n';
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
