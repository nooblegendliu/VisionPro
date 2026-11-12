#include "solution_codec.hpp"
#include "resource_codec.hpp"
#include "value_codec.hpp"

#include <array>
#include <type_traits>

namespace visionpro::infrastructure::detail {

template<> struct EnumNames<core::BindingSourceKind> {
    static constexpr auto values = std::to_array<std::string_view>({
        "constant", "run_output", "public_input", "resource", "global_read"});
};
template<> struct EnumNames<core::BindingCardinality> {
    static constexpr auto values = std::to_array<std::string_view>({
        "single_source", "multiple_sources", "collection_source"});
};
template<> struct EnumNames<core::BindingTime> {
    static constexpr auto values = std::to_array<std::string_view>({"snapshot_build", "before_invocation"});
};
template<> struct EnumNames<core::ConversionKind> {
    static constexpr auto values = std::to_array<std::string_view>({
        "exact", "wrap_optional", "int64_to_float64_explicit"});
};
template<> struct EnumNames<core::OutputAbsenceReason> {
    static constexpr auto values = std::to_array<std::string_view>({
        "not_selected", "disabled", "tool_did_not_produce", "failed", "cancelled", "timed_out", "condition_not_met"});
};
template<> struct EnumNames<workflow::SystemField> {
    static constexpr auto values = std::to_array<std::string_view>({"execution_status", "judgement", "diagnostics", "elapsed"});
};
template<> struct EnumNames<workflow::MissingReason> {
    static constexpr auto values = std::to_array<std::string_view>({
        "not_selected", "disabled", "tool_did_not_produce", "condition_not_met"});
};
template<> struct EnumNames<workflow::JoinPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"all_activated"});
};
template<> struct EnumNames<core::QueueOverflowPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"wait", "reject", "drop_oldest"});
};
template<> struct EnumNames<core::EmptyQueuePolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"wait", "return_empty", "fail"});
};
template<> struct EnumNames<core::QueueConsumerPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"single_consumer", "competing_consumers"});
};
template<> struct EnumNames<core::QueueBatchPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"up_to_limit", "require_exact"});
};

