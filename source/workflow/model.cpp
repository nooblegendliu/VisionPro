#include <visionpro/workflow/model.hpp>
#include <visionpro/core/text.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <type_traits>

namespace visionpro::workflow {
namespace {
using Check = core::Result<std::monostate>;
using Parents = std::map<core::NodeId, core::NodeId>;
core::Diagnostic error(core::ErrorCode code, std::string message)
{ return {code, core::Severity::Error, {}, std::move(message)}; }
bool failed(const Check& checked) { return std::holds_alternative<core::Diagnostic>(checked); }
bool valid_name(const std::string& name)
{ return !name.empty() && name.find('\0') == std::string::npos && core::is_valid_utf8(name); }

core::Result<Parents> parent_map(const Workflow& workflow)
{
    const auto fail = [&](core::NodeId node, std::string message) {
        auto result = error(core::ErrorCode::InvalidContainment, std::move(message));
        result.location.workflow = workflow.id;
        result.location.node = node;
        return result;
    };
    std::map<core::NodeId, const Node*> nodes;
    for (const auto& node : workflow.nodes) {
        if (!nodes.emplace(node.id, &node).second) { return fail(node.id, "Containment requires unique node identities."); }
    }
    Parents parents;
    for (const auto& link : workflow.containment) {
        const auto parent = nodes.find(link.parent);
        if (parent == nodes.end() || !nodes.contains(link.child) || link.parent == link.child
            || !std::holds_alternative<GroupConfiguration>(parent->second->configuration)
            || !parents.emplace(link.child, link.parent).second) {
            return fail(link.child, "Each contained node needs one existing Group parent; self ownership is forbidden.");
        }
    }
    for (const auto& [child, parent] : parents) {
        std::set<core::NodeId> visited{child};
        auto next = parent;
        while (true) {
            if (!visited.insert(next).second) { return fail(child, "Containment cannot form a cycle."); }
            const auto entry = parents.find(next);
            if (entry == parents.end()) { break; }
            next = entry->second;
        }
    }
    return parents;
}

std::vector<core::NodeId> direct_children(const Workflow& workflow, const Parents& parents,
    const std::optional<core::NodeId>& parent)
{
    std::vector<core::NodeId> result;
    for (const auto& node : workflow.nodes) {
        const auto found = parents.find(node.id);
        if ((!parent && found == parents.end()) || (parent && found != parents.end() && found->second == *parent)) {
            result.push_back(node.id);
        }
    }
    return result;
}

Check mappings(const PublicInterface& interface, const std::vector<OutputMapping>& outputs)
{
    std::set<core::OutputId> mapped;
    for (const auto& mapping : outputs) {
        if (!mapped.insert(mapping.output).second
            || std::ranges::find(interface.outputs, mapping.output, &core::OutputDescriptor::id) == interface.outputs.end()) {
            auto result = error(core::ErrorCode::InvalidModel, "Export targets must be unique, declared public output identities.");
            result.location.output = mapping.output;
            return result;
        }
        if (const auto* field = std::get_if<SystemField>(&mapping.source.output)) {
            switch (*field) {
            case SystemField::ExecutionStatus: case SystemField::Judgement:
            case SystemField::Diagnostics: case SystemField::Elapsed: break;
            default: return error(core::ErrorCode::InvalidModel, "Export references an unknown system field.");
            }
        }
    }
    return std::monostate{};
}

Check entry_check(const Workflow& workflow, const Parents& parents, const std::optional<core::NodeId>& parent,
    const std::vector<core::NodeId>& entries)
{
    const auto candidates = direct_children(workflow, parents, parent);
    std::set<core::NodeId> seen;
    for (const auto& entry : entries) {
        if (!seen.insert(entry).second || std::ranges::find(candidates, entry) == candidates.end()) {
            auto result = error(core::ErrorCode::InvalidModel, "Entry identities must be unique, existing nodes in this immediate scope.");
            result.location.node = entry;
            return result;
        }
    }
    return std::monostate{};
}

Check control_mappings(const PublicInterface& interface, const std::vector<ControlOutputMapping>& mappings)
{
    std::set<core::ControlOutputId> targets;
    for (const auto& mapping : mappings) {
        if (!targets.insert(mapping.output).second || std::ranges::find(interface.control_outputs,
            mapping.output, &core::ControlOutputDescriptor::id) == interface.control_outputs.end()) {
            return error(core::ErrorCode::InvalidModel, "Control export targets must be unique, declared public control outlets.");
        }
    }
    return std::monostate{};
}

Check workflow_shape(const Workflow& workflow)
{
    if (!valid_name(workflow.name)) { return error(core::ErrorCode::InvalidModel, "Workflow names require nonempty UTF-8 without NUL."); }
    auto ownership = parent_map(workflow);
    if (const auto* failure = std::get_if<core::Diagnostic>(&ownership)) { return *failure; }
    const auto& parents = std::get<Parents>(ownership);
    for (auto checked : {validate_public_interface(workflow.interface), mappings(workflow.interface, workflow.outputs),
        entry_check(workflow, parents, std::nullopt, workflow.entries), control_mappings(workflow.interface, workflow.control_exports)}) {
        if (failed(checked)) { return checked; }
    }
    for (const auto& node : workflow.nodes) {
        const auto with_node = [&](Check checked) {
            if (auto* failure = std::get_if<core::Diagnostic>(&checked)) { failure->location.node = node.id; }
            return checked;
        };
        if (!valid_name(node.name) || node.join != JoinPolicy::AllActivated) {
            return with_node(error(core::ErrorCode::InvalidModel, "Node name or join policy is invalid."));
        }
        if (const auto* tool = std::get_if<ToolConfiguration>(&node.configuration)) {
            if (tool->config_version == 0) { return with_node(error(core::ErrorCode::InvalidModel, "Tool configuration version must be positive.")); }
        } else {
            const auto& group = std::get<GroupConfiguration>(node.configuration);
            for (auto checked : {validate_public_interface(group.interface), mappings(group.interface, group.outputs),
                entry_check(workflow, parents, node.id, group.entries), control_mappings(group.interface, group.control_exports)}) {
                if (failed(checked)) { return with_node(std::move(checked)); }
            }
        }
        std::set<BindingTarget> targets;
        for (const auto& binding : node.bindings) {
            if (auto checked = validate_binding_shape(binding); failed(checked)) { return with_node(std::move(checked)); }
            if (!targets.insert(binding.target).second) {
                auto result = error(core::ErrorCode::InvalidBinding, "A node target cannot have two separately editable bindings.");
                std::visit([&](auto id) {
                    if constexpr (std::is_same_v<decltype(id), core::InputId>) { result.location.input = id; }
                    else { result.location.parameter = id; }
                }, binding.target);
                return with_node(std::move(result));
            }
        }
        std::set<core::SchemaId> slots;
        for (const auto& resource : node.resources) {
            if (!slots.insert(resource.slot).second) { return with_node(error(core::ErrorCode::InvalidModel, "Node resource slots must be unique.")); }
            if (auto checked = core::validate_resource_reference(resource.resource); failed(checked)) { return with_node(std::move(checked)); }
        }
    }
    return std::monostate{};
}
} // namespace

const Workflow* find_workflow(const Solution& solution, const core::WorkflowId& id) noexcept
{
    const auto found = std::ranges::find(solution.workflows, id, &Workflow::id);
    return found == solution.workflows.end() ? nullptr : &*found;
}
const Node* find_node(const Workflow& workflow, const core::NodeId& id) noexcept
{
    const auto found = std::ranges::find(workflow.nodes, id, &Node::id);
    return found == workflow.nodes.end() ? nullptr : &*found;
}

core::Result<std::vector<core::NodeId>> children(const Workflow& workflow, std::optional<core::NodeId> parent)
{
    auto ownership = parent_map(workflow);
    if (const auto* failure = std::get_if<core::Diagnostic>(&ownership)) { return *failure; }
    if (parent) {
        const auto* node = find_node(workflow, *parent);
        if (!node || !std::holds_alternative<GroupConfiguration>(node->configuration)) {
            auto result = error(core::ErrorCode::InvalidContainment, "Child enumeration requires an existing Group parent.");
            result.location.workflow = workflow.id; result.location.node = *parent;
            return result;
        }
    }
    return direct_children(workflow, std::get<Parents>(ownership), parent);
}

core::Result<std::vector<core::NodeId>> descendants(const Workflow& workflow, const core::NodeId& group)
{
    auto first = children(workflow, group);
    if (const auto* failure = std::get_if<core::Diagnostic>(&first)) { return *failure; }
    const auto parents = std::get<Parents>(parent_map(workflow));
    std::vector<core::NodeId> result;
    auto pending = std::get<std::vector<core::NodeId>>(first);
    std::ranges::reverse(pending);
    while (!pending.empty()) {
        const auto node = pending.back(); pending.pop_back(); result.push_back(node);
        auto nested = direct_children(workflow, parents, node);
        pending.insert(pending.end(), nested.rbegin(), nested.rend());
    }
    return result;
}

Check validate_public_interface(const PublicInterface& interface)
{
    const auto contextualize = [&](Check checked) {
        if (auto* failure = std::get_if<core::Diagnostic>(&checked)) { failure->location.schema = interface.id; }
        return checked;
    };
    if (interface.version == 0) { return contextualize(error(core::ErrorCode::InvalidSchema, "Public Schema version must be positive.")); }
    std::set<core::InputId> inputs;
    for (const auto& input : interface.inputs) {
        if (!inputs.insert(input.id).second) {
            auto result = error(core::ErrorCode::DuplicateIdentity, "Public input identities must be unique.");
            result.location.input = input.id; return contextualize(std::move(result));
        }
        if (auto checked = core::validate_input_descriptor(input); failed(checked)) { return contextualize(std::move(checked)); }
    }
    std::set<core::OutputId> outputs;
    for (const auto& output : interface.outputs) {
        if (!outputs.insert(output.id).second) {
            auto result = error(core::ErrorCode::DuplicateIdentity, "Public output identities must be unique.");
            result.location.output = output.id; return contextualize(std::move(result));
        }
        if (auto checked = core::validate_output_descriptor(output); failed(checked)) { return contextualize(std::move(checked)); }
    }
    std::set<core::ControlOutputId> controls;
    for (const auto& output : interface.control_outputs) {
        if (!controls.insert(output.id).second || !valid_name(output.display_name)) {
            return contextualize(error(core::ErrorCode::InvalidSchema, "Public control outlets require unique identities and valid names."));
        }
    }
    return std::monostate{};
}

Check validate_model_shape(const Solution& solution)
{
    const auto contextualize = [&](Check checked) {
        if (auto* failure = std::get_if<core::Diagnostic>(&checked)) { failure->location.solution = solution.id; }
        return checked;
    };
    if (!valid_name(solution.name) || solution.format_version == 0 || solution.revision == 0) {
        return contextualize(error(core::ErrorCode::InvalidModel, "Solution requires a valid name, positive format version and revision."));
    }
    std::set<core::WorkflowId> workflows;
    std::set<core::NodeId> nodes;
    std::set<core::ControlLinkId> links;
    for (const auto& workflow : solution.workflows) {
        const auto with_workflow = [&](Check checked) {
            if (auto* failure = std::get_if<core::Diagnostic>(&checked)) { failure->location.workflow = workflow.id; }
            return contextualize(std::move(checked));
        };
        if (!workflows.insert(workflow.id).second) { return with_workflow(error(core::ErrorCode::DuplicateIdentity, "Workflow identity is duplicated.")); }
        for (const auto& node : workflow.nodes) {
            if (!nodes.insert(node.id).second) {
                auto result = error(core::ErrorCode::DuplicateIdentity, "Node identities must be unique across the solution.");
                result.location.node = node.id; return with_workflow(std::move(result));
            }
        }
        for (const auto& link : workflow.control_links) {
            if (!links.insert(link.id).second) {
                auto result = error(core::ErrorCode::DuplicateIdentity, "Control link identities must be unique across the solution.");
                result.location.control_link = link.id; return with_workflow(std::move(result));
            }
        }
        if (auto checked = workflow_shape(workflow); failed(checked)) { return with_workflow(std::move(checked)); }
    }
    for (auto checked : {core::validate_resource_catalog(solution.resources),
        core::validate_global_variable_definitions(solution.variables), core::validate_queue_definitions(solution.queues)}) {
        if (failed(checked)) { return contextualize(std::move(checked)); }
    }
    std::set<core::CoordinateSystemId> coordinates;
    for (const auto& frame : solution.coordinates) {
        if (!coordinates.insert(frame.id).second) { return contextualize(error(core::ErrorCode::DuplicateIdentity, "Coordinate identity is duplicated.")); }
        if (auto checked = core::validate_coordinate_frame(frame); failed(checked)) { return contextualize(std::move(checked)); }
    }
    std::set<core::WorkflowId> layouts;
    for (const auto& layout : solution.layouts) {
        const auto* workflow = find_workflow(solution, layout.workflow);
        if (!workflow || !layouts.insert(layout.workflow).second) {
            auto result = error(core::ErrorCode::InvalidModel, "Each layout must identify one existing workflow.");
            result.location.workflow = layout.workflow; return contextualize(std::move(result));
        }
        std::set<core::NodeId> placements;
        for (const auto& placement : layout.nodes) {
            if (!find_node(*workflow, placement.node) || !placements.insert(placement.node).second
                || !std::isfinite(placement.x) || !std::isfinite(placement.y)) {
                auto result = error(core::ErrorCode::InvalidModel, "Placements require unique existing nodes and finite coordinates.");
                result.location.workflow = layout.workflow; result.location.node = placement.node;
                return contextualize(std::move(result));
            }
        }
    }
    return std::monostate{};
}

} // namespace visionpro::workflow
