#include "control_scene.hpp"
#include "image_source_fields.hpp"
#include "workspace_appearance.hpp"
#include "workspace_style.hpp"
#include "run_panel.hpp"
#include "toolbox_catalog.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/DefaultNodePainter>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/DefaultConnectionPainter.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/NodeStyle.hpp>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QSignalBlocker>
#include <QTextDocument>

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

constexpr int title_flags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;

// Control nodes have a title, one input and one or more execution outlets.
// The geometry is also used by QtNodes for wire endpoints and hit testing.
class ControlNodeGeometry final : public AbstractNodeGeometry {
public:
    ControlNodeGeometry(AbstractGraphModel& graph, Qt::Orientation orientation)
        : AbstractNodeGeometry(graph), vertical_(orientation == Qt::Vertical)
    {}

    QSize size(NodeId id) const override { return _graphModel.nodeData<QSize>(id, NodeRole::Size); }
    QRectF boundingRect(NodeId id) const override { return QRectF(QPointF{}, size(id)).adjusted(-20, -20, 20, 20); }
    QPointF widgetPosition(NodeId) const override { return {}; }
    QRect resizeHandleRect(NodeId) const override { return {}; }

    void recomputeSize(NodeId id) const override { _graphModel.setNodeData(id, NodeRole::Size, measured_size(id)); }

    QSize measured_size(NodeId id) const
    {
        const int ports = out_count(id);
        const int port_width = widest_label(id);
        const int width = vertical_ ? ports * slot_width(port_width) : port_width + 2 * padding;
        const int port_height = vertical_ ? (port_width ? port_metrics_.height() + 3 : 0) : ports * row_height();
        const int name_width = title_metrics_.horizontalAdvance(_graphModel.nodeData<QString>(id, NodeRole::Caption));
        const int title_width = std::clamp(name_width + 2 * padding + icon_space, 110, 134);
        return {std::max(width, title_width), header_height() + 2 * padding + port_height};
    }

    QRectF captionRect(NodeId id) const override
    {
        return {padding + icon_space, padding, qreal(size(id).width() - 2 * padding - icon_space),
            qreal(title_metrics_.height())};
    }
    QPointF captionPosition(NodeId id) const override { return captionRect(id).topLeft() + QPointF(0, title_metrics_.ascent()); }

    QPointF portPosition(NodeId id, PortType type, PortIndex index) const override
    {
        const QSize node = size(id);
        if (!vertical_) {
            const qreal y = padding + header_height() + (index + 0.5) * row_height();
            return {type == PortType::In ? 0.0 : qreal(node.width()), y};
        }
        if (type == PortType::In) { return {node.width() / 2.0, 0.0}; }
        const int step = slot_width(widest_label(id));
        return {node.width() / 2.0 + (index - (out_count(id) - 1) / 2.0) * step, qreal(node.height())};
    }

    QPointF portTextPosition(NodeId id, PortType type, PortIndex index) const override
    {
        const auto label = _graphModel.portData<QString>(id, type, index, PortRole::Caption);
        const int width = port_metrics_.horizontalAdvance(label);
        const QPointF port = portPosition(id, type, index);
        if (vertical_) { return {port.x() - width / 2.0, port.y() - 5 - port_metrics_.descent()}; }
        return {type == PortType::In ? qreal(padding) : port.x() - padding - width,
            port.y() + (port_metrics_.ascent() - port_metrics_.descent()) / 2.0};
    }

