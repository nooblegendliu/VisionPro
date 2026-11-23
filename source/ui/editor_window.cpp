#include "editor_window.hpp"

#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "image_panel.hpp"
#include "module_history_panel.hpp"
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
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStringList>
#include <QThread>
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

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

QWidget* panel_header(QLabel* title, QToolButton* action = nullptr)
{
    auto* header = new QWidget(title->parentWidget());
    header->setObjectName(QStringLiteral("workspacePanelHeader"));
    header->setStyleSheet(QStringLiteral("QWidget#workspacePanelHeader { border-bottom: 1px solid palette(midlight); }"));
    title->setFont(workspace_font(WorkspaceText::Heading));
    auto* row = new QHBoxLayout(header);
    row->setContentsMargins(9, 2, 6, 2);
    row->addWidget(title, 1);
    if (action) { row->addWidget(action); }
    header->setMinimumHeight(std::max(32, title->fontMetrics().height() + 8));
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

EditorWindow::EditorWindow(Solution draft, ToolRegistry registry, QWidget* parent)
    : QMainWindow(parent), registry_(std::move(registry))
{
    setFont(workspace_font());
    resize(1440, 800);
    validation_timer_ = new QTimer(this);
    validation_timer_->setSingleShot(true);
    validation_timer_->setInterval(90);
    connect(validation_timer_, &QTimer::timeout, this, [this] { validate_draft(); });
    create_docks();
    create_actions();
    replace_canvas(std::move(draft));
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
    remove_nodes_ = action(graph, tr("删除选中节点"), "removeNodes", [this] {
        scene_->remove_selected_nodes();
    }, QKeySequence::Delete);
    remove_link_ = action(graph, tr("删除选中连线"), "removeLink", [this] {
        if (const auto link = scene_->selected_control()) { scene_->remove_control(*link); }
    }, QKeySequence::Delete);
    up_ = action(graph, tr("返回上层"), "parentGroup", [this] { scene_->parent_scope(); });
    auto* props = action(graph, tr("模块属性…"), "showProperties", [this] { show_properties(); });
    auto* flow_props = action(graph, tr("流程属性…"), "workflowProperties", [this] {
        scene_->clearSelection();
        show_properties();
    });

    create_run_actions();
    auto& view = *menuBar()->addMenu(tr("视图(&V)"));
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
    for (auto* dock : findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
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
    toolbar->setIconSize({18, 18});
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    const auto add_action = [&](QAction* item, WorkspaceIcon icon, const QString& label) {
        item->setIcon(workspace_icon(icon));
        item->setIconText(label);
        toolbar->addAction(item);
    };
    add_action(new_, WorkspaceIcon::New, tr("新建"));
    add_action(open_, WorkspaceIcon::Open, tr("打开"));
    add_action(save_, WorkspaceIcon::Save, tr("保存"));
    toolbar->addSeparator();
    add_action(undo_, WorkspaceIcon::Undo, tr("撤销"));
    add_action(redo_, WorkspaceIcon::Redo, tr("重做"));
    toolbar->addSeparator();
    toolbar->addWidget(new QLabel(tr("流程 "), toolbar));
    workflows_ = new QComboBox(toolbar);
    workflows_->setObjectName(QStringLiteral("workflowSelector"));
    workflows_->setMinimumContentsLength(22);
    workflows_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    toolbar->addWidget(workflows_);
    connect(workflows_, &QComboBox::currentIndexChanged, this, [this](int row) {
        if (scene_ && row >= 0 && static_cast<std::size_t>(row) < draft().workflows.size()) {
            scene_->select_workflow(draft().workflows[static_cast<std::size_t>(row)].id);
        }
    });
    auto* manage = new QToolButton(toolbar);
    manage->setText(tr("管理"));
    manage->setPopupMode(QToolButton::InstantPopup);
    auto* management = new QMenu(manage);
    management->addAction(flow_props);
    management->addAction(new_workflow_);
    management->addAction(remove_workflow_);
    manage->setMenu(management);
    toolbar->addWidget(manage);
    add_action(up_, WorkspaceIcon::Parent, tr("上层"));
    scope_path_ = new QLabel(toolbar);
    toolbar->addWidget(scope_path_);
    toolbar->addSeparator();
    add_action(run_workflow_, WorkspaceIcon::Run, tr("单次运行"));
    workspace_trigger_ = action(graph, tr("启动当前流程触发"), "startWorkspaceTrigger", [this, flow_props] {
        if (arm_trigger_->isEnabled()) { arm_trigger_->trigger(); }
        else {
            flow_props->trigger();
            show_status(tr("请先在流程属性中配置连续时钟或外部触发源。"));
        }
    });
    add_action(workspace_trigger_, WorkspaceIcon::Repeat, tr("连续 / 触发"));
    auto* stop = new QToolButton(toolbar);
    stop->setText(tr("停止"));
    stop->setIcon(workspace_icon(WorkspaceIcon::Stop));
    stop->setIconSize(toolbar->iconSize());
    stop->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    stop->setPopupMode(QToolButton::InstantPopup);
    auto* stop_menu = new QMenu(stop);
    stop_menu->addAction(cancel_run_);
    stop_menu->addAction(drain_trigger_);
    stop_menu->addAction(cancel_trigger_);
    stop->setMenu(stop_menu);
    toolbar->addWidget(stop);
    toolbar->addSeparator();
    add_action(props, WorkspaceIcon::Properties, tr("模块属性"));
    auto* details = run_dock_->toggleViewAction();
    add_action(details, WorkspaceIcon::Details, details->text());
}

void EditorWindow::create_docks()
{
    auto* columns = new QSplitter(Qt::Horizontal, this);
    columns->setObjectName(QStringLiteral("workspaceSplitter"));
    columns->setChildrenCollapsible(false);
    setCentralWidget(columns);

    auto* toolbox = new QWidget(columns);
    toolbox->setObjectName(QStringLiteral("nodeToolbox"));
    auto* toolbox_layout = new QVBoxLayout(toolbox);
    toolbox_layout->setContentsMargins(0, 0, 0, 0);
    toolbox_layout->setSpacing(0);
    auto* toolbox_title = new QLabel(tr("节点工具箱"), toolbox);
    toolbox_toggle_ = new QToolButton(toolbox);
    toolbox_toggle_->setObjectName(QStringLiteral("collapseToolbox"));
    toolbox_toggle_->setArrowType(Qt::LeftArrow);
    toolbox_toggle_->setToolTip(tr("收起节点工具箱"));
    toolbox_toggle_->setCheckable(true);
    toolbox_layout->addWidget(panel_header(toolbox_title, toolbox_toggle_));
    auto* toolbox_body = new QVBoxLayout;
    toolbox_body->setContentsMargins(6, 6, 6, 0);
    toolbox_body->setSpacing(5);
    toolbox_layout->addLayout(toolbox_body, 1);
    auto* search = new QLineEdit(toolbox);
    search->setObjectName(QStringLiteral("toolSearch"));
    search->setPlaceholderText(tr("搜索节点…"));
    search->setClearButtonEnabled(true);
    toolbox_body->addWidget(search);
    tools_ = new WorkspaceToolbox(registry_, toolbox);
    toolbox_body->addWidget(tools_, 1);
    connect(toolbox_toggle_, &QToolButton::toggled, this, [this, toolbox, toolbox_title, search, columns](bool collapsed) {
        auto sizes = columns->sizes();
        if (collapsed && sizes.size() == 3) { expanded_toolbox_width_ = sizes.front(); }
        toolbox_title->setVisible(!collapsed);
        tools_->setVisible(!collapsed);
        search->setVisible(!collapsed);
        toolbox_toggle_->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
        toolbox_toggle_->setToolTip(collapsed ? tr("展开节点工具箱") : tr("收起节点工具箱"));
        toolbox->setMaximumWidth(collapsed ? 38 : QWIDGETSIZE_MAX);
        if (sizes.size() == 3) {
            const int left = collapsed ? 38 : std::max(150, expanded_toolbox_width_);
            sizes[1] += sizes[0] - left;
            sizes[0] = left;
            columns->setSizes(sizes);
        }
    });
    connect(search, &QLineEdit::textChanged, tools_, &WorkspaceToolbox::filter);
    connect(tools_, &WorkspaceToolbox::selectionChanged, this, &EditorWindow::refresh_actions);

    auto* editor = new QWidget(columns);
    editor->setObjectName(QStringLiteral("flowWorkspace"));
    canvas_layout_ = new QVBoxLayout(editor);
    canvas_layout_->setContentsMargins(0, 0, 0, 0);
    canvas_layout_->setSpacing(0);
    auto* stats_bar = new QWidget(editor);
    auto* stats_layout = new QHBoxLayout(stats_bar);
    stats_layout->setContentsMargins(7, 3, 5, 3);
    workspace_stats_ = new QLabel(stats_bar);
    workspace_stats_->setObjectName(QStringLiteral("workspaceStats"));
    workspace_stats_->setTextFormat(Qt::PlainText);
    workspace_stats_->setFont(workspace_font(WorkspaceText::Secondary));
    workspace_stats_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stats_layout->addWidget(workspace_stats_, 1);
    diagnostics_button_ = new QToolButton(stats_bar);
    diagnostics_button_->setText(tr("诊断"));
    stats_layout->addWidget(diagnostics_button_);
    canvas_layout_->addWidget(stats_bar);

    auto* results = new QSplitter(Qt::Vertical, columns);
    results->setObjectName(QStringLiteral("resultsSplitter"));
    results->setChildrenCollapsible(false);
    images_dock_ = new QDockWidget(tr("输出图"), results);
    images_dock_->setObjectName(QStringLiteral("imagesDock"));
    images_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
    images_dock_->setTitleBarWidget(panel_header(new QLabel(images_dock_->windowTitle(), images_dock_)));
    images_ = new ImagePanel(images_dock_);
    images_->setObjectName(QStringLiteral("imagePanel"));
    images_dock_->setWidget(images_);
    auto* history = new QDockWidget(tr("模块历史结果"), results);
    history->setObjectName(QStringLiteral("moduleHistoryDock"));
    history->setFeatures(QDockWidget::NoDockWidgetFeatures);
    history->setTitleBarWidget(panel_header(new QLabel(history->windowTitle(), history)));
    module_history_ = new ModuleHistoryPanel(history);
    history->setWidget(module_history_);
    results->addWidget(images_dock_);
    results->addWidget(history);
    results->setSizes({435, 290});
    columns->addWidget(toolbox);
    columns->addWidget(editor);
    columns->addWidget(results);
    columns->setStretchFactor(1, 2);
    columns->setStretchFactor(2, 1);
    columns->setSizes({244, 676, 510});

    inspector_dock_ = new QDockWidget(tr("属性"), this);
    inspector_dock_->setObjectName(QStringLiteral("inspectorDock"));
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    inspector_dock_->setAllowedAreas(Qt::NoDockWidgetArea);
    inspector_dock_->setFloating(true);
    inspector_dock_->resize(455, 540);
    inspector_dock_->hide();
    connect(images_, &ImagePanel::frame_changed, this, [this] {
        if (inspector_) { inspector_->set_preview_image(images_->frame()); }
        refresh_actions();
    });

    diagnostics_dock_ = new QDockWidget(tr("诊断"), this);
    diagnostics_dock_->setObjectName(QStringLiteral("diagnosticsDock"));
    diagnostics_ = new QListWidget(diagnostics_dock_);
    diagnostics_->setObjectName(QStringLiteral("editorDiagnostics"));
    diagnostics_dock_->setWidget(diagnostics_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock_);
    diagnostics_dock_->setAllowedAreas(Qt::NoDockWidgetArea);
    diagnostics_dock_->setFloating(true);
    diagnostics_dock_->resize(620, 270);
    diagnostics_dock_->hide();
    connect(diagnostics_button_, &QToolButton::clicked, this, [this] {
        diagnostics_dock_->show();
        diagnostics_dock_->raise();
    });
    connect(diagnostics_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { locate_diagnostic(item); });
    connect(diagnostics_, &QListWidget::currentItemChanged, this, [this] { if (scene_) { refresh_actions(); } });
    create_run_dock();
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
    canvas_layout_->insertWidget(0, view_, 1);
    inspector_dock_->setWidget(inspector_);
    inspector_->setEnabled(editable);
    showing_run_image_ = false;
    *workspace_image_selection_ = {};
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
        const QPointer<ControlScene> active{scene_.get()};
        QTimer::singleShot(0, this, [this, active, node] {
            if (active && !active->open_group(node)) { show_properties(); }
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
    connect(&scene_->undoStack(), &QUndoStack::indexChanged, this, [this] { queue_diagnostics(); refresh_actions(); });
    connect(&scene_->undoStack(), &QUndoStack::cleanChanged, this, [this] { refresh_actions(); });
    issues_.clear();
    refresh_canvas();
    queue_diagnostics();
}

void EditorWindow::refresh_canvas()
{
    const QSignalBlocker blocker(workflows_);
    workflows_->clear();
    int selected = -1;
    for (const auto& flow : draft().workflows) {
        auto name = QString::fromStdString(flow.name);
        if (!flow.enabled) { name += tr("（已禁用）"); }
        workflows_->addItem(name);
        workflows_->setItemData(workflows_->count() - 1, name + u'\n' + QString::fromStdString(flow.id.to_string()), Qt::ToolTipRole);
        if (model_->workflow_id() == flow.id) { selected = workflows_->count() - 1; }
    }
    workflows_->setCurrentIndex(selected);
    workflows_->setToolTip(workflows_->currentText());
    scope_path_->setText(model_->scope() ? model_->scope_path() : QString{});
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

void EditorWindow::show_properties()
{
    inspector_dock_->show();
    inspector_dock_->raise();
    inspector_dock_->activateWindow();
}

void EditorWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    constexpr int compact_width = 1100;
    if (!toolbox_toggle_) { return; }
    const bool narrow = event->size().width() < compact_width;
    if (!event->oldSize().isValid() || narrow != (event->oldSize().width() < compact_width)) {
        toolbox_toggle_->setChecked(narrow);
    }
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
    const bool active = model_->workflow_id().has_value();
    const auto selected = scene_->selected_nodes();
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
    const bool can_submit = editable && flow && run_controller_->can_submit(*flow);
    run_workflow_->setEnabled(can_submit);
    run_group_->setEnabled(can_submit && group && !group->loop);
    debug_node_->setEnabled(can_submit && selected_node && !(group && group->loop));
    debug_node_->setText(group ? tr("调试所选工具组…") : tr("调试所选模块…"));
    const auto selected_run = cancel_target();
    cancel_run_->setText(selected_run ? tr("取消轮次 %1").arg(selected_run->identity().run.value()) : tr("取消所选轮次"));
    cancel_run_->setEnabled(!close_confirmed_ && selected_run && run_controller_->can_cancel(*selected_run));
    arm_trigger_->setEnabled(editable && flow && run_controller_->can_arm_trigger(*flow));
    workspace_trigger_->setEnabled(editable && flow && (!flow->trigger || arm_trigger_->isEnabled()));
    run_panel_->set_camera_status(flow ? run_controller_->camera_status(flow->id) : QString{});
    send_frame_->setEnabled(available && flow && run_controller_->can_send_frame(flow->id));
    const bool armed = available && flow && run_controller_->can_stop_trigger(flow->id);
    const auto flow_name = flow ? QString::fromStdString(flow->name) : tr("当前流程");
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
    remove_nodes_->setEnabled(editable && has_nodes && !scene_->selected_control());
    remove_link_->setEnabled(editable && scene_->selected_control().has_value());
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
    setWindowTitle(QStringLiteral("VisionPro · %1[*]").arg(QString::fromStdString(draft().name)));
    setWindowModified(!history.isClean());
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
    event->accept();
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
    const auto& location = diagnostic->location;
    if (location.workflow) { scene_->select_workflow(*location.workflow); }
    if (location.node && scene_->reveal_node(*location.node)) {
        if (const auto* graphic = scene_->nodeGraphicsObject(model_->canvas_id(*location.node))) { view_->centerOn(graphic); }
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

void EditorWindow::show_status(const QString& message) { statusBar()->showMessage(message, 8500); }

} // namespace visionpro::ui