namespace {

using namespace core;
using namespace workflow;

constexpr std::string_view solution_format = "visionpro.solution";
constexpr std::uint32_t supported_format_version = 1;

Json encode_selection(const SelectionStep& step)
{
    if (const auto* field = std::get_if<FieldSelection>(&step)) {
        return {{"kind", "field"}, {"field", write_id(field->field)}};
    }
    return {{"kind", "element"}, {"index", std::to_string(std::get<ElementSelection>(step).index)}};
}

SelectionStep decode_selection(const Reader& reader)
{
    const auto kind = reader.field("kind");
    if (kind.text() == "field") {
        reader.object({"kind", "field"});
        return FieldSelection{read_id<FieldId>(reader.field("field"))};
    }
    if (kind.text() == "element") {
        reader.object({"kind", "index"});
        return ElementSelection{reader.field("index").size_value()};
    }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown selector kind.");
}

Json encode_output_reference(const OutputReference& reference)
{
    const auto* port = std::get_if<OutputId>(&reference.output);
    const auto output = port ? Json{{"kind", "port"}, {"id", write_id(*port)}}
        : Json{{"kind", "system"}, {"field", write_enum(std::get<SystemField>(reference.output))}};
    return {{"workflow", write_id(reference.workflow)}, {"node", write_id(reference.node)}, {"output", output},
        {"selector", write_array(reference.selector, encode_selection)}};
}

std::variant<OutputId, SystemField> decode_output_slot(const Reader& reader)
{
    const auto kind = reader.field("kind");
    if (kind.text() == "port") {
        reader.object({"kind", "id"});
        return read_id<OutputId>(reader.field("id"));
    }
    if (kind.text() == "system") {
        reader.object({"kind", "field"});
        return read_enum<SystemField>(reader.field("field"));
    }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown output reference kind.");
}

OutputReference decode_output_reference(const Reader& reader)
{
    reader.object({"workflow", "node", "output", "selector"});
    return {read_id<WorkflowId>(reader.field("workflow")), read_id<NodeId>(reader.field("node")),
        decode_output_slot(reader.field("output")), read_array<SelectionStep>(reader.field("selector"), decode_selection)};
}

Json encode_scoped_input(const ScopedInputReference& reference)
{
    return {{"workflow", write_id(reference.workflow)}, {"group", write_optional(reference.group, write_id<NodeId>)},
        {"input", write_id(reference.input)}, {"selector", write_array(reference.selector, encode_selection)}};
}

ScopedInputReference decode_scoped_input(const Reader& reader)
{
    reader.object({"workflow", "group", "input", "selector"});
    return {read_id<WorkflowId>(reader.field("workflow")), read_optional<NodeId>(reader.field("group"), read_id<NodeId>),
        read_id<InputId>(reader.field("input")), read_array<SelectionStep>(reader.field("selector"), decode_selection)};
}

Json encode_global_reference(const GlobalReference& reference)
{
    return {{"variable", write_id(reference.variable)},
        {"required_revision", write_optional(reference.required_revision, [](std::uint64_t value) { return std::to_string(value); })},
        {"selector", write_array(reference.selector, encode_selection)}};
}

GlobalReference decode_global_reference(const Reader& reader)
{
    reader.object({"variable", "required_revision", "selector"});
    return {read_id<VariableId>(reader.field("variable")),
        read_optional<std::uint64_t>(reader.field("required_revision"), [](const Reader& revision) { return revision.u64(); }),
        read_array<SelectionStep>(reader.field("selector"), decode_selection)};
}

Json encode_binding_source(const BindingSource& source)
{
    auto value = std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, ConfigValue>) { return encode_value(item); }
        else if constexpr (std::is_same_v<T, OutputReference>) { return encode_output_reference(item); }
        else if constexpr (std::is_same_v<T, ScopedInputReference>) { return encode_scoped_input(item); }
        else if constexpr (std::is_same_v<T, ResourceReference>) { return encode_resource_reference(item); }
        else { return encode_global_reference(item); }
    }, source);
    return {{"kind", write_enum(source_kind(source))}, {"value", std::move(value)}};
}

BindingSource decode_binding_source(const Reader& reader)
{
    reader.object({"kind", "value"});
    const auto kind = read_enum<BindingSourceKind>(reader.field("kind"));
    const auto value = reader.field("value");
    switch (kind) {
    case BindingSourceKind::Constant: return decode_value(value);
    case BindingSourceKind::RunOutput: return decode_output_reference(value);
    case BindingSourceKind::PublicInput: return decode_scoped_input(value);
    case BindingSourceKind::Resource: return decode_resource_reference(value);
    case BindingSourceKind::GlobalRead: return decode_global_reference(value);
    }
    reader.fail(ErrorCode::UnsupportedSolutionContent, "Unknown binding source kind.");
}

Json encode_binding_target(const BindingTarget& target)
{
    if (const auto* input = std::get_if<InputId>(&target)) {
        return {{"kind", "input"}, {"id", write_id(*input)}};
    }
    return {{"kind", "parameter"}, {"id", write_id(std::get<ParameterId>(target))}};
}

BindingTarget decode_binding_target(const Reader& reader)
{
    reader.object({"kind", "id"});
    const auto kind = reader.field("kind");
    if (kind.text() == "input") { return read_id<InputId>(reader.field("id")); }
    if (kind.text() == "parameter") { return read_id<ParameterId>(reader.field("id")); }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown binding target kind.");
}

Json encode_binding(const Binding& binding)
{
    return {{"target", encode_binding_target(binding.target)}, {"sources", write_array(binding.sources, encode_binding_source)},
        {"cardinality", write_enum(binding.cardinality)}, {"conversion", write_enum(binding.conversion)},
        {"accepted_absence", write_array(binding.availability.accepted_absence, write_enum<MissingReason>)}};
}

Binding decode_binding(const Reader& reader)
{
    reader.object({"target", "sources", "cardinality", "conversion", "accepted_absence"});
    return {decode_binding_target(reader.field("target")), read_array<BindingSource>(reader.field("sources"), decode_binding_source),
        read_enum<BindingCardinality>(reader.field("cardinality")), read_enum<ConversionKind>(reader.field("conversion")),
        {read_array<MissingReason>(reader.field("accepted_absence"), read_enum<MissingReason>)}};
}

