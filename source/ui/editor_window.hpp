#pragma once

#include "solution_run_actions.hpp"

#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/run_handle.hpp>

#include <QKeySequence>
#include <QMainWindow>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class QAction;
class QCloseEvent;
class QDialog;
class QDockWidget;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QSplitter;
class QSettings;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTabBar;
class QTimer;
class QToolButton;

namespace visionpro::ui {

class ControlGraphModel;
class ControlScene;
class EditorInspector;
class ImagePanel;
class ImageSourceStrip;
class ModuleHistoryPanel;
class RunController;
class RunPanel;
class WorkspaceToolbox;
class WorkspaceResults;
class WorkflowCanvas;
struct WorkspaceImageSelection;

class EditorWindow final : public QMainWindow {
public:
    EditorWindow(workflow::Solution draft, core::ToolRegistry registry, QWidget* parent = nullptr,
        QSettings* layout_settings = nullptr);
    ~EditorWindow() override;

    const workflow::Solution& draft() const noexcept;
    bool file_busy() const noexcept { return busy_; }
    void open_document(const std::filesystem::path& directory);
    void save_document();
    void transfer_document(const std::filesystem::path& directory, infrastructure::SolutionTransfer operation);
    void export_subgraph(const std::filesystem::path& directory, std::optional<core::NodeId> group = {});
    void import_subgraph(const std::filesystem::path& directory);
    void open_preview_image(const std::filesystem::path& file, core::CoordinateSystemId frame, bool grayscale);
    void open_preview_resource(core::ResourceReference resource, core::CoordinateSystemId frame, bool grayscale);
    void preview_selected_roi();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct CommittedDocument;
    using Continuation = std::function<void()>;

    QAction* action(QMenu& menu, const QString& label, const char* name, Continuation invoke,
        const QKeySequence& shortcut = {});
    void create_actions();
    void create_docks();
    void restore_default_layout();
    void restore_workspace_layout();
    void save_workspace_layout();
    void create_log_bar();
    void record_event(const QString& message);
    void create_run_actions();
    void create_run_dock();
    void add_workspace_tool(const QString& type, QPointF position);
    void choose_control_connection();
    void show_properties();
    void show_solution_resources(bool cameras_only = false);
    void map_source_variable(core::WorkflowId flow, core::NodeId node, core::OutputId port, QString field_name);
    void set_overview_visible(bool visible);
    void refresh_overview();
    void refresh_properties_status();
    void refresh_workspace_selection();
    void refresh_workspace_results();
    void refresh_workspace_statistics();
    void present_workspace_image();
    std::optional<workflow::RunHandle> workspace_cancel_target() const;
    void start_run(std::optional<workflow::DebugStatePolicy> debug = {},
        std::optional<workflow::NodeTarget> configured = {});
    void show_selected_run_image();
    void show_shared_preview();
    void choose_run_limits();
    void replace_canvas(workflow::Solution draft, bool writable = true);
    void refresh_canvas();
    void refresh_actions();
    void switch_canvas_view();
    void make_new_document();
    void add_coordinate_frame();
    bool confirm_replacement(Continuation action);
    void choose_open();
    void choose_transfer(infrastructure::SolutionTransfer operation, Continuation after = {});
    void choose_subgraph_export(bool selected_group);
    void choose_subgraph_import();
    void begin_save(Continuation after = {});
    void begin_transfer(const std::filesystem::path& directory, infrastructure::SolutionTransfer operation,
        Continuation after);
    void accept_opened(infrastructure::SolutionDocument source);
    void accept_committed(CommittedDocument committed, bool replace, const Continuation& after);
    void choose_resource_copy();
    void hash_resource_file(const QString& path, std::function<void(std::string)> apply);
    void choose_resource_root();
    void choose_preview_image();
    void choose_preview_resource();
    void open_source_preview(const QString& file, core::CoordinateSystemId frame, bool grayscale);
    void accept_preview_frame(const core::ImageHandle& frame);
    bool writable() const noexcept;
    template<class Task, class Apply>
    void start_file_task(const QString& status, Task task, Apply apply);

    void queue_diagnostics();
    void validate_draft();
    void render_diagnostics();
    void locate_diagnostic(QListWidgetItem* item);
    void remove_diagnosed_link();
    void report_error(const QString& message);
    void show_status(const QString& message);

