#include <visionpro/workflow/validation.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
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
template <class T> bool ok(const Result<T>& result) { return std::holds_alternative<T>(result); }
template <class T> void rejected(const Result<T>& result, std::string_view description,
    std::optional<NodeId> node = {})
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, description);
    check(error && error->severity == Severity::Error && !error->message.empty(), "rejection explains the error");
    if (node) { check(error && error->location.node == node, "rejection identifies the affected node"); }
}
template <UuidIdentity T> T uuid(unsigned value)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template <SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
InputDescriptor input(DataType type = DataType::integer())
{
    InputDescriptor result{key<InputId>("data"), "Data", type, false, ConfigValue::integer(0)};
    result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput,
        BindingSourceKind::PublicInput, BindingSourceKind::Resource, BindingSourceKind::GlobalRead};
    return result;
}
ToolDescriptor tool()
{
    return {key<ToolTypeId>("test.scalar"), "Scalar", 1,
        {key<SchemaId>("test.scalar.schema"), 1, {}, true}, {}, {input()},
        {{key<OutputId>("value"), "Value", DataType::integer()}},
        {{key<ControlOutputId>("complete"), "Complete"}}};
}
PublicInterface ports(std::string_view schema)
{ return {key<SchemaId>(schema), 1, {}, {}, {}}; }
Node node(unsigned id)
{ return {uuid<NodeId>(id), "Node " + std::to_string(id), ToolConfiguration{key<ToolTypeId>("test.scalar"), 1}}; }
struct Fixture {
    Solution solution;
    std::vector<ToolDescriptor> tools;
    Workflow& flow() { return solution.workflows.front(); }
    Node& at(unsigned id) { return *std::ranges::find(flow().nodes, uuid<NodeId>(id), &Node::id); }
    void edge(unsigned source, unsigned target, bool named = false)
    {
        flow().control_links.push_back({uuid<ControlLinkId>(static_cast<unsigned>(flow().control_links.size()) + 1),
            uuid<NodeId>(source), named ? std::optional{key<ControlOutputId>("complete")} : std::nullopt, uuid<NodeId>(target)});
    }
    OutputReference output(unsigned source, Selector selector = {})
    { return {flow().id, uuid<NodeId>(source), key<OutputId>("value"), std::move(selector)}; }
    void read(unsigned consumer, unsigned producer, Selector selector = {})
    { at(consumer).bindings = {{key<InputId>("data"), {output(producer, std::move(selector))}}}; }
    Result<ValidationIndex> validate() const { return validate_solution(solution, tools); }
};
Fixture fixture(unsigned count = 2)
{
    Fixture result{{uuid<SolutionId>(1), "Validation", 1, 1,
        {{uuid<WorkflowId>(1), "Workflow", ports("workflow.public")}}}, {tool()}};
    for (unsigned id = 1; id <= count; ++id) { result.flow().nodes.push_back(node(id)); }
    if (count != 0) { result.flow().entries = {uuid<NodeId>(1)}; }
    return result;
}
const ScopeDependencies& root_scope(const ValidationIndex& index)
{
    const auto found = std::ranges::find_if(index.scopes, [](const auto& scope) { return !scope.group; });
    if (found == index.scopes.end()) { throw std::runtime_error("Missing root scope index"); }
    return *found;
}

