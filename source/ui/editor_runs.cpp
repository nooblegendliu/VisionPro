#include "editor_window.hpp"

#include "control_scene.hpp"
#include "image_panel.hpp"
#include "image_source_strip.hpp"
#include "image_source_fields.hpp"
#include "module_history_panel.hpp"
#include "workspace_results.hpp"
#include "run_controller.hpp"
#include "run_image.hpp"
#include "run_panel.hpp"
#include "solution_run_actions.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/correlation_join.hpp>
#include <visionpro/core/shared_data.hpp>

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabBar>
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

std::optional<EnteredRunInputs> choose_delivered_frame(QWidget* parent, std::span<const RunHandle> runs,
    const Workflow& workflow, InputId port)
{
    QStringList labels;
    std::vector<DataValue> frames;
    const auto add = [&](const RunHandle& run, const DataValue& value, const QString& origin) {
        const auto* frame = value.as_image();
        if (!frame || !frame->frame) { return; }
        if (workflow.trigger && workflow.trigger->camera && workflow.trigger->camera->device != frame->frame->source) { return; }
        labels.append(QObject::tr("运行 #%1 · %2 · 设备 %3 · 帧 %4 · %5 × %6")
            .arg(run.identity().run.value()).arg(origin, QString::fromStdString(frame->frame->source.to_string()))
            .arg(frame->frame->sequence).arg(frame->image.width()).arg(frame->image.height()));
        frames.push_back(value);
    };
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        for (const auto& input : it->request().inputs()) { add(*it, input.value, QObject::tr("流程输入")); }
        auto inspected = it->inspect();
        const auto* progress = std::get_if<RunProgress>(&inspected);
        if (!progress) { continue; }
        const auto* flow = find_workflow(it->request().snapshot().solution(), it->identity().workflow);
        if (!flow) { continue; }
        for (const auto& call : progress->nodes) {
            if (!call.invocation || call.status != ExecutionStatus::Succeeded) { continue; }
            const auto* node = find_node(*flow, call.node);
            if (!is_image_source(node) || source_enum(*node, "source_type", "Local") != "Camera") { continue; }
            auto record = it->lookup(*call.invocation);
            const auto* result = std::get_if<std::optional<NodeResult>>(&record);
            const auto* slot = result && *result ? (*result)->output(source_key<OutputId>("image")) : nullptr;
            if (const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr) {
                add(*it, *value, QString::fromStdString(node->name));
            }
        }
    }
    if (frames.empty()) {
        QMessageBox::information(parent, QObject::tr("选择已交付帧"),
            QObject::tr("历史中还没有带设备身份的图像。请先运行一次相机采集，或启动相机触发源取得调试帧。"));
        return {};
    }
    QInputDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("deliveredDebugFrame"));
    dialog.setWindowTitle(QObject::tr("选择已交付帧"));
    dialog.setLabelText(QObject::tr("使用本次选定的历史帧独立执行"));
    dialog.setComboBoxItems(labels);
    if (dialog.exec() != QDialog::Accepted) { return {}; }
    const auto selected = dialog.findChild<QComboBox*>()->currentIndex();
    return EnteredRunInputs{{{port, frames.at(selected)}}, {}};
}
} // namespace

