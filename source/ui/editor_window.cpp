#include "editor_window.hpp"

#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "image_panel.hpp"

#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <algorithm>
#include <exception>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

std::optional<std::string> ask_name(QWidget* owner, const QString& title, const QString& initial = {})
{
    bool accepted = false;
    const auto name = QInputDialog::getText(owner, title, QStringLiteral("Name"), QLineEdit::Normal, initial, &accepted).trimmed();
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
    delete takeCentralWidget();
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
    auto& file = *menuBar()->addMenu(QStringLiteral("&File"));
    new_ = action(file, QStringLiteral("&New Solution…"), "newSolution",
        [this] { confirm_replacement([this] { make_new_document(); }); }, QKeySequence::New);
    open_ = action(file, QStringLiteral("&Open Solution…"), "openSolution", [this] { choose_open(); }, QKeySequence::Open);
    save_ = action(file, QStringLiteral("&Save"), "saveSolution", [this] { save_document(); }, QKeySequence::Save);
    save_as_ = action(file, QStringLiteral("Save &As…"), "saveSolutionAs",
        [this] { choose_transfer(infrastructure::SolutionTransfer::SaveAs); }, QKeySequence::SaveAs);
    independent_copy_ = action(file, QStringLiteral("Save &Independent Copy…"), "saveIndependentCopy",
        [this] { choose_transfer(infrastructure::SolutionTransfer::IndependentCopy); });
    file.addSeparator();
    resource_copy_ = action(file, QStringLiteral("Copy Resource Into Solution…"), "copyResource", [this] { choose_resource_copy(); });
    resource_root_ = action(file, QStringLiteral("Set Named Resource Root…"), "setResourceRoot", [this] { choose_resource_root(); });
    file.addSeparator();
    action(file, QStringLiteral("&Exit"), "exitEditor", [this] { close(); }, QKeySequence::Quit);

    auto& edit = *menuBar()->addMenu(QStringLiteral("&Edit"));
    undo_ = action(edit, QStringLiteral("&Undo"), "undoDraft", [this] { scene_->undoStack().undo(); }, QKeySequence::Undo);
    redo_ = action(edit, QStringLiteral("&Redo"), "redoDraft", [this] { scene_->undoStack().redo(); }, QKeySequence::Redo);
    copy_ = action(edit, QStringLiteral("&Copy Nodes"), "copyNodes", [this] { scene_->copy_selected_nodes(); }, QKeySequence::Copy);
    paste_ = action(edit, QStringLiteral("&Paste Nodes"), "pasteNodes", [this] { scene_->paste_nodes(); }, QKeySequence::Paste);
    edit.addSeparator();
    rename_ = action(edit, QStringLiteral("Rename Solution…"), "renameSolution", [this] {
        const auto name = ask_name(this, QStringLiteral("Rename Solution"), QString::fromStdString(draft().name));
        if (!name || *name == draft().name) { return; }
        scene_->edit_draft(QStringLiteral("Rename solution"), [&](Solution& candidate) -> Result<std::monostate> {
            candidate.name = *name;
            return std::monostate{};
        });
    });

    auto& graph = *menuBar()->addMenu(QStringLiteral("&Workflow"));
    new_workflow_ = action(graph, QStringLiteral("Add Workflow…"), "addWorkflow", [this] {
        if (const auto name = ask_name(this, QStringLiteral("Add Workflow"), QStringLiteral("Workflow"))) {
            scene_->add_workflow(*name);
        }
    });
    remove_workflow_ = action(graph, QStringLiteral("Remove Workflow"), "removeWorkflow", [this] {
        if (const auto id = model_->workflow_id()) { scene_->remove_workflow(*id); }
    });
    add_tool_ = action(graph, QStringLiteral("Add Selected Tool"), "addTool", [this] {
        const auto row = tools_->currentRow();
        const auto descriptors = registry_.descriptors();
        if (row >= 0 && static_cast<std::size_t>(row) < descriptors.size()) {
            scene_->add_tool(descriptors[static_cast<std::size_t>(row)].id, view_->mapToScene(view_->viewport()->rect().center()));
        }
    });
    add_group_ = action(graph, QStringLiteral("Add Group…"), "addGroup", [this] {
        if (const auto name = ask_name(this, QStringLiteral("Add Group"), QStringLiteral("Group"))) {
            scene_->add_group(*name, view_->mapToScene(view_->viewport()->rect().center()));
        }
    });
    remove_nodes_ = action(graph, QStringLiteral("Remove Selected Nodes"), "removeNodes", [this] {
        scene_->remove_selected_nodes();
    }, QKeySequence::Delete);
    remove_link_ = action(graph, QStringLiteral("Remove Selected Link"), "removeLink", [this] {
        if (const auto link = scene_->selected_control()) { scene_->remove_control(*link); }
    }, QKeySequence::Delete);
    up_ = action(graph, QStringLiteral("Parent Group"), "parentGroup", [this] { scene_->parent_scope(); });

    auto& view = *menuBar()->addMenu(QStringLiteral("&View"));
    for (auto* dock : findChildren<QDockWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        view.addAction(dock->toggleViewAction());
    }
    view.addSeparator();
    preview_image_ = action(view, QStringLiteral("Open Image…"), "openPreviewImage", [this] { choose_preview_image(); });
    preview_resource_ = action(view, QStringLiteral("Open Image Resource…"), "openPreviewResource",
        [this] { choose_preview_resource(); });
    preview_roi_ = action(view, QStringLiteral("Preview Selected ROI"), "previewSelectedRoi",
        [this] { preview_selected_roi(); });
    view.addSeparator();
    remove_invalid_link_ = action(view, QStringLiteral("Remove Diagnosed Control Link"), "removeDiagnosedLink",
        [this] { remove_diagnosed_link(); });

    auto* toolbar = addToolBar(QStringLiteral("Workflow"));
    toolbar->setMovable(false);
    toolbar->addAction(add_tool_);
    toolbar->addAction(add_group_);
    toolbar->addSeparator();
    toolbar->addAction(up_);
    scope_path_ = new QLabel(toolbar);
    toolbar->addWidget(scope_path_);
    toolbar->addSeparator();
    toolbar->addAction(undo_);
    toolbar->addAction(redo_);
}

