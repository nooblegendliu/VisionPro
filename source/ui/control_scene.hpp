#pragma once

#include "control_graph_model.hpp"

#include <QtNodes/BasicGraphicsScene>

namespace visionpro::ui {

class ControlScene final : public QtNodes::BasicGraphicsScene {
    Q_OBJECT

public:
    explicit ControlScene(ControlGraphModel& model, QObject* parent = nullptr);

    bool open_group(QtNodes::NodeId node);
    bool parent_scope();
    bool connect_control(QtNodes::ConnectionId connection);
    bool remove_control(QtNodes::ConnectionId connection);
    bool move_nodes(std::span<const CanvasMove> moves);
    std::optional<QtNodes::ConnectionId> selected_control() const;

Q_SIGNALS:
    void edit_failed(const QString& message);

protected:
    void onModelReset() override;
    void focusOutEvent(QFocusEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void finish_connection(const QtNodes::ConnectionId& draft, const QPointF& scene_point);

    ControlGraphModel& model_;
    std::vector<CanvasMove> drag_origins_;
};

} // namespace visionpro::ui
