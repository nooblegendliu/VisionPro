#include <visionpro/workflow/copy.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

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
template <class T> void expect(const Result<T>& result, ErrorCode code)
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error && error->code == code, "copy failure retains its typed cause");
}
template <PersistentId Id> Id id(unsigned number)
{
    std::ostringstream text;
    text << "00112233-4455-4677-8899-" << std::hex << std::setfill('0') << std::setw(12) << number;
    return take(IdFactory::parse<Id>(text.str()));
}
template <SchemaIdentity Id> Id key(std::string_view text) { return take(SchemaIdFactory::parse<Id>(text)); }
WorkflowId workflow_id() { return id<WorkflowId>(1); }
InputId input() { return key<InputId>("input"); }
OutputId output() { return key<OutputId>("value"); }
CoordinateSystemId frame() { return key<CoordinateSystemId>("image"); }
PublicInterface ports(std::string_view name)
{
    return {key<SchemaId>(name), 1, {{input(), "Input", DataType::integer()}},
        {{output(), "Output", DataType::integer()}}, {{key<ControlOutputId>("done"), "Done"}}};
}
Node tool(unsigned number)
{
    return {id<NodeId>(number), "Node " + std::to_string(number),
        ToolConfiguration{key<ToolTypeId>("fixture.tool"), 2}, {}, {}};
}
OutputReference ref(unsigned node)
{ return {workflow_id(), id<NodeId>(node), output(), {FieldSelection{key<FieldId>("rows")}, ElementSelection{2}}}; }
Binding binding(BindingSource source)
{ return {input(), {std::move(source)}}; }
Workflow graph()
{
    auto a = tool(1), b = tool(3), c = tool(5), consumer = tool(6);
    Node group{id<NodeId>(2), "Group", GroupConfiguration{ports("outer"), {b.id}, {{output(), ref(4)}}}, {}, {}};
    Node nested{id<NodeId>(4), "Nested", GroupConfiguration{ports("nested"), {c.id}, {{output(), ref(5)}}}, {}, {}};
    std::get<GroupConfiguration>(group.configuration).control_exports.push_back(
        {key<ControlOutputId>("done"), nested.id, key<ControlOutputId>("done")});
    std::get<GroupConfiguration>(nested.configuration).control_exports.push_back(
        {key<ControlOutputId>("done"), c.id, key<ControlOutputId>("accepted")});
    group.bindings.push_back(binding(ScopedInputReference{workflow_id(), {}, input(), {}}));
    b.bindings.push_back(binding(ScopedInputReference{workflow_id(), group.id, input(), {}}));
    nested.bindings.push_back(binding(ref(3)));
    c.bindings.push_back(binding(ScopedInputReference{workflow_id(), nested.id, input(), {}}));
    consumer.bindings.push_back(binding(ref(2)));
    return {workflow_id(), "Flow", ports("workflow"), {a, group, b, nested, c, consumer},
        {{id<ControlLinkId>(1), a.id, {}, group.id}, {id<ControlLinkId>(2), group.id, {}, consumer.id},
         {id<ControlLinkId>(3), b.id, key<ControlOutputId>("accepted"), nested.id}},
        {{group.id, b.id}, {group.id, nested.id}, {nested.id, c.id}}, {a.id}, {{output(), ref(6)}}, true,
        {{key<ControlOutputId>("done"), consumer.id, {}}}};
}
Solution solution()
{ return {id<SolutionId>(1), "Solution", 1, 9, {graph()}, {}, {}, {}, {}, {}}; }
const Node& copied_node(const SubgraphCopy& copy, unsigned original)
{
    const auto replacement = copy.identities.nodes.at(id<NodeId>(original));
    for (const auto& node : copy.nodes) { if (node.id == replacement) { return node; } }
    throw std::runtime_error("Missing copied node");
}
ResourceReference resource(unsigned number = 7, ResourceKind kind = ResourceKind::Image)
{ return {id<ResourceId>(number), kind, 3}; }
ManagedResource managed(std::string path)
{ return {{PathBase::SolutionDirectory, std::move(path), {}}, 3, std::string(64, 'a')}; }