void validation_graph()
{
    auto five = fixture(13);
    for (unsigned branch = 2; branch <= 6; ++branch) {
        five.edge(1, branch); five.edge(branch, branch + 5, true); five.edge(branch + 5, 12);
    }
    five.edge(12, 13);
    five.read(3, 2); // Independent control branches may have a data ordering.
    five.read(13, 1); // Non-adjacent producer, with intervening fork/join.
    const auto original_nodes = five.flow().nodes.size();
    const auto original_links = five.flow().control_links;
    auto checked = five.validate();
    check(ok(checked), "five branches, named conditions, many-input join and sibling dependency are legal");
    if (ok(checked)) {
        const auto& index = root_scope(std::get<ValidationIndex>(checked));
        check(index.reachable.size() == 13, "all explicitly activated branches are reachable");
        check(index.control_links.size() == 16, "all control links are retained in the derived index");
        check(index.data.size() == 2, "only configured data dependencies are derived");
        check(std::ranges::any_of(index.data, [](const auto& dependency) {
            return dependency.producer == uuid<NodeId>(2) && dependency.consumer == uuid<NodeId>(3)
                && dependency.target == BindingTarget{key<InputId>("data")} && dependency.source_index == 0;
        }), "dependency identifies producer, consumer and binding source");
    }
    check(five.flow().nodes.size() == original_nodes && five.flow().control_links.size() == original_links.size(),
        "validation preserves editable graph sizes");
    for (std::size_t i = 0; i < original_links.size(); ++i) {
        check(five.flow().control_links[i].id == original_links[i].id
            && five.flow().control_links[i].source == original_links[i].source
            && five.flow().control_links[i].target == original_links[i].target,
            "validation never rewrites explicit control order");
    }
    auto changed = five; changed.edge(13, 1);
    rejected(changed.validate(), "ordinary back edge is rejected");
    changed = five; changed.edge(1, 1);
    rejected(changed.validate(), "self control edge is rejected");
    changed = five; changed.read(1, 2);
    rejected(changed.validate(), "direct reverse dependency is rejected", uuid<NodeId>(1));
    changed = five; changed.read(1, 13);
    rejected(changed.validate(), "transitive reverse dependency is rejected", uuid<NodeId>(1));
    changed = five; changed.read(2, 3);
    rejected(changed.validate(), "data-only cycle between sibling branches is rejected");

    auto mixed = fixture(5);
    mixed.edge(1, 2); mixed.edge(1, 4); mixed.edge(2, 3); mixed.edge(4, 5);
    mixed.read(4, 3); mixed.read(2, 5);
    rejected(mixed.validate(), "alternating control/data dependency cycle is rejected");
    mixed.at(2).bindings.clear();
    check(ok(mixed.validate()), "removing the cycle permits valid cross-branch ordering");

    auto partial = fixture(3); partial.edge(1, 2);
    auto partial_result = partial.validate();
    check(ok(partial_result), "unconnected editable draft may remain outside the execution range");
    if (ok(partial_result)) {
        const auto& scope = root_scope(std::get<ValidationIndex>(partial_result));
        check(scope.reachable.size() == 2 && std::ranges::find(scope.reachable, uuid<NodeId>(3)) == scope.reachable.end(),
            "zero-indegree draft is not implicitly activated");
    }
    changed = partial; changed.edge(3, 2);
    rejected(changed.validate(), "external predecessor entering reachable join is rejected");
    changed = partial; changed.edge(3, 1);
    rejected(changed.validate(), "entry permission cannot bypass a predecessor outside the range");
    changed = partial; changed.read(2, 3);
    rejected(changed.validate(), "binding cannot activate a disconnected producer", uuid<NodeId>(2));
    changed = partial; changed.flow().entries.push_back(uuid<NodeId>(3)); changed.edge(3, 2);
    check(ok(changed.validate()), "explicit additional entry makes the predecessor participate");
    changed = partial; changed.flow().entries.push_back(uuid<NodeId>(2));
    check(ok(changed.validate()), "entry with a reachable predecessor is legal");
    changed = partial; changed.flow().control_links[0].target = uuid<NodeId>(99);
    auto dangling = changed.validate();
    rejected(dangling, "dangling control endpoint is rejected");
    if (const auto* error = std::get_if<Diagnostic>(&dangling)) {
        check(error->location.solution == changed.solution.id && error->location.workflow == changed.flow().id
            && error->location.control_link == changed.flow().control_links[0].id, "control errors retain graph and link identity");
    }
    changed = partial; changed.flow().control_links[0].output = key<ControlOutputId>("missing");
    rejected(changed.validate(), "undeclared control outlet is rejected");
    changed = partial; changed.read(3, 99);
    rejected(changed.validate(), "disconnected drafts still undergo reference checks", uuid<NodeId>(3));
}

