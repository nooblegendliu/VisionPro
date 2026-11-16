#include "control_graph_model.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/NodeData>
#include <QtNodes/StyleCollection>
#include <QStringList>
#include <QUndoStack>

#include <visionpro/core/id_factory.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <stdexcept>

namespace visionpro::ui {
namespace {

using namespace QtNodes;
using namespace workflow;

Workflow* editable_workflow(Solution& draft, const core::WorkflowId& workflow)
{
    const auto found = std::ranges::find(draft.workflows, workflow, &Workflow::id);
    return found == draft.workflows.end() ? nullptr : &*found;
}

std::vector<core::ControlOutputDescriptor> control_outlets(const Node& node, const core::ToolRegistry& registry)
{
    if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) {
        return group->interface.control_outputs;
    }
    const auto& tool = std::get<ToolConfiguration>(node.configuration);
    const auto* descriptor = registry.find_descriptor(tool.type);
    return descriptor && descriptor->config_version == tool.config_version
        ? descriptor->control_outputs : std::vector<core::ControlOutputDescriptor>{};
}

// Swapping the command's stored value with the draft preserves exact ordering,
// identities and absent layout entries across undo and redo.
class CanvasEdit final : public QUndoCommand {
public:
    CanvasEdit(const QString& label, std::function<void()> exchange)
        : QUndoCommand(label), exchange_(std::move(exchange)) {}

    void undo() override { exchange_(); }
    void redo() override { exchange_(); }

private:
    std::function<void()> exchange_;
};

} // namespace

ControlGraphModel::ControlGraphModel(Solution draft, std::optional<core::WorkflowId> workflow_id,
    const core::ToolRegistry& registry)
    : draft_(std::move(draft)), registry_(registry), workflow_id_(workflow_id)
{
    if (const auto checked = validate_global_invariants(draft_);
        const auto* failure = std::get_if<core::Diagnostic>(&checked)) {
        throw std::invalid_argument(failure->message);
    }
    if (workflow_id_ && !active_workflow()) {
        throw std::invalid_argument("The requested workflow is absent from the draft.");
    }
    if (!workflow_id_ && !draft_.workflows.empty()) { workflow_id_ = draft_.workflows.front().id; }
    nodes_ = project_nodes(active_workflow());
}

const Workflow* ControlGraphModel::active_workflow() const
{
    return workflow_id_ ? find_workflow(draft_, *workflow_id_) : nullptr;
}

ControlGraphModel::ProjectedNodes ControlGraphModel::project_nodes(const Workflow* flow)
{
    ProjectedNodes projected;
    if (!flow) { return projected; }
    for (std::size_t index = 0; index < flow->nodes.size(); ++index) {
        const auto& node = flow->nodes[index];
        if (!ids_.contains(node.id) && ids_.size() >= InvalidNodeId) {
            throw std::runtime_error("Canvas node addresses are exhausted.");
        }
        const auto entry = ids_.try_emplace(node.id, static_cast<NodeId>(ids_.size())).first;
        projected.emplace(entry->second, ProjectedNode{index, control_outlets(node, registry_), {}, {}});
    }
    for (const auto& relation : flow->containment) {
        const auto child_id = ids_.find(relation.child);
        const auto parent_id = ids_.find(relation.parent);
        if (child_id == ids_.end() || parent_id == ids_.end()) { continue; }
        const auto child = projected.find(child_id->second);
        const auto parent = projected.find(parent_id->second);
        if (child == projected.end() || parent == projected.end() || child->second.parent
            || !std::holds_alternative<GroupConfiguration>(flow->nodes[parent->second.source_index].configuration)) {
            continue;
        }
        // An older diagnosed draft can have invalid containment. Leave it
        // intact and keep the displayed hierarchy navigable.
        std::optional<core::NodeId> ancestor = relation.parent;
        while (ancestor && *ancestor != relation.child) {
            ancestor = projected.at(ids_.at(*ancestor)).parent;
        }
        if (!ancestor) { child->second.parent = relation.parent; }
    }

    std::map<std::optional<core::NodeId>, std::size_t> scope_counts;
    for (const auto& source : flow->nodes) {
        auto& node = projected.at(ids_.at(source.id));
        const auto ordinal = scope_counts[node.parent]++;
        node.default_position = QPointF(240.0 * static_cast<double>(ordinal % 4),
            128.0 * static_cast<double>(ordinal / 4));
    }
    return projected;
}