void EditorWindow::create_docks()
{
    auto* navigation = new QDockWidget(QStringLiteral("Workflows and Tools"), this);
    navigation->setObjectName(QStringLiteral("navigationDock"));
    auto* tabs = new QTabWidget(navigation);
    workflows_ = new QListWidget(tabs);
    workflows_->setObjectName(QStringLiteral("workflowList"));
    tools_ = new QListWidget(tabs);
    tools_->setObjectName(QStringLiteral("toolList"));
    tabs->addTab(workflows_, QStringLiteral("Workflows"));
    tabs->addTab(tools_, QStringLiteral("Tools"));
    for (const auto& tool : registry_.descriptors()) {
        auto* row = new QListWidgetItem(QString::fromStdString(tool.display_name), tools_);
        row->setToolTip(QString::fromStdString(std::string(tool.id.text())));
    }
    if (tools_->count()) { tools_->setCurrentRow(0); }
    navigation->setWidget(tabs);
    addDockWidget(Qt::LeftDockWidgetArea, navigation);
    connect(workflows_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (scene_ && row >= 0 && static_cast<std::size_t>(row) < draft().workflows.size()) {
            const auto id = draft().workflows[static_cast<std::size_t>(row)].id;
            scene_->select_workflow(id);
        }
    });
    connect(tools_, &QListWidget::currentRowChanged, this, [this] { if (scene_) { refresh_actions(); } });
    connect(tools_, &QListWidget::itemDoubleClicked, this, [this] { if (add_tool_) { add_tool_->trigger(); } });

    inspector_dock_ = new QDockWidget(QStringLiteral("Properties"), this);
    inspector_dock_->setObjectName(QStringLiteral("inspectorDock"));
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);

    images_dock_ = new QDockWidget(QStringLiteral("Images and ROI"), this);
    images_dock_->setObjectName(QStringLiteral("imagesDock"));
    images_ = new ImagePanel(images_dock_);
    images_->setObjectName(QStringLiteral("imagePanel"));
    images_dock_->setWidget(images_);
    addDockWidget(Qt::BottomDockWidgetArea, images_dock_);
    images_dock_->hide();
    connect(images_, &ImagePanel::frame_changed, this, [this] {
        if (inspector_) { inspector_->set_preview_image(images_->frame()); }
        refresh_actions();
    });

    diagnostics_dock_ = new QDockWidget(QStringLiteral("Diagnostics"), this);
    diagnostics_dock_->setObjectName(QStringLiteral("diagnosticsDock"));
    diagnostics_ = new QListWidget(diagnostics_dock_);
    diagnostics_->setObjectName(QStringLiteral("editorDiagnostics"));
    diagnostics_dock_->setWidget(diagnostics_);
    addDockWidget(Qt::BottomDockWidgetArea, diagnostics_dock_);
    diagnostics_dock_->hide();
    connect(diagnostics_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { locate_diagnostic(item); });
    connect(diagnostics_, &QListWidget::currentItemChanged, this, [this] { if (scene_) { refresh_actions(); } });
}

