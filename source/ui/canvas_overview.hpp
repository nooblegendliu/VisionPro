#pragma once

#include <QLineF>
#include <QTransform>
#include <QWidget>

#include <optional>
#include <vector>

namespace QtNodes {
class BasicGraphicsScene;
class GraphicsView;
}

namespace visionpro::ui {

class CanvasOverview final : public QWidget {
public:
    CanvasOverview(QtNodes::GraphicsView& view, QtNodes::BasicGraphicsScene& scene);
    void place_above(const QRect& toolbar, const QRect& available);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void queue_node_refresh();
    void queue_geometry_update();
    void rebuild_geometry();
    QRectF visible_scene() const;
    QTransform map_transform() const;
    void navigate(QPointF position, QPointF offset);

    QtNodes::GraphicsView& view_;
    std::vector<QRectF> nodes_;
    std::vector<QLineF> links_;
    QRectF graph_bounds_;
    bool has_space_{};
    bool update_pending_{};
    bool observe_nodes_{true};
    std::optional<QTransform> drag_map_;
    QRectF drag_bounds_;
    bool dragging_{};
    QPointF press_position_;
    QPointF drag_offset_;
};

} // namespace visionpro::ui