Json encode_binding_policy(const BindingPolicy& policy)
{
    return {{"allowed_sources", write_array(policy.allowed_sources, write_enum<BindingSourceKind>)},
        {"cardinality", write_enum(policy.cardinality)}, {"time", write_enum(policy.time)},
        {"allowed_conversions", write_array(policy.allowed_conversions, write_enum<ConversionKind>)}};
}

BindingPolicy decode_binding_policy(const Reader& reader)
{
    reader.object({"allowed_sources", "cardinality", "time", "allowed_conversions"});
    return {read_array<BindingSourceKind>(reader.field("allowed_sources"), read_enum<BindingSourceKind>),
        read_enum<BindingCardinality>(reader.field("cardinality")), read_enum<BindingTime>(reader.field("time")),
        read_array<ConversionKind>(reader.field("allowed_conversions"), read_enum<ConversionKind>)};
}

Json encode_constraints(const ValueConstraints& constraints)
{
    const auto integer_range = write_optional(constraints.integer_range, [](const IntegerRange& range) {
        return Json{{"minimum", std::to_string(range.minimum)}, {"maximum", std::to_string(range.maximum)}};
    });
    const auto real_range = write_optional(constraints.real_range, [](const RealRange& range) {
        return Json{{"minimum", range.minimum}, {"maximum", range.maximum}};
    });
    return {{"integer_range", integer_range}, {"real_range", real_range},
        {"allowed_values", write_array(constraints.allowed_values, encode_value)},
        {"max_length", write_optional(constraints.max_length, [](std::size_t length) { return std::to_string(length); })}};
}

ValueConstraints decode_constraints(const Reader& reader)
{
    reader.object({"integer_range", "real_range", "allowed_values", "max_length"});
    return {read_optional<IntegerRange>(reader.field("integer_range"), [](const Reader& range) {
            range.object({"minimum", "maximum"});
            return IntegerRange{range.field("minimum").i64(), range.field("maximum").i64()};
        }),
        read_optional<RealRange>(reader.field("real_range"), [](const Reader& range) {
            range.object({"minimum", "maximum"});
            return RealRange{range.field("minimum").number(), range.field("maximum").number()};
        }),
        read_array<ConfigValue>(reader.field("allowed_values"), decode_value),
        read_optional<std::size_t>(reader.field("max_length"), [](const Reader& length) { return length.size_value(); })};
}

Json encode_input(const InputDescriptor& input)
{
    return {{"id", write_id(input.id)}, {"display_name", input.display_name}, {"type", encode_type(input.type)},
        {"required", input.required}, {"default_value", write_optional(input.default_value, encode_value)},
        {"constraints", encode_constraints(input.constraints)}, {"binding", encode_binding_policy(input.binding)},
        {"semantics", encode_semantics(input.semantics)}};
}

InputDescriptor decode_input(const Reader& reader)
{
    reader.object({"id", "display_name", "type", "required", "default_value", "constraints", "binding", "semantics"});
    return {read_id<InputId>(reader.field("id")), std::string{reader.field("display_name").text()},
        decode_type(reader.field("type")), reader.field("required").boolean(),
        read_optional<ConfigValue>(reader.field("default_value"), decode_value),
        decode_constraints(reader.field("constraints")), decode_binding_policy(reader.field("binding")),
        decode_semantics(reader.field("semantics"))};
}

Json encode_output(const OutputDescriptor& output)
{
    return {{"id", write_id(output.id)}, {"display_name", output.display_name}, {"type", encode_type(output.type)},
        {"semantics", encode_semantics(output.semantics)}, {"required", output.required},
        {"allowed_absence", write_array(output.allowed_absence, write_enum<OutputAbsenceReason>)}};
}

