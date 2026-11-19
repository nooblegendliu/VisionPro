#include "editor_window.hpp"

#include "control_scene.hpp"
#include "image_panel.hpp"
#include "run_controller.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QMenuBar>
#include <QSpinBox>
#include <QTimer>

#include <exception>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

SchemaId trigger_id(bool independent)
{
    return std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>(
        independent ? "visionpro.editor.debug" : "visionpro.editor.run"));
}
} // namespace

void EditorWindow::create_run_actions()
{
    auto& menu = *menuBar()->addMenu(QStringLiteral("&Run"));
    run_workflow_ = action(menu, QStringLiteral("Run Workflow"), "runWorkflow", [this] { start_run(); },
        QKeySequence(Qt::Key_F5));
    run_group_ = action(menu, QStringLiteral("Run Selected Group…"), "runSelectedGroup",
        [this] { start_run(DebugStatePolicy::Active); });
    debug_node_ = action(menu, QStringLiteral("Debug Selected Node…"), "debugSelectedNode", [this] { start_run(DebugStatePolicy::Isolated); },
        QKeySequence(Qt::CTRL | Qt::Key_F5));
    cancel_run_ = action(menu, QStringLiteral("Cancel Run"), "cancelRun", [this] { run_controller_->cancel(); },
        QKeySequence(Qt::SHIFT | Qt::Key_F5));
    reset_state_ = action(menu, QStringLiteral("Reset Selected Node State"), "resetSelectedState", [this] {
        const auto flow = model_->workflow_id();
        const auto selected = scene_->selected_nodes();
        if (!flow || selected.size() != 1) { return; }
        auto result = run_controller_->reset_state({*flow, selected.front()});
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            report_error(QString::fromStdString(failure->message));
            return;
        }
        const auto& workflow = *find_workflow(draft(), *flow);
        show_status(QStringLiteral("Reset session state for %1.").arg(QString::fromStdString(find_node(workflow, selected.front())->name)));
    });
    menu.addSeparator();
    run_limits_ = action(menu, QStringLiteral("History and Notifications…"), "runDisplayLimits", [this] { choose_run_limits(); });
}

