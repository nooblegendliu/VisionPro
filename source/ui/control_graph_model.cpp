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

Workflow& checked_workflow(Solution& draft, const core::WorkflowId& workflow)
{
    const auto shape = validate_model_shape(draft);
    if (const auto* failure = std::get_if<core::Diagnostic>(&shape)) {
        throw std::invalid_argument(failure->message);
    }
    const auto found = std::ranges::find(draft.workflows, workflow, &Workflow::id);
    if (found == draft.workflows.end() || found->nodes.size() >= InvalidNodeId) {
        throw std::invalid_argument("The requested workflow cannot be opened in the canvas.");
    }
    return *found;
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

ControlGraphModel::ControlGraphModel(Solution draft, const core::WorkflowId& workflow_id,
    const core::ToolRegistry& registry)
    : draft_(std::move(draft)), flow_(checked_workflow(draft_, workflow_id))
{
    nodes_.reserve(flow_.nodes.size());
    for (const auto& node : flow_.nodes) {
        ids_.emplace(node.id, static_cast<NodeId>(nodes_.size()));
        nodes_.push_back({control_outlets(node, registry), {}, {}});
    }
    for (const auto& relation : flow_.containment) {
        nodes_[canvas_id(relation.child)].parent = relation.parent;
    }
    std::map<std::optional<core::NodeId>, std::size_t> scope_counts;
    for (auto& node : nodes_) {
        const auto ordinal = scope_counts[node.parent]++;
        node.default_position = QPointF(240.0 * static_cast<double>(ordinal % 4),
            128.0 * static_cast<double>(ordinal / 4));
    }
}

NodeId ControlGraphModel::canvas_id(const core::NodeId& id) const
{
    const auto found = ids_.find(id);
    return found == ids_.end() ? InvalidNodeId : found->second;
}

QString ControlGraphModel::scope_path() const
{
    QStringList names;
    for (auto group = scope_; group; group = nodes_[canvas_id(*group)].parent) {
        names.prepend(QString::fromStdString(flow_.nodes[canvas_id(*group)].name));
    }
    names.prepend(QString::fromStdString(flow_.name));
    return names.join(QStringLiteral(" / "));
}

bool ControlGraphModel::open_group(NodeId node)
{
    if (!visible(node) || !std::holds_alternative<GroupConfiguration>(flow_.nodes[node].configuration)) {
        return false;
    }
    scope_ = flow_.nodes[node].id;
    Q_EMIT modelReset();
    return true;
}

bool ControlGraphModel::parent_scope()
{
    if (!scope_) { return false; }
    scope_ = nodes_[canvas_id(*scope_)].parent;
    Q_EMIT modelReset();
    return true;
}

bool ControlGraphModel::nodeExists(NodeId node) const
{
    return node < nodes_.size();
}

bool ControlGraphModel::visible(NodeId node) const
{
    return nodeExists(node) && nodes_[node].parent == scope_;
}

std::unordered_set<NodeId> ControlGraphModel::allNodeIds() const
{
    std::unordered_set<NodeId> ids;
    for (NodeId node = 0; node < nodes_.size(); ++node) {
        if (visible(node)) { ids.insert(node); }
    }
    return ids;
}

std::optional<ConnectionId> ControlGraphModel::project(const ControlLink& link) const
{
    const auto source = canvas_id(link.source);
    const auto target = canvas_id(link.target);
    if (!visible(source) || !visible(target)) { return std::nullopt; }
    if (!link.output) { return ConnectionId{source, 0, target, 0}; }

    const auto& outlets = nodes_[source].outlets;
    const auto found = std::ranges::find(outlets, *link.output, &core::ControlOutputDescriptor::id);
    if (found == outlets.end()) { return std::nullopt; }
    const auto port = static_cast<PortIndex>(found - outlets.begin()) + 1;
    return ConnectionId{source, port, target, 0};
}

std::unordered_set<ConnectionId> ControlGraphModel::allConnectionIds(NodeId node) const
{
    std::unordered_set<ConnectionId> ids;
    for (const auto& link : flow_.control_links) {
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
    return std::ranges::any_of(flow_.control_links, [&](const ControlLink& link) {
        return project(link) == connection;
    });
}

bool ControlGraphModel::connectionPossible(ConnectionId connection) const
{
    const auto [source, outlet, target, inlet] = connection;
    if (!visible(source) || !visible(target) || inlet != 0 || outlet > nodes_[source].outlets.size()
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
        for (const auto& link : flow_.control_links) {
            if (link.source != flow_.nodes[current].id) { continue; }
            const auto next = canvas_id(link.target);
            if (next == source) { return false; }
            if (visible(next) && visited.insert(next).second) { pending.push_back(next); }
        }
    }
    return true;
}

QPointF ControlGraphModel::position(NodeId node) const
{
    const auto layout = std::ranges::find(draft_.layouts, flow_.id, &WorkflowLayout::workflow);
    if (layout != draft_.layouts.end()) {
        const auto found = std::ranges::find(layout->nodes, flow_.nodes[node].id, &NodePlacement::node);
        if (found != layout->nodes.end()) { return {found->x, found->y}; }
    }
    return nodes_[node].default_position;
}

QVariant ControlGraphModel::nodeData(NodeId node, NodeRole role) const
{
    if (!nodeExists(node)) { return {}; }
    switch (role) {
    case NodeRole::Caption: {
        const auto& value = flow_.nodes[node];
        auto name = QString::fromStdString(value.name);
        if (std::holds_alternative<GroupConfiguration>(value.configuration)) {
            name += QStringLiteral(" (Group)");
        }
        return name;
    }
    case NodeRole::CaptionVisible: return true;
    case NodeRole::InPortCount: return PortCount{1};
    case NodeRole::OutPortCount: return 1 + static_cast<PortCount>(nodes_[node].outlets.size());
    case NodeRole::Position: return position(node);
    case NodeRole::Size: return nodes_[node].size;
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
    nodes_[node].size = size;
    return true;
}

QVariant ControlGraphModel::portData(NodeId node, PortType type, PortIndex index, PortRole role) const
{
    if (!nodeExists(node) || type == PortType::None || (type == PortType::In && index != 0)
        || (type == PortType::Out && index > nodes_[node].outlets.size())) {
        return {};
    }
    switch (role) {
    case PortRole::Caption:
        if (type == PortType::In) { return QStringLiteral("In"); }
        return index == 0 ? QStringLiteral("Success")
                          : QString::fromStdString(nodes_[node].outlets[index - 1].display_name);
    case PortRole::CaptionVisible: return true;
    case PortRole::ConnectionPolicyRole: return QVariant::fromValue(ConnectionPolicy::Many);
    case PortRole::DataType:
        return QVariant::fromValue(NodeDataType{QStringLiteral("visionpro.control"), QStringLiteral("Control")});
    default: return {};
    }
}

void ControlGraphModel::edit_links(QUndoStack& history, ConnectionId connection, std::vector<ControlLink> next,
    const QString& label)
{
    history.push(new CanvasEdit(label, [this, connection, links = std::move(next)]() mutable {
        flow_.control_links.swap(links);
        if (!visible(connection.outNodeId) || !visible(connection.inNodeId)) { return; }
        if (connectionExists(connection)) {
            Q_EMIT connectionCreated(connection);
        } else {
            Q_EMIT connectionDeleted(connection);
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
        outlet = nodes_[connection.outNodeId].outlets[connection.outPortIndex - 1].id;
    }
    auto next = flow_.control_links;
    next.push_back({std::get<core::ControlLinkId>(generated), flow_.nodes[connection.outNodeId].id,
        outlet, flow_.nodes[connection.inNodeId].id});
    edit_links(history, connection, std::move(next), QStringLiteral("Connect control link"));
    return true;
}

core::Result<bool> ControlGraphModel::remove_control(QUndoStack& history, ConnectionId connection)
{
    const auto matches = [&](const ControlLink& link) { return project(link) == connection; };
    const auto first = std::ranges::find_if(flow_.control_links, matches);
    if (first == flow_.control_links.end()) { return false; }
    if (std::find_if(std::next(first), flow_.control_links.end(), matches) != flow_.control_links.end()) {
        core::DiagnosticLocation location;
        location.workflow = flow_.id;
        return core::Diagnostic{core::ErrorCode::InvalidControlLink, core::Severity::Error, location,
            "This line represents multiple control links. A single link cannot be selected on the canvas."};
    }
    auto next = flow_.control_links;
    next.erase(next.begin() + (first - flow_.control_links.begin()));
    edit_links(history, connection, std::move(next), QStringLiteral("Remove control link"));
    return true;
}

bool ControlGraphModel::move_nodes(QUndoStack& history, std::span<const CanvasMove> moves)
{
    auto next = draft_.layouts;
    auto layout = std::ranges::find(next, flow_.id, &WorkflowLayout::workflow);
    if (layout == next.end()) {
        next.push_back({flow_.id, {}});
        layout = std::prev(next.end());
    }
    std::vector<NodeId> changed;
    std::unordered_set<NodeId> seen;
    for (const auto& [node, point] : moves) {
        if (!visible(node) || !std::isfinite(point.x()) || !std::isfinite(point.y())
            || !seen.insert(node).second) { return false; }
        if (point == position(node)) { continue; }
        const auto id = flow_.nodes[node].id;
        const auto found = std::ranges::find(layout->nodes, id, &NodePlacement::node);
        const NodePlacement placement{id, point.x(), point.y()};
        if (found == layout->nodes.end()) {
            layout->nodes.push_back(placement);
        } else {
            *found = placement;
        }
        changed.push_back(node);
    }
    if (changed.empty()) { return false; }
    history.push(new CanvasEdit(QStringLiteral("Move nodes"),
        [this, layouts = std::move(next), nodes = std::move(changed)]() mutable {
            draft_.layouts.swap(layouts);
            for (const auto node : nodes) {
                if (visible(node)) { Q_EMIT nodePositionUpdated(node); }
            }
        }));
    return true;
}

} // namespace visionpro::ui