    const QFont title_font = workspace_font(WorkspaceText::NodeTitle);
    const QFont port_font = workspace_font(WorkspaceText::Secondary);

private:
    static constexpr int padding = workspace_size::gap;
    static constexpr int icon_space = workspace_size::toolbar_icon + workspace_size::small_gap;
    int header_height() const { return title_metrics_.height() + port_metrics_.height(); }
    int out_count(NodeId id) const { return _graphModel.nodeData<int>(id, NodeRole::OutPortCount); }
    int widest_label(NodeId id) const
    {
        int width = 0;
        const auto count = _graphModel.nodeData<PortCount>(id, NodeRole::OutPortCount);
        for (PortIndex index = 0; index < count; ++index) {
            const auto label = _graphModel.portData<QString>(id, PortType::Out, index, PortRole::Caption);
            width = std::max(width, port_metrics_.horizontalAdvance(label));
        }
        return width;
    }
    int row_height() const { return port_metrics_.height() + 7; }
    static int slot_width(int label_width) { return std::max(24, label_width + 10); }
    const QFontMetrics title_metrics_{title_font};
    const QFontMetrics port_metrics_{port_font};
    const bool vertical_;
};

class ControlNodePainter final : public DefaultNodePainter {
public:
    void paint(QPainter* painter, NodeGraphicsObject& node) const override
    {
        const auto& scene = static_cast<const ControlScene&>(*node.nodeScene());
        const bool show_ports = scene.show_available_ports(node);
        drawNodeRect(painter, node);
        if (show_ports) { drawConnectionPoints(painter, node); }
        drawFilledConnectionPoints(painter, node);
        const auto& model = node.graphModel();
        const auto id = node.nodeId();
        const auto& geometry = static_cast<const ControlNodeGeometry&>(scene.nodeGeometry());
        const NodeStyle style(QJsonDocument::fromVariant(model.nodeData(id, NodeRole::Style)).object());
        painter->setPen(style.FontColor);
        painter->setFont(geometry.title_font);
        const auto caption = geometry.captionRect(id);
        const auto name = model.nodeData<QString>(id, NodeRole::Caption);
        const auto type = model.nodeData<QString>(id, NodeRole::Type);
        if (dark_ != workspace_is_dark()) { icons_.clear(); dark_ = workspace_is_dark(); }
        auto icon = icons_.find(type);
        if (icon == icons_.end()) { icon = icons_.insert(type, toolbox_entry(type.toStdString(), {}).icon); }
        const int side = workspace_size::toolbar_icon;
        icon->paint(painter, QRect(workspace_size::gap, int(caption.center().y()) - side / 2, side, side));
        painter->drawText(caption, title_flags, QFontMetrics(geometry.title_font).elidedText(name, Qt::ElideMiddle, int(caption.width())));
        painter->setFont(geometry.port_font);
        const auto badge = scene.node_badge(id);
        auto status_box = caption.translated(0, caption.height());
        status_box.setHeight(QFontMetrics(geometry.port_font).height());
        const auto ordinal = QString::number(static_cast<const ControlGraphModel&>(model).display_number(id));
        painter->setPen(style.FontColorFaded);
        painter->drawText(QRectF(2, status_box.top(), caption.left() - 4, status_box.height()),
            Qt::AlignCenter, ordinal);
        const auto& colors = workspace_colors();
        QColor ink = style.FontColorFaded;
        if (badge.completion) { ink = *badge.completion == 1 ? colors.positive : colors.error; }
        else if (badge.status == ExecutionStatus::Failed || badge.status == ExecutionStatus::TimedOut) { ink = colors.error; }
        else if (badge.status == ExecutionStatus::Cancelled) { ink = colors.warning; }
        else if (badge.judgement == Judgement::OK) { ink = colors.positive; }
        else if (badge.judgement == Judgement::NG) { ink = colors.negative; }
        painter->setPen(ink);
        painter->drawText(status_box, title_flags, QFontMetrics(geometry.port_font).elidedText(badge.text, Qt::ElideRight, int(caption.width())));
        node.setToolTip(Qt::convertFromPlainText(ordinal + QStringLiteral("  ") + name + u'\n' + badge.text + badge.elapsed));
        painter->setPen(style.FontColor);
        if (show_ports) {
            drawEntryLabels(painter, node);
        } else {
            // The ordinary continuation is implicit. Keep only wired branch labels visible.
            const auto count = model.nodeData<PortCount>(id, NodeRole::OutPortCount);
            for (PortIndex port = 1; port < count; ++port) {
                if (!model.connections(id, PortType::Out, port).empty()) {
                    painter->drawText(geometry.portTextPosition(id, PortType::Out, port),
                        model.portData<QString>(id, PortType::Out, port, PortRole::Caption));
                }
            }
        }
        drawValidationIcon(painter, node);
    }
private:
    mutable QHash<QString, QIcon> icons_;
    mutable bool dark_{};
};

