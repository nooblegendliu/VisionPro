#include "editor_window.hpp"

#include "control_scene.hpp"
#include "image_panel.hpp"
#include "run_controller.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/correlation_join.hpp>
#include <visionpro/core/shared_data.hpp>

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QInputDialog>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

QString shared_report(const std::optional<SessionSharedPreview>& live, const std::vector<GlobalVariableDefinition>& variables,
    const std::vector<DataQueueDefinition>& queues)
{
    QStringList lines{live ? QStringLiteral("Active session · snapshot %1").arg(live->snapshot.identity().snapshot.value())
                           : QStringLiteral("Draft definitions · initial values and empty queues")};
    lines.append(QStringLiteral("\nGlobal variables (%1)").arg(variables.size()));
    std::vector<GlobalVariableEntry> entries;
    if (live) {
        auto read = live->data->read_all();
        if (const auto* issue = std::get_if<Diagnostic>(&read)) { return QString::fromStdString(issue->message); }
        entries = std::get<std::vector<GlobalVariableEntry>>(std::move(read));
    }
    for (const auto& variable : variables) {
        const auto entry = std::ranges::find(entries, variable.id, &GlobalVariableEntry::variable);
        const auto value = entry != entries.end() ? shared_value_text(entry->current_value)
            : live ? QStringLiteral("Unavailable") : shared_value_text(variable.initial_value);
        auto revision = entry != entries.end() ? QString::number(entry->revision) : live ? QStringLiteral("—") : QStringLiteral("0");
        lines.append(QStringLiteral("%1 [%2] · revision %3 · %4").arg(QString::fromStdString(variable.name),
            QString::fromStdString(variable.id.to_string()), revision, value));
    }
    lines.append(QStringLiteral("\nQueues (%1)").arg(queues.size()));
    for (const auto& queue : queues) {
        QueuePreview preview{queue.id};
        if (live) {
            auto read = live->data->preview(queue.id);
            if (const auto* issue = std::get_if<Diagnostic>(&read)) { return QString::fromStdString(issue->message); }
            preview = std::get<QueuePreview>(std::move(read));
        }
        lines.append(QStringLiteral("%1 [%2] · messages %3 · enqueued %4 · received %5 · dropped %6 · rejected %7")
            .arg(QString::fromStdString(queue.name), QString::fromStdString(queue.id.to_string())).arg(preview.messages.size())
            .arg(preview.enqueued).arg(preview.received).arg(preview.dropped_oldest).arg(preview.rejected));
        constexpr std::size_t limit = 100;
        for (std::size_t i = 0; i < std::min(limit, preview.messages.size()); ++i) {
            const auto& message = preview.messages[i];
            lines.append(QStringLiteral("  #%1 · %2 / %3 · %4").arg(message.sequence)
                .arg(QString::fromUtf8(message.correlation.domain().text()), QString::fromUtf8(message.correlation.key()), shared_value_text(message.payload)));
        }
        if (preview.messages.size() > limit) { lines.append(QStringLiteral("  Showing the oldest %1 messages.").arg(limit)); }
    }
    lines.append(QStringLiteral("\nCorrelation joins"));
    if (live) {
        const auto joins = live->joins->inspect_all();
        if (joins.empty()) { lines.append(QStringLiteral("No contributions in this session.")); }
        for (const auto& join : joins) {
            const auto& stats = join.stats;
            lines.append(QStringLiteral("%1 · batch %2 · pending %3 · completed %4 · expired %5 · duplicates %6 · late %7")
                .arg(QString::fromUtf8(join.join.text())).arg(stats.batch).arg(stats.pending).arg(stats.completed)
                .arg(stats.expired).arg(stats.duplicates).arg(stats.late));
            lines.append(QStringLiteral("  Retained keys %1 · abandoned at rollover %2 · pending capacity rejections %3 · batch key rejections %4")
                .arg(stats.retained_keys).arg(stats.abandoned_at_rollover).arg(stats.pending_capacity_rejections).arg(stats.batch_capacity_rejections));
        }
        lines.append(QStringLiteral("Session join capacity rejections %1").arg(live->joins->join_capacity_rejections()));
    } else { lines.append(QStringLiteral("Statistics are available after a session accepts contributions.")); }
    return lines.join(u'\n');
}

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
    cancel_run_ = action(menu, QStringLiteral("Cancel Selected Run"), "cancelRun", [this] {
        if (const auto run = run_panel_->selected_run()) { run_controller_->cancel(*run); }
    }, QKeySequence(Qt::SHIFT | Qt::Key_F5));
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
    arm_trigger_ = action(menu, QStringLiteral("Arm Workflow Trigger"), "armWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) {
            run_dock_->show();
            run_dock_->raise();
            run_controller_->arm_trigger(draft(), {source_, directory_, roots_}, *id);
        }
    });
    send_frame_ = action(menu, QStringLiteral("Send External Frame…"), "sendExternalFrame", [this] {
        const auto id = model_->workflow_id();
        if (!id || !run_controller_->can_send_frame(*id)) { return; }
        bool accepted{};
        const auto entered = QInputDialog::getText(this, QStringLiteral("External frame"), QStringLiteral("Frame number"),
            QLineEdit::Normal, {}, &accepted);
        if (!accepted) { return; }
        bool valid{};
        const auto frame = entered.toLongLong(&valid);
        if (!valid) { report_error(QStringLiteral("Enter a 64-bit integer frame number.")); return; }
        run_controller_->send_frame(*id, frame);
    });
    drain_trigger_ = action(menu, QStringLiteral("Stop Trigger and Drain"), "drainWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) { run_controller_->stop_trigger(*id, TriggerStopMode::Drain); }
    });
    cancel_trigger_ = action(menu, QStringLiteral("Stop Trigger and Cancel Runs"), "cancelWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) { run_controller_->stop_trigger(*id, TriggerStopMode::Cancel); }
    });
    menu.addSeparator();
    action(menu, QStringLiteral("Preview Shared Data…"), "previewSharedData", [this] { show_shared_preview(); });
    run_limits_ = action(menu, QStringLiteral("History and Notifications…"), "runDisplayLimits", [this] { choose_run_limits(); });
}

