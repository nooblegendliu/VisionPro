#include "canvas_overview.hpp"

#include "workspace_appearance.hpp"

#include <QtNodes/AbstractGraphModel>
#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

#include <algorithm>

namespace visionpro::ui {

CanvasOverview::CanvasOverview(QtNodes::GraphicsView& view, QtNodes::BasicGraphicsScene& scene)
    : QWidget(&view), view_(view)
{
    setObjectName(QStringLiteral("canvasOverview"));
    setAccessibleName(tr("流程画布小地图"));
    setToolTip(tr("点击定位，拖动平移画布"));
    setCursor(Qt::OpenHandCursor);
    hide();
    auto& model = scene.graphModel();
    using Model = QtNodes::AbstractGraphModel;
    connect(&model, &Model::modelReset, this, [this] {
        drag_map_.reset();
        setCursor(Qt::OpenHandCursor);
        queue_node_refresh();
    });
    connect(&model, &Model::nodeCreated, this, &CanvasOverview::queue_node_refresh);
    connect(&model, &Model::nodeDeleted, this, &CanvasOverview::queue_geometry_update);
    connect(&model, &Model::nodeUpdated, this, &CanvasOverview::queue_geometry_update);
    connect(&model, &Model::connectionCreated, this, &CanvasOverview::queue_geometry_update);
    connect(&model, &Model::connectionDeleted, this, &CanvasOverview::queue_geometry_update);
    const auto repaint = [this] { update(); };
    connect(&WorkspaceAppearance::instance(), &WorkspaceAppearance::colorsChanged, this, repaint);
    connect(&view, &QtNodes::GraphicsView::scaleChanged, this, repaint);
    for (auto* bar : {view.horizontalScrollBar(), view.verticalScrollBar()}) {
        connect(bar, &QScrollBar::valueChanged, this, repaint);
        connect(bar, &QScrollBar::rangeChanged, this, repaint);
    }
    rebuild_geometry();
}

void CanvasOverview::place_above(const QRect& toolbar, const QRect& available)
{
    const QSize size(144, 84);
    QRect map(QPoint{}, size);
    map.moveBottomRight(toolbar.topRight() - QPoint(0, 6));
    has_space_ = available.width() >= size.width() * 2 && available.contains(map);
    setGeometry(map);
    setVisible(has_space_ && !nodes_.empty());
    raise();
    update();
}

void CanvasOverview::queue_node_refresh()
{
    observe_nodes_ = true;
    queue_geometry_update();
}

void CanvasOverview::queue_geometry_update()
{
    if (update_pending_) { return; }
    update_pending_ = true;
    QMetaObject::invokeMethod(this, [this] {
        update_pending_ = false;
        rebuild_geometry();
    }, Qt::QueuedConnection);
}

void CanvasOverview::rebuild_geometry()
{
    nodes_.clear();
    links_.clear();
    graph_bounds_ = {};
    if (view_.scene()) {
        for (auto* item : view_.scene()->items(Qt::AscendingOrder)) {
            if (auto* node = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
                const auto size = node->nodeScene()->nodeGeometry().size(node->nodeId());
                const auto rect = node->mapRectToScene(QRectF(QPointF{}, size));
                nodes_.push_back(rect);
                graph_bounds_ |= rect;
                // Manual drag previews move the graphics item before the edit enters undo history.
                if (observe_nodes_) {
                    connect(node, &QGraphicsObject::xChanged, this, &CanvasOverview::queue_geometry_update, Qt::UniqueConnection);
                    connect(node, &QGraphicsObject::yChanged, this, &CanvasOverview::queue_geometry_update, Qt::UniqueConnection);
                    // QtNodes also recreates the graphics items when the port orientation changes.
                    connect(node, &QObject::destroyed, this, &CanvasOverview::queue_node_refresh, Qt::UniqueConnection);
                }
            } else if (const auto* edge = qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
                const auto& id = edge->connectionId();
                if (id.inNodeId != QtNodes::InvalidNodeId && id.outNodeId != QtNodes::InvalidNodeId) {
                    links_.emplace_back(edge->mapToScene(edge->out()), edge->mapToScene(edge->in()));
                }
            }
        }
    }
    observe_nodes_ = false;
    setVisible(has_space_ && !nodes_.empty());
    update();
}

QRectF CanvasOverview::visible_scene() const
{
    return view_.viewportTransform().inverted().mapRect(QRectF(view_.viewport()->rect()));
}

QTransform CanvasOverview::map_transform() const
{
    if (drag_map_) { return *drag_map_; }
    const auto bounds = graph_bounds_.united(visible_scene());
    const QRectF area = QRectF(rect()).adjusted(7, 7, -7, -7);
    const qreal scale = std::min(area.width() / bounds.width(), area.height() / bounds.height());
    QTransform map;
    map.translate(area.center().x(), area.center().y());
    map.scale(scale, scale);
    map.translate(-bounds.center().x(), -bounds.center().y());
    return map;
}

void CanvasOverview::paintEvent(QPaintEvent*)
{
    const auto& colors = workspace_colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(colors.border);
    painter.setBrush(colors.surface);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter.setClipRect(rect().adjusted(2, 2, -2, -2));
    const auto map = map_transform();
    painter.setPen(QPen(colors.secondary, 1));
    for (const auto& line : links_) { painter.drawLine(map.map(line)); }
    painter.setPen(QPen(colors.nodeBorder, 1));
    painter.setBrush(colors.node);
    for (const auto& node : nodes_) { painter.drawRect(map.mapRect(node)); }
    auto fill = colors.accent;
    fill.setAlpha(28);
    painter.setPen(QPen(colors.accent, 1.5));
    painter.setBrush(fill);
    painter.drawRect(map.mapRect(visible_scene()));
}

void CanvasOverview::navigate(QPointF position, QPointF offset)
{
    const auto center = drag_map_->inverted().map(position) + offset;
    view_.centerOn(std::clamp(center.x(), drag_bounds_.left(), drag_bounds_.right()),
        std::clamp(center.y(), drag_bounds_.top(), drag_bounds_.bottom()));
}

void CanvasOverview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) { event->ignore(); return; }
    // Freeze the overview scale during this gesture, including when panning beyond the graph.
    drag_map_ = map_transform();
    press_position_ = event->position();
    dragging_ = false;
    const auto point = drag_map_->inverted().map(press_position_);
    const auto visible = visible_scene();
    drag_bounds_ = graph_bounds_.united(visible);
    if (visible.contains(point)) {
        drag_offset_ = visible.center() - point;
    } else {
        drag_offset_ = {};
        navigate(press_position_, {});
    }
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void CanvasOverview::mouseMoveEvent(QMouseEvent* event)
{
    if (!drag_map_) { event->ignore(); return; }
    dragging_ = dragging_ || (event->position() - press_position_).manhattanLength() >= QApplication::startDragDistance();
    if (dragging_) { navigate(event->position(), drag_offset_); }
    event->accept();
}

void CanvasOverview::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || !drag_map_) { event->ignore(); return; }
    navigate(event->position(), dragging_ ? drag_offset_ : QPointF{});
    drag_map_.reset();
    setCursor(Qt::OpenHandCursor);
    update();
    event->accept();
}

} // namespace visionpro::ui