void ControlGraphModel::set_view(std::optional<core::WorkflowId> workflow_id, std::optional<core::NodeId> scope,
    ProjectedNodes nodes)
{
    workflow_id_ = workflow_id;
    scope_ = scope;
    nodes_.swap(nodes);
    Q_EMIT modelReset();
}

bool ControlGraphModel::select_workflow(const core::WorkflowId& workflow_id)
{
    if (workflow_id_ == workflow_id) { return true; }
    const auto* flow = find_workflow(draft_, workflow_id);
    if (!flow) { return false; }
    set_view(workflow_id, std::nullopt, project_nodes(flow));
    return true;
}

bool ControlGraphModel::reveal_node(const core::NodeId& node)
{
    if (const auto id = canvas_id(node); nodeExists(id)) {
        const auto parent = nodes_.at(id).parent;
        if (scope_ != parent) {
            scope_ = parent;
            Q_EMIT modelReset();
        }
        return true;
    }
    const auto flow = std::ranges::find_if(draft_.workflows, [&](const auto& candidate) {
        return find_node(candidate, node) != nullptr;
    });
    if (flow == draft_.workflows.end()) { return false; }
    auto projected = project_nodes(&*flow);
    const auto parent = projected.at(ids_.at(node)).parent;
    set_view(flow->id, parent, std::move(projected));
    return true;
}

NodeId ControlGraphModel::canvas_id(const core::NodeId& id) const
{
    const auto found = ids_.find(id);
    return found != ids_.end() && nodeExists(found->second) ? found->second : InvalidNodeId;
}

std::optional<core::NodeId> ControlGraphModel::domain_node(NodeId id) const
{
    const auto node = nodes_.find(id);
    if (node == nodes_.end()) { return std::nullopt; }
    return active_workflow()->nodes[node->second.source_index].id;
}

QString ControlGraphModel::scope_path() const
{
    const auto* flow = active_workflow();
    if (!flow) { return {}; }
    QStringList names;
    for (auto group = scope_; group; group = nodes_.at(canvas_id(*group)).parent) {
        names.prepend(QString::fromStdString(flow->nodes[nodes_.at(canvas_id(*group)).source_index].name));
    }
    names.prepend(QString::fromStdString(flow->name));
    return names.join(QStringLiteral(" / "));
}

bool ControlGraphModel::open_group(NodeId node)
{
    if (!visible(node) || !std::holds_alternative<GroupConfiguration>(
        active_workflow()->nodes[nodes_.at(node).source_index].configuration)) {
        return false;
    }
    scope_ = domain_node(node);
    Q_EMIT modelReset();
    return true;
}

bool ControlGraphModel::parent_scope()
{
    if (!scope_) { return false; }
    scope_ = nodes_.at(canvas_id(*scope_)).parent;
    Q_EMIT modelReset();
    return true;
}

bool ControlGraphModel::nodeExists(NodeId node) const
{
    return nodes_.contains(node);
}

bool ControlGraphModel::visible(NodeId node) const
{
    const auto projected = nodes_.find(node);
    return projected != nodes_.end() && projected->second.parent == scope_;
}

std::unordered_set<NodeId> ControlGraphModel::allNodeIds() const
{
    std::unordered_set<NodeId> ids;
    for (const auto& [id, node] : nodes_) {
        if (node.parent == scope_) { ids.insert(id); }
    }
    return ids;
}