void copy_subgraph()
{
    auto original = graph();
    const auto saved_reference = std::get<OutputReference>(original.nodes.back().bindings.front().sources.front());
    original.nodes[1].name = "Renamed group";
    check(std::get<OutputReference>(original.nodes.back().bindings.front().sources.front()) == saved_reference,
        "renaming a Group does not change a typed reference");
    const std::array selection{id<NodeId>(2)};
    auto copied = take(clone_subgraph(original, selection, ExternalReferencePolicy::Preserve));
    check(copied.workflow == original.id && copied.identities.workflows.empty(), "subgraph copy stays in its source workflow");
    check(copied.nodes.size() == 4 && copied.identities.nodes.size() == 4, "selecting a Group includes all nested descendants");
    check(copied.control_links.size() == 1 && copied.report.omitted_control_links.size() == 2,
        "only internal controls are copied; crossing activation edges are explicitly reported");
    check(copied.containment.size() == 3, "nested containment is retained independently of controls");
    check(copied.report.preserved_references.size() == 1
        && std::holds_alternative<ScopedInputReference>(copied.report.preserved_references.front().reference),
        "workflow-public input outside the fragment is explicitly preserved and reported");
    for (unsigned number : {2U, 3U, 4U, 5U}) {
        const auto& replacement = copied.identities.nodes.at(id<NodeId>(number));
        check(replacement != id<NodeId>(number), "copied nodes have fresh identities");
        check(copied_node(copied, number).id == replacement, "mapping addresses the actual copied node");
    }
    const auto& group = std::get<GroupConfiguration>(copied_node(copied, 2).configuration);
    const auto& nested = std::get<GroupConfiguration>(copied_node(copied, 4).configuration);
    check(group.entries == std::vector{copied.identities.nodes.at(id<NodeId>(3))}
        && nested.entries == std::vector{copied.identities.nodes.at(id<NodeId>(5))}, "Group entry identities are remapped");
    check(group.outputs.front().source.node == copied.identities.nodes.at(id<NodeId>(4))
        && nested.outputs.front().source.node == copied.identities.nodes.at(id<NodeId>(5)), "both levels of exported outputs are remapped");
    check(group.control_exports.front().source == copied.identities.nodes.at(id<NodeId>(4))
        && nested.control_exports.front().source == copied.identities.nodes.at(id<NodeId>(5))
        && nested.control_exports.front().source_output == key<ControlOutputId>("accepted"),
        "nested public control exports remap node sources and preserve typed control outlets");
    const auto& nested_binding = std::get<OutputReference>(copied_node(copied, 4).bindings.front().sources.front());
    check(nested_binding.node == copied.identities.nodes.at(id<NodeId>(3)) && nested_binding.workflow == original.id
        && nested_binding.selector == ref(3).selector, "internal output source changes identity while selectors remain structured");
    check(std::get<ScopedInputReference>(copied_node(copied, 5).bindings.front().sources.front()).group
        == copied.identities.nodes.at(id<NodeId>(4)), "public-input ownership points to the copied Group");
    check(copied.control_links.front().source == copied.identities.nodes.at(id<NodeId>(3))
        && copied.control_links.front().target == copied.identities.nodes.at(id<NodeId>(4))
        && copied.control_links.front().output == key<ControlOutputId>("accepted"), "control endpoints remap without changing named outlets");
    original.nodes.insert(original.nodes.end(), copied.nodes.begin(), copied.nodes.end());
    original.control_links.insert(original.control_links.end(), copied.control_links.begin(), copied.control_links.end());
    original.containment.insert(original.containment.end(), copied.containment.begin(), copied.containment.end());
    auto merged = solution();
    merged.workflows.front() = original;
    check(std::holds_alternative<std::monostate>(validate_model_shape(merged)), "copied fragment inserts without duplicate identities or containment conflicts");
    copied.nodes.front().name = "Copy edited";
    check(original.nodes[1].name == "Renamed group", "copy editing cannot mutate original editable configuration");

    auto closed = graph();
    closed.nodes[1].bindings.clear();
    auto strict = take(clone_subgraph(closed, selection, ExternalReferencePolicy::Reject));
    check(strict.report.preserved_references.empty(), "a closed subgraph can be copied under strict external-reference rejection");
    const std::array duplicated{id<NodeId>(2), id<NodeId>(3), id<NodeId>(2)};
    check(take(clone_subgraph(closed, duplicated, ExternalReferencePolicy::Reject)).nodes.size() == 4,
        "overlapping and repeated selections copy each owned node exactly once");
}

