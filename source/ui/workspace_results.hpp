#pragma once

#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/run_handle.hpp>

#include <QObject>
#include <QPointer>
#include <map>

class QComboBox;
class QCheckBox;
class QDialog;
class QDockWidget;
class QLabel;
class QMenu;
class QTabBar;
class QTreeWidget;

namespace visionpro::ui {

class ImagePanel;
class ImageComparison;
class ModuleHistoryPanel;

// All result pages use the run selected by the history panel.
class WorkspaceResults final : public QObject {
    Q_OBJECT
public:
    WorkspaceResults(QDockWidget& upper, QDockWidget& lower, ImagePanel& image, ModuleHistoryPanel& history);
    QTabBar* image_tabs() const { return image_tabs_; }
    QTabBar* result_tabs() const { return result_tabs_; }
    void reset_document();
    void update(const workflow::Solution& draft, const workflow::Workflow* draft_flow, const workflow::Node* draft_node,
        const core::ToolRegistry& registry, std::uint64_t edit_generation, const QString& context, bool editable);
signals:
    void view_changed();
    void source_preview_requested(const core::OutputId& output);
    void source_variable_requested(core::WorkflowId flow, core::NodeId node, core::OutputId output, QString field_name);

private:
    using Port = std::pair<core::NodeId, core::OutputId>;
    void update_flow_image(const workflow::Workflow* flow, const core::ToolRegistry& registry,
        const std::optional<workflow::RunHandle>& run, std::uint64_t edit_generation);
    void update_outputs(const workflow::Node* node, const core::ToolDescriptor* tool,
        const std::optional<workflow::RunHandle>& run, const QString& message,
        const workflow::Solution& draft, std::uint64_t edit_generation, bool editable);

    ModuleHistoryPanel& history_;
    QTabBar* image_tabs_;
    QTabBar* result_tabs_;
    QLabel* context_;
    QLabel* current_;
    QLabel* help_;
    QLabel* output_origin_;
    QTreeWidget* outputs_;
    QMenu* camera_variable_menu_;
    QLabel* reference_{};
    bool source_page_{};
    std::optional<workflow::NodeTarget> source_node_;
    QComboBox* source_;
    QComboBox* call_;
    ImagePanel* flow_image_{};
    QCheckBox* compare_mode_{};
    ImageComparison* comparison_{};
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
    std::optional<std::uint64_t> output_generation_;
    bool mappings_editable_{};
    std::optional<workflow::NodeTarget> rendered_source_;
    QString output_message_;
    std::optional<workflow::RunHandle> output_run_;
    std::optional<core::InvocationKey> output_call_;
    QPointer<QDialog> output_detail_;
};

} // namespace visionpro::ui
