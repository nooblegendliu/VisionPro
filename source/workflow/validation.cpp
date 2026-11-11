#include <visionpro/workflow/validation.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include "validation_values.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>

namespace visionpro::workflow {
namespace {
using namespace core;
using Check = Result<std::monostate>;
using Scope = std::optional<NodeId>;
using Graph = std::map<NodeId, std::vector<NodeId>>;
using Schemas = std::map<std::pair<SchemaId, std::uint32_t>, DataType>;
Diagnostic error(ErrorCode code, std::string message, DiagnosticLocation where = {})
{ return {code, Severity::Error, std::move(where), std::move(message)}; }
bool failed(const Check& result) { return std::holds_alternative<Diagnostic>(result); }
template<class T> bool contains(const std::vector<T>& values, const T& value)
{ return std::ranges::find(values, value) != values.end(); }
template<class T, class Id> const T* find(const std::vector<T>& values, const Id& id)
{
    const auto it = std::ranges::find(values, id, &T::id);
    return it == values.end() ? nullptr : &*it;
}
std::set<NodeId> reachable(const Graph& graph, std::vector<NodeId> pending)
{
    std::set<NodeId> result;
    while (!pending.empty()) {
        const auto node = pending.back(); pending.pop_back();
        if (!result.insert(node).second) { continue; }
        if (const auto it = graph.find(node); it != graph.end()) {
            pending.insert(pending.end(), it->second.begin(), it->second.end());
        }
    }
    return result;
}
bool path(const Graph& graph, NodeId from, NodeId to)
{ return reachable(graph, {from}).contains(to); }

Check register_type(const DataType& type, Schemas& schemas, DiagnosticLocation where)
{
    if (type.schema_id()) {
        auto [it, inserted] = schemas.emplace(std::pair{*type.schema_id(), type.schema_version()}, type);
        if (!inserted) {
            if (it->second == type) { return std::monostate{}; }
            where.schema = *type.schema_id();
            return error(ErrorCode::InvalidSchema, "One Schema identity/version has conflicting definitions.", where);
        }
    }
    if (const auto* element = type.element_type()) {
        if (auto checked = register_type(*element, schemas, where); failed(checked)) { return checked; }
    }
    for (const auto& field : type.fields()) {
        auto nested = where; nested.path.emplace_back(field.id);
        if (auto checked = register_type(field.type, schemas, nested); failed(checked)) { return checked; }
    }
    return std::monostate{};
}
Check compatible_semantics(const ValueSemantics& source, const ValueSemantics& target)
{
    if ((source.unit != MeasurementUnit::Unspecified && target.unit != MeasurementUnit::Unspecified
            && source.unit != target.unit)
        || (source.coordinate_system && target.coordinate_system
            && source.coordinate_system != target.coordinate_system)) {
        return error(ErrorCode::CoordinateMismatch, "Binding cannot implicitly convert units or coordinate frames.");
    }
    return std::monostate{};
}
struct SourceType { DataType type; ValueSemantics semantics; };
struct PortSchema {
    std::map<InputId, DataType> inputs;
    std::map<ParameterId, DataType> parameters;
    std::map<OutputId, DataType> outputs;
    std::set<ControlOutputId> controls;
    bool operator==(const PortSchema&) const = default;
};
Result<SourceType> select(SourceType source, const Selector& selector)
{
    DiagnosticLocation where;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) {
            where.path.emplace_back(field->field);
            if (source.type.kind() != TypeKind::Record) {
                return error(ErrorCode::InvalidSelector, "Field selection requires a Record, without implicit Optional unwrapping.", where);
            }
            const auto* member = find(source.type.fields(), field->field);
            if (!member) { return error(ErrorCode::UnknownField, "Selected field is not declared in the source Schema.", where); }
            source.type = member->type;
        } else {
            where.path.emplace_back(DiagnosticElementIndex{std::get<ElementSelection>(step).index});
            if (source.type.kind() != TypeKind::List) {
                return error(ErrorCode::InvalidSelector, "Element selection requires a List.", where);
            }
            source.type = *source.type.element_type();
        }
    }
    return source;
}