void EditorWindow::replace_canvas(Solution replacement, bool editable)
{
    const auto initial = replacement.workflows.empty() ? std::optional<WorkflowId>{} : replacement.workflows.front().id;
    auto model = std::make_unique<ControlGraphModel>(std::move(replacement), initial, registry_);
    auto scene = std::make_unique<ControlScene>(*model);
    scene->set_editing_enabled(editable);
    auto view = std::make_unique<QtNodes::GraphicsView>(scene.get());
    auto inspector = std::make_unique<EditorInspector>(*model, *scene, registry_);
    for (auto* registered : view->actions()) {
        if (registered != view->clearSelectionAction()) { view->removeAction(registered); }
    }
    view->setObjectName(QStringLiteral("workflowCanvas"));
    if (scene_) {
        disconnect(scene_.get(), nullptr, this, nullptr);
        disconnect(&scene_->undoStack(), nullptr, this, nullptr);
    }
    delete inspector_;
    delete takeCentralWidget();
    scene_.reset();
    model_.reset();
    model_ = std::move(model);
    scene_ = std::move(scene);
    view_ = view.release();
    inspector_ = inspector.release();
    setCentralWidget(view_);
    inspector_dock_->setWidget(inspector_);
    inspector_->setEnabled(editable);
    images_->clear_frame();
    shown_workflow_.reset();
    shown_scope_.reset();

    connect(scene_.get(), &ControlScene::edit_failed, this, [this](const QString& message) { report_error(message); });
    connect(scene_.get(), &ControlScene::edit_notice, this, [this](const QString& message) { show_status(message); });
    connect(scene_.get(), &QtNodes::BasicGraphicsScene::nodeDoubleClicked, this, [this](QtNodes::NodeId node) {
        const QPointer<ControlScene> active{scene_.get()};
        QTimer::singleShot(0, this, [active, node] { if (active) { active->open_group(node); } });
    });
    connect(model_.get(), &QtNodes::AbstractGraphModel::modelReset, this, [this] {
        images_->clear_overlays();
        refresh_canvas();
    });
    connect(scene_.get(), &QGraphicsScene::selectionChanged, this, [this] {
        images_->clear_overlays();
        refresh_actions();
    });
    connect(&scene_->undoStack(), &QUndoStack::indexChanged, this, [this] { queue_diagnostics(); refresh_actions(); });
    connect(&scene_->undoStack(), &QUndoStack::cleanChanged, this, [this] { refresh_actions(); });
    issues_.clear();
    refresh_canvas();
    queue_diagnostics();
    QTimer::singleShot(0, this, [this] { frame_canvas(); });
}

void EditorWindow::refresh_canvas()
{
    const QSignalBlocker blocker(workflows_);
    workflows_->clear();
    int selected = -1;
    for (const auto& flow : draft().workflows) {
        auto name = QString::fromStdString(flow.name);
        if (!flow.enabled) { name += QStringLiteral(" (disabled)"); }
        auto* item = new QListWidgetItem(name, workflows_);
        item->setToolTip(QString::fromStdString(flow.id.to_string()));
        if (model_->workflow_id() == flow.id) { selected = workflows_->count() - 1; }
    }
    workflows_->setCurrentRow(selected);
    scope_path_->setText(model_->scope_path());
    if (shown_workflow_ != model_->workflow_id() || shown_scope_ != model_->scope()) {
        shown_workflow_ = model_->workflow_id();
        shown_scope_ = model_->scope();
        frame_canvas();
    }
    refresh_actions();
}