std::optional<ConnectionId> ControlGraphModel::project(const ControlLink& link) const
{
    const auto source = canvas_id(link.source);
    const auto target = canvas_id(link.target);
    if (!visible(source) || !visible(target)) { return std::nullopt; }
    if (!link.output) { return ConnectionId{source, 0, target, 0}; }

    const auto& outlets = nodes_.at(source).outlets;
    const auto found = std::ranges::find(outlets, *link.output, &core::ControlOutputDescriptor::id);
    if (found == outlets.end()) { return std::nullopt; }
    const auto port = static_cast<PortIndex>(found - outlets.begin()) + 1;
    return ConnectionId{source, port, target, 0};
}

std::unordered_set<ConnectionId> ControlGraphModel::allConnectionIds(NodeId node) const
{
    std::unordered_set<ConnectionId> ids;
    if (!visible(node)) { return ids; }
    for (const auto& link : active_workflow()->control_links) {
        if (const auto id = project(link); id && (id->outNodeId == node || id->inNodeId == node)) {
            ids.insert(*id);
        }
    }
    return ids;
}

std::unordered_set<ConnectionId> ControlGraphModel::connections(
    NodeId node, PortType type, PortIndex index) const
{
    auto ids = allConnectionIds(node);
    std::erase_if(ids, [=](const ConnectionId& id) {
        return getNodeId(type, id) != node || getPortIndex(type, id) != index;
    });
    return ids;
}

bool ControlGraphModel::connectionExists(ConnectionId connection) const
{
    if (!visible(connection.outNodeId) || !visible(connection.inNodeId)) { return false; }
    return std::ranges::any_of(active_workflow()->control_links, [&](const ControlLink& link) {
        return project(link) == connection;
    });
}

bool ControlGraphModel::connectionPossible(ConnectionId connection) const
{
    const auto [source, outlet, target, inlet] = connection;
    if (!visible(source) || !visible(target) || inlet != 0 || outlet > nodes_.at(source).outlets.size()
        || source == target || connectionExists(connection)) {
        return false;
    }

    // A draft can contain unbound inputs. Check this structural edit without
    // requiring the entire draft to pass execution validation.
    std::unordered_set<NodeId> visited{target};
    std::vector<NodeId> pending{target};
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto current_node = *domain_node(current);
        for (const auto& link : active_workflow()->control_links) {
            if (link.source != current_node) { continue; }
            const auto next = canvas_id(link.target);
            if (next == source) { return false; }
            if (visible(next) && visited.insert(next).second) { pending.push_back(next); }
        }
    }
    return true;
}

QPointF ControlGraphModel::position(NodeId node) const
{
    const auto layout = std::ranges::find(draft_.layouts, *workflow_id_, &WorkflowLayout::workflow);
    if (layout != draft_.layouts.end()) {
        const auto found = std::ranges::find(layout->nodes, *domain_node(node), &NodePlacement::node);
        if (found != layout->nodes.end() && std::isfinite(found->x) && std::isfinite(found->y)) {
            return {found->x, found->y};
        }
    }
    return nodes_.at(node).default_position;
}

QVariant ControlGraphModel::nodeData(NodeId node, NodeRole role) const
{
    if (!nodeExists(node)) { return {}; }
    switch (role) {
    case NodeRole::Caption: {
        const auto& value = active_workflow()->nodes[nodes_.at(node).source_index];
        auto name = QString::fromStdString(value.name);
        if (std::holds_alternative<GroupConfiguration>(value.configuration)) {
            name += QStringLiteral(" (Group)");
        }
        if (!value.enabled) { name += QStringLiteral(" (Disabled)"); }
        return name;
    }
    case NodeRole::CaptionVisible: return true;
    case NodeRole::InPortCount: return PortCount{1};
    case NodeRole::OutPortCount: return 1 + static_cast<PortCount>(nodes_.at(node).outlets.size());
    case NodeRole::Position: return position(node);
    case NodeRole::Size: return nodes_.at(node).size;
    case NodeRole::Style: return StyleCollection::nodeStyle().toJson();
    default: return {};
    }
}

