#include <visionpro/workflow/model.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

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
template <class T> void invalid(const Result<T>& result, std::string_view description)
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, description);
    check(error && error->severity == Severity::Error && !error->message.empty(), "invalid models return an explained error");
}
template <UuidIdentity T> T uuid(unsigned value)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template <SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
PublicInterface ports(std::string_view schema = "workflow.public")
{ return {key<SchemaId>(schema), 1, {}, {}, {}}; }
Node node(unsigned id)
{ return {uuid<NodeId>(id), "Node " + std::to_string(id), ToolConfiguration{key<ToolTypeId>("test.inspect"), 1}}; }
Workflow workflow()
{ return {uuid<WorkflowId>(1), "检验流程", ports()}; }
Solution solution()
{ return {uuid<SolutionId>(1), "检验方案", 1, 1, {workflow()}}; }
OutputReference output(const Workflow& flow, unsigned producer, std::string_view name = "value")
{ return {flow.id, uuid<NodeId>(producer), key<OutputId>(name), {}}; }
Binding constant(std::int64_t value)
{ return {key<ParameterId>("threshold"), {ConfigValue::integer(value)}}; }

static_assert(!std::is_constructible_v<BindingSource, DataValue>);
static_assert(!std::is_constructible_v<BindingTarget, OutputId>);
static_assert(!std::is_same_v<ControlLink, Containment>);
static_assert(!std::is_constructible_v<Selector, std::string>);
static_assert(std::is_same_v<decltype(Solution::variables), std::vector<GlobalVariableDefinition>>);
static_assert(std::is_same_v<decltype(Solution::queues), std::vector<DataQueueDefinition>>);
static_assert(!std::is_constructible_v<OutputReference, SessionInvocationKey>);

Solution grouped_solution()
{
    auto result = solution();
    auto& flow = result.workflows.front();
    const auto input = key<InputId>("number");
    const auto out = key<OutputId>("value");
    auto public_ports = ports("group.public");
    public_ports.inputs.push_back({input, "Number", DataType::integer()});
    public_ports.outputs.push_back({out, "Value", DataType::integer()});
    const auto complete = key<ControlOutputId>("complete");
    public_ports.control_outputs.push_back({complete, "Complete"});
    flow.interface = public_ports;
    flow.nodes = {node(1), node(2), node(3), node(4), node(5)};
    flow.nodes[0].configuration = GroupConfiguration{public_ports, {uuid<NodeId>(2)}, {{out, output(flow, 3)}},
        {{complete, uuid<NodeId>(3), complete}}};
    flow.nodes[2].configuration = GroupConfiguration{public_ports, {uuid<NodeId>(4)}, {{out, output(flow, 4)}},
        {{complete, uuid<NodeId>(4), {}}}};
    flow.nodes[0].bindings.push_back({input, {ScopedInputReference{flow.id, {}, input, {}}}});
    flow.nodes[1].bindings.push_back({input, {ScopedInputReference{flow.id, uuid<NodeId>(1), input, {}}}});
    flow.nodes[2].bindings.push_back({input, {ScopedInputReference{flow.id, uuid<NodeId>(1), input, {}}}});
    flow.nodes[3].bindings.push_back({input, {ScopedInputReference{flow.id, uuid<NodeId>(3), input, {}}}});
    flow.containment = {{uuid<NodeId>(1), uuid<NodeId>(2)}, {uuid<NodeId>(1), uuid<NodeId>(3)}, {uuid<NodeId>(3), uuid<NodeId>(4)}};
    flow.control_links = {{uuid<ControlLinkId>(1), uuid<NodeId>(2), {}, uuid<NodeId>(3)},
        {uuid<ControlLinkId>(2), uuid<NodeId>(1), {}, uuid<NodeId>(5)}};
    flow.entries = {uuid<NodeId>(1)};
    flow.outputs = {{out, output(flow, 1)}};
    flow.control_exports = {{complete, uuid<NodeId>(1), complete}};
    result.layouts = {{flow.id, {{uuid<NodeId>(1), 10.5, -12.25}, {uuid<NodeId>(5), 500, 40}}}};
    return result;
}