class Validator {
public:
    Validator(const Solution& solution, std::span<const ToolDescriptor> tools,
        std::span<const SchemaSupport> support) : solution_(solution), tools_(tools), support_(support) {}
    Result<ValidationIndex> run();
private:
    Check workflow(const Workflow& flow);
    Check node(const Node& value);
    Check public_interface(const PublicInterface& interface, DiagnosticLocation where);
    Check exports(const PublicInterface& interface, const std::vector<OutputMapping>& outputs,
        const std::vector<ControlOutputMapping>& controls, Scope scope);
    Check control_outlet(const Node& source, const std::optional<ControlOutputId>& outlet);
    Result<SourceType> output(const OutputReference& ref, Scope scope);
    Result<SourceType> source_type(const BindingSource& source, Scope scope);
    Check dependency(NodeId producer, const Node& consumer, const BindingTarget& target,
        std::size_t source_index, bool inherited);
    template<class Target> Check target(const Node& node, const Target& descriptor);
    Check config(const ConfigValue& value, const Node* consumer, const BindingTarget* target,
        std::size_t source_index, const RegionCapabilities* capabilities, bool snapshot_build,
        const BindingPolicy* policy = nullptr, const AvailabilityPolicy* availability = nullptr);
    Check register_semantics(const ValueSemantics& semantics);
    Check register_ports(SchemaId id, std::uint32_t version, std::span<const InputDescriptor> inputs,
        std::span<const ParameterDescriptor> parameters, std::span<const OutputDescriptor> outputs,
        std::span<const ControlOutputDescriptor> controls, DiagnosticLocation where);
    DiagnosticLocation location(Scope node = {}) const;
    Check contextualize(Check result, DiagnosticLocation where) const;
    Scope parent(NodeId node) const;
    ScopeDependencies& scope_index(Scope scope) { return index_.scopes.at(scopes_.at(scope)); }
    const ToolDescriptor* descriptor(const Node& node) const;
    std::span<const InputDescriptor> inputs(const Node& node) const;
    std::span<const OutputDescriptor> outputs(const Node& node) const;
    std::span<const ControlOutputDescriptor> outlets(const Node& node) const;
    const Solution& solution_;
    std::span<const ToolDescriptor> tools_;
    std::span<const SchemaSupport> support_;
    const Workflow* flow_{nullptr};
    std::map<ToolTypeId, const ToolDescriptor*> tools_by_id_;
    Schemas schemas_;
    std::map<std::pair<SchemaId, std::uint32_t>, PortSchema> port_schemas_;
    ValidationIndex index_;
    std::map<NodeId, NodeId> parents_;
    std::map<Scope, std::size_t> scopes_;
    Graph control_;
    Graph combined_;
};

DiagnosticLocation Validator::location(Scope node) const
{
    DiagnosticLocation where; where.solution = solution_.id; where.node = node;
    if (flow_) { where.workflow = flow_->id; }
    return where;
}
Check Validator::contextualize(Check result, DiagnosticLocation where) const
{
    if (auto* failure = std::get_if<Diagnostic>(&result)) {
        auto& loc = failure->location;
        loc.solution = solution_.id;
        if (where.workflow) { loc.workflow = where.workflow; }
        if (where.node) { loc.node = where.node; }
        if (where.input) { loc.input = where.input; }
        if (where.parameter) { loc.parameter = where.parameter; }
        if (where.output) { loc.output = where.output; }
        if (where.control_link) { loc.control_link = where.control_link; }
        if (where.tool) { loc.tool = where.tool; }
        if (!loc.schema && where.schema) { loc.schema = where.schema; }
    }
    return result;
}
Scope Validator::parent(NodeId node) const
{
    const auto it = parents_.find(node);
    return it == parents_.end() ? Scope{} : it->second;
}
const ToolDescriptor* Validator::descriptor(const Node& node) const
{
    const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
    if (!tool) { return nullptr; }
    const auto it = tools_by_id_.find(tool->type);
    return it == tools_by_id_.end() ? nullptr : it->second;
}
std::span<const InputDescriptor> Validator::inputs(const Node& node) const
{
    if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { return group->interface.inputs; }
    return descriptor(node)->inputs;
}
std::span<const OutputDescriptor> Validator::outputs(const Node& node) const
{
    if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { return group->interface.outputs; }
    return descriptor(node)->outputs;
}
std::span<const ControlOutputDescriptor> Validator::outlets(const Node& node) const
{
    if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) { return group->interface.control_outputs; }
    return descriptor(node)->control_outputs;
}
Check Validator::register_semantics(const ValueSemantics& semantics)
{
    if (semantics.coordinate_system) {
        const auto* frame = find(solution_.coordinates, *semantics.coordinate_system);
        if (!frame || (semantics.unit != MeasurementUnit::Unspecified && semantics.unit != frame->unit)) {
            return error(ErrorCode::CoordinateMismatch, "Declared coordinate frame is absent or has a different unit.");
        }
    }
    return std::monostate{};
}
Check Validator::public_interface(const PublicInterface& interface, DiagnosticLocation where)
{
    where.schema = interface.id;
    if (auto checked = register_ports(interface.id, interface.version, interface.inputs, {},
        interface.outputs, interface.control_outputs, where); failed(checked)) { return checked; }
    for (const auto& input : interface.inputs) {
        auto loc = where; loc.input = input.id;
        for (auto checked : {register_type(input.type, schemas_, loc), register_semantics(input.semantics)}) {
            if (failed(checked)) { return contextualize(checked, loc); }
        }
        if (input.default_value) {
            for (auto checked : {compatible_semantics(detail::config_value_semantics(*input.default_value), input.semantics),
                config(*input.default_value, nullptr, nullptr, 0, nullptr, true)}) {
                if (failed(checked)) { return contextualize(checked, loc); }
            }
        }
    }
    for (const auto& output : interface.outputs) {
        auto loc = where; loc.output = output.id;
        for (auto checked : {register_type(output.type, schemas_, loc), register_semantics(output.semantics)}) {
            if (failed(checked)) { return contextualize(checked, loc); }
        }
    }
    return std::monostate{};
}