void EditorWindow::show_shared_preview()
{
    if (auto* shown = findChild<QDialog*>(QStringLiteral("sharedDataDialog")); shown && shown->isVisible()) {
        shown->raise();
        shown->activateWindow();
        return;
    }
    auto* dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("sharedDataDialog"));
    dialog->setWindowTitle(QStringLiteral("Shared Data"));
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(dialog);
    auto* details = new QPlainTextEdit(dialog);
    details->setObjectName(QStringLiteral("sharedDataPreview"));
    details->setReadOnly(true);
    layout->addWidget(details);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* refresh = buttons->addButton(QStringLiteral("Refresh"), QDialogButtonBox::ActionRole);
    refresh->setObjectName(QStringLiteral("refreshSharedPreview"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto load = [this, dialog, details, refresh] {
        auto view = run_controller_->shared_preview();
        if (const auto* issue = std::get_if<Diagnostic>(&view)) { details->setPlainText(QString::fromStdString(issue->message)); return; }
        const auto live = std::get<std::optional<SessionSharedPreview>>(std::move(view));
        const auto& configuration = live ? live->snapshot.solution() : draft();
        auto report = std::make_shared<QString>();
        refresh->setEnabled(false);
        details->setPlainText(QStringLiteral("Reading shared data…"));
        auto* worker = QThread::create([report, live, variables = configuration.variables, queues = configuration.queues] {
            try { *report = shared_report(live, variables, queues); }
            catch (const std::exception& error) { *report = QString::fromUtf8(error.what()); }
            catch (...) { *report = QStringLiteral("Could not read shared data."); }
        });
        connect(worker, &QThread::finished, dialog, [report, details, refresh] {
            details->setPlainText(*report);
            refresh->setEnabled(true);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    };
    connect(refresh, &QPushButton::clicked, dialog, load);
    dialog->resize(680, 420);
    dialog->show();
    load();
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
        if (visible) {
            run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped());
            refresh_actions();
        }
    });
    connect(run_controller_, &RunController::changed, this, [this] {
        if (run_dock_->isVisible()) { run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped()); }
        refresh_actions();
        if (close_confirmed_ && !run_controller_->has_session_work()) {
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    connect(run_controller_, &RunController::operation_failed, this, [this](const Diagnostic& diagnostic) {
        run_error_ = diagnostic;
        render_diagnostics();
        show_status(QString::fromStdString(diagnostic.message));
    });
    connect(run_controller_, &RunController::trigger_notice, this, &EditorWindow::show_status);
    connect(run_panel_, &RunPanel::image_requested, this, &EditorWindow::show_selected_run_image);
    connect(run_panel_, &RunPanel::selected_run_changed, this, &EditorWindow::refresh_actions);
}

void EditorWindow::start_run(std::optional<DebugStatePolicy> debug)
{
    if (busy_ || close_confirmed_ || !writable()) { return; }
    const auto id = model_->workflow_id();
    const auto* flow = id ? find_workflow(draft(), *id) : nullptr;
    if (!flow || !run_controller_->can_submit(*flow)) { return; }
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
