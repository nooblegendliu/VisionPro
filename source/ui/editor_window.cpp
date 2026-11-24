#include "editor_window.hpp"

#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "image_panel.hpp"
#include "module_history_panel.hpp"
#include "workspace_results.hpp"
#include "run_controller.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"
#include "workspace_appearance.hpp"
#include "workspace_icons.hpp"
#include "workspace_toolbox.hpp"
#include "workspace_style.hpp"

#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QSignalBlocker>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <algorithm>
#include <exception>
#include <ranges>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

class PropertiesDialog final : public QDialog {
public:
    explicit PropertiesDialog(QWidget* parent) : QDialog(parent)
    {
        setObjectName(QStringLiteral("propertiesDialog"));
        setWindowFlag(Qt::WindowContextHelpButtonHint, false);
        setSizeGripEnabled(true);
        resize(680, 540);
        auto* layout = new QVBoxLayout(this);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
        buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        // Inline editors commit on Enter. An unhandled Enter must not activate a different field's button.
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            event->accept();
            return;
        }
        QDialog::keyPressEvent(event);
    }
};

class WorkflowTabs final : public QTabBar {
public:
    using QTabBar::QTabBar;

protected:
    void paintEvent(QPaintEvent* event) override
    {
        QTabBar::paintEvent(event);
        if (currentIndex() < 0) { return; }
        auto underline = tabRect(currentIndex()).adjusted(3, 0, -3, 0);
        underline.setTop(underline.bottom() - 1);
        QPainter(this).fillRect(underline, workspace_colors().accent);
    }
};

QWidget* panel_header(QLabel* title, QWidget* action = nullptr)
{
    auto* header = new QWidget(title->parentWidget());
    header->setObjectName(QStringLiteral("workspacePanelHeader"));
    title->setFont(workspace_font(WorkspaceText::Heading));
    auto* layout = new QVBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(9, 2, 6, 2);
    row->addWidget(title, 1);
    if (action) { row->addWidget(action); }
    layout->addLayout(row);
    auto* separator = new QFrame(header);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setForegroundRole(QPalette::Midlight);
    separator->setFixedHeight(1);
    layout->addWidget(separator);
    header->setMinimumHeight(std::max(workspace_size::header_height, title->fontMetrics().height() + workspace_size::gap));
    return header;
}

std::optional<std::string> ask_name(QWidget* owner, const QString& title, const QString& initial = {})
{
    bool accepted = false;
    const auto name = QInputDialog::getText(owner, title, QObject::tr("名称"), QLineEdit::Normal, initial, &accepted).trimmed();
    if (!accepted || name.isEmpty()) { return std::nullopt; }
    return name.toStdString();
}

QString diagnostic_location(const Solution& draft, const DiagnosticLocation& location)
{
    QStringList parts;
    const auto* flow = location.workflow ? find_workflow(draft, *location.workflow) : nullptr;
    if (flow) {
        parts.push_back(QString::fromStdString(flow->name));
        if (location.node) {
            const auto* node = find_node(*flow, *location.node);
            parts.push_back(QString::fromStdString(node ? node->name : location.node->to_string()));
        }
    }
    if (location.input) { parts.push_back(QString::fromStdString(std::string(location.input->text()))); }
    if (location.parameter) { parts.push_back(QString::fromStdString(std::string(location.parameter->text()))); }
    return parts.join(QStringLiteral(" / "));
}
} // namespace

EditorWindow::EditorWindow(Solution draft, ToolRegistry registry, QWidget* parent, QSettings* layout_settings)
    : QMainWindow(parent), registry_(std::move(registry)), layout_settings_(layout_settings)
{
    setFont(workspace_font());
    resize(1440, 800);
    validation_timer_ = new QTimer(this);
    validation_timer_->setSingleShot(true);
    validation_timer_->setInterval(90);
    connect(validation_timer_, &QTimer::timeout, this, [this] { validate_draft(); });
    create_docks();
    properties_dialog_ = new PropertiesDialog(this);
    auto* buttons = properties_dialog_->findChild<QDialogButtonBox*>();
    configured_debug_ = buttons->addButton(tr("调试本模块"), QDialogButtonBox::ActionRole);
    configured_debug_->setObjectName(QStringLiteral("debugConfiguredNode"));
    configured_debug_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F5));
    configured_debug_->setAutoDefault(false);
    configured_status_ = new QLabel(properties_dialog_);
    configured_status_->setObjectName(QStringLiteral("configuredRunStatus"));
    static_cast<QVBoxLayout*>(properties_dialog_->layout())->insertWidget(0, configured_status_);
    connect(configured_debug_, &QPushButton::clicked, this, [this] {
        if (const auto target = inspector_->pinned_node()) { start_run(DebugStatePolicy::Isolated, target); }
    });
    connect(properties_dialog_, &QDialog::finished, this, [this] { inspector_->pin({}); });
    create_actions();
    create_log_bar();
    replace_canvas(std::move(draft));
    default_window_state_ = saveState(1);
    default_canvas_state_ = canvas_host_->saveState(1);
    restore_workspace_layout();
}

EditorWindow::~EditorWindow()
{
    if (scene_) {
        disconnect(scene_.get(), nullptr, this, nullptr);
        disconnect(&scene_->undoStack(), nullptr, this, nullptr);
    }
    delete inspector_;
    delete view_;
    scene_.reset();
    model_.reset();
}

const Solution& EditorWindow::draft() const noexcept { return model_->draft(); }

bool EditorWindow::writable() const noexcept
{ return !source_ || source_->can_save(); }

QAction* EditorWindow::action(QMenu& menu, const QString& label, const char* name, Continuation invoke,
    const QKeySequence& shortcut)
{
    auto* item = menu.addAction(label);
    item->setObjectName(QString::fromLatin1(name));
    item->setShortcut(shortcut);
    connect(item, &QAction::triggered, this, [this, invoke = std::move(invoke)] {
        try { invoke(); }
        catch (const std::exception& error) { report_error(QString::fromUtf8(error.what())); }
        catch (...) { report_error(QStringLiteral("The editor operation failed.")); }
    });
    return item;
}