OutputDescriptor decode_output(const Reader& reader)
{
    reader.object({"id", "display_name", "type", "semantics", "required", "allowed_absence"});
    return {read_id<OutputId>(reader.field("id")), std::string{reader.field("display_name").text()},
        decode_type(reader.field("type")), decode_semantics(reader.field("semantics")), reader.field("required").boolean(),
        read_array<OutputAbsenceReason>(reader.field("allowed_absence"), read_enum<OutputAbsenceReason>)};
}

Json encode_control_output(const ControlOutputDescriptor& output)
{
    return {{"id", write_id(output.id)}, {"display_name", output.display_name}};
}

ControlOutputDescriptor decode_control_output(const Reader& reader)
{
    reader.object({"id", "display_name"});
    return {read_id<ControlOutputId>(reader.field("id")), std::string{reader.field("display_name").text()}};
}

Json encode_interface(const PublicInterface& interface)
{
    return {{"id", write_id(interface.id)}, {"version", interface.version},
        {"inputs", write_array(interface.inputs, encode_input)}, {"outputs", write_array(interface.outputs, encode_output)},
        {"control_outputs", write_array(interface.control_outputs, encode_control_output)}};
}

PublicInterface decode_interface(const Reader& reader)
{
    reader.object({"id", "version", "inputs", "outputs", "control_outputs"});
    return {read_id<SchemaId>(reader.field("id")), reader.field("version").u32(),
        read_array<InputDescriptor>(reader.field("inputs"), decode_input),
        read_array<OutputDescriptor>(reader.field("outputs"), decode_output),
        read_array<ControlOutputDescriptor>(reader.field("control_outputs"), decode_control_output)};
}

Json encode_output_mapping(const OutputMapping& mapping)
{
    return {{"output", write_id(mapping.output)}, {"source", encode_output_reference(mapping.source)}};
}

OutputMapping decode_output_mapping(const Reader& reader)
{
    reader.object({"output", "source"});
    return {read_id<OutputId>(reader.field("output")), decode_output_reference(reader.field("source"))};
}

Json encode_control_mapping(const ControlOutputMapping& mapping)
{
    return {{"output", write_id(mapping.output)}, {"source", write_id(mapping.source)},
        {"source_output", write_optional(mapping.source_output, write_id<ControlOutputId>)}};
}

ControlOutputMapping decode_control_mapping(const Reader& reader)
{
    reader.object({"output", "source", "source_output"});
    return {read_id<ControlOutputId>(reader.field("output")), read_id<NodeId>(reader.field("source")),
        read_optional<ControlOutputId>(reader.field("source_output"), read_id<ControlOutputId>)};
}

Json encode_configuration(const std::variant<ToolConfiguration, GroupConfiguration>& configuration)
{
    if (const auto* tool = std::get_if<ToolConfiguration>(&configuration)) {
        return {{"kind", "tool"}, {"type", write_id(tool->type)}, {"config_version", tool->config_version}};
    }
    const auto& group = std::get<GroupConfiguration>(configuration);
    return {{"kind", "group"}, {"interface", encode_interface(group.interface)},
        {"entries", write_array(group.entries, write_id<NodeId>)},
        {"outputs", write_array(group.outputs, encode_output_mapping)},
        {"control_exports", write_array(group.control_exports, encode_control_mapping)}};
}

std::variant<ToolConfiguration, GroupConfiguration> decode_configuration(const Reader& reader)
{
    const auto kind = reader.field("kind");
    if (kind.text() == "tool") {
        reader.object({"kind", "type", "config_version"});
        return ToolConfiguration{read_id<ToolTypeId>(reader.field("type")), reader.field("config_version").u32()};
    }
    if (kind.text() == "group") {
        reader.object({"kind", "interface", "entries", "outputs", "control_exports"});
        return GroupConfiguration{decode_interface(reader.field("interface")),
            read_array<NodeId>(reader.field("entries"), read_id<NodeId>),
            read_array<OutputMapping>(reader.field("outputs"), decode_output_mapping),
            read_array<ControlOutputMapping>(reader.field("control_exports"), decode_control_mapping)};
    }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown node configuration kind.");
}

Json encode_node_resource(const NodeResourceBinding& binding)
{
    return {{"slot", write_id(binding.slot)}, {"resource", encode_resource_reference(binding.resource)}};
}

