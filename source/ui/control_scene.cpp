#include "control_scene.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QSignalBlocker>

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/copy.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <set>

namespace visionpro::ui {

using namespace QtNodes;

namespace {

using EditResult = core::Result<std::monostate>;
using namespace workflow;

core::Diagnostic edit_error(core::ErrorCode code, std::string message)
{
    return {code, core::Severity::Error, {}, std::move(message)};
}

Workflow* editable_workflow(Solution& draft, const core::WorkflowId& id)
{
    const auto found = std::ranges::find(draft.workflows, id, &Workflow::id);
    return found == draft.workflows.end() ? nullptr : &*found;
}

WorkflowLayout& layout_for(Solution& draft, const core::WorkflowId& id)
{
    const auto found = std::ranges::find(draft.layouts, id, &WorkflowLayout::workflow);
    if (found != draft.layouts.end()) { return *found; }
    return draft.layouts.emplace_back(WorkflowLayout{id, {}});
}

EditResult append_node(Solution& draft, const ControlGraphModel& model, Node node, QPointF position)
{
    const auto selected = model.workflow_id();
    auto* flow = selected ? editable_workflow(draft, *selected) : nullptr;
    if (!flow) { return edit_error(core::ErrorCode::UnknownTarget, "Select a workflow before adding nodes."); }
    const auto scope = model.scope();
    const bool first_in_scope = model.allNodeIds().empty();
    const auto id = node.id;
    if (scope) {
        const auto parent = std::ranges::find(flow->nodes, *scope, &Node::id);
        if (parent == flow->nodes.end() || !std::holds_alternative<GroupConfiguration>(parent->configuration)) {
            return edit_error(core::ErrorCode::InvalidContainment, "The selected Group is absent from the workflow.");
        }
        flow->containment.push_back({*scope, id});
        if (first_in_scope) { std::get<GroupConfiguration>(parent->configuration).entries.push_back(id); }
    } else if (first_in_scope) {
        flow->entries.push_back(id);
    }
    flow->nodes.push_back(std::move(node));
    layout_for(draft, flow->id).nodes.push_back({id, position.x(), position.y()});
    return std::monostate{};
}

void delete_subgraph(Solution& draft, Workflow& flow, const std::set<core::NodeId>& removed)
{
    const auto obsolete_output = [&](const OutputMapping& export_item) {
        return export_item.source.workflow == flow.id && removed.contains(export_item.source.node);
    };
    const auto obsolete_control = [&](const ControlOutputMapping& export_item) {
        return removed.contains(export_item.source);
    };
    const auto obsolete_entry = [&](const core::NodeId& entry) { return removed.contains(entry); };
    std::erase_if(flow.nodes, [&](const Node& node) { return removed.contains(node.id); });
    std::erase_if(flow.control_links, [&](const ControlLink& link) {
        return removed.contains(link.source) || removed.contains(link.target);
    });
    std::erase_if(flow.containment, [&](const Containment& relation) {
        return removed.contains(relation.parent) || removed.contains(relation.child);
    });
    std::erase_if(flow.entries, obsolete_entry);
    std::erase_if(flow.outputs, obsolete_output);
    std::erase_if(flow.control_exports, obsolete_control);
    for (auto& node : flow.nodes) {
        if (auto* group = std::get_if<GroupConfiguration>(&node.configuration)) {
            if (group->loop && group->loop->break_source && removed.contains(*group->loop->break_source)) {
                group->loop->break_source.reset();
            }
            std::erase_if(group->entries, obsolete_entry);
            std::erase_if(group->outputs, obsolete_output);
            std::erase_if(group->control_exports, obsolete_control);
        }
    }
    if (const auto layout = std::ranges::find(draft.layouts, flow.id, &WorkflowLayout::workflow);
        layout != draft.layouts.end()) {
        std::erase_if(layout->nodes, [&](const NodePlacement& position) { return removed.contains(position.node); });
    }
}

} // namespace

ControlScene::ControlScene(ControlGraphModel& model, QObject* parent)
    : BasicGraphicsScene(model, parent), model_(model) {}

bool ControlScene::open_group(NodeId node)
{
    return model_.open_group(node);
}

bool ControlScene::parent_scope()
{
    return model_.parent_scope();
}

bool ControlScene::select_workflow(const core::WorkflowId& workflow)
{
    try {
        return model_.select_workflow(workflow);
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ControlScene::reveal_node(const core::NodeId& node)
{
    try {
        if (!model_.reveal_node(node)) { return false; }
        auto* item = nodeGraphicsObject(model_.canvas_id(node));
        if (!item) { return false; }
        clearSelection();
        item->setSelected(true);
        for (auto* view : views()) { view->ensureVisible(item); }
        return true;
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
        return false;
    }
}

std::vector<core::NodeId> ControlScene::selected_nodes() const
{
    std::vector<core::NodeId> result;
    for (const auto* item : selectedItems()) {
        if (const auto* node = qgraphicsitem_cast<const NodeGraphicsObject*>(item)) {
            if (const auto id = model_.domain_node(node->nodeId())) { result.push_back(*id); }
        }
    }
    return result;
}

bool ControlScene::report_edit(core::Result<bool> result)
{
    if (const auto* changed = std::get_if<bool>(&result)) { return *changed; }
    Q_EMIT edit_failed(QString::fromStdString(std::get<core::Diagnostic>(result).message));
    return false;
}

bool ControlScene::edit_draft(const QString& label, const DraftEdit& edit)
{
    if (!editing_enabled_) { return false; }
    try {
        return report_edit(model_.edit_draft(undoStack(), label, edit));
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
        return false;
    }
}

void ControlScene::set_editing_enabled(bool enabled)
{
    editing_enabled_ = enabled;
    if (!enabled) { cancel_gesture(); }
}

bool ControlScene::add_workflow(std::string name)
{
    std::optional<core::WorkflowId> added;
    const bool changed = edit_draft(tr("Add workflow"), [&](workflow::Solution& draft) -> EditResult {
        auto generated = core::IdFactory::generate<core::WorkflowId>();
        if (auto* failure = std::get_if<core::Diagnostic>(&generated)) { return std::move(*failure); }
        const auto id = std::get<core::WorkflowId>(generated);
        auto schema = core::SchemaIdFactory::parse<core::SchemaId>("visionpro.workflow." + id.to_string());
        if (auto* failure = std::get_if<core::Diagnostic>(&schema)) { return std::move(*failure); }
        draft.workflows.push_back({id, std::move(name), {std::get<core::SchemaId>(schema), 1, {}, {}, {}},
            {}, {}, {}, {}, {}, true, {}});
        added = id;
        return std::monostate{};
    });
    if (changed) { select_workflow(*added); }
    return changed;
}

bool ControlScene::remove_workflow(core::WorkflowId workflow)
{
    return edit_draft(tr("Delete workflow"), [workflow](workflow::Solution& draft) -> EditResult {
        if (!std::erase_if(draft.workflows, [workflow](const auto& flow) { return flow.id == workflow; })) {
            return edit_error(core::ErrorCode::UnknownTarget, "The workflow is absent from the draft.");
        }
        std::erase_if(draft.layouts, [workflow](const auto& layout) { return layout.workflow == workflow; });
        return std::monostate{};
    });
}

bool ControlScene::add_tool(core::ToolTypeId type, QPointF position)
{
    std::optional<core::NodeId> added;
    const bool changed = edit_draft(tr("Add tool"), [&](workflow::Solution& draft) -> EditResult {
        const auto* tool = model_.registry_.find_descriptor(type);
        if (!tool) { return edit_error(core::ErrorCode::UnknownTool, "The selected tool is not installed."); }
        auto generated = core::IdFactory::generate<core::NodeId>();
        if (auto* failure = std::get_if<core::Diagnostic>(&generated)) { return std::move(*failure); }
        added = std::get<core::NodeId>(generated);
        return append_node(draft, model_, {*added, tool->display_name, workflow::ToolConfiguration{type, tool->config_version},
            {}, {}}, position);
    });
    if (changed) { reveal_node(*added); }
    return changed;
}

bool ControlScene::add_group(std::string name, QPointF position, GroupKind kind)
{
    std::optional<core::NodeId> added;
    const bool changed = edit_draft(kind == GroupKind::Loop ? tr("Add Loop") : tr("Add Group"), [&](workflow::Solution& draft) -> EditResult {
        auto generated = core::IdFactory::generate<core::NodeId>();
        if (auto* failure = std::get_if<core::Diagnostic>(&generated)) { return std::move(*failure); }
        added = std::get<core::NodeId>(generated);
        auto schema = core::SchemaIdFactory::parse<core::SchemaId>("visionpro.group." + added->to_string());
        if (auto* failure = std::get_if<core::Diagnostic>(&schema)) { return std::move(*failure); }
        workflow::GroupConfiguration group{{std::get<core::SchemaId>(schema), 1, {}, {}, {}}, {}, {}, {}};
        std::vector<workflow::Binding> bindings;
        if (kind == GroupKind::Loop) {
            group.loop.emplace();
            const core::BindingPolicy policy{{core::BindingSourceKind::Constant, core::BindingSourceKind::PublicInput,
                core::BindingSourceKind::RunOutput, core::BindingSourceKind::GlobalRead}};
            for (const auto* key : {"start", "end"}) {
                const auto id = std::get<core::InputId>(core::SchemaIdFactory::parse<core::InputId>(key));
                group.interface.inputs.push_back({id, key, core::DataType::integer(), true, {}, {}, policy, {}});
                bindings.push_back({id, {core::ConfigValue::integer(0)}});
            }
            const auto index = std::get<core::InputId>(core::SchemaIdFactory::parse<core::InputId>("index"));
            group.interface.inputs.push_back({index, "Iteration index", core::DataType::integer(), false,
                core::ConfigValue::integer(0), {}, {}, {}});
        }
        return append_node(draft, model_, {*added, std::move(name), std::move(group), std::move(bindings), {}}, position);
    });
    if (changed) { reveal_node(*added); }
    return changed;
}

bool ControlScene::remove_selected_nodes()
{
    if (!editing_enabled_ || !model_.workflow_id()) { return false; }
    const auto selected = selected_nodes();
    if (selected.empty()) { return false; }
    const auto workflow = *model_.workflow_id();
    return edit_draft(tr("Delete nodes"), [&](workflow::Solution& draft) -> EditResult {
        auto& flow = *editable_workflow(draft, workflow);
        std::set<core::NodeId> removed(selected.begin(), selected.end());
        for (const auto& id : selected) {
            const auto* node = workflow::find_node(flow, id);
            if (!node || !std::holds_alternative<workflow::GroupConfiguration>(node->configuration)) { continue; }
            auto descendants = workflow::descendants(flow, id);
            if (auto* failure = std::get_if<core::Diagnostic>(&descendants)) { return std::move(*failure); }
            const auto& children = std::get<std::vector<core::NodeId>>(descendants);
            removed.insert(children.begin(), children.end());
        }
        // Other bindings remain addressable for diagnosis and repair if
        // their source was deleted. Undo can restore that exact source.
        delete_subgraph(draft, flow, removed);
        return std::monostate{};
    });
}

bool ControlScene::copy_selected_nodes()
{
    try {
        auto selected = selected_nodes();
        const auto* flow = model_.active_workflow();
        if (!flow || selected.empty()) { return false; }
        Clipboard copied{*flow, model_.scope(), std::move(selected), {}};
        copied.placements.reserve(flow->nodes.size());
        for (const auto& node : flow->nodes) {
            const auto point = model_.position(model_.canvas_id(node.id));
            copied.placements.push_back({node.id, point.x(), point.y()});
        }
        clipboard_ = std::move(copied);
        return true;
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
        return false;
    }
}

bool ControlScene::paste_nodes()
{
    if (!editing_enabled_ || !clipboard_) { return false; }
    if (model_.workflow_id() != clipboard_->source.id || model_.scope() != clipboard_->scope) {
        Q_EMIT edit_failed(tr("Paste into the workflow and Group where the nodes were copied."));
        return false;
    }
    std::vector<core::NodeId> roots;
    std::size_t omitted = 0;
    const bool changed = edit_draft(tr("Paste nodes"), [&](workflow::Solution& draft) -> EditResult {
        auto copied = workflow::clone_subgraph(clipboard_->source, clipboard_->selection,
            workflow::ExternalReferencePolicy::Preserve);
        if (auto* failure = std::get_if<core::Diagnostic>(&copied)) { return std::move(*failure); }
        auto& fragment = std::get<workflow::SubgraphCopy>(copied);
        auto& flow = *editable_workflow(draft, clipboard_->source.id);
        auto& placements = layout_for(draft, flow.id).nodes;
        const double offset = 28.0 * (static_cast<double>(clipboard_->paste_count) + 1.0);
        for (const auto& position : clipboard_->placements) {
            const auto id = fragment.identities.nodes.find(position.node);
            if (id == fragment.identities.nodes.end()) { continue; }
            const bool root = std::ranges::find(clipboard_->selection, position.node) != clipboard_->selection.end();
            if (root) { roots.push_back(id->second); }
            placements.push_back({id->second, position.x + (root ? offset : 0.0), position.y + (root ? offset : 0.0)});
        }
        flow.nodes.insert(flow.nodes.end(), std::make_move_iterator(fragment.nodes.begin()),
            std::make_move_iterator(fragment.nodes.end()));
        flow.control_links.insert(flow.control_links.end(), std::make_move_iterator(fragment.control_links.begin()),
            std::make_move_iterator(fragment.control_links.end()));
        flow.containment.insert(flow.containment.end(), fragment.containment.begin(), fragment.containment.end());
        omitted = fragment.report.omitted_control_links.size();
        return std::monostate{};
    });
    if (!changed) { return false; }
    ++clipboard_->paste_count;
    clearSelection();
    for (const auto& root : roots) {
        if (auto* item = nodeGraphicsObject(model_.canvas_id(root))) { item->setSelected(true); }
    }
    if (omitted) {
        Q_EMIT edit_notice(tr("Pasted the selected subgraph. %1 boundary control links stayed in the source.")
            .arg(omitted));
    }
    return true;
}

bool ControlScene::connect_control(ConnectionId connection)
{
    if (!editing_enabled_) { return false; }
    try {
        return report_edit(model_.connect_control(undoStack(), connection));
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
    }
    return false;
}

bool ControlScene::remove_control(ConnectionId connection)
{
    if (!editing_enabled_) { return false; }
    try {
        return report_edit(model_.remove_control(undoStack(), connection));
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
    }
    return false;
}

bool ControlScene::move_nodes(std::span<const CanvasMove> moves)
{
    if (!editing_enabled_) { return false; }
    try {
        return model_.move_nodes(undoStack(), moves);
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
        return false;
    }
}

std::optional<ConnectionId> ControlScene::selected_control() const
{
    const auto selected = selectedItems();
    if (selected.size() == 1) {
        if (const auto* link = qgraphicsitem_cast<ConnectionGraphicsObject*>(selected.front())) {
            return link->connectionId();
        }
    }
    return std::nullopt;
}

void ControlScene::onModelReset()
{
    const bool had_selection = !selectedItems().isEmpty();
    const auto selected = selected_nodes();
    drag_origins_.clear();
    resetDraftConnection();
    {
        const QSignalBlocker block(this);
        BasicGraphicsScene::onModelReset();
        for (const auto& id : selected) {
            if (auto* item = nodeGraphicsObject(model_.canvas_id(id))) { item->setSelected(true); }
        }
    }
    if (had_selection) { Q_EMIT selectionChanged(); }
}

void ControlScene::cancel_gesture()
{
    resetDraftConnection();
    for (const auto& move : drag_origins_) {
        if (auto* item = nodeGraphicsObject(move.node)) {
            item->setPos(model_.nodeData(move.node, NodeRole::Position).toPointF());
        }
    }
    drag_origins_.clear();
}

void ControlScene::focusOutEvent(QFocusEvent* event)
{
    cancel_gesture();
    BasicGraphicsScene::focusOutEvent(event);
}

void ControlScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    if (auto* link = qgraphicsitem_cast<ConnectionGraphicsObject*>(mouseGrabberItem())) {
        // Qt 6 puts the viewport widget on the event. QtNodes 3.0.16's
        // connection handlers cast it to QGraphicsView, so handle the gesture here.
        const auto port = link->connectionState().requiredPort();
        if (port != PortType::None) {
            if (!editing_enabled_) {
                resetDraftConnection();
                event->accept();
                return;
            }
            const auto previous_bounds = link->sceneBoundingRect();
            link->setEndPoint(port, link->mapFromScene(event->scenePos()));
            link->move();
            update(previous_bounds);
        }
        event->accept();
        return;
    }
    auto* grabbed = qgraphicsitem_cast<NodeGraphicsObject*>(mouseGrabberItem());
    if (!grabbed || !event->buttons().testFlag(Qt::LeftButton)) {
        BasicGraphicsScene::mouseMoveEvent(event);
        return;
    }
    if (!editing_enabled_) {
        event->accept();
        return;
    }

    if (drag_origins_.empty()) {
        if (!grabbed->isSelected()) {
            if (!event->modifiers().testFlag(Qt::ControlModifier)) { clearSelection(); }
            grabbed->setSelected(true);
        }
        for (auto* item : selectedItems()) {
            if (const auto* node = qgraphicsitem_cast<NodeGraphicsObject*>(item)) {
                drag_origins_.push_back({node->nodeId(), node->pos()});
            }
        }
    }
    // Preview only. A completed gesture creates a single layout command.
    const auto delta = event->scenePos() - event->buttonDownScenePos(Qt::LeftButton);
    for (const auto& [node, origin] : drag_origins_) {
        nodeGraphicsObject(node)->setPos(origin + delta);
    }
    event->accept();
}

void ControlScene::finish_connection(const ConnectionId& draft, const QPointF& scene_point)
{
    const auto required = draft.inNodeId == InvalidNodeId ? PortType::In : PortType::Out;
    for (auto* item : items(scene_point, Qt::IntersectsItemBoundingRect)) {
        if (auto* node = qgraphicsitem_cast<NodeGraphicsObject*>(item)) {
            const auto port = nodeGeometry().checkPortHit(node->nodeId(), required, node->mapFromScene(scene_point));
            if (port != InvalidPortIndex) {
                connect_control(makeCompleteConnectionId(draft, node->nodeId(), port));
                return;
            }
        }
    }
}

void ControlScene::mouseReleaseEvent(QGraphicsSceneMouseEvent* event)
{
    if (auto* link = qgraphicsitem_cast<ConnectionGraphicsObject*>(mouseGrabberItem())) {
        const auto connection = link->connectionId();
        const bool draft = link->connectionState().requiredPort() != PortType::None;
        if (draft && event->buttons() == Qt::NoButton) {
            // The domain command owns the stable ID of the completed connection.
            resetDraftConnection();
            if (event->button() == Qt::LeftButton) { finish_connection(connection, event->scenePos()); }
        } else if (!draft && event->buttons() == Qt::NoButton) {
            link->ungrabMouse();
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        if (!drag_origins_.empty()) {
            auto moves = std::move(drag_origins_);
            drag_origins_.clear();
            const auto delta = event->scenePos() - event->buttonDownScenePos(Qt::LeftButton);
            for (auto& [node, origin] : moves) {
                nodeGraphicsObject(node)->setPos(origin);
                origin += delta;
            }
            move_nodes(moves);
        }
    }
    BasicGraphicsScene::mouseReleaseEvent(event);
}

} // namespace visionpro::ui