void EditorWindow::create_actions()
{
    auto& file = *menuBar()->addMenu(tr("文件(&F)"));
    new_ = action(file, tr("新建方案…"), "newSolution",
        [this] { confirm_replacement([this] { make_new_document(); }); }, QKeySequence::New);
    open_ = action(file, tr("打开方案…"), "openSolution", [this] { choose_open(); }, QKeySequence::Open);
    save_ = action(file, tr("保存方案"), "saveSolution", [this] { save_document(); }, QKeySequence::Save);
    save_as_ = action(file, tr("另存为…"), "saveSolutionAs",
        [this] { choose_transfer(infrastructure::SolutionTransfer::SaveAs); }, QKeySequence::SaveAs);
    independent_copy_ = action(file, tr("保存独立副本…"), "saveIndependentCopy",
        [this] { choose_transfer(infrastructure::SolutionTransfer::IndependentCopy); });
    file.addSeparator();
    export_workflow_ = action(file, tr("导出当前流程…"), "exportWorkflow",
        [this] { choose_subgraph_export(false); });
    export_group_ = action(file, tr("导出选中的工具组…"), "exportGroup",
        [this] { choose_subgraph_export(true); });
    import_subgraph_ = action(file, tr("导入子图包…"), "importSubgraph",
        [this] { choose_subgraph_import(); });
    file.addSeparator();
    resource_copy_ = action(file, tr("复制资源到方案…"), "copyResource", [this] { choose_resource_copy(); });
    resource_root_ = action(file, tr("设置资源根目录…"), "setResourceRoot", [this] { choose_resource_root(); });
    resources_ = action(file, tr("方案资源…"), "solutionResources", [this] { show_solution_resources(); });
    file.addSeparator();
    action(file, tr("退出"), "exitEditor", [this] { close(); }, QKeySequence::Quit);

    auto& edit = *menuBar()->addMenu(tr("编辑(&E)"));
    undo_ = action(edit, tr("撤销"), "undoDraft", [this] { scene_->undoStack().undo(); }, QKeySequence::Undo);
    redo_ = action(edit, tr("重做"), "redoDraft", [this] { scene_->undoStack().redo(); }, QKeySequence::Redo);
    copy_ = action(edit, tr("复制节点"), "copyNodes", [this] { scene_->copy_selected_nodes(); }, QKeySequence::Copy);
    paste_ = action(edit, tr("粘贴节点"), "pasteNodes", [this] { scene_->paste_nodes(); }, QKeySequence::Paste);
    edit.addSeparator();
    rename_ = action(edit, tr("重命名方案…"), "renameSolution", [this] {
        const auto name = ask_name(this, tr("重命名方案"), QString::fromStdString(draft().name));
        if (!name || *name == draft().name) { return; }
        scene_->edit_draft(QStringLiteral("Rename solution"), [&](Solution& candidate) -> Result<std::monostate> {
            candidate.name = *name;
            return std::monostate{};
        });
    });
    add_frame_ = action(edit, tr("添加坐标系…"), "addCoordinateFrame", [this] { add_coordinate_frame(); });

    auto& graph = *menuBar()->addMenu(tr("流程(&W)"));
    new_workflow_ = action(graph, tr("添加流程…"), "addWorkflow", [this] {
        if (const auto name = ask_name(this, tr("新建流程"), tr("流程"))) {
            scene_->add_workflow(*name);
        }
    });
    remove_workflow_ = action(graph, tr("移除当前流程"), "removeWorkflow", [this] {
        if (const auto id = model_->workflow_id()) { scene_->remove_workflow(*id); }
    });
    add_tool_ = action(graph, tr("添加选中工具"), "addTool", [this] {
        add_workspace_tool(tools_->selected_type(), view_->mapToScene(view_->viewport()->rect().center()));
    });
    connect(tools_, &WorkspaceToolbox::toolActivated, add_tool_, &QAction::trigger);
    add_group_ = action(graph, tr("添加工具组…"), "addGroup", [this] {
        if (const auto name = ask_name(this, tr("新建工具组"), tr("工具组"))) {
            scene_->add_group(*name, view_->mapToScene(view_->viewport()->rect().center()));
        }
    });
    add_loop_ = action(graph, tr("添加循环…"), "addLoop", [this] {
        if (const auto name = ask_name(this, tr("新建循环"), tr("循环"))) {
            scene_->add_group(*name, view_->mapToScene(view_->viewport()->rect().center()), ControlScene::GroupKind::Loop);
        }
    });
    connect_steps_ = action(graph, tr("连接执行步骤…"), "connectSteps",
        [this] { choose_control_connection(); }, QKeySequence(Qt::CTRL | Qt::Key_L));
    remove_nodes_ = action(graph, tr("删除选中节点"), "removeNodes", [this] {
        scene_->remove_selected_nodes();
    }, QKeySequence::Delete);
    remove_link_ = action(graph, tr("删除选中连线"), "removeLink", [this] {
        if (const auto link = scene_->selected_control()) { scene_->remove_control(*link); }
    }, QKeySequence::Delete);
    up_ = action(graph, tr("返回上层"), "parentGroup", [this] { scene_->parent_scope(); });
    arrange_ = action(graph, tr("整理当前层级"), "arrangeScope", [this] { scene_->arrange_scope(); });
    auto* props = action(graph, tr("模块属性…"), "showProperties", [this] { show_properties(); });
    auto* flow_props = action(graph, tr("流程属性…"), "workflowProperties", [this] {
        scene_->clearSelection();
        show_properties();
    });

    create_run_actions();
    auto& view = *menuBar()->addMenu(tr("视图(&V)"));
    auto* developer_tools = view.addAction(tr("显示模拟工具"));
    developer_tools->setObjectName(QStringLiteral("showDeveloperTools"));
    developer_tools->setCheckable(true);
    connect(developer_tools, &QAction::toggled, tools_, &WorkspaceToolbox::set_developer_tools);
    show_history_ = view.addAction(tr("显示结果面板"));
    show_history_->setObjectName(QStringLiteral("showResultPanel"));
    show_history_->setCheckable(true);
    show_history_->setChecked(true);
    connect(show_history_, &QAction::toggled, result_history_dock_, &QWidget::setVisible);
    action(view, tr("恢复默认布局"), "resetWorkspaceLayout", [this] { restore_default_layout(); });
    auto* appearance_menu = view.addMenu(tr("外观"));
    auto* choices = new QActionGroup(appearance_menu);
    auto& appearance = WorkspaceAppearance::instance();
    const auto add_mode = [&](AppearanceMode mode, const QString& label, const char* name) {
        auto* choice = appearance_menu->addAction(label);
        choice->setObjectName(QString::fromLatin1(name));
        choices->addAction(choice);
        choice->setCheckable(true);
        choice->setChecked(appearance.mode() == mode);
        connect(choice, &QAction::triggered, &appearance, [&appearance, mode] { appearance.setMode(mode); });
        connect(&appearance, &WorkspaceAppearance::modeChanged, choice, [choice, mode](AppearanceMode selected) {
            choice->setChecked(selected == mode);
        });
    };
    add_mode(AppearanceMode::System, tr("跟随系统"), "appearanceSystem");
    add_mode(AppearanceMode::Light, tr("浅色"), "appearanceLight");
    add_mode(AppearanceMode::Dark, tr("深色"), "appearanceDark");
    view.addSeparator();
    view.addAction(props);
    for (auto* dock : {diagnostics_dock_, run_dock_}) {
        view.addAction(dock->toggleViewAction());
    }
    view.addSeparator();
    preview_image_ = action(view, tr("打开图像…"), "openPreviewImage", [this] { choose_preview_image(); });
    preview_resource_ = action(view, tr("打开图像资源…"), "openPreviewResource",
        [this] { choose_preview_resource(); });
    preview_roi_ = action(view, tr("预览选中 ROI"), "previewSelectedRoi",
        [this] { preview_selected_roi(); });
    view.addSeparator();
    remove_invalid_link_ = action(view, tr("移除诊断指示的控制线"), "removeDiagnosedLink",
        [this] { remove_diagnosed_link(); });

    auto* toolbar = addToolBar(tr("工作区工具栏"));
    toolbar->setObjectName(QStringLiteral("workspaceToolbar"));
    toolbar->setMovable(false);
    toolbar->setIconSize({workspace_size::toolbar_icon, workspace_size::toolbar_icon});
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const auto add_action = [&](QAction* item, WorkspaceIcon icon, const QString& label) {
        item->setIcon(workspace_icon(icon));
        item->setIconText(label);
        toolbar->addAction(item);
    };
    add_action(new_, WorkspaceIcon::New, tr("新建"));
    add_action(open_, WorkspaceIcon::Open, tr("打开"));
    add_action(save_, WorkspaceIcon::Save, tr("保存"));
    add_action(resources_, WorkspaceIcon::Properties, tr("方案资源"));
    toolbar->addSeparator();
    add_action(undo_, WorkspaceIcon::Undo, tr("撤销"));
    add_action(redo_, WorkspaceIcon::Redo, tr("重做"));
    toolbar->addSeparator();
    add_action(solution_actions_.start, WorkspaceIcon::RunSolution, tr("运行整方案"));
    add_action(solution_actions_.stop, WorkspaceIcon::Stop, tr("取消整方案"));
    workspace_trigger_ = action(graph, tr("启动当前流程触发"), "startWorkspaceTrigger", [this, flow_props] {
        if (arm_trigger_->isEnabled()) { arm_trigger_->trigger(); }
        else {
            flow_props->trigger();
            show_status(tr("请先在流程属性中配置连续时钟或外部触发源。"));
        }
    });
    toolbar->addSeparator();
    add_action(props, WorkspaceIcon::Properties, tr("模块属性"));
    properties_dialog_->addActions({undo_, redo_, save_, run_workflow_, cancel_run_});
    auto* details = run_dock_->toggleViewAction();
    add_action(details, WorkspaceIcon::Details, details->text());

    solution_name_ = new QLabel(menuBar());
    solution_name_->setTextFormat(Qt::PlainText);
    solution_name_->setFont(workspace_font(WorkspaceText::Category));
    solution_name_->setMargin(4);
    menuBar()->setCornerWidget(solution_name_, Qt::TopRightCorner);

    auto* strip = new QToolBar(tr("流程栏"), canvas_host_);
    strip->setObjectName(QStringLiteral("workflowBar"));
    strip->setMovable(false);
    strip->setFloatable(false);
    strip->setIconSize({workspace_size::toolbar_icon, workspace_size::toolbar_icon});
    canvas_host_->addToolBar(strip);
    overview_action_ = strip->addAction(tr("方案总览"));
    overview_action_->setObjectName(QStringLiteral("showSolutionOverview"));
    overview_action_->setCheckable(true);
    connect(overview_action_, &QAction::triggered, this, &EditorWindow::set_overview_visible);
    workflows_ = new WorkflowTabs(strip);
    workflows_->setObjectName(QStringLiteral("workflowSelector"));
    workflows_->setAccessibleName(tr("方案中的流程"));
    workflows_->setDocumentMode(true);
    workflows_->setExpanding(false);
    workflows_->setElideMode(Qt::ElideMiddle);
    workflows_->setUsesScrollButtons(true);
    workflows_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    strip->addWidget(workflows_);
    connect(workflows_, &QTabBar::tabBarClicked, this, [this](int row) {
        if (row >= 0) { set_overview_visible(false); }
    });
    connect(workflows_, &QTabBar::currentChanged, this, [this](int row) {
        if (!scene_ || row < 0) { return; }
        set_overview_visible(false);
        auto id = IdFactory::parse<WorkflowId>(workflows_->tabData(row).toString().toStdString());
        if (const auto* target = std::get_if<WorkflowId>(&id)) { scene_->select_workflow(*target); }
    });
    auto* add = new QToolButton(strip);
    new_workflow_->setIconText(QStringLiteral("+"));
    add->setDefaultAction(new_workflow_);
    add->setToolTip(tr("添加流程"));
    add->setAccessibleName(tr("添加流程"));
    strip->addWidget(add);
    auto* manage = new QToolButton(strip);
    manage->setText(tr("管理"));
    manage->setAccessibleName(tr("管理当前流程"));
    manage->setPopupMode(QToolButton::InstantPopup);
    auto* management = new QMenu(manage);
    management->addAction(flow_props);
    management->addAction(arrange_);
    management->addAction(remove_workflow_);
    manage->setMenu(management);
    strip->addWidget(manage);

    canvas_host_->addToolBarBreak();
    auto* commands = new QToolBar(tr("当前流程操作"), canvas_host_);
    commands->setObjectName(QStringLiteral("workflowRunBar"));
    commands->setMovable(false);
    commands->setFloatable(false);
    commands->setIconSize(toolbar->iconSize());
    commands->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    canvas_host_->addToolBar(commands);
    run_workflow_->setIcon(workspace_icon(WorkspaceIcon::Run));
    run_workflow_->setIconText(tr("运行流程"));
    commands->addAction(run_workflow_);
    cancel_workflow_->setIcon(workspace_icon(WorkspaceIcon::Stop));
    cancel_workflow_->setIconText(tr("取消流程"));
    commands->addAction(cancel_workflow_);
    commands->addSeparator();
    workspace_trigger_->setIcon(workspace_icon(WorkspaceIcon::Repeat));
    workspace_trigger_->setIconText(tr("启动触发"));
    commands->addAction(workspace_trigger_);
    auto* stop_trigger = new QToolButton(commands);
    stop_trigger->setObjectName(QStringLiteral("stopWorkflowTrigger"));
    drain_trigger_->setIconText(tr("停止触发"));
    stop_trigger->setDefaultAction(drain_trigger_);
    stop_trigger->setPopupMode(QToolButton::MenuButtonPopup);
    auto* strategies = new QMenu(stop_trigger);
    strategies->addAction(drain_trigger_);
    strategies->addAction(cancel_trigger_);
    stop_trigger->setMenu(strategies);
    commands->addWidget(stop_trigger);
    up_->setIcon(workspace_icon(WorkspaceIcon::Parent));
    strip->addAction(up_);
    scope_path_ = new QLabel(strip);
    scope_path_->setTextFormat(Qt::PlainText);
    strip->addWidget(scope_path_);
}