void validation_bindings()
{
    auto base = fixture(); base.edge(1, 2); base.read(2, 1);
    check(ok(base.validate()), "ordinary output binding is valid");
    auto changed = base;
    std::get<OutputReference>(changed.at(2).bindings[0].sources[0]).output = key<OutputId>("missing");
    auto missing_output = changed.validate();
    rejected(missing_output, "deleted output cannot silently become default", uuid<NodeId>(2));
    if (const auto* error = std::get_if<Diagnostic>(&missing_output)) {
        check(error->location.input == key<InputId>("data"), "binding error identifies its target input");
    }
    changed = base; changed.at(2).bindings[0].target = key<InputId>("missing");
    rejected(changed.validate(), "unknown target input is rejected", uuid<NodeId>(2));
    changed = base; changed.at(2).bindings[0].target = key<ParameterId>("data");
    rejected(changed.validate(), "input token cannot be used as a parameter identity", uuid<NodeId>(2));
    changed = base; changed.tools[0].inputs[0].binding.allowed_sources = {BindingSourceKind::Constant};
    rejected(changed.validate(), "metadata source restriction is enforced", uuid<NodeId>(2));
    changed = base; changed.at(2).bindings[0].cardinality = BindingCardinality::MultipleSources;
    rejected(changed.validate(), "binding cardinality must match metadata", uuid<NodeId>(2));
    changed = base; changed.at(2).bindings[0].sources = {ConfigValue::boolean(true)};
    rejected(changed.validate(), "wrong constant type is rejected", uuid<NodeId>(2));
    changed = base; changed.tools[0].inputs[0].constraints.integer_range = IntegerRange{0, 10};
    changed.at(2).bindings[0].sources = {ConfigValue::integer(11)};
    rejected(changed.validate(), "known constant range failure is diagnosed statically", uuid<NodeId>(2));

    auto record = take(DataType::record(key<SchemaId>("test.record"), 1,
        {{key<FieldId>("numbers"), DataType::list(DataType::integer())}}));
    changed = base; changed.tools[0].outputs[0].type = record;
    changed.read(2, 1, {FieldSelection{key<FieldId>("numbers")}, ElementSelection{99}});
    check(ok(changed.validate()), "record field then fixed list index derives its type without guessing runtime length");
    auto bad_field = changed;
    std::get<OutputReference>(bad_field.at(2).bindings[0].sources[0]).selector[0] = FieldSelection{key<FieldId>("absent")};
    auto bad_field_result = bad_field.validate();
    rejected(bad_field_result, "missing record field is rejected", uuid<NodeId>(2));
    if (const auto* error = std::get_if<Diagnostic>(&bad_field_result)) {
        check(!error->location.path.empty(), "field error retains structured selector path");
    }
    changed = base; changed.read(2, 1, {ElementSelection{0}});
    rejected(changed.validate(), "list selector on scalar is rejected", uuid<NodeId>(2));
    changed = base; changed.read(2, 1, {FieldSelection{key<FieldId>("numbers")}});
    rejected(changed.validate(), "field selector on scalar is rejected", uuid<NodeId>(2));
    changed = base; changed.tools[0].outputs[0].type = DataType::list(record);
    changed.read(2, 1, {ElementSelection{0}, FieldSelection{key<FieldId>("numbers")}, ElementSelection{0}});
    check(ok(changed.validate()), "list element then record field may be composed");
    changed.tools[0].outputs[0].type = DataType::optional(record);
    changed.read(2, 1, {FieldSelection{key<FieldId>("numbers")}, ElementSelection{0}});
    rejected(changed.validate(), "selector cannot implicitly unwrap Optional", uuid<NodeId>(2));

    changed = base;
    changed.tools[0].inputs[0].type = DataType::optional(DataType::integer());
    changed.tools[0].inputs[0].default_value = ConfigValue::none(DataType::integer());
    changed.at(2).bindings[0].conversion = ConversionKind::WrapOptional;
    check(ok(changed.validate()), "explicit allowed Optional wrapping is valid");
    changed.at(2).bindings[0].availability.accepted_absence = {MissingReason::Disabled};
    changed.at(1).enabled = false;
    check(ok(changed.validate()), "Optional consumer may explicitly accept disabled producer absence");
    changed = base; changed.at(2).bindings[0].availability.accepted_absence = {MissingReason::Disabled};
    rejected(changed.validate(), "absence policy does not invent a value for plain Int64", uuid<NodeId>(2));
    changed = base;
    changed.tools[0].inputs[0].type = DataType::optional(DataType::integer());
    changed.tools[0].inputs[0].required = true; changed.tools[0].inputs[0].default_value.reset();
    changed.at(1).bindings = {{key<InputId>("data"), {ConfigValue::none(DataType::integer())}}};
    changed.at(2).bindings.clear();
    rejected(changed.validate(), "required Optional still needs a binding", uuid<NodeId>(2));
    changed.at(2).bindings = {{key<InputId>("data"), {ConfigValue::none(DataType::integer())}}};
    check(ok(changed.validate()), "explicit typed None satisfies a required Optional binding");

    changed = base; changed.tools[0].inputs[0].type = DataType::real();
    changed.tools[0].inputs[0].default_value = take(ConfigValue::real(0));
    rejected(changed.validate(), "Int64 to Float64 is not an implicit exact conversion", uuid<NodeId>(2));
    changed.at(2).bindings[0].conversion = ConversionKind::Int64ToFloat64Explicit;
    rejected(changed.validate(), "explicit conversion still needs metadata permission", uuid<NodeId>(2));
    changed.tools[0].inputs[0].binding.allowed_conversions.push_back(ConversionKind::Int64ToFloat64Explicit);
    check(ok(changed.validate()), "declared explicit numeric conversion is valid");
    changed = base; changed.tools[0].inputs[0].type = DataType::list(DataType::integer());
    changed.tools[0].inputs[0].default_value = take(ConfigValue::list(DataType::integer(), {}));
    changed.tools[0].inputs[0].binding.cardinality = BindingCardinality::MultipleSources;
    changed.at(2).bindings[0].cardinality = BindingCardinality::MultipleSources;
    changed.at(2).bindings[0].sources = {changed.output(1), ConfigValue::integer(2)};
    check(ok(changed.validate()), "multiple-source list validates every element source");
    changed.at(2).bindings[0].sources.push_back(ConfigValue::boolean(false));
    rejected(changed.validate(), "bad later source in a list cannot be skipped", uuid<NodeId>(2));
    changed.at(2).bindings[0].sources = {changed.output(1), changed.output(1)};
    changed.tools[0].inputs[0].constraints.max_length = 1;
    rejected(changed.validate(), "two required runtime sources cannot fit a list limited to one element", uuid<NodeId>(2));
    changed.tools[0].inputs[0].constraints.max_length = 2;
    check(ok(changed.validate()), "runtime source count exactly at the declared list limit is legal");
    changed.at(2).bindings[0].sources = {changed.output(1), ConfigValue::integer(2)};
    changed.tools[0].inputs[0].constraints.max_length = 1;
    rejected(changed.validate(), "mixed constant and runtime sources still have a known minimum list length", uuid<NodeId>(2));
    changed.tools[0].inputs[0].constraints.max_length = 2;
    check(ok(changed.validate()), "mixed source count exactly at the list limit is legal");
    changed = base; changed.tools[0].inputs[0].type = DataType::list(DataType::integer());
    changed.tools[0].inputs[0].default_value = take(ConfigValue::list(DataType::integer(), {}));
    changed.tools[0].inputs[0].binding.cardinality = BindingCardinality::CollectionSource;
    changed.tools[0].outputs[0].type = DataType::list(DataType::integer());
    changed.at(2).bindings[0].cardinality = BindingCardinality::CollectionSource;
    check(ok(changed.validate()), "collection source preserves a list as one source");

    changed = base;
    ParameterDescriptor parameter{key<ParameterId>("threshold"), "Threshold", DataType::integer(), false, ConfigValue::integer(0)};
    changed.tools[0].parameters = {parameter};
    changed.at(2).bindings = {{key<ParameterId>("threshold"), {changed.output(1)}}};
    rejected(changed.validate(), "non-subscribable parameter rejects output source", uuid<NodeId>(2));
    changed.tools[0].parameters[0].subscribable = true;
    changed.tools[0].parameters[0].binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    check(ok(changed.validate()), "declared parameter subscription shares input binding checks");

    changed = base;
    changed.solution.variables.push_back({uuid<VariableId>(1), "Counter", DataType::integer(), ConfigValue::integer(0)});
    changed.at(2).bindings[0].sources = {GlobalReference{uuid<VariableId>(1), 2, {}}};
    check(ok(changed.validate()), "declared global type is checked without reading its current revision");
    std::get<GlobalReference>(changed.at(2).bindings[0].sources[0]).variable = uuid<VariableId>(99);
    rejected(changed.validate(), "dangling global reference is rejected", uuid<NodeId>(2));
    changed = base; changed.flow().interface.inputs = {input()};
    changed.at(2).bindings[0].sources = {ScopedInputReference{changed.flow().id, {}, key<InputId>("data"), {}}};
    check(ok(changed.validate()), "workflow public input can feed nodes in its immediate scope");
    std::get<ScopedInputReference>(changed.at(2).bindings[0].sources[0]).input = key<InputId>("absent");
    rejected(changed.validate(), "dangling public input is rejected", uuid<NodeId>(2));

    changed = base; changed.tools[0].outputs[0].semantics.unit = MeasurementUnit::Pixel;
    changed.tools[0].inputs[0].semantics.unit = MeasurementUnit::Millimetre;
    rejected(changed.validate(), "equal scalar types do not permit implicit unit conversion", uuid<NodeId>(2));
    changed.tools[0].inputs[0].semantics.unit = MeasurementUnit::Pixel;
    check(ok(changed.validate()), "matching explicit units preserve scalar compatibility");

    auto geometry = fixture(1);
    const auto first_frame = key<CoordinateSystemId>("test.first_frame");
    const auto second_frame = key<CoordinateSystemId>("test.second_frame");
    geometry.solution.coordinates = {
        {first_frame, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded},
        {second_frame, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
    auto point_input = input(DataType::point2d());
    point_input.default_value = take(ConfigValue::point2d({first_frame, MeasurementUnit::Pixel, 2, 3}));
    point_input.semantics = {MeasurementUnit::Pixel, second_frame};
    geometry.tools[0].inputs = {point_input};
    rejected(geometry.validate(), "tool default geometry cannot bypass declared target coordinate semantics");
    geometry.tools[0].inputs[0].semantics.coordinate_system = first_frame;
    check(ok(geometry.validate()), "tool default geometry with matching coordinate semantics is legal");
    geometry.tools[0].inputs = {input()};
    geometry.flow().interface.inputs = {point_input};
    rejected(geometry.validate(), "public input default geometry must match its declared coordinate frame");
    geometry.flow().interface.inputs[0].semantics.coordinate_system = first_frame;
    check(ok(geometry.validate()), "public input default geometry with matching coordinates is legal");

    ResourceDefinition resource{uuid<ResourceId>(1), "Input", ResourceKind::Image,
        ManagedResource{{PathBase::SolutionDirectory, "resources/input.png", {}}, 3, std::string(64, 'a')}};
    ResourceReference reference{resource.id, resource.kind, 3};
    changed = base; changed.solution.resources = {resource};
    changed.tools[0].inputs[0].type = DataType::resource_reference();
    changed.tools[0].inputs[0].default_value = take(ConfigValue::resource_reference(reference));
    changed.at(2).bindings[0].sources = {reference};
    check(ok(changed.validate()), "resource source checks catalog identity, category and pinned revision without opening files");
    std::get<ResourceReference>(changed.at(2).bindings[0].sources[0]).revision = 4;
    rejected(changed.validate(), "resource source cannot substitute a different pinned revision", uuid<NodeId>(2));
    std::get<ResourceReference>(changed.at(2).bindings[0].sources[0]) = {resource.id, ResourceKind::Calibration, 3};
    rejected(changed.validate(), "resource source category mismatch is rejected", uuid<NodeId>(2));
    std::get<ResourceReference>(changed.at(2).bindings[0].sources[0]) = {uuid<ResourceId>(99), resource.kind, 3};
    rejected(changed.validate(), "dangling resource source is rejected", uuid<NodeId>(2));
    changed = base; changed.solution.resources = {resource};
    changed.tools[0].resources = {{key<SchemaId>("image.slot"), ResourceKind::Image, true}};
    changed.at(1).resources = {{key<SchemaId>("image.slot"), reference}};
    changed.at(2).resources = changed.at(1).resources;
    check(ok(changed.validate()), "declared required resource slots are checked against the catalog");
    changed.at(2).resources.clear();
    rejected(changed.validate(), "missing required resource slot is rejected", uuid<NodeId>(2));
    changed.at(2).resources = {{key<SchemaId>("unknown.slot"), reference}};
    rejected(changed.validate(), "undeclared resource slot is rejected", uuid<NodeId>(2));

    for (const auto field : {SystemField::ExecutionStatus, SystemField::Judgement, SystemField::Diagnostics, SystemField::Elapsed}) {
        auto system = fixture(); system.edge(1, 2);
        auto consumer = tool(); consumer.id = key<ToolTypeId>("test.system"); consumer.schema.id = key<SchemaId>("test.system.schema");
        consumer.inputs[0].type = take(system_field_type(field)); consumer.inputs[0].required = true;
        consumer.inputs[0].default_value.reset();
        system.tools.push_back(consumer); system.at(2).configuration = ToolConfiguration{consumer.id, 1};
        system.at(2).bindings = {{key<InputId>("data"), {OutputReference{system.flow().id, uuid<NodeId>(1), field, {}}}}};
        check(ok(system.validate()), "system result field uses its published static Schema");
    }
    rejected(system_field_type(static_cast<SystemField>(99)), "unknown system field has no fabricated Schema");
}

Fixture grouped_fixture()
{
    auto result = fixture(5);
    auto public_ports = ports("group.public");
    public_ports.inputs = {input()};
    public_ports.outputs = {{key<OutputId>("value"), "Value", DataType::integer()}};
    public_ports.control_outputs = {{key<ControlOutputId>("complete"), "Complete"}};
    result.flow().interface = public_ports;
    result.at(1).configuration = GroupConfiguration{public_ports, {uuid<NodeId>(3)},
        {{key<OutputId>("value"), result.output(4)}},
        {{key<ControlOutputId>("complete"), uuid<NodeId>(4), key<ControlOutputId>("complete")}}};
    result.at(4).configuration = GroupConfiguration{public_ports, {uuid<NodeId>(5)},
        {{key<OutputId>("value"), result.output(5)}},
        {{key<ControlOutputId>("complete"), uuid<NodeId>(5), key<ControlOutputId>("complete")}}};
    result.flow().containment = {{uuid<NodeId>(1), uuid<NodeId>(3)}, {uuid<NodeId>(1), uuid<NodeId>(4)},
        {uuid<NodeId>(4), uuid<NodeId>(5)}};
    result.edge(1, 2); result.edge(3, 4);
    result.at(1).bindings = {{key<InputId>("data"), {ScopedInputReference{result.flow().id, {}, key<InputId>("data"), {}}}}};
    result.at(3).bindings = {{key<InputId>("data"), {ScopedInputReference{result.flow().id, uuid<NodeId>(1), key<InputId>("data"), {}}}}};
    result.at(4).bindings = result.at(3).bindings;
    result.at(5).bindings = {{key<InputId>("data"), {ScopedInputReference{result.flow().id, uuid<NodeId>(4), key<InputId>("data"), {}}}}};
    result.read(2, 1);
    result.flow().outputs = {{key<OutputId>("value"), result.output(2)}};
    result.flow().control_exports = {{key<ControlOutputId>("complete"), uuid<NodeId>(2), key<ControlOutputId>("complete")}};
    return result;
}

void validation_groups()
{
    const auto original = grouped_fixture();
    auto checked = original.validate();
    check(ok(checked), "nested Group inputs, data exports and control exports are valid through public boundaries");
    if (ok(checked)) {
        const auto& index = std::get<ValidationIndex>(checked);
        check(index.scopes.size() == 3, "nested Groups have distinct scope indexes");
        check(root_scope(index).reachable.size() == 2, "Group internals are not flattened into root activation range");
    }
    auto changed = original; changed.read(2, 5);
    rejected(changed.validate(), "outer consumer cannot read nested internal result", uuid<NodeId>(2));
    changed = original; changed.read(5, 3);
    rejected(changed.validate(), "inner consumer cannot bypass the Group input boundary", uuid<NodeId>(5));
    changed = original; changed.edge(2, 3);
    rejected(changed.validate(), "control links cannot cross Group boundary");
    changed = original;
    std::get<ScopedInputReference>(changed.at(5).bindings[0].sources[0]).group = uuid<NodeId>(1);
    rejected(changed.validate(), "scoped input cannot skip its immediate Group boundary", uuid<NodeId>(5));
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).outputs.clear();
    rejected(changed.validate(), "public Group data outputs require complete mappings", uuid<NodeId>(1));
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).control_exports.clear();
    rejected(changed.validate(), "public Group control outputs require complete mappings", uuid<NodeId>(1));
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).outputs[0].source = changed.output(5);
    rejected(changed.validate(), "Group export must come from its immediate internal scope", uuid<NodeId>(1));
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).outputs[0].source = changed.output(2);
    rejected(changed.validate(), "Group export cannot point outside its region", uuid<NodeId>(1));
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).interface.outputs[0].type = DataType::boolean();
    rejected(changed.validate(), "Group output export type mismatch is rejected");
    changed = original;
    std::get<GroupConfiguration>(changed.at(1).configuration).control_exports[0].source_output = key<ControlOutputId>("unknown");
    rejected(changed.validate(), "Group control export must identify a declared source outlet", uuid<NodeId>(1));
    changed = original;
    changed.flow().control_links.erase(changed.flow().control_links.begin() + 1);
    rejected(changed.validate(), "export from unreachable Group child is rejected", uuid<NodeId>(1));
    changed = original; changed.flow().outputs.clear();
    rejected(changed.validate(), "workflow public data exports must be complete");
    changed = original; changed.flow().control_exports.clear();
    rejected(changed.validate(), "workflow public control exports must be complete");
    changed = original; changed.flow().outputs[0].source = changed.output(3);
    rejected(changed.validate(), "workflow public export cannot penetrate Group internals");
    changed = original;
    auto other = Workflow{uuid<WorkflowId>(2), "Other", ports("other.public")};
    other.nodes = {node(99)}; other.entries = {uuid<NodeId>(99)};
    changed.solution.workflows.push_back(other);
    std::get<OutputReference>(changed.at(2).bindings[0].sources[0]).workflow = other.id;
    std::get<OutputReference>(changed.at(2).bindings[0].sources[0]).node = uuid<NodeId>(99);
    rejected(changed.validate(), "ordinary output binding cannot cross workflows", uuid<NodeId>(2));
}