class ControlConnectionPainter final : public DefaultConnectionPainter {
public:
    void paint(QPainter* painter, const ConnectionGraphicsObject& edge) const override
    {
        DefaultConnectionPainter::paint(painter, edge);
        if (edge.connectionState().requiresPort()) { return; }
        const auto [c1, c2] = edge.pointsC1C2();
        QPainterPath path(edge.endPoint(PortType::Out));
        path.cubicTo(c1, c2, edge.endPoint(PortType::In));
        const auto tip = path.pointAtPercent(0.84);
        const QLineF tangent(path.pointAtPercent(0.82), tip);
        if (tangent.length() < 0.1) { return; }
        const auto direction = tangent.unitVector().p2() - tangent.p1();
        const QPointF perpendicular(-direction.y(), direction.x());
        const auto base = tip - 8 * direction;
        painter->save();
        painter->setPen(Qt::NoPen);
        const auto& colors = workspace_colors();
        painter->setBrush(edge.isSelected() ? colors.connectionSelected : colors.connection);
        painter->drawPolygon(QPolygonF{tip, base + 4 * perpendicular, base - 4 * perpendicular});
        painter->restore();
    }
};

void apply_connection_colors()
{
    const auto& colors = workspace_colors();
    auto style = StyleCollection::connectionStyle();
    style.loadJson({{QStringLiteral("ConnectionStyle"), QJsonObject{
        {QStringLiteral("NormalColor"), colors.connection.name()},
        {QStringLiteral("ConstructionColor"), colors.connection.name()},
        {QStringLiteral("SelectedColor"), colors.connectionSelected.name()},
        {QStringLiteral("SelectedHaloColor"), colors.connectionSelected.darker(175).name()},
        {QStringLiteral("HoveredColor"), colors.connection.darker(150).name()},
        {QStringLiteral("UseDataDefinedColors"), false}}}});
    StyleCollection::setConnectionStyle(style);
}

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
        std::erase_if(node.display_styles,[&](const auto& style) {
            return style.source.workflow == flow.id && removed.contains(style.source.node);
        });
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
    : BasicGraphicsScene(model, parent), model_(model)
{
    setNodePainter(std::make_unique<ControlNodePainter>());
    setConnectionPainter(std::make_unique<ControlConnectionPainter>());
    apply_connection_colors();
    connect(&WorkspaceAppearance::instance(), &WorkspaceAppearance::colorsChanged, this, [this] {
        apply_connection_colors();
        // QtNodes caches node pixels in device coordinates. Invalidate each item in place.
        for (auto* item : items()) { item->update(); }
    });
    onModelReset();
}

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

