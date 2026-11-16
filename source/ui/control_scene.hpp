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
    bool select_workflow(const core::WorkflowId& workflow);
    bool reveal_node(const core::NodeId& node);
    std::vector<core::NodeId> selected_nodes() const;

    bool edit_draft(const QString& label, const DraftEdit& edit);
    void set_editing_enabled(bool enabled);
    bool add_workflow(std::string name);
    bool remove_workflow(core::WorkflowId workflow);
    bool add_tool(core::ToolTypeId type, QPointF position);
    bool add_group(std::string name, QPointF position);
    bool remove_selected_nodes();
    bool copy_selected_nodes();
    bool paste_nodes();

    bool connect_control(QtNodes::ConnectionId connection);
    bool remove_control(QtNodes::ConnectionId connection);
    bool move_nodes(std::span<const CanvasMove> moves);
    std::optional<QtNodes::ConnectionId> selected_control() const;

Q_SIGNALS:
    void edit_failed(const QString& message);
    void edit_notice(const QString& message);

protected:
    void onModelReset() override;
    void focusOutEvent(QFocusEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    struct Clipboard {
        workflow::Workflow source;
        std::optional<core::NodeId> scope;
        std::vector<core::NodeId> selection;
        std::vector<workflow::NodePlacement> placements;
        std::size_t paste_count{0};
    };

    bool report_edit(core::Result<bool> result);
    void cancel_gesture();
    void finish_connection(const QtNodes::ConnectionId& draft, const QPointF& scene_point);

    ControlGraphModel& model_;
    std::vector<CanvasMove> drag_origins_;
    std::optional<Clipboard> clipboard_;
    bool editing_enabled_{true};
};

} // namespace visionpro::ui