void validation_schema()
{
    auto base = fixture(); base.edge(1, 2);
    auto changed = base; changed.tools.clear();
    rejected(changed.validate(), "missing tool metadata is reported");
    changed = base; changed.tools.push_back(changed.tools[0]);
    rejected(changed.validate(), "duplicate registered tool identity is rejected");
    changed = base; std::get<ToolConfiguration>(changed.at(2).configuration).config_version = 2;
    rejected(changed.validate(), "node config must match the registered tool version", uuid<NodeId>(2));
    changed = base; changed.tools[0].schema.complete = false;
    rejected(changed.validate(), "incomplete tool Schema cannot certify configured nodes");
    changed = base; changed.tools[0].schema.required_capabilities = {key<CapabilityId>("records")};
    std::vector<SchemaSupport> support{{changed.tools[0].schema.id, 1, 1, {key<CapabilityId>("records")}}};
    check(ok(validate_solution(changed.solution, changed.tools, support)), "explicit backend Schema support with capabilities is accepted");
    support[0].capabilities.clear();
    rejected(validate_solution(changed.solution, changed.tools, support), "missing explicit backend capability is diagnosed");
    support[0].capabilities = {key<CapabilityId>("records")}; support[0].minimum_version = 2; support[0].maximum_version = 3;
    rejected(validate_solution(changed.solution, changed.tools, support), "unsupported backend Schema version is rejected");
    support[0].id = key<SchemaId>("different.schema");
    rejected(validate_solution(changed.solution, changed.tools, support), "explicit support cannot substitute an unrelated Schema identity");
    check(ok(changed.validate()), "metadata-only validation does not invent a backend support assertion");

    changed = base;
    auto producer = tool(); producer.id = key<ToolTypeId>("test.producer"); producer.schema.id = key<SchemaId>("test.producer.schema");
    producer.outputs[0].type = take(DataType::record(key<SchemaId>("shared.record"), 1,
        {{key<FieldId>("x"), DataType::integer()}}));
    changed.tools.push_back(producer); changed.at(1).configuration = ToolConfiguration{producer.id, 1};
    changed.tools[0].outputs[0].type = take(DataType::record(key<SchemaId>("shared.record"), 1,
        {{key<FieldId>("x"), DataType::boolean()}}));
    rejected(changed.validate(), "same record identity/version cannot have conflicting shapes across tools");
    changed.tools[0].outputs[0].type = producer.outputs[0].type;
    check(ok(changed.validate()), "identical record definitions may be shared across tools");
    changed.flow().interface.inputs = {input(producer.outputs[0].type)};
    changed.flow().interface.inputs[0].required = true; changed.flow().interface.inputs[0].default_value.reset();
    check(ok(changed.validate()), "matching public Schema participates without requiring a configured runtime input");
    changed.flow().interface.inputs[0].type = take(DataType::record(key<SchemaId>("shared.record"), 1,
        {{key<FieldId>("x"), DataType::real()}}));
    rejected(changed.validate(), "public interface cannot redefine a registered record Schema");

    changed = base;
    const auto message_schema = take(DataType::record(key<SchemaId>("queue.message"), 1,
        {{key<FieldId>("value"), DataType::integer()}}));
    changed.tools[0].outputs[0].type = message_schema;
    changed.solution.queues = {{uuid<QueueId>(1), "Messages",
        take(DataType::record(key<SchemaId>("queue.message"), 1, {{key<FieldId>("value"), DataType::boolean()}})),
        4, QueueOverflowPolicy::Reject,
        {EmptyQueuePolicy::Fail, 1, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::UpToLimit}}};
    rejected(changed.validate(), "queue message Schema cannot conflict with the same tool type identity/version");
    changed.solution.queues[0].message_schema = message_schema;
    check(ok(changed.validate()), "matching queue and tool record Schemas share a consistent definition");

    auto shared_groups = fixture();
    auto shared_interface = ports("shared.group.interface");
    shared_interface.inputs = {input()};
    shared_groups.at(1).configuration = GroupConfiguration{shared_interface, {}, {}, {}};
    shared_groups.at(2).configuration = GroupConfiguration{shared_interface, {}, {}, {}};
    check(ok(shared_groups.validate()), "Groups may share an identical public interface identity and version");
    auto& second_interface = std::get<GroupConfiguration>(shared_groups.at(2).configuration).interface;
    second_interface.inputs[0].display_name = "Renamed public input";
    check(ok(shared_groups.validate()), "public display labels do not redefine port Schema structure");
    second_interface.inputs[0].type = DataType::boolean();
    second_interface.inputs[0].default_value = ConfigValue::boolean(false);
    rejected(shared_groups.validate(), "same public interface identity/version cannot declare different input types");
    second_interface.version = 2;
    check(ok(shared_groups.validate()), "distinct public interface versions may declare different input types");
    second_interface = shared_interface;
    auto extra_input = input(); extra_input.id = key<InputId>("additional");
    second_interface.inputs.push_back(extra_input);
    rejected(shared_groups.validate(), "same public interface identity/version cannot declare a different port set");
    second_interface.version = 2;
    check(ok(shared_groups.validate()), "a new public interface version may add a port");

    auto shared_tools = base;
    auto alias_tool = tool(); alias_tool.id = key<ToolTypeId>("test.scalar.alias");
    alias_tool.display_name = "Another implementation";
    alias_tool.outputs[0].type = DataType::boolean();
    shared_tools.tools.push_back(alias_tool);
    shared_tools.at(2).configuration = ToolConfiguration{alias_tool.id, 1};
    rejected(shared_tools.validate(), "different tools cannot redefine outputs under the same tool Schema identity/version");
    shared_tools.tools[1].outputs[0].type = DataType::integer();
    check(ok(shared_tools.validate()), "different tool identities may share identical tool Schema port structure");
    shared_tools.tools[1].outputs[0].display_name = "Renamed output";
    check(ok(shared_tools.validate()), "tool output display labels do not change Schema compatibility");
    shared_tools.tools[1].schema.version = 2;
    shared_tools.tools[1].outputs[0].type = DataType::boolean();
    check(ok(shared_tools.validate()), "different tool Schema versions may declare distinct output types");
    changed = base; changed.flow().nodes.push_back(changed.flow().nodes[0]);
    rejected(changed.validate(), "combined validator retains model identity checks");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "validation_graph") { validation_graph(); }
        else if (group == "validation_bindings") { validation_bindings(); }
        else if (group == "validation_groups") { validation_groups(); }
        else if (group == "validation_schema") { validation_schema(); }
        else { return 2; }
    } catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