NodeResourceBinding decode_node_resource(const Reader& reader)
{
    reader.object({"slot", "resource"});
    return {read_id<SchemaId>(reader.field("slot")), decode_resource_reference(reader.field("resource"))};
}

Json encode_node(const Node& node)
{
    return {{"id", write_id(node.id)}, {"name", node.name}, {"configuration", encode_configuration(node.configuration)},
        {"bindings", write_array(node.bindings, encode_binding)}, {"resources", write_array(node.resources, encode_node_resource)},
        {"enabled", node.enabled}, {"join", write_enum(node.join)}};
}

Node decode_node(const Reader& reader)
{
    reader.object({"id", "name", "configuration", "bindings", "resources", "enabled", "join"});
    return {read_id<NodeId>(reader.field("id")), std::string{reader.field("name").text()}, decode_configuration(reader.field("configuration")),
        read_array<Binding>(reader.field("bindings"), decode_binding), read_array<NodeResourceBinding>(reader.field("resources"), decode_node_resource),
        reader.field("enabled").boolean(), read_enum<JoinPolicy>(reader.field("join"))};
}

Json encode_control_link(const ControlLink& link)
{
    return {{"id", write_id(link.id)}, {"source", write_id(link.source)},
        {"output", write_optional(link.output, write_id<ControlOutputId>)}, {"target", write_id(link.target)}};
}

ControlLink decode_control_link(const Reader& reader)
{
    reader.object({"id", "source", "output", "target"});
    return {read_id<ControlLinkId>(reader.field("id")), read_id<NodeId>(reader.field("source")),
        read_optional<ControlOutputId>(reader.field("output"), read_id<ControlOutputId>), read_id<NodeId>(reader.field("target"))};
}

Json encode_containment(const Containment& relationship)
{
    return {{"parent", write_id(relationship.parent)}, {"child", write_id(relationship.child)}};
}

Containment decode_containment(const Reader& reader)
{
    reader.object({"parent", "child"});
    return {read_id<NodeId>(reader.field("parent")), read_id<NodeId>(reader.field("child"))};
}

Json encode_workflow(const Workflow& flow)
{
    return {{"id", write_id(flow.id)}, {"name", flow.name}, {"interface", encode_interface(flow.interface)},
        {"nodes", write_array(flow.nodes, encode_node)}, {"control_links", write_array(flow.control_links, encode_control_link)},
        {"containment", write_array(flow.containment, encode_containment)}, {"entries", write_array(flow.entries, write_id<NodeId>)},
        {"outputs", write_array(flow.outputs, encode_output_mapping)}, {"enabled", flow.enabled},
        {"control_exports", write_array(flow.control_exports, encode_control_mapping)}};
}

Workflow decode_workflow(const Reader& reader)
{
    reader.object({"id", "name", "interface", "nodes", "control_links", "containment", "entries", "outputs", "enabled", "control_exports"});
    return {read_id<WorkflowId>(reader.field("id")), std::string{reader.field("name").text()}, decode_interface(reader.field("interface")),
        read_array<Node>(reader.field("nodes"), decode_node), read_array<ControlLink>(reader.field("control_links"), decode_control_link),
        read_array<Containment>(reader.field("containment"), decode_containment), read_array<NodeId>(reader.field("entries"), read_id<NodeId>),
        read_array<OutputMapping>(reader.field("outputs"), decode_output_mapping), reader.field("enabled").boolean(),
        read_array<ControlOutputMapping>(reader.field("control_exports"), decode_control_mapping)};
}

Json encode_variable(const GlobalVariableDefinition& variable)
{
    return {{"id", write_id(variable.id)}, {"name", variable.name}, {"type", encode_type(variable.type)},
        {"initial_value", encode_value(variable.initial_value)}};
}

GlobalVariableDefinition decode_variable(const Reader& reader)
{
    reader.object({"id", "name", "type", "initial_value"});
    return {read_id<VariableId>(reader.field("id")), std::string{reader.field("name").text()},
        decode_type(reader.field("type")), decode_value(reader.field("initial_value"))};
}