void EditorWindow::create_log_bar()
{
    auto* history = new QDialog(this);
    history->setWindowTitle(tr("工作区日志"));
    history->resize(650, 320);
    auto* layout = new QVBoxLayout(history);
    event_history_ = new QPlainTextEdit(history);
    event_history_->setObjectName(QStringLiteral("workspaceEventHistory"));
    event_history_->setReadOnly(true);
    event_history_->setMaximumBlockCount(200);
    layout->addWidget(event_history_);

    last_event_ = new QLabel(statusBar());
    last_event_->setTextFormat(Qt::PlainText);
    last_event_->setFont(workspace_font(WorkspaceText::Secondary));
    last_event_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(last_event_, 1);
    solution_run_status_ = new QLabel(statusBar());
    solution_run_status_->setObjectName(QStringLiteral("solutionRunStatus"));
    solution_run_status_->setAccessibleName(tr("方案运行状态"));
    statusBar()->addPermanentWidget(solution_run_status_);
    auto* logs = new QToolButton(statusBar());
    logs->setObjectName(QStringLiteral("openWorkspaceLog"));
    logs->setText(tr("日志"));
    logs->setToolTip(tr("查看当前方案的工作区日志"));
    statusBar()->addPermanentWidget(logs);
    connect(logs, &QToolButton::clicked, history, [history] {
        history->show();
        history->raise();
        history->activateWindow();
    });
}

void EditorWindow::record_event(const QString& message)
{
    const auto entry = QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + message.simplified();
    event_history_->appendPlainText(entry);
    last_event_->setText(entry);
    last_event_->setToolTip(entry);
}