void model_graph()
{
    auto model = solution();
    auto& flow = model.workflows.front();
    for (unsigned index = 1; index <= 13; ++index) { flow.nodes.push_back(node(index)); }
    unsigned edge = 1;
    for (unsigned branch = 0; branch < 5; ++branch) {
        const unsigned measurement = 2 + branch, condition = 7 + branch;
        flow.control_links.push_back({uuid<ControlLinkId>(edge++), uuid<NodeId>(1), {}, uuid<NodeId>(measurement)});
        flow.control_links.push_back({uuid<ControlLinkId>(edge++), uuid<NodeId>(measurement), {}, uuid<NodeId>(condition)});
        flow.control_links.push_back({uuid<ControlLinkId>(edge++), uuid<NodeId>(condition), key<ControlOutputId>("pass"), uuid<NodeId>(12)});
        flow.nodes[measurement - 1].bindings.push_back({key<InputId>("image"), {output(flow, 1, "image")}});
        auto selected = output(flow, measurement, "measurements");
        selected.selector = {ElementSelection{0}, FieldSelection{key<FieldId>("width")}};
        flow.nodes[condition - 1].bindings.push_back({key<InputId>("measured"), {selected}});
    }
    flow.control_links.push_back({uuid<ControlLinkId>(edge), uuid<NodeId>(12), {}, uuid<NodeId>(13)});
    flow.entries = {uuid<NodeId>(1)};
    std::vector<BindingSource> measurements;
    for (unsigned producer = 2; producer <= 6; ++producer) { measurements.emplace_back(output(flow, producer, "measurements")); }
    flow.nodes[11].bindings.push_back({key<InputId>("values"), measurements, BindingCardinality::MultipleSources});
    check(ok(validate_model_shape(model)), "five measurements, five conditions, a five-predecessor join and send are representable");
    check(std::ranges::count(flow.control_links, uuid<NodeId>(1), &ControlLink::source) == 5, "one source has five independent control successors");
    check(std::ranges::count(flow.control_links, uuid<NodeId>(12), &ControlLink::target) == 5, "the join has five predecessors without dummy nodes");
    check(flow.nodes[11].join == JoinPolicy::AllActivated && flow.nodes[11].bindings.front().sources.size() == 5,
        "control join policy and ordered business inputs are separate");
    check(take(children(flow)).size() == 13, "root enumeration preserves all uncontained nodes");
    check(std::get<OutputReference>(flow.nodes[6].bindings.front().sources.front()).node == uuid<NodeId>(2),
        "data source is expressed by stable identity rather than control-link adjacency");
    flow.nodes[1].bindings.push_back(constant(5));
    flow.nodes[2].bindings.push_back(constant(9));
    flow.nodes[1].bindings.back().sources.front() = ConfigValue::integer(20);
    flow.nodes[1].enabled = false;
    check(*std::get<ConfigValue>(flow.nodes[2].bindings.back().sources.front()).as_int64() == 9 && flow.nodes[2].enabled,
        "two nodes of the same tool retain independent parameter and enable configuration");
    const auto old_reference = std::get<OutputReference>(flow.nodes[6].bindings.front().sources.front());
    flow.nodes[1].name = "改名后的测量节点";
    model.layouts = {{flow.id, {{flow.nodes[1].id, 5, 10}}}};
    model.layouts.front().nodes.front().x = 900;
    check(std::get<OutputReference>(flow.nodes[6].bindings.front().sources.front()) == old_reference,
        "renaming and layout edits do not rewrite data references");
    check(flow.control_links.front().target == uuid<NodeId>(2), "renaming preserves control references");
    flow.entries.clear();
    check(ok(validate_model_shape(model)) && flow.entries.empty(), "an editable draft does not infer entries from zero indegree");

    auto grouped = grouped_solution();
    auto& nested = grouped.workflows.front();
    check(ok(validate_model_shape(grouped)), "nested groups expose stable input/output mappings");
    check(take(children(nested)) == std::vector{uuid<NodeId>(1), uuid<NodeId>(5)}, "root enumeration excludes group descendants");
    check(take(children(nested, uuid<NodeId>(1))) == std::vector{uuid<NodeId>(2), uuid<NodeId>(3)}, "direct children exclude grandchildren");
    check(take(descendants(nested, uuid<NodeId>(1))) == std::vector{uuid<NodeId>(2), uuid<NodeId>(3), uuid<NodeId>(4)},
        "recursive enumeration retains depth-first order and hierarchy");
    const auto group_input = std::get<ScopedInputReference>(nested.nodes[3].bindings.front().sources.front());
    check(group_input.group == uuid<NodeId>(3) && group_input.workflow == nested.id, "a child references its explicit group public boundary");
    check(nested.outputs.front().source.node == uuid<NodeId>(1)
        && std::get<GroupConfiguration>(nested.nodes[0].configuration).outputs.front().source.node == uuid<NodeId>(3),
        "workflow exports traverse explicit group output mappings");
    check(nested.control_exports.front().source == uuid<NodeId>(1)
        && nested.control_exports.front().source_output == key<ControlOutputId>("complete")
        && std::get<GroupConfiguration>(nested.nodes[0].configuration).control_exports.front().source == uuid<NodeId>(3),
        "selected control outlets traverse explicit nested group public mappings");
    check(!std::get<GroupConfiguration>(nested.nodes[2].configuration).control_exports.front().source_output,
        "an unconditional successful child may feed a public control outlet without inventing a data port");
    nested.control_links.clear();
    check(take(descendants(nested, uuid<NodeId>(1))).size() == 3 && nested.nodes[3].bindings.size() == 1,
        "removing control links does not remove containment or bindings");
    check(find_workflow(grouped, nested.id) == &nested && find_node(nested, uuid<NodeId>(4)) == &nested.nodes[3],
        "lookup uses stable identities and returns the correct borrowed object");
    check(find_workflow(grouped, uuid<WorkflowId>(999)) == nullptr && find_node(nested, uuid<NodeId>(999)) == nullptr,
        "lookup does not substitute a same-name object for a missing identity");
}