Json encode_queue(const DataQueueDefinition& queue)
{
    return {{"id", write_id(queue.id)}, {"name", queue.name}, {"message_schema", encode_type(queue.message_schema)},
        {"capacity", std::to_string(queue.capacity)}, {"overflow", write_enum(queue.overflow)},
        {"receive", Json{{"empty", write_enum(queue.receive.empty)}, {"maximum_batch_size", std::to_string(queue.receive.maximum_batch_size)},
            {"consumers", write_enum(queue.receive.consumers)}, {"batch", write_enum(queue.receive.batch)}}}};
}

DataQueueDefinition decode_queue(const Reader& reader)
{
    reader.object({"id", "name", "message_schema", "capacity", "overflow", "receive"});
    const auto receive = reader.field("receive");
    receive.object({"empty", "maximum_batch_size", "consumers", "batch"});
    return {read_id<QueueId>(reader.field("id")), std::string{reader.field("name").text()},
        decode_type(reader.field("message_schema")), reader.field("capacity").size_value(),
        read_enum<QueueOverflowPolicy>(reader.field("overflow")),
        {read_enum<EmptyQueuePolicy>(receive.field("empty")), receive.field("maximum_batch_size").size_value(),
            read_enum<QueueConsumerPolicy>(receive.field("consumers")), read_enum<QueueBatchPolicy>(receive.field("batch"))}};
}

Json encode_placement(const NodePlacement& placement)
{
    return {{"node", write_id(placement.node)}, {"x", placement.x}, {"y", placement.y}};
}

NodePlacement decode_placement(const Reader& reader)
{
    reader.object({"node", "x", "y"});
    return {read_id<NodeId>(reader.field("node")), reader.field("x").number(), reader.field("y").number()};
}

Json encode_layout(const WorkflowLayout& layout)
{
    return {{"workflow", write_id(layout.workflow)}, {"nodes", write_array(layout.nodes, encode_placement)}};
}

WorkflowLayout decode_layout(const Reader& reader)
{
    reader.object({"workflow", "nodes"});
    return {read_id<WorkflowId>(reader.field("workflow")), read_array<NodePlacement>(reader.field("nodes"), decode_placement)};
}

} // namespace

Json encode_solution(const workflow::Solution& solution)
{
    return {{"format", solution_format}, {"format_version", solution.format_version},
        {"id", write_id(solution.id)}, {"name", solution.name}, {"revision", std::to_string(solution.revision)},
        {"workflows", write_array(solution.workflows, encode_workflow)}, {"resources", write_array(solution.resources, encode_resource)},
        {"variables", write_array(solution.variables, encode_variable)}, {"queues", write_array(solution.queues, encode_queue)},
        {"coordinates", write_array(solution.coordinates, encode_coordinate)}, {"layouts", write_array(solution.layouts, encode_layout)}};
}

workflow::Solution decode_solution(const Reader& reader)
{
    reader.object({"format", "format_version", "id", "name", "revision", "workflows", "resources", "variables", "queues", "coordinates", "layouts"});
    const auto format = reader.field("format");
    if (format.text() != solution_format) {
        format.fail(core::ErrorCode::UnsupportedSolutionFormat, "Unknown solution manifest format.");
    }
    const auto version = reader.field("format_version");
    const auto number = version.u32();
    if (number != supported_format_version) {
        version.fail(core::ErrorCode::UnsupportedSolutionFormat, "Unsupported solution manifest version.");
    }
    return {read_id<core::SolutionId>(reader.field("id")), std::string{reader.field("name").text()}, number, reader.field("revision").u64(),
        read_array<workflow::Workflow>(reader.field("workflows"), decode_workflow),
        read_array<core::ResourceDefinition>(reader.field("resources"), decode_resource),
        read_array<core::GlobalVariableDefinition>(reader.field("variables"), decode_variable),
        read_array<core::DataQueueDefinition>(reader.field("queues"), decode_queue),
        read_array<core::CoordinateFrameDefinition>(reader.field("coordinates"), decode_coordinate),
        read_array<workflow::WorkflowLayout>(reader.field("layouts"), decode_layout)};
}

} // namespace visionpro::infrastructure::detail