void EditorWindow::create_docks()
{
    auto* columns = new QSplitter(Qt::Horizontal, this);
    workspace_splitter_ = columns;
    columns->setHandleWidth(workspace_size::splitter);
    columns->setObjectName(QStringLiteral("workspaceSplitter"));
    columns->setChildrenCollapsible(false);
    setCentralWidget(columns);

    auto* toolbox = new QWidget(columns);
    toolbox->setObjectName(QStringLiteral("nodeToolbox"));
    toolbox->setFixedWidth(WorkspaceToolbox::rail_width);
    tools_ = new WorkspaceToolbox(registry_, this);
    tools_->attach_to_slot(*toolbox);
    connect(tools_, &WorkspaceToolbox::drawerChanged, this, [this, toolbox, columns](bool open) {
        auto sizes = columns->sizes();
        if (sizes.size() != 3) { return; }
        if (open) {
            toolbox->setMaximumWidth(QWIDGETSIZE_MAX);
            toolbox->setMinimumWidth(240);
        } else {
            toolbox->setFixedWidth(WorkspaceToolbox::rail_width);
        }
        const int left = open ? tools_->drawer_width() : WorkspaceToolbox::rail_width;
        sizes[1] = std::max(0, sizes[1] + sizes[0] - left);
        sizes[0] = left;
        columns->setSizes(sizes);
    });
    connect(tools_, &WorkspaceToolbox::selectionChanged, this, &EditorWindow::refresh_actions);
    connect(columns, &QSplitter::splitterMoved, tools_, [this, columns] {
        if (tools_->docked()) { tools_->set_drawer_width(columns->sizes().front()); }
    });

    // Diagnostics dock beneath the canvas, leaving the image and history visible.
    auto* editor = new QWidget(columns);
    editor->setObjectName(QStringLiteral("flowWorkspace"));
    auto* editor_layout = new QVBoxLayout(editor);
    editor_layout->setSizeConstraint(QLayout::SetMinimumSize);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    editor_layout->setSpacing(0);
    canvas_host_ = new QMainWindow(editor);
    canvas_host_->setWindowFlags(Qt::Widget);
    work_pages_ = new QStackedWidget(canvas_host_);
    canvas_host_->setCentralWidget(work_pages_);
    overview_ = new QTreeWidget(work_pages_);
    overview_->setObjectName(QStringLiteral("solutionOverview"));
    overview_->setAccessibleName(tr("方案总览"));
    overview_->setHeaderLabels({tr("流程与关系"), tr("定义 / 本轮状态")});
    overview_->setAlternatingRowColors(true);
    overview_->setUniformRowHeights(true);
    work_pages_->addWidget(overview_);
    connect(overview_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const auto id = IdFactory::parse<WorkflowId>(item->data(0, Qt::UserRole).toString().toStdString());
        const auto* target = std::get_if<WorkflowId>(&id);
        const auto row = item->data(1, Qt::UserRole);
        if (!target) { return; }
        const auto child = row.isValid() ? std::optional{run_controller_->solution_report()->runs.at(row.toULongLong())}
                                         : std::nullopt;
        set_overview_visible(false);
        if (scene_->select_workflow(*target) && child) { module_history_->browse_run(*child); }
    });
    editor_layout->addWidget(canvas_host_, 1);
    auto* stats_bar = new QWidget(editor);
    auto* stats_layout = new QHBoxLayout(stats_bar);
    stats_layout->setContentsMargins(7, 3, 5, 3);
    workspace_stats_ = new QLabel(stats_bar);
    workspace_stats_->setObjectName(QStringLiteral("workspaceStats"));
    workspace_stats_->setTextFormat(Qt::PlainText);
    workspace_stats_->setFont(workspace_font(WorkspaceText::Secondary));
    workspace_stats_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    stats_layout->addWidget(workspace_stats_, 1);
    diagnostics_button_ = new QToolButton(stats_bar);
    diagnostics_button_->setObjectName(QStringLiteral("openDiagnostics"));
    diagnostics_button_->setText(tr("诊断"));
    stats_layout->addWidget(diagnostics_button_);
    editor_layout->addWidget(stats_bar);

    auto* results = new QSplitter(Qt::Vertical, columns);
    results_splitter_ = results;
    results->setHandleWidth(workspace_size::splitter);
    results->setObjectName(QStringLiteral("resultsSplitter"));
    results->setChildrenCollapsible(false);
    images_dock_ = new QDockWidget(tr("输出图"), results);
    images_dock_->setObjectName(QStringLiteral("imagesDock"));
    images_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
    images_ = new ImagePanel(images_dock_);
    images_->setObjectName(QStringLiteral("imagePanel"));
    auto* history = new QDockWidget(tr("模块历史结果"), results);
    result_history_dock_ = history;
    history->setObjectName(QStringLiteral("moduleHistoryDock"));
    history->setFeatures(QDockWidget::NoDockWidgetFeatures);
    module_history_ = new ModuleHistoryPanel(history);
    results_ = new WorkspaceResults(*images_dock_, *history, *images_, *module_history_);
    images_dock_->setTitleBarWidget(panel_header(new QLabel(tr("输出"), images_dock_), results_->image_tabs()));
    history->setTitleBarWidget(panel_header(new QLabel(tr("结果"), history), results_->result_tabs()));
    connect(results_, &WorkspaceResults::view_changed, this, &EditorWindow::refresh_workspace_statistics);
    results->addWidget(images_dock_);
    results->addWidget(history);
    results->setSizes({510, 215});
    columns->addWidget(toolbox);
    columns->addWidget(editor);
    columns->addWidget(results);
    columns->setStretchFactor(1, 1);
    columns->setStretchFactor(2, 1);
    columns->setSizes({76, 700, 650});

    connect(images_, &ImagePanel::frame_changed, this, [this] {
        if (inspector_) { inspector_->set_preview_image(images_->frame()); }
        refresh_actions();
    });

    diagnostics_dock_ = new QDockWidget(tr("诊断"), canvas_host_);
    diagnostics_dock_->setObjectName(QStringLiteral("diagnosticsDock"));
    diagnostics_ = new QListWidget(diagnostics_dock_);
    diagnostics_->setObjectName(QStringLiteral("editorDiagnostics"));
    diagnostics_dock_->setWidget(diagnostics_);
    diagnostics_dock_->setAllowedAreas(Qt::BottomDockWidgetArea);
    canvas_host_->addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock_);
    diagnostics_dock_->hide();
    connect(diagnostics_button_, &QToolButton::clicked, this, [this] {
        diagnostics_dock_->show();
        diagnostics_dock_->raise();
        diagnostics_dock_->activateWindow();
        if (!diagnostics_->currentItem() && diagnostics_->count()) { diagnostics_->setCurrentRow(0); }
        diagnostics_->setFocus(Qt::OtherFocusReason);
    });
    connect(diagnostics_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        try { locate_diagnostic(item); }
        catch (const std::exception& failure) { report_error(QString::fromUtf8(failure.what())); }
        catch (...) { report_error(tr("无法定位此诊断。")); }
    });
    connect(diagnostics_, &QListWidget::currentItemChanged, this, [this] { if (scene_) { refresh_actions(); } });
    create_run_dock();
    connect(run_controller_, &RunController::changed, this, [this, last = std::optional<std::uint64_t>{}, waiting = false]() mutable {
        const auto& report = run_controller_->solution_report();
        const auto sequence = report ? std::optional{report->sequence} : std::nullopt;
        const bool pending = !report && run_controller_->solution_activity() != SolutionActivity::Idle;
        if ((sequence != last || pending != waiting) && overview_action_ && overview_action_->isChecked()) { refresh_overview(); }
        last = sequence;
        waiting = pending;
    });
}

void EditorWindow::replace_canvas(Solution replacement, bool editable)
{
    const auto initial = replacement.workflows.empty() ? std::optional<WorkflowId>{} : replacement.workflows.front().id;
    auto model = std::make_unique<ControlGraphModel>(std::move(replacement), initial, registry_);
    auto scene = std::make_unique<ControlScene>(*model);
    scene->set_editing_enabled(editable);
    auto view = std::make_unique<WorkflowCanvas>(scene.get(), tools_, [this](const QString& type, QPointF position) {
        try { add_workspace_tool(type, position); }
        catch (const std::exception& error) { report_error(QString::fromUtf8(error.what())); }
        catch (...) { report_error(tr("无法添加工具。")); }
    });
    auto inspector = std::make_unique<EditorInspector>(*model, *scene, registry_, nullptr,
        [this](const QString& path, std::function<void(std::string)> apply) { hash_resource_file(path, std::move(apply)); });
    for (auto* registered : view->actions()) {
        if (registered != view->clearSelectionAction()) { view->removeAction(registered); }
    }
    view->setObjectName(QStringLiteral("workflowCanvas"));
    if (auto* dialog = findChild<QDialog*>(QStringLiteral("solutionResourcesDialog"))) { dialog->close(); }
    properties_dialog_->hide();
    if (scene_) {
        disconnect(scene_.get(), nullptr, this, nullptr);
        disconnect(&scene_->undoStack(), nullptr, this, nullptr);
    }
    delete inspector_;
    delete view_;
    scene_.reset();
    model_.reset();
    model_ = std::move(model);
    scene_ = std::move(scene);
    view_ = view.release();
    inspector_ = inspector.release();
    work_pages_->addWidget(view_);
    work_pages_->setCurrentWidget(view_);
    overview_action_->setChecked(false);
    canvas_host_->findChild<QToolBar*>(QStringLiteral("workflowRunBar"))->show();
    static_cast<QVBoxLayout*>(properties_dialog_->layout())->insertWidget(0, inspector_, 1);
    inspector_->setEnabled(editable);
    event_history_->clear();
    last_event_->clear();
    last_event_->setToolTip({});
    statusBar()->clearMessage();
    showing_run_image_ = false;
    *workspace_image_selection_ = {};
    results_->reset_document();
    images_->set_sources({}, -1);
    images_->set_manual_view(false);
    images_->clear_frame();
    run_error_.reset();
    module_history_->clear();
    run_panel_->clear();
    run_controller_->reset_document();
    shown_workflow_.reset();
    shown_scope_.reset();
    canvas_views_.clear();

    connect(scene_.get(), &ControlScene::edit_failed, this, [this](const QString& message) { report_error(message); });
    connect(scene_.get(), &ControlScene::edit_notice, this, [this](const QString& message) { show_status(message); });
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, [this](QtNodes::NodeId node) {
        QTimer::singleShot(0, scene_.get(), [this, node] {
            auto* item = scene_->nodeGraphicsObject(node);
            if (!item || scene_->open_group(node)) { return; }
            if (!item->isSelected() || scene_->selectedItems().size() != 1) {
                scene_->clearSelection();
                item->setSelected(true);
            }
            show_properties();
        });
    });
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeContextMenu, this, [this](QtNodes::NodeId node, const QPointF& point) {
        const auto id = model_->domain_node(node);
        if (!id || !scene_->reveal_node(*id)) { return; }
        auto* menu = new QMenu(view_);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(tr("属性…"), this, [this] { show_properties(); });
        menu->addAction(debug_node_);
        menu->popup(view_->viewport()->mapToGlobal(view_->mapFromScene(point)));
    });
    connect(model_.get(), &QtNodes::AbstractGraphModel::modelReset, this, [this] {
        if (!showing_run_image_) { images_->clear_overlays(); }
        if (auto refreshed = images_->refresh_class_palette(draft()); const auto* failure = std::get_if<Diagnostic>(&refreshed)) {
            report_error(QString::fromStdString(failure->message));
        }
        refresh_canvas();
    });
    connect(scene_.get(), &QGraphicsScene::selectionChanged, this, [this] {
        if (!showing_run_image_) { images_->clear_overlays(); }
        refresh_actions();
        refresh_workspace_selection();
    });
    connect(&scene_->undoStack(), &QUndoStack::indexChanged, this, [this] {
        ++workspace_image_selection_->edit_generation;
        refresh_workspace_statistics();
        queue_diagnostics();
        refresh_actions();
    });
    connect(&scene_->undoStack(), &QUndoStack::cleanChanged, this, [this] { refresh_actions(); });
    issues_.clear();
    refresh_canvas();
    queue_diagnostics();
}