void model_bindings()
{
    const auto flow = workflow();
    std::vector<BindingSource> sources{ConfigValue::integer(3), output(flow, 1),
        ScopedInputReference{flow.id, {}, key<InputId>("number"), {FieldSelection{key<FieldId>("items")}, ElementSelection{0}}},
        ResourceReference{uuid<ResourceId>(1), ResourceKind::Image, 2},
        GlobalReference{uuid<VariableId>(1), 0, {FieldSelection{key<FieldId>("width")}}}};
    const std::vector expected{BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput,
        BindingSourceKind::Resource, BindingSourceKind::GlobalRead};
    for (std::size_t index = 0; index < sources.size(); ++index) {
        check(source_kind(sources[index]) == expected[index], "each source alternative maps to its declared metadata kind");
        check(ok(validate_binding_shape({key<InputId>("input"), {sources[index]}})), "every supported source has a valid standalone shape");
    }
    for (auto field : {SystemField::ExecutionStatus, SystemField::Judgement, SystemField::Diagnostics, SystemField::Elapsed}) {
        auto reference = output(flow, 1);
        reference.output = field;
        check(ok(validate_binding_shape({key<InputId>("system"), {reference}})), "system fields remain separate from ordinary OutputId values");
    }
    Binding binding{key<ParameterId>("threshold"), {sources.front()}};
    check(ok(validate_binding_shape(binding)), "input and parameter targets use the same binding representation");
    binding.sources.clear();
    invalid(validate_binding_shape(binding), "an empty source list is not a configured binding");
    binding.sources = sources;
    invalid(validate_binding_shape(binding), "single-source bindings cannot silently discard additional sources");
    binding.cardinality = BindingCardinality::MultipleSources;
    check(ok(validate_binding_shape(binding)), "ordered multiple-source configuration is representable before type checking");
    binding.cardinality = BindingCardinality::CollectionSource;
    invalid(validate_binding_shape(binding), "a collection source is one collection, not many sources");
    binding.sources = {sources.front()};
    check(ok(validate_binding_shape(binding)), "one collection-source declaration is locally valid");
    binding.cardinality = static_cast<BindingCardinality>(99);
    invalid(validate_binding_shape(binding), "unknown binding cardinality is rejected");
    binding.cardinality = BindingCardinality::SingleSource;
    for (auto conversion : {ConversionKind::Exact, ConversionKind::WrapOptional, ConversionKind::Int64ToFloat64Explicit}) {
        binding.conversion = conversion;
        check(ok(validate_binding_shape(binding)), "recognized conversion is retained for later metadata validation");
    }
    binding.conversion = static_cast<ConversionKind>(99);
    invalid(validate_binding_shape(binding), "unknown conversion is rejected");
    binding.conversion = ConversionKind::Exact;
    binding.availability.accepted_absence = {MissingReason::NotSelected, MissingReason::Disabled};
    check(ok(validate_binding_shape(binding)), "only explicitly declared normal absence reasons are retained");
    binding.availability.accepted_absence.push_back(MissingReason::Disabled);
    invalid(validate_binding_shape(binding), "duplicate absence reasons are invalid declarations");
    binding.availability.accepted_absence = {static_cast<MissingReason>(99)};
    invalid(validate_binding_shape(binding), "unknown absence cannot stand for Pending or source failure");
    binding.availability.accepted_absence.clear();
    auto bad_system = output(flow, 1);
    bad_system.output = static_cast<SystemField>(99);
    binding.sources = {bad_system};
    invalid(validate_binding_shape(binding), "unknown system fields cannot masquerade as data output identities");
    binding.sources = {ResourceReference{uuid<ResourceId>(1), ResourceKind::Image, 0}};
    invalid(validate_binding_shape(binding), "resource binding applies the existing revision validity contract");
    auto fixed_index = output(flow, 1);
    fixed_index.selector = {FieldSelection{key<FieldId>("objects")}, ElementSelection{std::numeric_limits<std::size_t>::max()},
        FieldSelection{key<FieldId>("radius")}};
    binding.sources = {fixed_index};
    check(ok(validate_binding_shape(binding)), "fixed indices preserve exact values; runtime bounds are not guessed from configuration");
}

