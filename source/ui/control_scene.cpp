#include "control_scene.hpp"

#include <QtNodes/ConnectionIdUtils>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QGraphicsSceneMouseEvent>

#include <exception>

namespace visionpro::ui {

using namespace QtNodes;

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

bool ControlScene::connect_control(ConnectionId connection)
{
    try {
        const auto result = model_.connect_control(undoStack(), connection);
        if (const auto* changed = std::get_if<bool>(&result)) { return *changed; }
        Q_EMIT edit_failed(QString::fromStdString(std::get<core::Diagnostic>(result).message));
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
    }
    return false;
}

bool ControlScene::remove_control(ConnectionId connection)
{
    try {
        const auto result = model_.remove_control(undoStack(), connection);
        if (const auto* changed = std::get_if<bool>(&result)) { return *changed; }
        Q_EMIT edit_failed(QString::fromStdString(std::get<core::Diagnostic>(result).message));
    } catch (const std::exception& error) {
        Q_EMIT edit_failed(QString::fromUtf8(error.what()));
    }
    return false;
}

bool ControlScene::move_nodes(std::span<const CanvasMove> moves)
{
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
    drag_origins_.clear();
    resetDraftConnection();
    BasicGraphicsScene::onModelReset();
}

void ControlScene::focusOutEvent(QFocusEvent* event)
{
    resetDraftConnection();
    for (const auto& move : drag_origins_) {
        nodeGraphicsObject(move.node)->setPos(model_.nodeData(move.node, NodeRole::Position).toPointF());
    }
    drag_origins_.clear();
    BasicGraphicsScene::focusOutEvent(event);
}

void ControlScene::mouseMoveEvent(QGraphicsSceneMouseEvent* event)
{
    if (auto* link = qgraphicsitem_cast<ConnectionGraphicsObject*>(mouseGrabberItem())) {
        // Qt 6 puts the viewport widget on the event. QtNodes 3.0.16's
        // connection handlers cast it to QGraphicsView, so handle the gesture here.
        const auto port = link->connectionState().requiredPort();
        if (port != PortType::None) {
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