void EditorWindow::refresh_canvas()
{
    if (overview_action_->isChecked()) { refresh_overview(); }
    const QSignalBlocker blocker(workflows_);
    while (workflows_->count() > static_cast<int>(draft().workflows.size())) { workflows_->removeTab(workflows_->count() - 1); }
    int selected = -1;
    int row = 0;
    for (const auto& flow : draft().workflows) {
        auto name = QString::fromStdString(flow.name);
        if (!flow.enabled) { name += tr("（已禁用）"); }
        if (row == workflows_->count()) { workflows_->addTab(name); }
        else { workflows_->setTabText(row, name); }
        const auto id = QString::fromStdString(flow.id.to_string());
        workflows_->setTabData(row, id);
        workflows_->setTabToolTip(row, name + u'\n' + id);
        if (model_->workflow_id() == flow.id) { selected = row; }
        ++row;
    }
    workflows_->setCurrentIndex(selected);
    const auto path = model_->scope() ? model_->scope_path() : QString{};
    scope_path_->setText(scope_path_->fontMetrics().elidedText(path, Qt::ElideLeft, 150));
    scope_path_->setToolTip(path);
    up_->setVisible(model_->scope().has_value());
    if (shown_workflow_ != model_->workflow_id() || shown_scope_ != model_->scope()) {
        switch_canvas_view();
    }
    refresh_actions();
    refresh_workspace_selection();
}

void EditorWindow::add_workspace_tool(const QString& type, QPointF position)
{
    if (!writable() || busy_ || close_confirmed_ || !model_->workflow_id() || type.isEmpty()) { return; }
    if (type == QStringLiteral("@group") || type == QStringLiteral("@loop")) {
        const bool loop = type == QStringLiteral("@loop");
        if (const auto name = ask_name(this, loop ? tr("新建循环") : tr("新建工具组"), loop ? tr("循环") : tr("工具组"))) {
            scene_->add_group(*name, position, loop ? ControlScene::GroupKind::Loop : ControlScene::GroupKind::Group);
        }
        return;
    }
    const auto parsed = SchemaIdFactory::parse<ToolTypeId>(type.toStdString());
    if (const auto* id = std::get_if<ToolTypeId>(&parsed); id && registry_.find_descriptor(*id)) {
        scene_->add_tool(*id, position);
    }
}

