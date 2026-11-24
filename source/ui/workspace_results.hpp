#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/run_handle.hpp>

#include <QObject>
#include <QPointer>
#include <map>

class QComboBox;
class QDialog;
class QDockWidget;
class QLabel;
class QTabBar;
class QTreeWidget;

namespace visionpro::ui {

class ImagePanel;
class ModuleHistoryPanel;

// All result pages use the run selected by the history panel.
class WorkspaceResults final : public QObject {
    Q_OBJECT
public:
    WorkspaceResults(QDockWidget& upper, QDockWidget& lower, ImagePanel& image, ModuleHistoryPanel& history);
    QTabBar* image_tabs() const { return image_tabs_; }
    QTabBar* result_tabs() const { return result_tabs_; }
    void reset_document();
    void update(const workflow::Workflow* draft_flow, const workflow::Node* draft_node,
        const core::ToolRegistry& registry, std::uint64_t edit_generation, const QString& context);
signals:
    void view_changed();

private:
    using Port = std::pair<core::NodeId, core::OutputId>;
    void update_flow_image(const workflow::Workflow* flow, const core::ToolRegistry& registry,
        const std::optional<workflow::RunHandle>& run, std::uint64_t edit_generation);
    void update_outputs(const workflow::Node* node, const core::ToolDescriptor* tool,
        const std::optional<workflow::RunHandle>& run, const QString& message);

    ModuleHistoryPanel& history_;
    QTabBar* image_tabs_;
    QTabBar* result_tabs_;
    QLabel* context_;
    QLabel* current_;
    QLabel* help_;
    QLabel* output_origin_;
    QTreeWidget* outputs_;
    QComboBox* source_;
    QComboBox* call_;
    ImagePanel* flow_image_{};
    std::optional<core::WorkflowId> workflow_;
    std::map<core::WorkflowId, std::pair<Port, QString>> chosen_;
    std::vector<Port> ports_;
    std::vector<core::InvocationKey> calls_;
    std::optional<core::InvocationKey> chosen_call_;
    std::optional<workflow::RunIdentity> calls_run_;
    std::optional<core::ExecutionScopeId> calls_scope_;
    std::optional<workflow::ResultIdentity> rendered_image_;
    std::optional<core::OutputId> rendered_port_;
    std::optional<workflow::SnapshotIdentity> source_snapshot_;
    std::optional<std::uint64_t> source_generation_;
    std::optional<workflow::ResultIdentity> rendered_outputs_;
    std::optional<workflow::RunHandle> output_run_;
    std::optional<core::InvocationKey> output_call_;
    QPointer<QDialog> output_detail_;
};

} // namespace visionpro::ui
