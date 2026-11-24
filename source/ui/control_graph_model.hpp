#pragma once

#include <QtNodes/AbstractGraphModel>
#include <QPointF>
#include <QSize>

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/model.hpp>

#include <functional>
#include <map>
#include <span>

class QUndoStack;

namespace visionpro::ui {

struct CanvasMove {
    QtNodes::NodeId node;
    QPointF position;
};

using DraftEdit = std::function<core::Result<std::monostate>(workflow::Solution&)>;

// Canvas IDs are stable view addresses within this draft, including when a
// workflow is hidden or a node is brought back by undo.
class ControlGraphModel final : public QtNodes::AbstractGraphModel {
public:
    ControlGraphModel(workflow::Solution draft, std::optional<core::WorkflowId> workflow_id,
        const core::ToolRegistry& registry);

    const workflow::Solution& draft() const noexcept { return draft_; }
    std::optional<core::WorkflowId> workflow_id() const noexcept { return workflow_id_; }
    QtNodes::NodeId canvas_id(const core::NodeId& id) const;
    std::optional<core::NodeId> domain_node(QtNodes::NodeId id) const;
    std::span<const core::ControlOutputDescriptor> named_control_outlets(QtNodes::NodeId id) const;
    std::optional<core::NodeId> scope() const noexcept { return scope_; }
    QString scope_path() const;
    Qt::Orientation layout_orientation() const;
    void set_saved_revision(std::uint64_t revision) noexcept { draft_.revision = revision; }

    std::unordered_set<QtNodes::NodeId> allNodeIds() const override;
    std::unordered_set<QtNodes::ConnectionId> allConnectionIds(QtNodes::NodeId node) const override;
    std::unordered_set<QtNodes::ConnectionId> connections(QtNodes::NodeId node, QtNodes::PortType type,
        QtNodes::PortIndex index) const override;
    bool nodeExists(QtNodes::NodeId node) const override;
    bool connectionExists(QtNodes::ConnectionId connection) const override;
    bool connectionPossible(QtNodes::ConnectionId connection) const override;
    QVariant nodeData(QtNodes::NodeId node, QtNodes::NodeRole role) const override;
    QVariant portData(QtNodes::NodeId node, QtNodes::PortType type, QtNodes::PortIndex index,
        QtNodes::PortRole role) const override;

    // Interactive edits use the typed commands below. QtNodes' default
    // mutators do not carry the stable identities required for undo.
    QtNodes::NodeId newNodeId() override { return QtNodes::InvalidNodeId; }
    QtNodes::NodeId addNode(QString = {}) override { return QtNodes::InvalidNodeId; }
    void addConnection(QtNodes::ConnectionId) override {}
    bool deleteConnection(QtNodes::ConnectionId) override { return false; }
    bool deleteNode(QtNodes::NodeId) override { return false; }
    bool detachPossible(QtNodes::ConnectionId) const override { return false; }
    bool setNodeData(QtNodes::NodeId node, QtNodes::NodeRole role, QVariant value) override;
    bool setPortData(QtNodes::NodeId, QtNodes::PortType, QtNodes::PortIndex,
        const QVariant&, QtNodes::PortRole) override { return false; }

private:
    friend class ControlScene;

    struct ProjectedNode {
        std::size_t source_index;
        std::vector<core::ControlOutputDescriptor> outlets;
        std::optional<core::NodeId> parent;
        QPointF default_position;
        QSize size{0, 0};
    };
    using ProjectedNodes = std::map<QtNodes::NodeId, ProjectedNode>;

    const workflow::Workflow* active_workflow() const;
    void update_paint_style();
    ProjectedNodes project_nodes(const workflow::Workflow* flow);
    void set_view(std::optional<core::WorkflowId> workflow_id, std::optional<core::NodeId> scope,
        ProjectedNodes nodes);
    bool select_workflow(const core::WorkflowId& workflow_id);
    bool reveal_node(const core::NodeId& node);
    bool visible(QtNodes::NodeId node) const;
    QPointF position(QtNodes::NodeId node) const;
    std::optional<QtNodes::ConnectionId> project(const workflow::ControlLink& link) const;
    bool open_group(QtNodes::NodeId node);
    bool parent_scope();
    core::Result<bool> edit_draft(QUndoStack& history, const QString& label, const DraftEdit& edit);
    void edit_links(QUndoStack& history, workflow::ControlLink changed, std::vector<workflow::ControlLink> next,
        const QString& label);
    core::Result<bool> connect_control(QUndoStack& history, QtNodes::ConnectionId connection);
    core::Result<bool> remove_control(QUndoStack& history, QtNodes::ConnectionId connection);
    bool move_nodes(QUndoStack& history, std::span<const CanvasMove> moves);

    workflow::Solution draft_;
    const core::ToolRegistry& registry_;
    std::optional<core::WorkflowId> workflow_id_;
    std::map<core::NodeId, QtNodes::NodeId> ids_;
    ProjectedNodes nodes_;
    QVariantMap paint_style_;
    std::optional<core::NodeId> scope_;
};

} // namespace visionpro::ui