bool ControlGraphModel::setNodeData(NodeId node, NodeRole role, QVariant value)
{
    if (role != NodeRole::Size || !nodeExists(node)) { return false; }
    // QtNodes computes its own item extents during geometry updates.
    const auto size = value.toSize();
    if (!size.isValid()) { return false; }
    nodes_.at(node).size = size;
    return true;
}

QVariant ControlGraphModel::portData(NodeId node, PortType type, PortIndex index, PortRole role) const
{
    if (!nodeExists(node) || type == PortType::None || (type == PortType::In && index != 0)
        || (type == PortType::Out && index > nodes_.at(node).outlets.size())) {
        return {};
    }
    switch (role) {
    case PortRole::Caption:
        if (type == PortType::In) { return QStringLiteral("In"); }
        return index == 0 ? QStringLiteral("Success")
                          : QString::fromStdString(nodes_.at(node).outlets[index - 1].display_name);
    case PortRole::CaptionVisible: return true;
    case PortRole::ConnectionPolicyRole: return QVariant::fromValue(ConnectionPolicy::Many);
    case PortRole::DataType:
        return QVariant::fromValue(NodeDataType{QStringLiteral("visionpro.control"), QStringLiteral("Control")});
    default: return {};
    }
}

core::Result<bool> ControlGraphModel::edit_draft(QUndoStack& history, const QString& label, const DraftEdit& edit)
{
    auto next = draft_;
    auto edited = edit(next);
    if (auto* failure = std::get_if<core::Diagnostic>(&edited)) { return std::move(*failure); }
    if (next.id != draft_.id || next.format_version != draft_.format_version || next.revision != draft_.revision) {
        return core::Diagnostic{core::ErrorCode::InvalidModel, core::Severity::Error, {},
            "An editor command must keep the draft identity, format and saved revision."};
    }
    if (auto checked = validate_model_shape(next); auto* failure = std::get_if<core::Diagnostic>(&checked)) {
        if (auto global = validate_global_invariants(next); auto* error = std::get_if<core::Diagnostic>(&global)) {
            return std::move(*error);
        }
        // A loaded problem in another field can coexist with an edit. A new
        // shape error is rejected before it can enter the graph or history.
        const auto previous = validate_model_shape(draft_);
        const auto* old = std::get_if<core::Diagnostic>(&previous);
        if (!old || old->code != failure->code || old->message != failure->message
            || old->location.workflow != failure->location.workflow || old->location.node != failure->location.node
            || old->location.schema != failure->location.schema || old->location.input != failure->location.input
            || old->location.output != failure->location.output || old->location.parameter != failure->location.parameter) {
            return std::move(*failure);
        }
    }
    history.push(new CanvasEdit(label, [this, next = std::move(next)]() mutable {
        auto scope = scope_;
        const auto* flow = workflow_id_ ? find_workflow(next, *workflow_id_) : nullptr;
        if (!flow) {
            flow = next.workflows.empty() ? nullptr : &next.workflows.front();
            scope.reset();
        }
        if (scope) {
            const auto* group = flow ? find_node(*flow, *scope) : nullptr;
            if (!group || !std::holds_alternative<GroupConfiguration>(group->configuration)) { scope.reset(); }
        }
        const auto workflow_id = flow ? std::optional{flow->id} : std::nullopt;
        auto projected = project_nodes(flow);
        // Save is a file operation. Its revision continues to apply to the
        // current draft even when an earlier edit is undone.
        next.revision = draft_.revision;
        std::swap(draft_, next);
        set_view(workflow_id, scope, std::move(projected));
    }));
    return true;
}