void EditorWindow::create_run_actions()
{
    auto& menu = *menuBar()->addMenu(QStringLiteral("运行(&R)"));
    solution_actions_ = add_solution_run_actions(menu, *run_controller_, [this] {
        return model_ && !busy_ && !close_confirmed_ && writable() ? &draft() : nullptr;
    }, [this] { return RunDocument{source_, directory_, roots_, workspace_image_selection_->edit_generation}; });
    run_workflow_ = action(menu, QStringLiteral("运行当前流程"), "runWorkflow", [this] { start_run(); },
        QKeySequence(Qt::Key_F5));
    cancel_workflow_ = action(menu, tr("取消本次流程运行"), "cancelWorkflowRun", [this] {
        const auto id = model_->workflow_id();
        if (!id) { return; }
        const auto targets = run_controller_->cancellable_workflow_runs(*id);
        if (targets.empty()) { return; }
        if (targets.size() == 1) { run_controller_->cancel(targets.front()); return; }
        QStringList rounds;
        for (const auto& run : targets) { rounds << tr("第 %1 轮").arg(run.identity().run.value()); }
        bool accepted{};
        const auto choice = QInputDialog::getItem(this, tr("取消流程运行"), tr("选择要取消的轮次"),
            rounds, 0, false, &accepted);
        if (accepted) { run_controller_->cancel(targets.at(static_cast<std::size_t>(rounds.indexOf(choice)))); }
    });
    run_group_ = action(menu, QStringLiteral("运行所选 Group…"), "runSelectedGroup",
        [this] { start_run(DebugStatePolicy::Active); });
    debug_node_ = action(menu, QStringLiteral("调试所选模块…"), "debugSelectedNode", [this] { start_run(DebugStatePolicy::Isolated); },
        QKeySequence(Qt::CTRL | Qt::Key_F5));
    cancel_run_ = action(menu, tr("取消工作台所示轮次"), "cancelRun", [this] {
        if (const auto run = workspace_cancel_target()) { run_controller_->cancel(*run); }
    }, QKeySequence(Qt::SHIFT | Qt::Key_F5));
    run_dock_->addAction(cancel_run_);
    reset_state_ = action(menu, QStringLiteral("重置所选模块状态"), "resetSelectedState", [this] {
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
    arm_trigger_ = action(menu, QStringLiteral("启动当前流程触发源"), "armWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) {
            run_controller_->arm_trigger(draft(), {source_, directory_, roots_, workspace_image_selection_->edit_generation}, *id);
        }
    });
    send_frame_ = action(menu, QStringLiteral("发送外部帧…"), "sendExternalFrame", [this] {
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
    drain_trigger_ = action(menu, QStringLiteral("停止触发并等待在途运行"), "drainWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) { run_controller_->stop_trigger(*id, TriggerStopMode::Drain); }
    });
    cancel_trigger_ = action(menu, QStringLiteral("停止触发并取消在途运行"), "cancelWorkflowTrigger", [this] {
        if (const auto id = model_->workflow_id()) { run_controller_->stop_trigger(*id, TriggerStopMode::Cancel); }
    });
    menu.addSeparator();
    action(menu, QStringLiteral("查看共享数据…"), "previewSharedData", [this] { show_shared_preview(); });
    run_limits_ = action(menu, QStringLiteral("历史与通知…"), "runDisplayLimits", [this] { choose_run_limits(); });
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
    workspace_image_selection_ = std::make_unique<WorkspaceImageSelection>();
    connect(run_controller_, &RunController::snapshot_published, this, [this](const SnapshotIdentity& id, std::uint64_t generation) {
        workspace_image_selection_->capture_generations[id] = generation;
        workspace_image_selection_->last_publication = id;
    });
    run_dock_ = new QDockWidget(QStringLiteral("运行详情"), this);
    run_dock_->setObjectName(QStringLiteral("runDock"));
    run_panel_ = new RunPanel(run_dock_);
    run_dock_->setWidget(run_panel_);
    addDockWidget(Qt::RightDockWidgetArea, run_dock_);
    run_dock_->setFloating(true);
    run_dock_->resize(780, 480);
    run_dock_->hide();
    run_controller_->set_observing(true);
    connect(run_dock_, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (!model_) { return; }
        if (visible) {
            run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped());
        } else { run_panel_->suspend(); }
        refresh_actions();
    });
    connect(run_controller_, &RunController::changed, this, [this] {
        if (run_dock_->isVisible()) { run_panel_->update_runs(run_controller_->runs(), run_controller_->dropped()); }
        refresh_workspace_results();
        refresh_actions();
        if (close_confirmed_ && !run_controller_->has_session_work()) {
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    connect(run_controller_, &RunController::operation_failed, this, [this](const Diagnostic& diagnostic) {
        if (close_confirmed_) { close_confirmed_ = false; }
        run_error_ = diagnostic;
        render_diagnostics();
        show_status(QString::fromStdString(diagnostic.message));
    });
    connect(run_controller_, &RunController::trigger_notice, this, &EditorWindow::show_status);
    connect(run_controller_, &RunController::run_finished, this, [this](const RunHandle& run, ExecutionStatus status) {
        const auto& id = run.identity();
        const auto* flow = find_workflow(run.request().snapshot().solution(), id.workflow);
        auto name = QString::fromStdString(flow ? flow->name : id.workflow.to_string());
        if (const auto* target = std::get_if<NodeTarget>(&run.request().target())) {
            const auto* node = flow ? find_node(*flow, target->node) : nullptr;
            name += QStringLiteral(" / ") + QString::fromStdString(node ? node->name : target->node.to_string());
        }
        record_event(tr("%1 · 第 %2 轮 · %3").arg(name).arg(id.run.value()).arg(execution_status_text(status)));
    });
    connect(run_panel_, &RunPanel::image_requested, this, &EditorWindow::show_selected_run_image);
    connect(run_panel_, &RunPanel::cancel_requested, this, [this] {
        if (const auto run = run_panel_->selected_run()) { run_controller_->cancel(*run); }
    });
    connect(run_panel_, &RunPanel::selected_run_changed, this, &EditorWindow::refresh_actions);
    connect(module_history_, &ModuleHistoryPanel::selection_changed, this, [this] {
        refresh_workspace_statistics();
        present_workspace_image();
        refresh_actions();
    });
    const auto follow = [this] {
        if (workspace_image_selection_->mode == WorkspaceImageSelection::Mode::Follow) { return; }
        workspace_image_selection_->mode = WorkspaceImageSelection::Mode::Follow;
        workspace_image_selection_->rendered_index = -2;
        images_->set_manual_view(false);
        present_workspace_image();
    };
    connect(module_history_, &ModuleHistoryPanel::browse_requested, this, follow);
    connect(images_, &ImagePanel::follow_requested, this, follow);
    connect(images_, &ImagePanel::source_changed, this, &EditorWindow::present_workspace_image);
}

std::optional<RunHandle> EditorWindow::workspace_cancel_target() const
{
    const auto run = module_history_->selected_run();
    return run && run->identity().workflow == model_->workflow_id() ? run : std::nullopt;
}

void EditorWindow::refresh_workspace_selection()
{
    if (!scene_ || !module_history_) { return; }
    auto& display = *workspace_image_selection_;
    const auto flow = model_->workflow_id();
    const auto selected = scene_->selected_nodes();
    auto node = selected.size() == 1 ? std::optional{selected.front()} : std::nullopt;
    const auto* current_flow = flow ? find_workflow(draft(), *flow) : nullptr;
    const Node* current_node = current_flow && node ? find_node(*current_flow, *node) : nullptr;
    image_source_strip_->set_selection(model_.get(), scene_.get(),
        flow && node ? std::optional{NodeTarget{*flow, *node}} : std::nullopt,
        directory_, roots_, writable() && !busy_);
    const auto pinned = module_history_->selected_run();
    if (selected.empty() && flow == display.workflow && current_flow && display.node
        && !find_node(*current_flow, *display.node) && !module_history_->following_latest() && pinned) {
        const auto* snapshot_flow = find_workflow(pinned->request().snapshot().solution(), *flow);
        current_node = snapshot_flow ? find_node(*snapshot_flow, *display.node) : nullptr;
        if (current_node) { node = current_node->id; }
    }
    if ((flow != display.workflow && display.mode != WorkspaceImageSelection::Mode::DraftPreview)
        || (node != display.node && display.mode == WorkspaceImageSelection::Mode::RunDetails)) {
        display.mode = WorkspaceImageSelection::Mode::Follow;
        display.rendered_index = -2;
        images_->set_manual_view(false);
    }
    display.workflow = flow;
    display.node = node;
    module_history_->set_selection(current_flow, current_node);
    refresh_workspace_statistics();
    present_workspace_image();
}

void EditorWindow::refresh_workspace_results()
{
    if (!model_ || !module_history_ || close_confirmed_) { return; }
    if (image_source_strip_) { image_source_strip_->refresh_bound_folder(); }
    module_history_->update_runs(run_controller_->runs());
    const auto selected = module_history_->selected_run();
    auto& display = *workspace_image_selection_;
    std::erase_if(display.capture_generations, [&](const auto& entry) {
        return entry.first != display.last_publication && !(selected && entry.first == selected->identity().snapshot)
            && std::ranges::none_of(run_controller_->runs(), [&](const RunHandle& run) { return run.identity().snapshot == entry.first; });
    });
    refresh_workspace_statistics();
}

void EditorWindow::refresh_workspace_statistics()
{
    if (scene_) {
        scene_->display_run(module_history_->selected_run(), module_history_->selected_scope(), module_history_->selected_invocation());
    }
    workspace_stats_->setText(module_history_->statistics_text());
    workspace_stats_->setToolTip(module_history_->statistics_details());
    workspace_stats_->setAccessibleDescription(module_history_->statistics_details());
    if (!model_ || !workspace_image_selection_) { return; }
    const auto& display = *workspace_image_selection_;
    const auto* flow = display.workflow ? find_workflow(draft(), *display.workflow) : nullptr;
    const auto run = module_history_->selected_run();
    const auto* result_flow = run ? find_workflow(run->request().snapshot().solution(), run->identity().workflow) : flow;
    const auto* node = result_flow && display.node ? find_node(*result_flow, *display.node) : nullptr;
    QStringList source;
    if (run) {
        const auto generation = display.capture_generations.find(run->identity().snapshot);
        if (generation == display.capture_generations.end() || generation->second != display.edit_generation) {
            source << tr("结果来自此前配置");
        }
        source << tr("流程第 %1 轮").arg(run->identity().run.value());
        const auto& report = run_controller_->solution_report();
        source << (report && std::ranges::find(report->runs, run->identity(), &RunHandle::identity) != report->runs.end()
            ? tr("方案第 %1 轮").arg(report->sequence) : tr("无可用的所属方案报告"));
    }
    results_->update(draft(), flow, node, registry_, display.edit_generation, source.join(QStringLiteral(" · ")), writable() && !busy_);
}

void EditorWindow::present_workspace_image()
{
    if (close_confirmed_ || !workspace_image_selection_ || !module_history_) { return; }
    auto& display = *workspace_image_selection_;
    if (display.mode != WorkspaceImageSelection::Mode::Follow) { return; }
    const auto empty = [this](const QString& message, bool retain_view = false) {
        showing_run_image_ = false;
        if (images_->frame()) { images_->clear_frame(retain_view); }
        else if (!retain_view) { images_->reset_view_source(); }
        images_->set_provenance(message);
    };
    try {
        const auto run = module_history_->selected_run();
        const auto call = module_history_->selected_invocation();
        std::optional<NodeResult> selected;
        if (run && call) {
            const auto result = run->lookup(*call);
            if (const auto* record = std::get_if<std::optional<NodeResult>>(&result)) { selected = *record; }
        }
        const auto no_result = [&](const QString& message) {
            if (display.consumer && !display.waiting_for_result) { images_->set_sources({}, -1); }
            display.waiting_for_result = display.consumer && run && display.node == display.consumer->invocation.node
                && run->identity().workflow == display.consumer->run.workflow
                && run->identity().snapshot.session == display.consumer->run.snapshot.session;
            if (!display.waiting_for_result) {
                display.consumer.reset();
                display.sources.clear();
                display.rendered_index = -2;
            }
            empty(message, display.waiting_for_result);
        };
        if (!selected) {
            no_result(module_history_->selection_message());
            return;
        }
        if (display.waiting_for_result || display.consumer != selected->data().identity) {
            const auto& consumer = selected->data().identity;
            auto sources = workspace_image_sources(*run, *selected);
            if (sources.empty()) {
                no_result(QStringLiteral("该调用没有可显示的图像"));
                return;
            }
            const bool choose = sources.size() > 1 && sources.front().upstream;
            int index = choose ? -1 : 0;
            const int previous = display.rendered_index;
            const bool same_consumer = display.consumer && display.consumer->invocation.node == consumer.invocation.node
                && display.consumer->run.workflow == consumer.run.workflow && display.consumer->run.snapshot.session == consumer.run.snapshot.session;
            if (same_consumer && previous >= 0 && static_cast<std::size_t>(previous) < display.sources.size()) {
                const auto& prior = display.sources[static_cast<std::size_t>(previous)];
                const auto same_channel = [&](const WorkspaceImageSource& candidate) {
                    return prior.result.invocation.node == candidate.result.invocation.node && prior.output == candidate.output;
                };
                // Several loop invocations may offer the same port. Do not guess which call corresponds.
                if (std::ranges::count_if(display.sources, same_channel) == 1 && std::ranges::count_if(sources, same_channel) == 1) {
                    index = static_cast<int>(std::ranges::find_if(sources, same_channel) - sources.begin());
                }
            } else if (!same_consumer) { images_->reset_view_source(); }
            display.consumer = consumer;
            display.waiting_for_result = false;
            display.sources = std::move(sources);
            QStringList labels;
            for (const auto& source : display.sources) { labels.push_back(source.label); }
            images_->set_sources(labels, index, [this, consumer](int row) -> Result<QImage> {
                const auto current = module_history_->selected_run();
                const auto& selection = *workspace_image_selection_;
                if (!current || current->identity() != consumer.run || selection.consumer != consumer
                    || row < 0 || static_cast<std::size_t>(row) >= selection.sources.size()) {
                    return Diagnostic{ErrorCode::ResultIdentityMismatch, Severity::Error, {}, "The preview no longer belongs to the selected invocation."};
                }
                return workspace_thumbnail(*current, selection.sources[static_cast<std::size_t>(row)], draft());
            });
            display.rendered_index = -2;
        }
        const int index = images_->selected_source();
        if (display.rendered_index == index) { return; }
        display.rendered_index = index;
        if (index < 0 || static_cast<std::size_t>(index) >= display.sources.size()) {
            empty(QStringLiteral("请选择本次调用的来源图"));
            return;
        }
        const auto& source = display.sources[static_cast<std::size_t>(index)];
        const auto result = show_run_image(*images_, *run, source.result.invocation, source.output, &draft());
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            empty(QString::fromStdString(failure->message));
            return;
        }
        showing_run_image_ = true;
        images_->set_provenance(QStringLiteral("轮次 %1 · 快照 %2 · %3 · 调用 %4")
            .arg(run->identity().run.value()).arg(run->identity().snapshot.snapshot.value())
            .arg(source.label).arg(source.result.invocation.invocation.value()));
    } catch (const std::exception& error) {
        images_->set_sources({}, -1);
        display.consumer.reset();
        display.sources.clear();
        display.rendered_index = -2;
        empty(QString::fromUtf8(error.what()));
    }
}

void EditorWindow::start_run(std::optional<DebugStatePolicy> debug, std::optional<NodeTarget> configured)
{
    if (busy_ || close_confirmed_ || !writable()) { return; }
    const auto id = configured ? std::optional{configured->workflow} : model_->workflow_id();
    const auto* flow = id ? find_workflow(draft(), *id) : nullptr;
    if (!flow || !run_controller_->can_submit(*flow)) { return; }
    RunTarget target = WorkflowTarget{flow->id};
    std::span<const InputDescriptor> inputs = flow->interface.inputs;
    std::span<const PixelFormat> formats;
    const bool independent = debug.has_value();
    const bool isolated = debug == DebugStatePolicy::Isolated;
    std::optional<ToolDescriptor> source_descriptor;
    bool sdk_sample = false, delivered_frame = false;
    bool group_target = false;
    if (independent) {
        const auto selected = configured ? std::vector<NodeId>{configured->node} : scene_->selected_nodes();
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
            if (is_image_source(node)) {
                source_descriptor = specialize_tool(*descriptor, *node);
                descriptor = &*source_descriptor;
                const auto source = source_enum(*node, "source_type", "Local");
                sdk_sample = source == "SDK";
                delivered_frame = source == "Camera" && source_enum(*node, "camera_input", "Acquire") == "Delivered";
                if (isolated) { debug = DebugStatePolicy::IsolatedSession; }
            }
            inputs = descriptor->inputs;
            formats = descriptor->image_formats;
        }
        target = NodeTarget{flow->id, node->id};
    }
    auto entered = delivered_frame && !inputs.empty() ? choose_delivered_frame(this, run_controller_->runs(), *flow, inputs.front().id)
        : choose_run_inputs(this, draft(), inputs, isolated, formats, group_target, sdk_sample);
    if (!entered) { return; }
    if (run_error_) {
        run_error_.reset();
        render_diagnostics();
    }
    RunArguments arguments{target, std::move(entered->values),
        {independent ? TriggerKind::Debug : TriggerKind::Manual, trigger_id(independent)},
        {}, debug};
    if (independent) { run_dock_->show(); run_dock_->raise(); }
    else if (module_history_->following_latest()) {
        const auto selected = scene_->selected_nodes();
        if (selected.size() == 1 && is_image_source(find_node(*flow, selected.front()))) {
            workspace_image_selection_->mode = WorkspaceImageSelection::Mode::Follow;
            images_->set_manual_view(false);
        }
    }
    statusBar()->clearMessage();
    run_controller_->start(draft(), {source_, directory_, roots_, workspace_image_selection_->edit_generation},
        std::move(arguments), std::move(entered->images));
}