Check Validator::register_ports(SchemaId id, std::uint32_t version, std::span<const InputDescriptor> inputs,
    std::span<const ParameterDescriptor> parameters, std::span<const OutputDescriptor> outputs,
    std::span<const ControlOutputDescriptor> controls, DiagnosticLocation where)
{
    PortSchema shape;
    for (const auto& input : inputs) { shape.inputs.emplace(input.id, input.type); }
    for (const auto& parameter : parameters) { shape.parameters.emplace(parameter.id, parameter.type); }
    for (const auto& output : outputs) { shape.outputs.emplace(output.id, output.type); }
    for (const auto& control : controls) { shape.controls.insert(control.id); }
    const auto [it, inserted] = port_schemas_.emplace(std::pair{id, version}, shape);
    if (!inserted && it->second != shape) {
        where.schema = id;
        return error(ErrorCode::InvalidSchema, "One port Schema identity/version has conflicting member identities or types.", where);
    }
    return std::monostate{};
}

Result<ValidationIndex> Validator::run()
{
    if (auto shape = validate_model_shape(solution_); const auto* failure = std::get_if<Diagnostic>(&shape)) { return *failure; }
    std::set<SchemaId> support_ids;
    for (const auto& support : support_) {
        if (!support_ids.insert(support.id).second || support.minimum_version == 0
            || support.minimum_version > support.maximum_version
            || std::set<CapabilityId>(support.capabilities.begin(), support.capabilities.end()).size() != support.capabilities.size()) {
            return error(ErrorCode::InvalidDescriptor, "Schema support declarations must be valid and unique.", location());
        }
    }
    for (const auto& tool : tools_) {
        auto where = location(); where.tool = tool.id; where.schema = tool.schema.id;
        if (!tools_by_id_.emplace(tool.id, &tool).second) {
            return error(ErrorCode::DuplicateIdentity, "Tool registry contains duplicate identities.", where);
        }
        if (auto checked = contextualize(validate_descriptor(tool), where);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        if (auto checked = register_ports(tool.schema.id, tool.schema.version, tool.inputs, tool.parameters,
            tool.outputs, tool.control_outputs, where); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        const auto check_members = [&](const auto& members) -> Check {
            for (const auto& member : members) {
                if (auto checked = register_type(member.type, schemas_, where); failed(checked)) { return checked; }
            }
            return std::monostate{};
        };
        for (auto checked : {check_members(tool.inputs), check_members(tool.parameters), check_members(tool.outputs)}) {
            if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        }
        if (tool.state) {
            if (auto checked = register_type(tool.state->schema, schemas_, where);
                const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        }
    }
    for (const auto& variable : solution_.variables) {
        auto where = location(); where.variable = variable.id;
        for (auto checked : {register_type(variable.type, schemas_, where),
            config(variable.initial_value, nullptr, nullptr, 0, nullptr, true)}) {
            if (auto contextual = contextualize(checked, where); const auto* failure = std::get_if<Diagnostic>(&contextual)) {
                auto result = *failure; result.location.variable = variable.id; return result;
            }
        }
    }
    for (const auto& queue : solution_.queues) {
        auto where = location(); where.queue = queue.id;
        if (auto checked = register_type(queue.message_schema, schemas_, where);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    }
    for (const auto& resource : solution_.resources) {
        if (const auto* device = std::get_if<DeviceResource>(&resource.location)) {
            auto where = location(); where.resource = resource.id;
            for (auto checked : {register_type(device->configuration.type(), schemas_, where),
                config(device->configuration, nullptr, nullptr, 0, nullptr, true)}) {
                if (const auto* failure = std::get_if<Diagnostic>(&checked)) {
                    auto result = *failure; result.location.solution = solution_.id;
                    if (!result.location.resource) { result.location.resource = resource.id; }
                    return result;
                }
            }
        }
    }
    for (const auto& flow : solution_.workflows) {
        flow_ = &flow;
        if (auto checked = workflow(flow); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    }
    return index_;
}

Check Validator::workflow(const Workflow& flow)
{
    parents_.clear(); scopes_.clear(); control_.clear(); combined_.clear();
    for (const auto& link : flow.containment) { parents_.emplace(link.child, link.parent); }
    const auto add_scope = [&](Scope scope) {
        scopes_.emplace(scope, index_.scopes.size());
        index_.scopes.push_back({flow.id, scope, {}, {}, {}});
    };
    add_scope({});
    if (auto checked = public_interface(flow.interface, location()); failed(checked)) { return checked; }
    for (const auto& node : flow.nodes) {
        if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) {
            add_scope(node.id);
            if (auto checked = public_interface(group->interface, location(node.id)); failed(checked)) { return checked; }
        } else {
            const auto& configured = std::get<ToolConfiguration>(node.configuration);
            const auto* tool = descriptor(node);
            auto where = location(node.id); where.tool = configured.type;
            if (!tool) { return error(ErrorCode::UnknownTool, "Node tool is not registered.", where); }
            if (tool->config_version != configured.config_version) {
                return error(ErrorCode::IncompatibleSchemaVersion, "Node configuration version does not match its tool.", where);
            }
            if (!support_.empty()) {
                const auto it = std::ranges::find(support_, tool->schema.id, &SchemaSupport::id);
                if (it == support_.end()) { return error(ErrorCode::IncompatibleSchemaVersion, "Tool Schema has no backend support declaration.", where); }
                if (auto checked = contextualize(validate_schema_support(*tool, *it), where); failed(checked)) { return checked; }
            }
        }
    }
    for (const auto& link : flow.control_links) {
        auto where = location(link.target); where.control_link = link.id;
        const auto* from = find_node(flow, link.source);
        if (!from || !find_node(flow, link.target)) {
            return error(ErrorCode::InvalidControlLink, "Control endpoints must exist in this workflow.", where);
        }
        if (parent(link.source) != parent(link.target)) {
            return error(ErrorCode::ScopeViolation, "Control links cannot cross a Group boundary.", where);
        }
        if (auto checked = contextualize(control_outlet(*from, link.output), where); failed(checked)) { return checked; }
        if (path(control_, link.target, link.source)) {
            return error(ErrorCode::ControlCycle, "Ordinary control links cannot form a cycle.", where);
        }
        control_[link.source].push_back(link.target);
        scope_index(parent(link.source)).control_links.push_back(link);
    }
    combined_ = control_;
    for (const auto& [scope, index] : scopes_) {
        const auto& entries = scope ? std::get<GroupConfiguration>(find_node(flow, *scope)->configuration).entries : flow.entries;
        const auto nodes = reachable(control_, entries);
        index_.scopes[index].reachable.assign(nodes.begin(), nodes.end());
        for (const auto& link : index_.scopes[index].control_links) {
            if (nodes.contains(link.target) && !nodes.contains(link.source)) {
                auto where = location(link.target); where.control_link = link.id;
                return error(ErrorCode::IncompleteExecutionScope, "Reachable node has a control predecessor outside the explicit entry range.", where);
            }
        }
    }
    for (const auto& value : flow.nodes) {
        if (auto checked = contextualize(node(value), location(value.id)); failed(checked)) { return checked; }
    }
    if (auto checked = exports(flow.interface, flow.outputs, flow.control_exports, {}); failed(checked)) { return checked; }
    for (const auto& value : flow.nodes) {
        if (const auto* group = std::get_if<GroupConfiguration>(&value.configuration)) {
            if (auto checked = exports(group->interface, group->outputs, group->control_exports, value.id); failed(checked)) { return checked; }
        }
    }
    return std::monostate{};
}

Check Validator::control_outlet(const Node& source, const std::optional<ControlOutputId>& outlet)
{
    if (outlet && std::ranges::find(outlets(source), *outlet, &ControlOutputDescriptor::id) == outlets(source).end()) {
        return error(ErrorCode::InvalidControlLink, "Named control outlet is not declared by its source.");
    }
    return std::monostate{};
}
Result<SourceType> Validator::output(const OutputReference& ref, Scope scope)
{
    if (ref.workflow != flow_->id) { return error(ErrorCode::ScopeViolation, "Ordinary result references cannot cross workflows."); }
    const auto* source = find_node(*flow_, ref.node);
    if (!source) { return error(ErrorCode::UnknownTarget, "Referenced result node does not exist."); }
    if (parent(source->id) != scope) { return error(ErrorCode::ScopeViolation, "Result references must use the immediate scope's public node outputs."); }
    if (const auto* id = std::get_if<OutputId>(&ref.output)) {
        const auto ports = outputs(*source);
        const auto it = std::ranges::find(ports, *id, &OutputDescriptor::id);
        if (it == ports.end()) {
            auto where = DiagnosticLocation{}; where.output = *id;
            return error(ErrorCode::UnknownTarget, "Referenced output is not declared.", where);
        }
        return select({it->type, it->semantics}, ref.selector);
    }
    auto type = system_field_type(std::get<SystemField>(ref.output));
    if (const auto* failure = std::get_if<Diagnostic>(&type)) { return *failure; }
    return select({std::get<DataType>(type), {}}, ref.selector);
}
Result<SourceType> Validator::source_type(const BindingSource& source, Scope scope)
{
    return std::visit([&](const auto& value) -> Result<SourceType> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ConfigValue>) { return SourceType{value.type(), detail::config_value_semantics(value)}; }
        else if constexpr (std::is_same_v<T, OutputReference>) { return output(value, scope); }
        else if constexpr (std::is_same_v<T, ScopedInputReference>) {
            if (value.workflow != flow_->id || value.group != scope) {
                return error(ErrorCode::ScopeViolation, "Public inputs must come from the immediate enclosing Workflow or Group.");
            }
            const auto& interface = scope ? std::get<GroupConfiguration>(find_node(*flow_, *scope)->configuration).interface : flow_->interface;
            const auto* input = find(interface.inputs, value.input);
            if (!input) { return error(ErrorCode::UnknownTarget, "Referenced public input is not declared."); }
            return select({input->type, input->semantics}, value.selector);
        } else if constexpr (std::is_same_v<T, ResourceReference>) {
            if (auto checked = validate_resource_reference(value, solution_.resources); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
            return SourceType{DataType::resource_reference(), {}};
        } else {
            const auto* variable = find(solution_.variables, value.variable);
            if (!variable) {
                auto where = DiagnosticLocation{}; where.variable = value.variable;
                return error(ErrorCode::UnknownTarget, "Referenced global variable is not declared.", where);
            }
            return select({variable->type, {}}, value.selector);
        }
    }, source);
}

Check Validator::dependency(NodeId producer, const Node& consumer, const BindingTarget& target,
    std::size_t source_index, bool inherited)
{
    auto& scope = scope_index(parent(consumer.id));
    if (contains(scope.reachable, consumer.id) && !contains(scope.reachable, producer)) {
        return error(ErrorCode::IncompleteExecutionScope, "A binding cannot activate a producer outside the control entry range.");
    }
    if (path(control_, consumer.id, producer)) {
        return error(ErrorCode::ControlOrderConflict, "Data dependency reverses explicit control order or references the consumer itself.");
    }
    if (path(combined_, consumer.id, producer)) {
        return error(ErrorCode::DependencyCycle, "Control and data dependencies form a wait cycle.");
    }
    combined_[producer].push_back(consumer.id);
    scope.data.push_back({producer, consumer.id, target, source_index, inherited});
    return std::monostate{};
}
Check Validator::config(const ConfigValue& value, const Node* consumer, const BindingTarget* target,
    std::size_t source_index, const RegionCapabilities* capabilities, bool snapshot_build,
    const BindingPolicy* policy, const AvailabilityPolicy* availability)
{
    return detail::validate_config_references(value, solution_, capabilities, [&](const InheritedRegion& inherited) -> Check {
        if (!consumer || !target || snapshot_build) {
            return error(ErrorCode::DisallowedBindingSource, "Runtime ROI inheritance requires a node binding evaluated before invocation.");
        }
        if (!policy || !contains(policy->allowed_sources, BindingSourceKind::RunOutput)) {
            return error(ErrorCode::DisallowedBindingSource, "Embedded ROI inheritance requires permission to read runtime outputs.");
        }
        auto source = output({inherited.workflow, inherited.node, inherited.output, {}}, parent(consumer->id));
        if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
        const auto& info = std::get<SourceType>(source);
        if (info.type != DataType::region2d()) { return error(ErrorCode::TypeMismatch, "Inherited ROI requires a Region2D output."); }
        if (info.semantics.coordinate_system != inherited.coordinate_frame || info.semantics.unit != inherited.unit) {
            return error(ErrorCode::CoordinateMismatch, "Inherited ROI declaration must match its output frame and unit before correction.");
        }
        if (!find_node(*flow_, inherited.node)->enabled
            && (!availability || !contains(availability->accepted_absence, MissingReason::Disabled))) {
            return error(ErrorCode::InvalidBinding, "Disabled inherited ROI producer needs an explicit accepted absence policy.");
        }
        return dependency(inherited.node, *consumer, *target, source_index, true);
    });
}

template<class Target> Check Validator::target(const Node& node, const Target& descriptor)
{
    BindingTarget target_id{descriptor.id};
    auto where = location(node.id);
    if constexpr (std::is_same_v<Target, InputDescriptor>) { where.input = descriptor.id; }
    else { where.parameter = descriptor.id; }
    const auto check = [&](Check result) { return contextualize(std::move(result), where); };
    if (auto checked = check(register_semantics(descriptor.semantics)); failed(checked)) { return checked; }
    const auto* tool = this->descriptor(node);
    const auto* capabilities = tool && tool->region_capabilities ? &*tool->region_capabilities : nullptr;
    const bool snapshot_build = descriptor.binding.time == BindingTime::SnapshotBuild;
    const auto it = std::ranges::find(node.bindings, target_id, &Binding::target);
    if (it == node.bindings.end()) {
        if (!descriptor.default_value) { return check(error(ErrorCode::MissingRequiredValue, "Required target has no binding, even if its value type is Optional.")); }
        if (auto checked = compatible_semantics(detail::config_value_semantics(*descriptor.default_value), descriptor.semantics);
            failed(checked)) { return check(checked); }
        return check(config(*descriptor.default_value, &node, &target_id, 0, capabilities, snapshot_build, &descriptor.binding));
    }
    const auto& binding = *it;
    if (binding.cardinality != descriptor.binding.cardinality) {
        return check(error(ErrorCode::InvalidBinding, "Configured source cardinality does not match the target metadata."));
    }
    const auto& expected = binding.cardinality == BindingCardinality::MultipleSources ? *descriptor.type.element_type() : descriptor.type;
    if (!binding.availability.accepted_absence.empty() && expected.kind() != TypeKind::Optional
        && binding.cardinality != BindingCardinality::MultipleSources) {
        return check(error(ErrorCode::InvalidBinding, "Accepted absence needs an Optional value or explicit multiple-source collection."));
    }
    if (binding.cardinality == BindingCardinality::MultipleSources && descriptor.constraints.max_length
        && binding.availability.accepted_absence.empty() && binding.sources.size() > *descriptor.constraints.max_length) {
        return check(error(ErrorCode::ConstraintViolation, "Required source count exceeds the target collection length limit."));
    }
    std::vector<ConfigValue> constants;
    for (std::size_t index = 0; index < binding.sources.size(); ++index) {
        const auto& source = binding.sources[index];
        if (snapshot_build && source_kind(source) != BindingSourceKind::Constant) {
            return check(error(ErrorCode::DisallowedBindingSource, "Snapshot-time values must be fixed constants."));
        }
        auto info = source_type(source, parent(node.id));
        if (const auto* failure = std::get_if<Diagnostic>(&info)) { return check(*failure); }
        const auto& typed = std::get<SourceType>(info);
        for (auto checked : {core::validate_binding(descriptor, source_kind(source), typed.type, binding.conversion),
            compatible_semantics(typed.semantics, descriptor.semantics)}) {
            if (failed(checked)) { return check(checked); }
        }
        if (const auto* reference = std::get_if<OutputReference>(&source)) {
            const auto* producer = find_node(*flow_, reference->node);
            if (!producer->enabled && !contains(binding.availability.accepted_absence, MissingReason::Disabled)) {
                return check(error(ErrorCode::InvalidBinding, "Disabled producer needs an explicit accepted absence policy."));
            }
            if (auto checked = dependency(reference->node, node, target_id, index, false); failed(checked)) { return check(checked); }
        }
        if (const auto* value = std::get_if<ConfigValue>(&source)) {
            if (auto checked = config(*value, &node, &target_id, index, capabilities, snapshot_build,
                &descriptor.binding, &binding.availability); failed(checked)) { return check(checked); }
            switch (binding.conversion) {
            case ConversionKind::Exact: constants.push_back(*value); break;
            case ConversionKind::WrapOptional: constants.push_back(ConfigValue::some(*value)); break;
            case ConversionKind::Int64ToFloat64Explicit:
                constants.push_back(std::get<ConfigValue>(ConfigValue::real(static_cast<double>(*value->as_int64())))); break;
            }
        }
    }
    if (constants.size() == binding.sources.size()) {
        const auto value = binding.cardinality == BindingCardinality::MultipleSources
            ? std::get<ConfigValue>(ConfigValue::list(*descriptor.type.element_type(), constants)) : constants.front();
        const auto checked = [&]() {
            if constexpr (std::is_same_v<Target, InputDescriptor>) { return validate_input_value(descriptor, value); }
            else { return validate_parameter_value(descriptor, value); }
        }();
        if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return check(*failure); }
    }
    return std::monostate{};
}

Check Validator::node(const Node& value)
{
    const auto* tool = descriptor(value);
    for (const auto& binding : value.bindings) {
        bool known = false;
        auto where = location(value.id);
        if (const auto* id = std::get_if<InputId>(&binding.target)) {
            known = std::ranges::find(inputs(value), *id, &InputDescriptor::id) != inputs(value).end(); where.input = *id;
        } else {
            const auto parameter_id = std::get<ParameterId>(binding.target); where.parameter = parameter_id;
            known = tool && find(tool->parameters, parameter_id);
        }
        if (!known) { return error(ErrorCode::UnknownTarget, "Binding target is not declared by this node.", where); }
    }
    for (const auto& input : inputs(value)) { if (auto checked = target(value, input); failed(checked)) { return checked; } }
    if (tool) {
        if (tool->state) {
            if (auto checked = config(tool->state->initial_value, nullptr, nullptr, 0, nullptr, true); failed(checked)) {
                auto where = location(value.id); where.tool = tool->id;
                return contextualize(std::move(checked), where);
            }
        }
        for (const auto& parameter : tool->parameters) { if (auto checked = target(value, parameter); failed(checked)) { return checked; } }
    }
    for (const auto& output : outputs(value)) {
        auto where = location(value.id); where.output = output.id;
        if (auto checked = contextualize(register_semantics(output.semantics), where); failed(checked)) { return checked; }
    }
    for (const auto& bound : value.resources) {
        const auto requirement = tool ? std::ranges::find(tool->resources, bound.slot, &ResourceRequirement::slot)
            : std::vector<ResourceRequirement>::const_iterator{};
        if (!tool || requirement == tool->resources.end()) { return error(ErrorCode::UnknownTarget, "Resource slot is not declared by this node."); }
        if (bound.resource.kind != requirement->kind) { return error(ErrorCode::ResourceKindMismatch, "Resource does not match the slot's required kind."); }
        if (auto checked = validate_resource_reference(bound.resource, solution_.resources); failed(checked)) { return checked; }
    }
    if (tool) {
        for (const auto& requirement : tool->resources) {
            if (requirement.required && std::ranges::find(value.resources, requirement.slot, &NodeResourceBinding::slot) == value.resources.end()) {
                return error(ErrorCode::MissingRequiredValue, "Required resource slot has no binding.");
            }
        }
    }
    return std::monostate{};
}

Check Validator::exports(const PublicInterface& interface, const std::vector<OutputMapping>& outputs,
    const std::vector<ControlOutputMapping>& controls, Scope scope)
{
    for (const auto& declared : interface.outputs) {
        auto where = location(scope); where.output = declared.id;
        const auto it = std::ranges::find(outputs, declared.id, &OutputMapping::output);
        if (it == outputs.end()) { return error(ErrorCode::MissingRequiredValue, "Public output is missing its internal export mapping.", where); }
        auto source = output(it->source, scope);
        if (const auto* failure = std::get_if<Diagnostic>(&source)) { return contextualize(*failure, where); }
        const auto& info = std::get<SourceType>(source);
        if (info.type != declared.type) { return error(ErrorCode::TypeMismatch, "Public output mapping requires an exact type match.", where); }
        if (auto checked = contextualize(compatible_semantics(info.semantics, declared.semantics), where); failed(checked)) { return checked; }
        if (!contains(scope_index(scope).reachable, it->source.node)) {
            return error(ErrorCode::IncompleteExecutionScope, "Public output producer is outside the explicit entry range.", where);
        }
    }
    for (const auto& declared : interface.control_outputs) {
        auto where = location(scope);
        const auto it = std::ranges::find(controls, declared.id, &ControlOutputMapping::output);
        if (it == controls.end()) { return error(ErrorCode::MissingRequiredValue, "Public control outlet is missing its internal export mapping.", where); }
        const auto* source = find_node(*flow_, it->source);
        if (!source) { return error(ErrorCode::UnknownTarget, "Control export source does not exist.", where); }
        if (parent(source->id) != scope) { return error(ErrorCode::ScopeViolation, "Control exports must originate in their immediate internal scope.", where); }
        if (auto checked = contextualize(control_outlet(*source, it->source_output), where); failed(checked)) { return checked; }
        if (!contains(scope_index(scope).reachable, source->id)) {
            return error(ErrorCode::IncompleteExecutionScope, "Control export source is outside the explicit entry range.", where);
        }
    }
    return std::monostate{};
}

template<SchemaIdentity T> T key(std::string_view text)
{ return std::get<T>(SchemaIdFactory::parse<T>(text)); }
DataType enumeration(std::string_view schema, std::initializer_list<std::string_view> names)
{
    std::vector<EnumMember> members;
    for (const auto name : names) { members.push_back({key<EnumMemberId>(name), std::string(name)}); }
    return std::get<DataType>(DataType::enumeration(key<SchemaId>(schema), 1, members));
}
} // namespace

core::Result<core::DataType> system_field_type(SystemField field)
{
    switch (field) {
    case SystemField::ExecutionStatus:
        return enumeration("visionpro.execution_status", {"Pending", "Running", "Succeeded", "Failed", "Skipped", "Cancelled", "TimedOut"});
    case SystemField::Judgement: return enumeration("visionpro.judgement", {"NotEvaluated", "OK", "NG"});
    case SystemField::Elapsed: return core::DataType::integer();
    case SystemField::Diagnostics: {
        const auto severity = enumeration("visionpro.severity", {"Info", "Warning", "Error"});
        auto record = core::DataType::record(key<core::SchemaId>("visionpro.diagnostic"), 1, {
            {key<core::FieldId>("code"), core::DataType::integer()},
            {key<core::FieldId>("severity"), severity},
            {key<core::FieldId>("message"), core::DataType::string()}});
        return core::DataType::list(std::get<core::DataType>(record));
    }
    }
    return error(core::ErrorCode::InvalidBinding, "Unknown system result field.");
}
core::Result<ValidationIndex> validate_solution(const Solution& solution,
    std::span<const core::ToolDescriptor> tools, std::span<const core::SchemaSupport> support)
{ return Validator(solution, tools, support).run(); }

} // namespace visionpro::workflow