void model_structure()
{
    const auto original = grouped_solution();
    auto changed = original;
    changed.format_version = 0;
    invalid(validate_model_shape(changed), "solution format version must be positive");
    changed = original; changed.revision = 0;
    invalid(validate_model_shape(changed), "configuration revision must be positive");
    changed = original; changed.name.clear();
    invalid(validate_model_shape(changed), "solution display name cannot be empty");
    changed = original; changed.workflows[0].name = std::string("x\0y", 3);
    invalid(validate_model_shape(changed), "workflow names cannot contain NUL");
    changed = original; changed.workflows[0].nodes[1].name = std::string(1, static_cast<char>(0xff));
    invalid(validate_model_shape(changed), "node names require valid UTF-8");
    changed = original; changed.workflows.push_back(changed.workflows.front());
    invalid(validate_model_shape(changed), "workflow identities cannot be repeated");
    changed = original; changed.workflows[0].nodes.push_back(changed.workflows[0].nodes[1]);
    invalid(validate_model_shape(changed), "node identities cannot be repeated");
    changed = original;
    auto other = workflow(); other.id = uuid<WorkflowId>(2); other.nodes.push_back(changed.workflows[0].nodes[1]);
    changed.workflows.push_back(other);
    invalid(validate_model_shape(changed), "node identities are unique across the solution, not merely within one workflow");
    changed = original; changed.workflows[0].control_links.push_back(changed.workflows[0].control_links.front());
    invalid(validate_model_shape(changed), "control-link identities cannot be repeated");
    changed = original; std::get<ToolConfiguration>(changed.workflows[0].nodes[1].configuration).config_version = 0;
    invalid(validate_model_shape(changed), "node tool configuration version must be positive");
    changed = original; changed.workflows[0].nodes[1].join = static_cast<JoinPolicy>(99);
    invalid(validate_model_shape(changed), "unknown join policy is rejected");
    changed = original; changed.workflows[0].nodes[1].bindings.push_back(changed.workflows[0].nodes[1].bindings.front());
    invalid(validate_model_shape(changed), "a target has one binding rather than two editable source definitions");
    changed = original;
    auto& resources = changed.workflows[0].nodes[1].resources;
    resources.push_back({key<SchemaId>("camera"), {uuid<ResourceId>(1), ResourceKind::Device, {}}});
    resources.push_back(resources.front());
    invalid(validate_model_shape(changed), "resource slots cannot be assigned twice");
    changed = original; changed.workflows[0].containment.push_back({uuid<NodeId>(3), uuid<NodeId>(2)});
    invalid(validate_model_shape(changed), "a node cannot belong to two parents");
    changed = original; changed.workflows[0].containment[0].parent = uuid<NodeId>(5);
    invalid(validate_model_shape(changed), "only groups can contain nodes");
    changed = original; changed.workflows[0].containment[0].parent = uuid<NodeId>(999);
    invalid(validate_model_shape(changed), "containment cannot name an absent parent");
    changed = original; changed.workflows[0].containment[0].child = uuid<NodeId>(999);
    invalid(validate_model_shape(changed), "containment cannot name an absent child");
    changed = original; changed.workflows[0].containment.push_back({uuid<NodeId>(3), uuid<NodeId>(1)});
    invalid(validate_model_shape(changed), "containment cycles are invalid even before graph dependency validation");
    invalid(descendants(changed.workflows[0], uuid<NodeId>(1)), "descendant enumeration diagnoses a cycle instead of looping");
    changed = original; changed.workflows[0].containment.push_back({uuid<NodeId>(1), uuid<NodeId>(1)});
    invalid(validate_model_shape(changed), "self-containment is invalid");
    invalid(children(original.workflows[0], uuid<NodeId>(999)), "enumeration rejects an unknown group");
    invalid(descendants(original.workflows[0], uuid<NodeId>(2)), "recursive enumeration does not treat an ordinary tool as a group");
    changed = original; changed.workflows[0].entries = {uuid<NodeId>(999)};
    invalid(validate_model_shape(changed), "an explicitly selected entry must exist");
    changed = original; changed.workflows[0].entries = {uuid<NodeId>(2)};
    invalid(validate_model_shape(changed), "workflow entries cannot cross directly into a group");
    changed = original; changed.workflows[0].entries.push_back(changed.workflows[0].entries.front());
    invalid(validate_model_shape(changed), "an entry identity cannot be selected twice");
    changed = original; std::get<GroupConfiguration>(changed.workflows[0].nodes[0].configuration).entries = {uuid<NodeId>(4)};
    invalid(validate_model_shape(changed), "group entries must be direct children rather than arbitrary descendants");
    changed = original; changed.workflows[0].outputs[0].output = key<OutputId>("undeclared");
    invalid(validate_model_shape(changed), "an export must name its declared public output");
    changed = original; changed.workflows[0].outputs.push_back(changed.workflows[0].outputs.front());
    invalid(validate_model_shape(changed), "public outputs cannot have duplicate editable mappings");
    changed = original; changed.workflows[0].control_exports.front().output = key<ControlOutputId>("undeclared");
    invalid(validate_model_shape(changed), "a workflow control export must target a declared public control outlet");
    changed = original; changed.workflows[0].control_exports.push_back(changed.workflows[0].control_exports.front());
    invalid(validate_model_shape(changed), "workflow control export targets cannot have duplicate mappings");
    changed = original;
    std::get<GroupConfiguration>(changed.workflows[0].nodes[0].configuration).control_exports.front().output = key<ControlOutputId>("undeclared");
    invalid(validate_model_shape(changed), "a group control export must target its own declared control interface");
    changed = original;
    auto& controls = std::get<GroupConfiguration>(changed.workflows[0].nodes[0].configuration).control_exports;
    controls.push_back(controls.front());
    invalid(validate_model_shape(changed), "group control export targets cannot have duplicate mappings");
    changed = original; changed.workflows[0].control_exports.front().source = uuid<NodeId>(999);
    check(ok(validate_model_shape(changed)), "control export source existence and boundary validation remains explicit S2.5 work");
    changed = original; changed.layouts.front().workflow = uuid<WorkflowId>(999);
    invalid(validate_model_shape(changed), "layout has an explicit existing workflow owner");
    changed = original; changed.layouts.front().nodes.front().node = uuid<NodeId>(999);
    invalid(validate_model_shape(changed), "layout cannot claim an absent node");
    changed = original; changed.layouts.front().nodes.front().x = std::numeric_limits<double>::infinity();
    invalid(validate_model_shape(changed), "layout coordinates must be finite");
    changed = original; changed.layouts.front().nodes.push_back(changed.layouts.front().nodes.front());
    invalid(validate_model_shape(changed), "node placements cannot be duplicated");
    auto public_schema = ports();
    public_schema.version = 0;
    invalid(validate_public_interface(public_schema), "public Schema version must be positive");
    public_schema = ports();
    public_schema.inputs.push_back({key<InputId>("value"), "Value", DataType::integer()});
    check(ok(validate_public_interface(public_schema)), "required public input is a declaration without a configured value");
    public_schema.inputs.push_back(public_schema.inputs.front());
    invalid(validate_public_interface(public_schema), "public input identities must be unique");
    public_schema.inputs.pop_back(); public_schema.inputs.front().required = false;
    invalid(validate_public_interface(public_schema), "a non-required public input needs an explicit default");
    public_schema.inputs.front().default_value = ConfigValue::boolean(false);
    invalid(validate_public_interface(public_schema), "public defaults must have the declared type");
    public_schema.inputs.front().default_value = ConfigValue::integer(2);
    check(ok(validate_public_interface(public_schema)), "valid public defaults retain core metadata semantics");
    public_schema.outputs.push_back({key<OutputId>("value"), "Value", DataType::integer()});
    public_schema.control_outputs.push_back({key<ControlOutputId>("value"), "Control"});
    check(ok(validate_public_interface(public_schema)), "data and control output namespaces are distinct even with equal token text");
    public_schema.outputs.push_back(public_schema.outputs.front());
    invalid(validate_public_interface(public_schema), "data output identity duplicates are rejected");
    public_schema.outputs.pop_back(); public_schema.control_outputs.push_back(public_schema.control_outputs.front());
    invalid(validate_public_interface(public_schema), "control output identity duplicates are rejected");
    changed = original;
    changed.workflows[0].control_links.push_back({uuid<ControlLinkId>(99), uuid<NodeId>(5), {}, uuid<NodeId>(1)});
    check(ok(validate_model_shape(changed)), "local model validation does not pretend to certify control/data acyclicity");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "model_graph") { model_graph(); }
        else if (group == "model_bindings") { model_bindings(); }
        else if (group == "model_structure") { model_structure(); }
        else { return 2; }
    } catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