void ControlScene::display_run(const std::optional<RunHandle>& run, std::optional<core::ExecutionScopeId> scope,
    std::optional<core::InvocationKey> invocation)
{
    if (run && finished_run_ == run->identity() && display_scope_ == scope && display_call_ == invocation) { return; }
    finished_run_.reset();
    display_scope_ = scope;
    display_call_ = invocation;
    std::map<core::NodeId, NodeBadge> next;
    if (run && run->identity().workflow == model_.workflow_id()) {
        auto inspected = run->inspect();
        if (const auto* progress = std::get_if<RunProgress>(&inspected)) {
            for (const auto& call : progress->nodes) {
                // Root progress has no explicit scope; its invocation uses scope zero.
                if (scope && scope->value() != (call.scope ? call.scope->value() : 0)) { continue; }
                if (invocation && call.node == invocation->node && call.invocation != invocation) { continue; }
                auto [item, unique] = next.try_emplace(call.node);
                if (!unique) { item->second = {tr("请选择调用")}; continue; }
                auto& badge = item->second;
                badge.status = call.status;
                auto& label = badge.text;
                label = call.skip_reason == NodeSkipReason::Disabled ? tr("已禁用") : execution_status_text(call.status);
                const auto* flow = find_workflow(run->request().snapshot().solution(), run->identity().workflow);
                if (flow && is_image_output(find_node(*flow, call.node))) {
                    badge.completion = module_completion_code(call.status);
                    if (badge.completion) { label += tr(" · 状态 %1").arg(*badge.completion); }
                }
                if (call.invocation && call.status == ExecutionStatus::Succeeded) {
                    const auto lookup = run->lookup(*call.invocation);
                    if (const auto* result = std::get_if<std::optional<NodeResult>>(&lookup); result && *result) {
                        const auto& data = (*result)->data();
                        badge.judgement = data.judgement;
                        if (data.elapsed) {
                            badge.elapsed = tr(" · 耗时 %1 ms").arg(std::chrono::duration<double, std::milli>(*data.elapsed).count(), 0, 'f', 2);
                        }
                        switch (badge.judgement) {
                        case Judgement::OK: label += QStringLiteral(" · OK"); break;
                        case Judgement::NG: label += QStringLiteral(" · NG"); break;
                        case Judgement::NotEvaluated: break;
                        }
                    }
                }
            }
            if (progress->quiescent) { finished_run_ = run->identity(); }
        }
    }
    if (next == statuses_) { return; }
    next.swap(statuses_);
    for (auto id : model_.allNodeIds()) {
        const auto key = *model_.domain_node(id);
        const auto old = next.find(key), current = statuses_.find(key);
        if (old == next.end() && current == statuses_.end()) { continue; }
        if (old != next.end() && current != statuses_.end() && old->second == current->second) { continue; }
        if (auto* item = nodeGraphicsObject(id)) { item->update(); }
    }
}