void EditorWindow::choose_control_connection()
{
    const QPointer<QWidget> previous_focus = focusWidget();
    QDialog dialog(this);
    dialog.setWindowTitle(tr("连接执行步骤"));
    auto* form = new QFormLayout(&dialog);
    auto* source = new QComboBox(&dialog);
    auto* outlet = new QComboBox(&dialog);
    auto* target = new QComboBox(&dialog);
    source->setObjectName(QStringLiteral("controlSource"));
    outlet->setObjectName(QStringLiteral("controlOutlet"));
    target->setObjectName(QStringLiteral("controlTarget"));
    form->addRow(tr("起点(&S)"), source);
    form->addRow(tr("执行出口(&O)"), outlet);
    form->addRow(tr("终点(&T)"), target);
    auto* issue = new QLabel(&dialog);
    form->addRow(issue);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* confirm = buttons->button(QDialogButtonBox::Ok);
    confirm->setText(tr("连接"));
    confirm->setDefault(true);
    form->addRow(buttons);

    const auto visible = model_->allNodeIds();
    const auto& flow = *find_workflow(draft(), *model_->workflow_id());
    QHash<QString, int> name_counts;
    for (const auto id : visible) {
        ++name_counts[model_->nodeData(id, QtNodes::NodeRole::Caption).toString()];
    }
    for (const auto& node : flow.nodes) {
        const auto id = model_->canvas_id(node.id);
        if (!visible.contains(id)) { continue; }
        auto name = model_->nodeData(id, QtNodes::NodeRole::Caption).toString();
        if (name_counts.value(name) > 1) { name += QStringLiteral(" [%1]").arg(QString::fromStdString(node.id.to_string())); }
        source->addItem(name, id);
        target->addItem(name, id);
    }
    const auto selected = scene_->selected_nodes();
    if (selected.size() == 1) { source->setCurrentIndex(source->findData(model_->canvas_id(selected.front()))); }
    target->setCurrentIndex(source->currentIndex() == 0 ? 1 : 0);
    const auto update_outlets = [&] {
        const QSignalBlocker blocker(outlet);
        outlet->clear();
        outlet->addItem(tr("正常完成"));
        for (const auto& port : model_->named_control_outlets(source->currentData().toUInt())) {
            const auto id = QString::fromUtf8(port.id.text());
            const auto name = QString::fromStdString(port.display_name);
            outlet->addItem(name.isEmpty() || name == id ? id : tr("%1 [%2]").arg(name, id));
        }
    };
    const auto connection = [&] {
        return QtNodes::ConnectionId{source->currentData().toUInt(), static_cast<QtNodes::PortIndex>(outlet->currentIndex()),
            target->currentData().toUInt(), 0};
    };
    const auto validate = [&] {
        const auto link = connection();
        const bool possible = model_->connectionPossible(link);
        confirm->setEnabled(possible);
        if (!possible) {
            if (link.outNodeId == link.inNodeId) { issue->setText(tr("请选择不同的起点和终点。")); }
            else if (model_->connectionExists(link)) { issue->setText(tr("所选出口已连接到此终点。")); }
            else { issue->setText(tr("此连线会形成循环，请选择其他步骤。")); }
        }
        issue->setVisible(!possible);
    };
    connect(source, &QComboBox::currentIndexChanged, &dialog, [&] { update_outlets(); validate(); });
    connect(outlet, &QComboBox::currentIndexChanged, &dialog, validate);
    connect(target, &QComboBox::currentIndexChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    update_outlets();
    validate();
    source->setFocus();
    if (dialog.exec() == QDialog::Accepted && scene_->connect_control(connection())) {
        show_status(tr("已将 %1 的 %2 出口连接到 %3。").arg(source->currentText(), outlet->currentText(), target->currentText()));
    }
    activateWindow();
    QWidget* next_focus = previous_focus && previous_focus->isVisible() && previous_focus->isEnabled()
        ? previous_focus.data() : view_;
    next_focus->setFocus(Qt::OtherFocusReason);
}

void EditorWindow::show_properties()
{
    const auto selected = scene_->selected_nodes();
    const auto flow = model_->workflow_id();
    const auto target = flow && selected.size() == 1 ? std::optional{NodeTarget{*flow, selected.front()}} : std::nullopt;
    inspector_->pin(target);
    properties_dialog_->show();
    refresh_properties_status();
    properties_dialog_->raise();
    properties_dialog_->activateWindow();
}

void EditorWindow::refresh_properties_status()
{
    if (!properties_dialog_->isVisible()) { return; }
    const auto target = inspector_->pinned_node();
    const auto flow_id = target ? std::optional{target->workflow} : model_->workflow_id();
    const auto* flow = flow_id ? find_workflow(draft(), *flow_id) : nullptr;
    const auto* node = flow && target ? find_node(*flow, target->node) : nullptr;
    const auto flow_name = flow ? QString::fromStdString(flow->name) : tr("流程");
    QString title = tr("流程属性 · %1").arg(flow_name);
    if (target) {
        title = node ? tr("模块属性 · %1 / %2").arg(flow_name, QString::fromStdString(node->name))
                     : tr("模块属性 · 模块已删除");
        if (node) {
            const auto parent = std::ranges::find(flow->containment, node->id, &Containment::child);
            if (parent != flow->containment.end()) {
                if (const auto* group = find_node(*flow, parent->parent)) {
                    title = tr("模块属性 · %1 / %2 / %3").arg(flow_name, QString::fromStdString(group->name),
                        QString::fromStdString(node->name));
                }
            }
            if (const auto* config = std::get_if<ToolConfiguration>(&node->configuration)) {
                if (const auto* tool = registry_.find_descriptor(config->type)) {
                    title += QStringLiteral(" · ") + QString::fromStdString(tool->display_name);
                }
            }
        }
    }
    properties_dialog_->setWindowTitle(title);
    const auto* group = node ? std::get_if<GroupConfiguration>(&node->configuration) : nullptr;
    configured_debug_->setVisible(node && !(group && group->loop));
    configured_debug_->setEnabled(node && writable() && !busy_ && !close_confirmed_ && run_controller_->can_submit(*flow));
    configured_status_->setVisible(node);
    if (!node) { return; }
    for (const auto& run : std::views::reverse(run_controller_->runs())) {
        const auto* debugged = std::get_if<NodeTarget>(&run.request().target());
        if (!debugged || *debugged != *target) { continue; }
        const auto result = run.inspect();
        if (const auto* progress = std::get_if<RunProgress>(&result)) {
            auto text = tr("模块调试 · 第 %1 轮 · %2 · 配置快照 %3").arg(run.identity().run.value())
                .arg(execution_status_text(progress->status)).arg(run.identity().snapshot.snapshot.value());
            if (progress->elapsed) { text += tr(" · %1 ms").arg(std::chrono::duration<double, std::milli>(*progress->elapsed).count(), 0, 'f', 1); }
            const auto& display = *workspace_image_selection_;
            const auto capture = display.capture_generations.find(run.identity().snapshot);
            if (capture == display.capture_generations.end() || capture->second != display.edit_generation) {
                text += tr(" · 结果来自此前配置");
            }
            configured_status_->setText(text);
            return;
        }
    }
    configured_status_->setText(tr("暂无本模块的独立调试记录"));
}

void EditorWindow::switch_canvas_view()
{
    if (shown_workflow_ && view_->isVisible()) {
        canvas_views_[{*shown_workflow_, shown_scope_}] = {view_->getScale(), view_->view_center()};
    }
    shown_workflow_ = model_->workflow_id();
    shown_scope_ = model_->scope();
    scene_->setOrientation(model_->layout_orientation());
    if (shown_workflow_) {
        const auto saved = canvas_views_.find({*shown_workflow_, shown_scope_});
        if (saved != canvas_views_.end()) {
            view_->restore_view(saved->second.scale, saved->second.center);
            return;
        }
    }
    QRectF entry;
    if (const auto* flow = shown_workflow_ ? find_workflow(draft(), *shown_workflow_) : nullptr) {
        const auto* entries = &flow->entries;
        if (shown_scope_) {
            if (const auto* node = find_node(*flow, *shown_scope_)) {
                if (const auto* group = std::get_if<GroupConfiguration>(&node->configuration)) { entries = &group->entries; }
            }
        }
        for (const auto& id : *entries) {
            if (auto* graphic = scene_->nodeGraphicsObject(model_->canvas_id(id))) {
                entry = graphic->sceneBoundingRect();
                break;
            }
        }
    }
    view_->open_at_actual_size(entry);
}

void EditorWindow::refresh_actions()
{
    if (!scene_) { return; }
    const bool available = !busy_ && !close_confirmed_;
    const bool editable = available && writable();
    const bool on_canvas = !overview_action_->isChecked();
    const bool active = on_canvas && model_->workflow_id().has_value();
    const auto editable_node_count = editable && active ? model_->allNodeIds().size() : 0;
    arrange_->setEnabled(editable_node_count > 0);
    const auto selected = on_canvas ? scene_->selected_nodes() : std::vector<NodeId>{};
    const bool has_nodes = !selected.empty();
    const auto* flow = active ? find_workflow(draft(), *model_->workflow_id()) : nullptr;
    const auto* selected_node = flow && selected.size() == 1 ? find_node(*flow, selected.front()) : nullptr;
    const auto* group = selected_node ? std::get_if<GroupConfiguration>(&selected_node->configuration) : nullptr;
    const auto& history = scene_->undoStack();
    const auto selection = diagnostics_->currentItem();
    const int issue = selection ? selection->data(Qt::UserRole).toInt() : -1;
    const bool has_resource = std::ranges::any_of(draft().resources, [](const ResourceDefinition& resource) {
        return std::holds_alternative<ExternalFileResource>(resource.location);
    });
    new_->setEnabled(available);
    open_->setEnabled(available);
    save_->setEnabled(editable && (!directory_ || !history.isClean() || !source_));
    save_as_->setEnabled(editable);
    independent_copy_->setEnabled(editable);
    export_workflow_->setEnabled(editable && active);
    export_group_->setEnabled(editable && group && !group->loop);
    import_subgraph_->setEnabled(editable);
    resource_copy_->setEnabled(editable && source_.has_value() && has_resource);
    resource_root_->setEnabled(available);
    resources_->setEnabled(available);
    const bool can_submit = editable && flow && run_controller_->can_submit(*flow);
    run_workflow_->setEnabled(can_submit);
    run_workflow_->setToolTip(can_submit ? tr("单次运行当前流程（F5）") : tr("需要可运行的当前流程和可用的运行会话"));
    const auto targets = flow ? run_controller_->cancellable_workflow_runs(flow->id) : std::vector<RunHandle>{};
    cancel_workflow_->setEnabled(!close_confirmed_ && !targets.empty());
    cancel_workflow_->setToolTip(targets.empty() ? tr("当前流程没有可取消的独立运行")
        : targets.size() == 1 ? tr("取消当前流程的第 %1 轮").arg(targets.front().identity().run.value()) : tr("选择要取消的流程轮次"));
    solution_actions_.start->setEnabled(editable && draft().graph && run_controller_->can_start());
    solution_actions_.start->setToolTip(draft().graph ? tr("从方案入口执行已编排的流程") : tr("当前方案未配置流程间编排"));
    solution_actions_.stop->setEnabled(run_controller_->can_cancel_solution());
    solution_actions_.stop->setToolTip(solution_actions_.stop->isEnabled() ? tr("取消当前整方案运行") : tr("当前没有可取消的整方案运行"));
    QString solution_state;
    const auto activity = run_controller_->solution_activity();
    if (activity == SolutionActivity::Running) { solution_state = tr("方案运行中"); }
    else if (activity == SolutionActivity::CancelRequested) { solution_state = tr("方案已请求取消"); }
    else if (const auto& report = run_controller_->solution_report()) {
        solution_state = tr("方案第 %1 轮 · %2").arg(report->sequence).arg(execution_status_text(report->status));
        if (activity == SolutionActivity::CleaningUp) { solution_state += tr(" · 正在清理"); }
    }
    solution_run_status_->setText(solution_state);
    solution_actions_.report->setEnabled(run_controller_->solution_report().has_value());
    run_group_->setEnabled(can_submit && group && !group->loop);
    debug_node_->setEnabled(can_submit && selected_node && !(group && group->loop));
    debug_node_->setText(group ? tr("调试所选工具组…") : tr("调试所选模块…"));
    const auto selected_run = workspace_cancel_target();
    const auto flow_name = flow ? QString::fromStdString(flow->name) : tr("当前流程");
    cancel_run_->setText(selected_run ? tr("取消“%1”第 %2 轮").arg(flow_name).arg(selected_run->identity().run.value())
                                      : tr("取消工作台所示轮次"));
    cancel_run_->setEnabled(!close_confirmed_ && selected_run && run_controller_->can_cancel(*selected_run));
    const auto detail_run = run_panel_->selected_run();
    run_panel_->set_cancel_enabled(!close_confirmed_ && run_dock_->isVisible() && detail_run && run_controller_->can_cancel(*detail_run));
    arm_trigger_->setEnabled(editable && flow && run_controller_->can_arm_trigger(*flow));
    workspace_trigger_->setEnabled(editable && flow && (!flow->trigger || arm_trigger_->isEnabled()));
    run_panel_->set_camera_status(flow ? run_controller_->camera_status(flow->id) : QString{});
    send_frame_->setEnabled(available && flow && run_controller_->can_send_frame(flow->id));
    const bool armed = available && flow && run_controller_->can_stop_trigger(flow->id);
    drain_trigger_->setText(tr("停止“%1”触发并等待在途运行").arg(flow_name));
    cancel_trigger_->setText(tr("停止“%1”触发并取消在途运行").arg(flow_name));
    drain_trigger_->setEnabled(armed);
    cancel_trigger_->setEnabled(armed);
    reset_state_->setEnabled(editable && selected_node && std::holds_alternative<ToolConfiguration>(selected_node->configuration)
        && run_controller_->can_reset_state({*model_->workflow_id(), selected_node->id}));
    run_limits_->setEnabled(!close_confirmed_);
#ifdef VISIONPRO_WITH_OPENCV
    preview_image_->setEnabled(available);
    preview_resource_->setEnabled(available && std::ranges::any_of(draft().resources,
        [](const ResourceDefinition& resource) { return resource.kind == ResourceKind::Image; }));
    preview_roi_->setEnabled(available && images_->frame().has_value() && selected.size() == 1);
#else
    preview_image_->setEnabled(false);
    preview_resource_->setEnabled(false);
    preview_roi_->setEnabled(false);
#endif
    rename_->setEnabled(editable);
    add_frame_->setEnabled(editable);
    new_workflow_->setEnabled(editable);
    remove_workflow_->setEnabled(editable && active);
    tools_->setEnabled(editable && active);
    add_tool_->setEnabled(editable && active && !tools_->selected_type().isEmpty());
    add_group_->setEnabled(editable && active);
    add_loop_->setEnabled(editable && active);
    connect_steps_->setEnabled(editable_node_count >= 2);
    remove_nodes_->setEnabled(editable && has_nodes && !scene_->selected_control());
    remove_link_->setEnabled(editable && on_canvas && scene_->selected_control().has_value());
    remove_invalid_link_->setEnabled(editable && displayed_validation_revision_ == validation_revision_
        && issue >= 0 && static_cast<std::size_t>(issue) < issues_.size()
        && issues_[static_cast<std::size_t>(issue)].location.control_link.has_value());
    copy_->setEnabled(!busy_ && has_nodes);
    paste_->setEnabled(editable && active);
    undo_->setEnabled(editable && history.canUndo());
    redo_->setEnabled(editable && history.canRedo());
    up_->setEnabled(model_->scope().has_value());
    inspector_->setEnabled(editable);
    scene_->set_editing_enabled(editable);
    const auto name = QString::fromStdString(draft().name);
    setWindowTitle(QStringLiteral("VisionPro · %1[*]").arg(name));
    solution_name_->setText(solution_name_->fontMetrics().elidedText(tr("方案 · %1").arg(name), Qt::ElideMiddle, 280));
    solution_name_->setToolTip(name);
    setWindowModified(!history.isClean());
    refresh_properties_status();
}

void EditorWindow::add_coordinate_frame()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add Coordinate Frame"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    name->setObjectName(QStringLiteral("coordinateFrameId"));
    auto* kind = new QComboBox(&dialog);
    kind->setObjectName(QStringLiteral("coordinateFrameKind"));
    kind->addItems({QStringLiteral("Physical (mm, X right, Y up)"), QStringLiteral("Image (px, X right, Y down)")});
    form->addRow(QStringLiteral("Frame ID"), name);
    form->addRow(QStringLiteral("Convention"), kind);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) { return; }
    scene_->edit_draft(QStringLiteral("Add coordinate frame"), [&](Solution& candidate) -> Result<std::monostate> {
        auto parsed = SchemaIdFactory::parse<CoordinateSystemId>(name->text().trimmed().toStdString());
        if (auto* issue = std::get_if<Diagnostic>(&parsed)) { return *issue; }
        const auto id = std::get<CoordinateSystemId>(parsed);
        if (std::ranges::find(candidate.coordinates, id, &CoordinateFrameDefinition::id) != candidate.coordinates.end()) {
            return Diagnostic{ErrorCode::DuplicateIdentity, Severity::Error, {}, "This coordinate frame ID is already declared."};
        }
        const bool image = kind->currentIndex() == 1;
        candidate.coordinates.push_back({id, image ? CoordinateKind::Image : CoordinateKind::Physical,
            image ? MeasurementUnit::Pixel : MeasurementUnit::Millimetre, {0, 0}, {1, 0}, {0, image ? -1.0 : 1.0},
            image ? Handedness::LeftHanded : Handedness::RightHanded});
        return std::monostate{};
    });
}