    core::ToolRegistry registry_;
    std::unique_ptr<ControlGraphModel> model_;
    std::unique_ptr<ControlScene> scene_;
    EditorInspector* inspector_{};
    ImagePanel* images_{};
    ImageSourceStrip* image_source_strip_{};
    WorkflowCanvas* view_{};
    std::optional<core::WorkflowId> shown_workflow_;
    std::optional<core::NodeId> shown_scope_;
    struct CanvasView {
        qreal scale;
        QPointF center;
    };
    std::map<std::pair<core::WorkflowId, std::optional<core::NodeId>>, CanvasView> canvas_views_;
    std::optional<std::filesystem::path> directory_;
    std::optional<infrastructure::SolutionDocument> source_;
    std::optional<workflow::Solution> pending_baseline_;
    std::vector<infrastructure::NamedResourceRoot> roots_;
    bool busy_{false};
    bool close_when_idle_{false};
    bool close_confirmed_{false};
    bool showing_run_image_{false};

    QTabBar* workflows_{};
    QLabel* solution_name_{};
    QLabel* last_event_{};
    QLabel* solution_run_status_{};
    QPlainTextEdit* event_history_{};
    SolutionRunActions solution_actions_{};
    WorkspaceToolbox* tools_{};
    QSettings* layout_settings_{};
    QSplitter* workspace_splitter_{};
    QSplitter* results_splitter_{};
    QDockWidget* result_history_dock_{};
    QAction* show_history_{};
    QAction* arrange_{};
    QByteArray default_window_state_;
    QByteArray default_canvas_state_;
    QMainWindow* canvas_host_{};
    QLabel* workspace_stats_{};
    QToolButton* diagnostics_button_{};
    ModuleHistoryPanel* module_history_{};
    WorkspaceResults* results_{};
    std::unique_ptr<WorkspaceImageSelection> workspace_image_selection_;
    QDialog* properties_dialog_{};
    QPushButton* configured_debug_{};
    QLabel* configured_status_{};
    QAction* overview_action_{};
    QStackedWidget* work_pages_{};
    QTreeWidget* overview_{};
    QDockWidget* images_dock_{};
    RunController* run_controller_{};
    RunPanel* run_panel_{};
    QDockWidget* run_dock_{};
    QDockWidget* diagnostics_dock_{};
    QListWidget* diagnostics_{};
    QLabel* scope_path_{};
    QTimer* validation_timer_{};
    bool validating_{false};
    std::size_t validation_revision_{0};
    std::size_t displayed_validation_revision_{0};
    std::vector<core::Diagnostic> issues_;
    std::optional<core::Diagnostic> run_error_;
    std::vector<QString> file_notices_;

    QAction* new_{};
    QAction* open_{};
    QAction* save_{};
    QAction* save_as_{};
    QAction* independent_copy_{};
    QAction* export_workflow_{};
    QAction* export_group_{};
    QAction* import_subgraph_{};
    QAction* resource_copy_{};
    QAction* archive_policy_{};
    QAction* flush_archive_{};
    QAction* resources_{};
    QAction* cameras_{};
    QAction* resource_root_{};
    QAction* preview_image_{};
    QAction* preview_resource_{};
    QAction* preview_roi_{};
    QAction* run_workflow_{};
    QAction* run_group_{};
    QAction* debug_node_{};
    QAction* cancel_run_{};
    QAction* cancel_workflow_{};
    QAction* arm_trigger_{};
    QAction* workspace_trigger_{};
    QAction* send_frame_{};
    QAction* drain_trigger_{};
    QAction* cancel_trigger_{};
    QAction* reset_state_{};
    QAction* run_limits_{};
    QAction* rename_{};
    QAction* add_frame_{};
    QAction* new_workflow_{};
    QAction* remove_workflow_{};
    QAction* add_tool_{};
    QAction* add_group_{};
    QAction* add_loop_{};
    QAction* connect_steps_{};
    QAction* remove_nodes_{};
    QAction* remove_link_{};
    QAction* remove_invalid_link_{};
    QAction* copy_{};
    QAction* paste_{};
    QAction* undo_{};
    QAction* redo_{};
    QAction* up_{};
};

} // namespace visionpro::ui