void EditorWindow::create_run_dock()
{
    run_controller_ = new RunController(registry_, this);
    run_dock_ = new QDockWidget(QStringLiteral("Run and Results"), this);
    run_dock_->setObjectName(QStringLiteral("runDock"));
    run_panel_ = new RunPanel(run_dock_);
    run_dock_->setWidget(run_panel_);
    addDockWidget(Qt::RightDockWidgetArea, run_dock_);
    splitDockWidget(inspector_dock_, run_dock_, Qt::Vertical);
    run_dock_->hide();
    connect(run_dock_, &QDockWidget::visibilityChanged, run_controller_, [this](bool visible) {
        run_controller_->set_observing(visible && !close_confirmed_);
        if (visible) { run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped()); }
    });
    connect(run_controller_, &RunController::changed, this, [this] {
        refresh_actions();
        if (run_dock_->isVisible()) { run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped()); }
        if (close_confirmed_ && !run_controller_->has_session_work()) {
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    connect(run_controller_, &RunController::operation_failed, this, [this](const Diagnostic& diagnostic) {
        run_error_ = diagnostic;
        render_diagnostics();
        show_status(QString::fromStdString(diagnostic.message));
    });
    connect(run_panel_, &RunPanel::image_requested, this, &EditorWindow::show_selected_run_image);
}

void EditorWindow::start_run(std::optional<DebugStatePolicy> debug)
{
    if (busy_ || close_confirmed_ || !writable() || !run_controller_->can_start()) { return; }
    const auto id = model_->workflow_id();
    const auto* flow = id ? find_workflow(draft(), *id) : nullptr;
    if (!flow) { return; }
    RunTarget target = WorkflowTarget{flow->id};
    std::span<const InputDescriptor> inputs = flow->interface.inputs;
    std::span<const PixelFormat> formats;
    const bool independent = debug.has_value();
    const bool isolated = debug == DebugStatePolicy::Isolated;
    bool group_target = false;
    if (independent) {
        const auto selected = scene_->selected_nodes();
        const auto* node = selected.size() == 1 ? find_node(*flow, selected.front()) : nullptr;
        if (!node) { return; }
        if (const auto* group = std::get_if<GroupConfiguration>(&node->configuration)) {
            if (group->loop) { return; }
            inputs = group->interface.inputs;
            group_target = true;
        } else {
            const auto& tool = std::get<ToolConfiguration>(node->configuration);
            const auto* descriptor = registry_.find_descriptor(tool.type);
            if (!descriptor || descriptor->config_version != tool.config_version) {
                report_error(QStringLiteral("The selected node requires an available tool and its exact configuration version."));
                return;
            }
            inputs = descriptor->inputs;
            formats = descriptor->image_formats;
        }
        target = NodeTarget{flow->id, node->id};
    }
    auto entered = inputs.empty() ? std::optional{EnteredRunInputs{}}
        : choose_run_inputs(this, draft(), inputs, isolated, formats, group_target);
    if (!entered) { return; }
    if (run_error_) {
        run_error_.reset();
        render_diagnostics();
    }
    RunArguments arguments{target, std::move(entered->values),
        {independent ? TriggerKind::Debug : TriggerKind::Manual, trigger_id(independent)},
        {}, debug};
    run_dock_->show();
    run_dock_->raise();
    show_status(isolated ? QStringLiteral("Preparing isolated debug call…")
        : group_target ? QStringLiteral("Preparing Group run…") : QStringLiteral("Preparing workflow run…"));
    run_controller_->start(draft(), {source_, directory_, roots_}, std::move(arguments), std::move(entered->images));
}

void EditorWindow::show_selected_run_image()
{
    if (busy_ || close_confirmed_) { return; }
    try {
        const auto run = run_panel_->selected_run();
        const auto invocation = run_panel_->selected_invocation();
        if (!run || !invocation) { return; }
        const auto displayed = show_run_image(*images_, *run, *invocation, run_panel_->selected_output());
        if (const auto* failure = std::get_if<Diagnostic>(&displayed)) {
            report_error(QString::fromStdString(failure->message));
            return;
        }
        const auto* flow = find_workflow(run->request().snapshot().solution(), run->identity().workflow);
        const auto* node = flow ? find_node(*flow, invocation->node) : nullptr;
        showing_run_image_ = true;
        auto provenance = QStringLiteral("Run %1  ·  Snapshot %2  ·  %3")
            .arg(run->identity().run.value()).arg(run->identity().snapshot.snapshot.value())
            .arg(QString::fromStdString(node ? node->name : invocation->node.to_string()));
        const auto scope = run_panel_->selected_scope_path();
        if (!scope.isEmpty()) {
            provenance += QStringLiteral("  ·  %1  ·  Invocation %2").arg(scope).arg(invocation->invocation.value());
        }
        images_->set_provenance(provenance);
        images_dock_->show();
        images_dock_->raise();
    } catch (const std::exception& failure) {
        report_error(QString::fromUtf8(failure.what()));
    } catch (...) {
        report_error(QStringLiteral("Could not display the selected run image."));
    }
}

void EditorWindow::choose_run_limits()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("History and Notifications"));
    auto* layout = new QFormLayout(&dialog);
    auto* history = new QSpinBox(&dialog);
    history->setObjectName(QStringLiteral("runHistoryLimit"));
    history->setRange(0, 256);
    history->setValue(static_cast<int>(run_controller_->history_limit()));
    layout->addRow(QStringLiteral("Completed runs"), history);
    auto* capacity = new QSpinBox(&dialog);
    capacity->setObjectName(QStringLiteral("runNotificationCapacity"));
    capacity->setRange(1, 512);
    capacity->setValue(static_cast<int>(run_controller_->observation_capacity()));
    layout->addRow(QStringLiteral("Buffered notifications"), capacity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        run_controller_->set_limits(static_cast<std::size_t>(history->value()), static_cast<std::size_t>(capacity->value()));
    }
}

} // namespace visionpro::ui