void copy_solution()
{
    auto original = solution();
    original.workflows.push_back({id<WorkflowId>(2), "Other flow", ports("other.workflow"), {tool(21)}, {}, {},
        {id<NodeId>(21)}, {{output(), {id<WorkflowId>(2), id<NodeId>(21), output(), {}}}}});
    original.resources = {{id<ResourceId>(7), "Image", ResourceKind::Image, managed("resources/image.bin")},
        {id<ResourceId>(8), "Calibration", ResourceKind::Calibration, managed("resources/calibration.bin")},
        {id<ResourceId>(9), "Mask", ResourceKind::Mask, managed("resources/mask.bin")}};
    original.variables.push_back({id<VariableId>(1), "Reference", DataType::resource_reference(),
        take(ConfigValue::resource_reference(resource()))});
    const auto message_type = take(DataType::record(key<SchemaId>("message"), 1, {{key<FieldId>("count"), DataType::integer()}}));
    original.queues.push_back({id<QueueId>(1), "Queue", message_type, 10, QueueOverflowPolicy::Reject,
        {EmptyQueuePolicy::ReturnEmpty, 2, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::UpToLimit}});
    auto& flow = original.workflows.front();
    const auto image_reference = take(ConfigValue::resource_reference(resource()));
    flow.interface.inputs.push_back({key<InputId>("resource"), "Image resource", DataType::resource_reference(), false,
        image_reference, {{}, {}, {image_reference}, {}}});
    flow.nodes.front().resources.push_back({key<SchemaId>("image"), resource()});
    flow.nodes.back().bindings.push_back({key<ParameterId>("global"), {GlobalReference{id<VariableId>(1), 4, {}}}});
    Transform2D calibrated{frame(), frame(), MeasurementUnit::Pixel, MeasurementUnit::Pixel,
        TransformModel::Calibrated, {1,0,0,0,1,0,0,0,1}, resource(8, ResourceKind::Calibration), TransformDomain{0,0,10,10}};
    RegionDefinition roi{id<RegionId>(1), frame(), MeasurementUnit::Pixel,
        InheritedRegion{flow.id, id<NodeId>(1), output(), frame(), MeasurementUnit::Pixel, 0, calibrated},
        {RegionPrimitive{frame(), MeasurementUnit::Pixel,
            MaskConfig{resource(9, ResourceKind::Mask), frame(), 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono},
                {frame(), 2, 2, {}, MaskSampling::None}, {}}}}};
    const auto roi_value = take(ConfigValue::region2d(roi));
    const auto nested_type = take(DataType::record(key<SchemaId>("nested.config"), 1,
        {{key<FieldId>("roi"), DataType::list(DataType::optional(DataType::region2d()))},
         {key<FieldId>("resource"), DataType::resource_reference()}, {key<FieldId>("transform"), DataType::transform2d()}}));
    const auto nested = take(ConfigValue::record(nested_type,
        {{key<FieldId>("roi"), take(ConfigValue::list(DataType::optional(DataType::region2d()), {ConfigValue::some(roi_value)}))},
         {key<FieldId>("resource"), image_reference}, {key<FieldId>("transform"), take(ConfigValue::transform2d(calibrated))}}));
    flow.nodes.front().bindings.push_back({key<ParameterId>("configuration"), {nested}});
    original.resources.push_back({id<ResourceId>(10), "Device", ResourceKind::Device,
        DeviceResource{key<SchemaId>("fixture.adapter"), nested}});
    original.layouts.push_back({flow.id, {{id<NodeId>(1), 1.5, -2.5}, {id<NodeId>(2), 10, 20}}});
    const auto copy = take(clone_solution(original, ExternalReferencePolicy::Reject));
    check(copy.report.preserved_references.empty() && copy.report.omitted_control_links.empty(), "closed solution copies all ownership without external references");
    check(copy.solution.id != original.id && copy.identities.solutions.at(original.id) == copy.solution.id,
        "copy as a new solution assigns a fresh solution identity");
    check(copy.solution.format_version == original.format_version && copy.solution.revision == original.revision,
        "copy retains configuration format and revision metadata");
    check(copy.identities.workflows.size() == 2 && copy.identities.nodes.size() == 7
        && copy.identities.control_links.size() == 3 && copy.identities.resources.size() == 4
        && copy.identities.variables.size() == 1 && copy.identities.queues.size() == 1 && copy.identities.regions.size() == 1,
        "identity mapping covers all persistent ownership categories");
    check(std::holds_alternative<std::monostate>(validate_model_shape(copy.solution)), "copied solution retains a valid editable model shape");
    const auto& copied_flow = copy.solution.workflows.front();
    check(copied_flow.id == copy.identities.workflows.at(flow.id) && copied_flow.entries.front() == copy.identities.nodes.at(id<NodeId>(1)),
        "workflow and its explicit entry remap together");
    check(copied_flow.outputs.front().source.workflow == copied_flow.id
        && copied_flow.outputs.front().source.node == copy.identities.nodes.at(id<NodeId>(6)), "workflow export uses copied workflow and source node");
    check(copied_flow.control_exports.front().source == copy.identities.nodes.at(id<NodeId>(6))
        && !copied_flow.control_exports.front().source_output,
        "workflow control export remaps source and preserves unconditional completion semantics");
    const auto& second_flow = copy.solution.workflows.back();
    check(second_flow.id == copy.identities.workflows.at(id<WorkflowId>(2))
        && second_flow.outputs.front().source.workflow == second_flow.id
        && second_flow.outputs.front().source.node == copy.identities.nodes.at(id<NodeId>(21)),
        "separate workflow ownership is preserved when copying a multi-workflow solution");
    check(std::get<ScopedInputReference>(copied_flow.nodes[1].bindings.front().sources.front()).workflow == copied_flow.id,
        "workflow public-input bindings are internal to a whole-solution copy");
    const auto& copied_nested = std::get<ConfigValue>(copied_flow.nodes.front().bindings.front().sources.front());
    const auto& copied_roi = *copied_nested.field(key<FieldId>("roi"))->elements()->front().optional_value()->as_region2d();
    const auto& inherited = std::get<InheritedRegion>(copied_roi.source);
    check(copied_roi.id == copy.identities.regions.at(roi.id) && copied_roi.id != roi.id,
        "Region identity nested in Record/List/Optional is renewed");
    check(inherited.workflow == copied_flow.id && inherited.node == copy.identities.nodes.at(id<NodeId>(1)),
        "inherited ROI source follows copied workflow/node identities");
    check(inherited.transform->calibration->id == copy.identities.resources.at(id<ResourceId>(8))
        && copied_nested.field(key<FieldId>("transform"))->as_transform2d()->calibration->id == copy.identities.resources.at(id<ResourceId>(8)),
        "calibration references remap in both ROI and standalone Transform payloads");
    check(std::get<MaskConfig>(std::get<RegionPrimitive>(copied_roi.exclusions.front()).geometry).resource.id
        == copy.identities.resources.at(id<ResourceId>(9)), "exclusion mask resource remaps recursively");
    check(copied_nested.field(key<FieldId>("resource"))->as_resource_reference()->id == copy.identities.resources.at(id<ResourceId>(7)),
        "structured constants remap resource references");
    check(copied_flow.nodes.front().resources.front().resource.id == copy.identities.resources.at(id<ResourceId>(7)),
        "node resource slots point to copied resource definitions");
    const auto& copied_default = copied_flow.interface.inputs.back();
    check(copied_default.default_value->as_resource_reference()->id == copy.identities.resources.at(id<ResourceId>(7))
        && copied_default.constraints.allowed_values.front().as_resource_reference()->id == copy.identities.resources.at(id<ResourceId>(7)),
        "public defaults and allowed-value constants remap consistently");
    check(copy.solution.variables.front().id != original.variables.front().id
        && copy.solution.variables.front().initial_value.as_resource_reference()->id == copy.identities.resources.at(id<ResourceId>(7)),
        "global definition identity and configuration initial value both remap");
    check(std::get<GlobalReference>(copied_flow.nodes.back().bindings.back().sources.front()).variable == copy.solution.variables.front().id,
        "global read binding points to the copied variable");
    check(copy.solution.queues.front().id != original.queues.front().id && copy.solution.queues.front().message_schema == message_type,
        "queue definition gets new identity while its Schema remains stable");
    check(copy.solution.layouts.front().workflow == copied_flow.id
        && copy.solution.layouts.front().nodes.front().node == copy.identities.nodes.at(id<NodeId>(1))
        && copy.solution.layouts.front().nodes.front().x == 1.5, "layout identities remap independently of numeric editor positions");
    check(std::get<ManagedResource>(copy.solution.resources.front().location).path.path == "resources/image.bin",
        "configuration copy does not relocate or write resource files");
    const auto& device_config = std::get<DeviceResource>(copy.solution.resources.back().location).configuration;
    check(device_config.field(key<FieldId>("resource"))->as_resource_reference()->id == copy.identities.resources.at(id<ResourceId>(7)),
        "device configuration values participate in reference remapping");
    check(std::get<ConfigValue>(flow.nodes.front().bindings.front().sources.front()).field(key<FieldId>("roi"))
        ->elements()->front().optional_value()->as_region2d()->id == roi.id,
        "copy never rewrites original immutable configuration values");
    check(std::get<ToolConfiguration>(copied_flow.nodes.front().configuration).type
        == std::get<ToolConfiguration>(flow.nodes.front().configuration).type, "tool and Schema token identities are not regenerated");
}

