#pragma once

#include <QtNodes/AbstractGraphModel>
#include <QPointF>
#include <QSize>

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/model.hpp>

#include <map>
#include <span>

class QUndoStack;

namespace visionpro::ui {

struct CanvasMove {
    QtNodes::NodeId node;
    QPointF position;
};

// Owns the editable domain draft. The canvas IDs and ports are view addresses
// for the fixed set of nodes and control outlets in this editing session.
class ControlGraphModel final : public QtNodes::AbstractGraphModel {
public:
    ControlGraphModel(workflow::Solution draft, const core::WorkflowId& workflow_id,
        const core::ToolRegistry& registry);

    const workflow::Solution& draft() const noexcept { return draft_; }
    QtNodes::NodeId canvas_id(const core::NodeId& id) const;
    std::optional<core::NodeId> scope() const noexcept { return scope_; }
    QString scope_path() const;

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
        std::vector<core::ControlOutputDescriptor> outlets;
        std::optional<core::NodeId> parent;
        QPointF default_position;
        QSize size{0, 0};
    };

    bool visible(QtNodes::NodeId node) const;
    QPointF position(QtNodes::NodeId node) const;
    std::optional<QtNodes::ConnectionId> project(const workflow::ControlLink& link) const;
    bool open_group(QtNodes::NodeId node);
    bool parent_scope();
    void edit_links(QUndoStack& history, QtNodes::ConnectionId connection, std::vector<workflow::ControlLink> next,
        const QString& label);
    core::Result<bool> connect_control(QUndoStack& history, QtNodes::ConnectionId connection);
    core::Result<bool> remove_control(QUndoStack& history, QtNodes::ConnectionId connection);
    bool move_nodes(QUndoStack& history, std::span<const CanvasMove> moves);

    workflow::Solution draft_;
    workflow::Workflow& flow_;
    std::map<core::NodeId, QtNodes::NodeId> ids_;
    std::vector<ProjectedNode> nodes_;
    std::optional<core::NodeId> scope_;
};

} // namespace visionpro::ui