void ControlGraphModel::edit_links(QUndoStack& history, ControlLink changed, std::vector<ControlLink> next,
    const QString& label)
{
    history.push(new CanvasEdit(label, [this, workflow = *workflow_id_, changed = std::move(changed),
        links = std::move(next)]() mutable {
        auto* flow = editable_workflow(draft_, workflow);
        if (!flow) { return; }
        flow->control_links.swap(links);
        if (workflow_id_ != workflow) { return; }
        const auto connection = project(changed);
        if (!connection) { return; }
        if (connectionExists(*connection)) {
            Q_EMIT connectionCreated(*connection);
        } else {
            Q_EMIT connectionDeleted(*connection);
        }
    }));
}

core::Result<bool> ControlGraphModel::connect_control(QUndoStack& history, ConnectionId connection)
{
    if (!connectionPossible(connection)) { return false; }
    const auto generated = core::IdFactory::generate<core::ControlLinkId>();
    if (const auto* failure = std::get_if<core::Diagnostic>(&generated)) { return *failure; }
    std::optional<core::ControlOutputId> outlet;
    if (connection.outPortIndex != 0) {
        outlet = nodes_.at(connection.outNodeId).outlets[connection.outPortIndex - 1].id;
    }
    auto next = active_workflow()->control_links;
    const ControlLink changed{std::get<core::ControlLinkId>(generated), *domain_node(connection.outNodeId),
        outlet, *domain_node(connection.inNodeId)};
    next.push_back(changed);
    edit_links(history, changed, std::move(next), QStringLiteral("Connect control link"));
    return true;
}

core::Result<bool> ControlGraphModel::remove_control(QUndoStack& history, ConnectionId connection)
{
    const auto* flow = active_workflow();
    if (!flow) { return false; }
    const auto& links = flow->control_links;
    const auto matches = [&](const ControlLink& link) { return project(link) == connection; };
    const auto first = std::ranges::find_if(links, matches);
    if (first == links.end()) { return false; }
    if (std::find_if(std::next(first), links.end(), matches) != links.end()) {
        core::DiagnosticLocation location;
        location.workflow = flow->id;
        return core::Diagnostic{core::ErrorCode::InvalidControlLink, core::Severity::Error, location,
            "This line represents multiple control links. A single link cannot be selected on the canvas."};
    }
    auto next = links;
    next.erase(next.begin() + (first - links.begin()));
    edit_links(history, *first, std::move(next), QStringLiteral("Remove control link"));
    return true;
}

bool ControlGraphModel::move_nodes(QUndoStack& history, std::span<const CanvasMove> moves)
{
    if (!workflow_id_) { return false; }
    auto next = draft_.layouts;
    auto layout = std::ranges::find(next, *workflow_id_, &WorkflowLayout::workflow);
    if (layout == next.end()) {
        next.push_back({*workflow_id_, {}});
        layout = std::prev(next.end());
    }
    std::vector<core::NodeId> changed;
    std::unordered_set<NodeId> seen;
    for (const auto& [node, point] : moves) {
        if (!visible(node) || !std::isfinite(point.x()) || !std::isfinite(point.y())
            || !seen.insert(node).second) { return false; }
        if (point == position(node)) { continue; }
        const auto id = *domain_node(node);
        const auto found = std::ranges::find(layout->nodes, id, &NodePlacement::node);
        const NodePlacement placement{id, point.x(), point.y()};
        if (found == layout->nodes.end()) {
            layout->nodes.push_back(placement);
        } else {
            *found = placement;
        }
        changed.push_back(id);
    }
    if (changed.empty()) { return false; }
    history.push(new CanvasEdit(QStringLiteral("Move nodes"),
        [this, layouts = std::move(next), nodes = std::move(changed)]() mutable {
            draft_.layouts.swap(layouts);
            for (const auto& id : nodes) {
                const auto node = canvas_id(id);
                if (visible(node)) { Q_EMIT nodePositionUpdated(node); }
            }
        }));
    return true;
}

} // namespace visionpro::ui