void EditorWindow::make_new_document()
{
    const auto name = ask_name(this, tr("新建方案"), tr("未命名方案"));
    if (!name) { return; }
    const auto solution = IdFactory::generate<SolutionId>();
    const auto flow = IdFactory::generate<WorkflowId>();
    for (const auto* failure : {std::get_if<Diagnostic>(&solution), std::get_if<Diagnostic>(&flow)}) {
        if (failure) { report_error(QString::fromStdString(failure->message)); return; }
    }
    const auto schema = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("visionpro.editor.workflow"));
    const auto pixels = std::get<CoordinateSystemId>(SchemaIdFactory::parse<CoordinateSystemId>("visionpro.image.pixels"));
    Workflow initial{std::get<WorkflowId>(flow), tr("流程").toStdString(), {schema, 1, {}, {}, {}}, {}, {}, {}, {}, {}, true, {}};
    replace_canvas({std::get<SolutionId>(solution), *name, infrastructure::solution_format_version, 1,
        {std::move(initial)}, {}, {}, {}, {
            {pixels, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}
        }, {}});
    directory_.reset();
    source_.reset();
    pending_baseline_.reset();
    file_notices_.clear();
    render_diagnostics();
    refresh_actions();
}

bool EditorWindow::confirm_replacement(Continuation next)
{
    if (busy_) { return false; }
    if (scene_->undoStack().isClean()) { next(); return true; }
    const auto choice = QMessageBox::question(this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save changes to %1?").arg(QString::fromStdString(draft().name)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Save) { begin_save(std::move(next)); }
    if (choice == QMessageBox::Discard) { next(); return true; }
    return false;
}

void EditorWindow::closeEvent(QCloseEvent* event)
{
    event->ignore();
    if (busy_) {
        close_when_idle_ = true;
        show_status(QStringLiteral("The document operation will finish before closing."));
        return;
    }
    if (!close_confirmed_ && !scene_->undoStack().isClean()) {
        try {
            confirm_replacement([this] {
                close_confirmed_ = true;
                refresh_actions();
                QTimer::singleShot(0, this, [this] { close(); });
            });
        } catch (...) {
            report_error(QStringLiteral("Could not close the editor."));
        }
        return;
    }
    close_confirmed_ = true;
    if (run_controller_->has_session_work()) {
        run_panel_->clear();
        run_controller_->reset_document();
        show_status(QStringLiteral("Waiting for run cleanup before closing…"));
        refresh_actions();
        return;
    }
    save_workspace_layout();
    properties_dialog_->hide();
    event->accept();
}

void EditorWindow::restore_default_layout()
{
    const QSize available = screen()->availableGeometry().size();
    setWindowState(windowState() & ~Qt::WindowMaximized & ~Qt::WindowFullScreen);
    resize(QSize(1440, 800).boundedTo(available));
    restoreState(default_window_state_, 1);
    canvas_host_->restoreState(default_canvas_state_, 1);
    tools_->set_drawer_width(294);
    tools_->set_pinned(false);
    show_history_->setChecked(true);
    workspace_splitter_->setSizes({WorkspaceToolbox::rail_width, 700, 650});
    results_splitter_->setSizes({510, 215});
    properties_dialog_->resize(QSize(680, 540).boundedTo(available));
    if (layout_settings_) { layout_settings_->remove(QStringLiteral("workspace/layout")); }
}

void EditorWindow::restore_workspace_layout()
{
    if (!layout_settings_) { return; }
    auto& settings = *layout_settings_;
    settings.beginGroup(QStringLiteral("workspace/layout"));
    if (settings.value("version").toInt() != 1) {
        settings.endGroup();
        return;
    }
    tools_->set_drawer_width(settings.value("drawerWidth", 294).toInt());
    tools_->set_pinned(settings.value("pinned").toBool());
    const bool restored = restoreGeometry(settings.value("geometry").toByteArray())
        && restoreState(settings.value("window").toByteArray(), 1)
        && canvas_host_->restoreState(settings.value("canvasWindow").toByteArray(), 1)
        && workspace_splitter_->restoreState(settings.value("columns").toByteArray())
        && results_splitter_->restoreState(settings.value("results").toByteArray());
    show_history_->setChecked(settings.value("showResults", true).toBool());
    properties_dialog_->resize(settings.value("propertiesSize", QSize(680, 540)).toSize()
        .boundedTo(screen()->availableGeometry().size()));
    settings.endGroup();
    if (!restored) { restore_default_layout(); }
}

void EditorWindow::save_workspace_layout()
{
    if (!layout_settings_) { return; }
    auto& settings = *layout_settings_;
    settings.beginGroup(QStringLiteral("workspace/layout"));
    settings.setValue("version", 1);
    settings.setValue("geometry", saveGeometry());
    settings.setValue("window", saveState(1));
    settings.setValue("canvasWindow", canvas_host_->saveState(1));
    settings.setValue("columns", workspace_splitter_->saveState());
    settings.setValue("results", results_splitter_->saveState());
    settings.setValue("showResults", show_history_->isChecked());
    settings.setValue("pinned", tools_->docked());
    settings.setValue("drawerWidth", tools_->docked() ? workspace_splitter_->sizes().front() : tools_->drawer_width());
    settings.setValue("propertiesSize", properties_dialog_->size());
    settings.endGroup();
}

void EditorWindow::queue_diagnostics()
{
    ++validation_revision_;
    validation_timer_->start();
}

void EditorWindow::validate_draft()
{
    if (validating_) { return; }
    const auto revision = validation_revision_;
    auto result = std::make_shared<std::vector<Diagnostic>>();
    auto* worker = QThread::create([candidate = draft(), registry = registry_, result] {
        try {
            auto validation = diagnose_solution(candidate, registry.descriptors());
            if (auto* failure = std::get_if<Diagnostic>(&validation)) {
                result->push_back(std::move(*failure));
            } else {
                *result = std::get<std::vector<Diagnostic>>(std::move(validation));
            }
        } catch (...) {
            result->push_back({ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not validate the draft."});
        }
    });
    validating_ = true;
    connect(worker, &QThread::finished, this, [this, revision, result] {
        validating_ = false;
        if (revision == validation_revision_) {
            issues_ = std::move(*result);
            displayed_validation_revision_ = revision;
            render_diagnostics();
        } else {
            validation_timer_->start();
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void EditorWindow::render_diagnostics()
{
    diagnostics_->clear();
    for (const auto& notice : file_notices_) {
        auto* item = new QListWidgetItem(notice, diagnostics_);
        item->setData(Qt::UserRole, -1);
        item->setToolTip(notice);
    }
    for (std::size_t index = 0; index < issues_.size(); ++index) {
        const auto& diagnostic = issues_[index];
        const auto location = diagnostic_location(draft(), diagnostic.location);
        const auto message = QString::fromStdString(diagnostic.message);
        auto* item = new QListWidgetItem(location.isEmpty() ? message : location + QStringLiteral("  —  ") + message, diagnostics_);
        item->setData(Qt::UserRole, static_cast<int>(index));
        item->setToolTip(item->text());
    }
    if (run_error_) {
        const auto location = diagnostic_location(draft(), run_error_->location);
        const auto message = QStringLiteral("Run: %1").arg(run_diagnostic_text(*run_error_));
        auto* item = new QListWidgetItem(location.isEmpty() ? message : location + QStringLiteral("  —  ") + message, diagnostics_);
        item->setData(Qt::UserRole, -1);
        item->setData(Qt::UserRole + 1, true);
        item->setToolTip(item->text());
    }
    diagnostics_button_->setText(diagnostics_->count() ? tr("诊断 (%1)").arg(diagnostics_->count()) : tr("诊断"));
    refresh_actions();
}

void EditorWindow::locate_diagnostic(QListWidgetItem* item)
{
    const int index = item->data(Qt::UserRole).toInt();
    const auto* diagnostic = item->data(Qt::UserRole + 1).toBool() && run_error_ ? &*run_error_
        : (index >= 0 && static_cast<std::size_t>(index) < issues_.size() ? &issues_[static_cast<std::size_t>(index)] : nullptr);
    if (!diagnostic) { return; }
    const auto location = diagnostic->location;
    if (location.workflow && overview_action_->isChecked()) { set_overview_visible(false); }
    if (location.workflow && !scene_->select_workflow(*location.workflow)) { return; }
    if (location.node) {
        if (!scene_->reveal_node(*location.node)) { return; }
        if (const auto* graphic = scene_->nodeGraphicsObject(model_->canvas_id(*location.node))) { view_->centerOn(graphic); }
    } else if (location.workflow && location.input) {
        while (model_->scope() && scene_->parent_scope()) {}
        scene_->clearSelection();
    } else {
        return;
    }
    if (location.input || location.parameter) {
        show_properties();
        if (!inspector_->focus_diagnostic_field(location)) {
            show_status(tr("当前属性中找不到此诊断对应的字段。"));
        }
    }
}

void EditorWindow::remove_diagnosed_link()
{
    if (displayed_validation_revision_ != validation_revision_) { return; }
    const auto* selected = diagnostics_->currentItem();
    if (!selected) { return; }
    const int index = selected->data(Qt::UserRole).toInt();
    if (index < 0 || static_cast<std::size_t>(index) >= issues_.size()) { return; }
    const auto location = issues_[static_cast<std::size_t>(index)].location;
    if (!location.workflow || !location.control_link) { return; }
    scene_->edit_draft(QStringLiteral("Remove diagnosed control link"), [location](Solution& candidate) -> Result<std::monostate> {
        const auto flow = std::ranges::find(candidate.workflows, *location.workflow, &Workflow::id);
        if (flow != candidate.workflows.end() && std::erase_if(flow->control_links, [&](const ControlLink& edge) {
                return edge.id == *location.control_link;
            })) {
            return std::monostate{};
        }
        return Diagnostic{ErrorCode::InvalidControlLink, Severity::Error, location, "The control link is no longer present."};
    });
}

void EditorWindow::report_error(const QString& message)
{
    file_notices_ = {message};
    render_diagnostics();
    show_status(message);
}

void EditorWindow::show_status(const QString& message)
{
    record_event(message);
    statusBar()->showMessage(message, 8500);
}

} // namespace visionpro::ui