void EditorWindow::frame_canvas()
{
    if (!view_) { return; }
    const auto bounds = scene_->itemsBoundingRect().adjusted(-25, -25, 25, 25);
    view_->resetTransform();
    if (bounds.width() > view_->viewport()->width() || bounds.height() > view_->viewport()->height()) {
        view_->fitInView(bounds, Qt::KeepAspectRatio);
    }
    view_->centerOn(bounds.center());
}

void EditorWindow::refresh_actions()
{
    if (!scene_) { return; }
    const bool editable = !busy_ && writable();
    const bool active = model_->workflow_id().has_value();
    const auto selected = scene_->selected_nodes();
    const bool has_nodes = !selected.empty();
    const auto& history = scene_->undoStack();
    const auto selection = diagnostics_->currentItem();
    const int issue = selection ? selection->data(Qt::UserRole).toInt() : -1;
    const bool has_resource = std::ranges::any_of(draft().resources, [](const ResourceDefinition& resource) {
        return std::holds_alternative<ExternalFileResource>(resource.location);
    });
    new_->setEnabled(!busy_);
    open_->setEnabled(!busy_);
    save_->setEnabled(editable && (!directory_ || !history.isClean() || !source_));
    save_as_->setEnabled(editable);
    independent_copy_->setEnabled(editable);
    resource_copy_->setEnabled(editable && source_.has_value() && has_resource);
    resource_root_->setEnabled(!busy_);
#ifdef VISIONPRO_WITH_OPENCV
    preview_image_->setEnabled(!busy_);
    preview_resource_->setEnabled(!busy_ && std::ranges::any_of(draft().resources,
        [](const ResourceDefinition& resource) { return resource.kind == ResourceKind::Image; }));
    preview_roi_->setEnabled(!busy_ && images_->frame().has_value() && selected.size() == 1);
#else
    preview_image_->setEnabled(false);
    preview_resource_->setEnabled(false);
    preview_roi_->setEnabled(false);
#endif
    rename_->setEnabled(editable);
    new_workflow_->setEnabled(editable);
    remove_workflow_->setEnabled(editable && active);
    add_tool_->setEnabled(editable && active && tools_->currentRow() >= 0);
    add_group_->setEnabled(editable && active);
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
    setWindowTitle(QStringLiteral("VisionPro - Workflow Canvas - %1[*]").arg(QString::fromStdString(draft().name)));
    setWindowModified(!history.isClean());
}

void EditorWindow::make_new_document()
{
    const auto name = ask_name(this, QStringLiteral("New Solution"), QStringLiteral("Untitled"));
    if (!name) { return; }
    const auto solution = IdFactory::generate<SolutionId>();
    const auto flow = IdFactory::generate<WorkflowId>();
    for (const auto* failure : {std::get_if<Diagnostic>(&solution), std::get_if<Diagnostic>(&flow)}) {
        if (failure) { report_error(QString::fromStdString(failure->message)); return; }
    }
    const auto schema = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("visionpro.editor.workflow"));
    const auto pixels = std::get<CoordinateSystemId>(SchemaIdFactory::parse<CoordinateSystemId>("visionpro.image.pixels"));
    Workflow initial{std::get<WorkflowId>(flow), "Workflow", {schema, 1, {}, {}, {}}, {}, {}, {}, {}, {}, true, {}};
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
    if (busy_) {
        close_when_idle_ = true;
        show_status(QStringLiteral("The document operation will finish before closing."));
        event->ignore();
        return;
    }
    if (scene_->undoStack().isClean()) {
        event->accept();
        return;
    }
    try {
        if (confirm_replacement([this] {
                // A save can finish after the current close event.
                if (!scene_->undoStack().isClean()) { return; }
                QTimer::singleShot(0, this, [this] { if (isVisible()) { close(); } });
            })) {
            event->accept();
        } else {
            event->ignore();
        }
    } catch (...) {
        event->ignore();
        report_error(QStringLiteral("Could not close the editor."));
    }
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
    if (!file_notices_.empty() || !issues_.empty()) { diagnostics_dock_->show(); }
    refresh_actions();
}

void EditorWindow::locate_diagnostic(QListWidgetItem* item)
{
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || static_cast<std::size_t>(index) >= issues_.size()) { return; }
    const auto location = issues_[static_cast<std::size_t>(index)].location;
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