void copy_boundaries()
{
    const auto original = graph();
    const std::array group{id<NodeId>(2)};
    expect(clone_subgraph(original, group, ExternalReferencePolicy::Reject), ErrorCode::ExternalReference);
    const std::array child{id<NodeId>(3)};
    const auto copied = take(clone_subgraph(original, child, ExternalReferencePolicy::Preserve));
    check(copied.containment.size() == 1 && copied.containment.front().parent == id<NodeId>(2)
        && copied.containment.front().child == copied.identities.nodes.at(id<NodeId>(3)), "copying one child explicitly preserves its external Group parent");
    check(copied.report.preserved_references.size() == 2 && copied.report.omitted_control_links.size() == 1,
        "external input ownership, parent containment and boundary control are separately reported");
    expect(clone_subgraph(original, std::span<const NodeId>{}, ExternalReferencePolicy::Preserve), ErrorCode::InvalidModel);
    const std::array missing{id<NodeId>(99)};
    expect(clone_subgraph(original, missing, ExternalReferencePolicy::Preserve), ErrorCode::UnknownTarget);
    expect(clone_subgraph(original, group, static_cast<ExternalReferencePolicy>(255)), ErrorCode::InvalidModel);
    auto duplicate = original;
    duplicate.nodes.push_back(duplicate.nodes.front());
    expect(clone_subgraph(duplicate, group, ExternalReferencePolicy::Preserve), ErrorCode::DuplicateIdentity);

    auto source = solution();
    const auto foreign = OutputReference{id<WorkflowId>(99), id<NodeId>(88), SystemField::ExecutionStatus, {}};
    source.workflows.front().nodes.front().bindings.push_back(binding(foreign));
    expect(clone_solution(source, ExternalReferencePolicy::Reject), ErrorCode::ExternalReference);
    const auto preserved = take(clone_solution(source, ExternalReferencePolicy::Preserve));
    check(preserved.report.preserved_references.size() == 1
        && std::get<OutputReference>(preserved.report.preserved_references.front().reference) == foreign,
        "external workflow/system reference is retained verbatim with an explicit report");
    check(std::get<OutputReference>(preserved.solution.workflows.front().nodes.front().bindings.front().sources.front()) == foreign,
        "preserving an external reference does not partially remap its workflow or node");
    source.workflows.front().control_links.front().source = id<NodeId>(99);
    expect(clone_solution(source, ExternalReferencePolicy::Preserve), ErrorCode::InvalidModel);
    check(source.workflows.front().nodes.front().id == id<NodeId>(1) && source.id == id<SolutionId>(1),
        "failure does not partially assign any source identity");
    auto dangling = solution();
    auto mismatched = ref(1);
    mismatched.workflow = id<WorkflowId>(99);
    dangling.workflows.front().nodes.front().bindings.push_back(binding(mismatched));
    const auto untouched_pair = take(clone_solution(dangling, ExternalReferencePolicy::Preserve));
    check(std::get<OutputReference>(untouched_pair.solution.workflows.front().nodes.front().bindings.front().sources.front()) == mismatched,
        "an owned NodeId paired with the wrong WorkflowId is not silently repaired during copy");
    auto invalid_export = solution();
    invalid_export.workflows.front().control_exports.front().source = id<NodeId>(99);
    expect(clone_solution(invalid_export, ExternalReferencePolicy::Reject), ErrorCode::ExternalReference);
    const auto preserved_export = take(clone_solution(invalid_export, ExternalReferencePolicy::Preserve));
    check(preserved_export.solution.workflows.front().control_exports.front().source == id<NodeId>(99)
        && preserved_export.report.preserved_references.size() == 1
        && std::holds_alternative<ControlExportReference>(preserved_export.report.preserved_references.front().reference),
        "unresolved control export is preserved with a distinct typed external-reference report");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "copy_subgraph") { copy_subgraph(); }
        else if (group == "copy_solution") { copy_solution(); }
        else if (group == "copy_boundaries") { copy_boundaries(); }
        else { return 2; }
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1;
    } catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