void EditorWindow::show_selected_run_image()
{
    if (busy_ || close_confirmed_) { return; }
    results_->image_tabs()->setCurrentIndex(0);
    const auto unavailable = [this](const QString& message) {
        workspace_image_selection_->mode = WorkspaceImageSelection::Mode::RunDetails;
        showing_run_image_ = false;
        images_->set_manual_view(true);
        images_->clear_frame();
        images_->set_provenance(message);
        report_error(message);
    };
    try {
        const auto run = run_panel_->selected_run();
        const auto invocation = run_panel_->selected_invocation();
        if (!run || !invocation) { return; }
        const auto displayed = show_run_image(*images_, *run, *invocation, run_panel_->selected_output(), &draft());
        if (const auto* failure = std::get_if<Diagnostic>(&displayed)) {
            unavailable(QString::fromStdString(failure->message));
            return;
        }
        const auto* flow = find_workflow(run->request().snapshot().solution(), run->identity().workflow);
        const auto* node = flow ? find_node(*flow, invocation->node) : nullptr;
        showing_run_image_ = true;
        workspace_image_selection_->mode = WorkspaceImageSelection::Mode::RunDetails;
        images_->set_manual_view(true);
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
        unavailable(QString::fromUtf8(failure.what()));
    } catch (...) {
        unavailable(QStringLiteral("Could not display the selected run image."));
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
