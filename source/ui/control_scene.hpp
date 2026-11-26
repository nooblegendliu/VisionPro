#pragma once

#include "control_graph_model.hpp"
#include <visionpro/workflow/run_handle.hpp>

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
    bool set_class_palette(const core::NodeId& node, workflow::ClassPalette palette);
    void set_editing_enabled(bool enabled);
    bool show_available_ports(const QtNodes::NodeGraphicsObject& node) const;
    bool add_workflow(std::string name);
    bool remove_workflow(core::WorkflowId workflow);
    bool add_tool(core::ToolTypeId type, QPointF position);
    enum class GroupKind { Group, Loop };
    bool add_group(std::string name, QPointF position, GroupKind kind = GroupKind::Group);
    bool remove_selected_nodes();
    bool copy_selected_nodes();
    bool paste_nodes();

    bool connect_control(QtNodes::ConnectionId connection);
    bool remove_control(QtNodes::ConnectionId connection);
    bool move_nodes(std::span<const CanvasMove> moves);
    bool arrange_scope();
    void display_run(const std::optional<workflow::RunHandle>& run, std::optional<core::ExecutionScopeId> scope,
        std::optional<core::InvocationKey> invocation);
    struct NodeBadge {
        QString text;
        QString elapsed;
        workflow::ExecutionStatus status{workflow::ExecutionStatus::Pending};
        workflow::Judgement judgement{workflow::Judgement::NotEvaluated};
        std::optional<std::int64_t> completion;
        bool operator==(const NodeBadge&) const = default;
    };
    NodeBadge node_badge(QtNodes::NodeId id) const;
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
    void set_wiring_target(QtNodes::NodeId target);
    void finish_connection(const QtNodes::ConnectionId& draft, const QPointF& scene_point);

    ControlGraphModel& model_;
    std::vector<CanvasMove> drag_origins_;
    std::optional<Clipboard> clipboard_;
    QtNodes::NodeId wiring_target_{QtNodes::InvalidNodeId};
    bool editing_enabled_{true};
    std::map<core::NodeId, NodeBadge> statuses_;
    std::optional<workflow::RunIdentity> finished_run_;
    std::optional<core::ExecutionScopeId> display_scope_;
    std::optional<core::InvocationKey> display_call_;
};

} // namespace visionpro::ui