ControlScene::NodeBadge ControlScene::node_badge(NodeId id) const
{
    if (const auto entry = statuses_.find(*model_.domain_node(id)); entry != statuses_.end()) { return entry->second; }
    const auto& node = model_.active_workflow()->nodes[model_.nodes_.at(id).source_index];
    return {node.enabled ? tr("未运行") : tr("已禁用")};
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

bool ControlScene::set_class_palette(const core::NodeId& node, workflow::ClassPalette palette)
{
    const auto* flow = model_.active_workflow();
    if (!flow || !find_node(*flow, node)) { return false; }
    const auto position = model_.position(model_.canvas_id(node));
    return edit_draft(tr("Change class display color"), [&](workflow::Solution& draft) -> EditResult {
        auto& nodes = layout_for(draft, flow->id).nodes;
        const auto found = std::ranges::find(nodes, node, &workflow::NodePlacement::node);
        if (found == nodes.end()) { nodes.push_back({node, position.x(), position.y(), std::move(palette)}); }
        else { found->class_palette = std::move(palette); }
        return std::monostate{};
    });
}

void ControlScene::set_editing_enabled(bool enabled)
{
    if (editing_enabled_ == enabled) { return; }
    editing_enabled_ = enabled;
    if (!enabled) { cancel_gesture(); }
    for (const auto id : model_.allNodeIds()) {
        if (auto* node = nodeGraphicsObject(id)) { node->update(); }
    }
}

bool ControlScene::show_available_ports(const NodeGraphicsObject& node) const
{
    return editing_enabled_ && (node.nodeState().hovered() || wiring_target_ == node.nodeId());
}

void ControlScene::set_wiring_target(NodeId target)
{
    if (wiring_target_ == target) { return; }
    if (auto* previous = nodeGraphicsObject(wiring_target_)) { previous->update(); }
    wiring_target_ = target;
    if (auto* next = nodeGraphicsObject(target)) { next->update(); }
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
        workflow::Node node{*added, tool->display_name, workflow::ToolConfiguration{type, tool->config_version}, {}, {}};
        if (is_image_source(&node)) {
            const auto created = ensure_source_frame(draft, node, false);
            if (const auto* failure = std::get_if<core::Diagnostic>(&created)) { return *failure; }
        }
        return append_node(draft, model_, std::move(node), position);
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
            if (const auto* saved = find_placement(model_.draft(), flow->id, node.id)) {
                copied.placements.push_back(*saved);
            } else {
                const auto point = model_.position(model_.canvas_id(node.id));
                copied.placements.push_back({node.id, point.x(), point.y()});
            }
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
            placements.push_back({id->second, position.x + (root ? offset : 0.0),
                position.y + (root ? offset : 0.0), position.class_palette});
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

bool ControlScene::arrange_scope()
{
    if (!editing_enabled_ || !model_.active_workflow()) { return false; }
    // Place a node only after every visible predecessor. Each edge contributes
    // one count even when two different branch outlets share a destination.
    std::map<NodeId, int> incoming;
    std::map<NodeId, std::vector<NodeId>> next;
    for (auto id : model_.allNodeIds()) { incoming.emplace(id, 0); }
    for (const auto& edge : model_.active_workflow()->control_links) {
        const auto source = model_.canvas_id(edge.source);
        const auto target = model_.canvas_id(edge.target);
        if (incoming.contains(source) && incoming.contains(target)) {
            next[source].push_back(target);
            ++incoming[target];
        }
    }
    std::vector<NodeId> row;
    for (const auto& [node, count] : incoming) { if (count == 0) { row.push_back(node); } }
    std::vector<CanvasMove> moves;
    const ControlNodeGeometry vertical(model_, Qt::Vertical);
    qreal top = 0;
    while (!row.empty()) {
        std::ranges::sort(row);
        qreal left = 0;
        int height = 0;
        std::vector<NodeId> following;
        for (auto node : row) {
            const auto size = vertical.measured_size(node);
            moves.push_back({node, {left, top}});
            left += size.width() + 48;
            height = std::max(height, size.height());
            for (auto target : next[node]) {
                if (--incoming[target] == 0) { following.push_back(target); }
            }
        }
        top += height + 72;
        row.swap(following);
    }
    if (moves.size() != incoming.size()) {
        Q_EMIT edit_notice(tr("当前层级含循环控制连接，无法按执行先后整理。"));
        return false;
    }
    return model_.move_nodes(undoStack(), moves, [this, workflow = model_.workflow_id(), scope = model_.scope()] {
        if (workflow == model_.workflow_id() && scope == model_.scope()) { setOrientation(model_.layout_orientation()); }
    });
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
    wiring_target_ = InvalidNodeId;
    resetDraftConnection();
    {
        const QSignalBlocker block(this);
        // QtNodes replaces its geometry when the port orientation changes.
        setNodeGeometry(std::make_unique<ControlNodeGeometry>(model_, orientation()));
        BasicGraphicsScene::onModelReset();
        for (const auto& id : selected) {
            if (auto* item = nodeGraphicsObject(model_.canvas_id(id))) { item->setSelected(true); }
        }
    }
    if (had_selection) { Q_EMIT selectionChanged(); }
}

void ControlScene::cancel_gesture()
{
    set_wiring_target(InvalidNodeId);
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
            NodeId target = InvalidNodeId;
            for (auto* item : items(event->scenePos(), Qt::IntersectsItemBoundingRect)) {
                if (auto* node = qgraphicsitem_cast<NodeGraphicsObject*>(item)) { target = node->nodeId(); break; }
            }
            set_wiring_target(target);
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
            set_wiring_target(InvalidNodeId);
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
