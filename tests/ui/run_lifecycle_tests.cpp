#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QComboBox>
#include <QTabBar>
#include <QCryptographicHash>
#include <QDockWidget>
#include <QFile>
#include <QGraphicsView>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template<class T>
T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T>
T uuid(char suffix)
{
    std::string value = "00000000-0000-4000-8000-000000000000";
    value.back() = suffix;
    return take(IdFactory::parse<T>(value));
}

template<SchemaIdentity T>
T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }

struct PendingCall {
    InvocationCompletion completion;
    ToolInvocationIdentity identity;
};

ToolCompletion cancelled(const PendingCall& call)
{
    return {call.identity, ToolCompletionStatus::Cancelled, ToolJudgement::NotEvaluated, {}, {}};
}

ToolCompletion succeeded(const PendingCall& call)
{
    return {call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
        {{key<OutputId>("measurement"), DataValue::integer(13)}}, {}};
}

void release(const PendingCall& call)
{
    take(call.completion.complete(cancelled(call)));
    take(call.completion.mark_exited());
}

class Driver {
public:
    bool record(const PendingCall& call)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        calls_.push_back(call);
        return true;
    }

    std::optional<PendingCall> call(std::size_t index)
    {
        const std::lock_guard lock(mutex_);
        return index < calls_.size() ? std::optional{calls_[index]} : std::nullopt;
    }

    static void finish(const PendingCall& call)
    {
        // Both calls are safe during teardown after a fixture has already
        // completed or exited this operation.
        static_cast<void>(call.completion.complete(cancelled(call)));
        static_cast<void>(call.completion.mark_exited());
    }

    void drain()
    {
        std::vector<PendingCall> calls;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            calls.swap(calls_);
        }
        for (const auto& call : calls) { finish(call); }
    }

    std::atomic<unsigned> cancellations{0};

private:
    std::mutex mutex_;
    std::vector<PendingCall> calls_;
    bool draining_{false};
};

struct DrainOnExit {
    Driver& driver;
    ~DrainOnExit() { driver.drain(); }
};

class ControlledInvocation final : public InvocationControl {
public:
    explicit ControlledInvocation(const std::shared_ptr<Driver>& driver) : driver_(driver) {}
    void request_cancel() override
    {
        if (const auto driver = driver_.lock()) { ++driver->cancellations; }
    }

private:
    std::weak_ptr<Driver> driver_;
};

class ControlledTool final : public ToolInstance {
public:
    ControlledTool(const ToolExecutionConfiguration& configuration, std::shared_ptr<Driver> driver)
        : ToolInstance(configuration), driver_(std::move(driver)) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        auto control = std::make_shared<ControlledInvocation>(driver_);
        if (auto registered = completion.register_control(control); const auto* failure = std::get_if<Diagnostic>(&registered)) {
            return *failure;
        }
        const PendingCall call{completion, context->data().identity};
        if (!driver_->record(call)) { Driver::finish(call); }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }

    std::shared_ptr<Driver> driver_;
};

ToolTypeId image_tool_type() { return key<ToolTypeId>("test.ui.resource_image"); }
CoordinateSystemId frame() { return key<CoordinateSystemId>("test.ui.resource_frame"); }

class ResourceImageTool final : public ToolInstance {
public:
    explicit ResourceImageTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& call = context->data();
        const auto lease = std::dynamic_pointer_cast<const FileResourceLease>(call.resources.front().lease);
        if (!lease || lease->bytes().size() != 1) {
            return Diagnostic{ErrorCode::InvalidResource, Severity::Error, {}, "Expected one fixture byte."};
        }
        const auto setting = static_cast<std::byte>(*call.parameters.front().value.as_int64());
        const auto byte = lease->bytes().front();
        const std::array pixels{setting, byte, byte, setting};
        const auto image = take(ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, pixels, frame()));
        return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("image"), take(DataValue::image({image, {}, {}}))}}, {}}};
    }
};

class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Driver> driver) : driver_(std::move(driver)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        if (configuration.data().tool.id == image_tool_type()) {
            return std::shared_ptr<ToolInstance>{std::make_shared<ResourceImageTool>(configuration)};
        }
        return std::shared_ptr<ToolInstance>{std::make_shared<ControlledTool>(configuration, driver_)};
    }

private:
    std::shared_ptr<Driver> driver_;
};

ToolTypeId tool_type() { return key<ToolTypeId>("test.ui.async"); }

ToolRegistry registry_for(const std::shared_ptr<Driver>& driver)
{
    ToolDescriptor tool{tool_type(), "Controlled async", 1, {key<SchemaId>("test.ui.async.schema"), 1},
        {}, {}, {{key<OutputId>("measurement"), "Measurement", DataType::integer()}}};
    tool.execution.mode = ToolExecutionMode::Asynchronous;
    tool.execution.cancellation = ToolCancellation::Cooperative;
    ParameterDescriptor setting{key<ParameterId>("setting"), "Setting", DataType::integer()};
    setting.constraints.integer_range = IntegerRange{0, 255};
    ToolDescriptor image{image_tool_type(), "Resource image", 1, {key<SchemaId>("test.ui.resource_image.schema"), 1},
        {setting}, {}, {{key<OutputId>("image"), "Image", DataType::image(), {MeasurementUnit::Pixel, frame()}}}};
    image.resources = {{key<SchemaId>("payload"), ResourceKind::Binary}};
    image.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono}};
    const auto factory = std::make_shared<Factory>(driver);
    return take(ToolRegistry::create(std::array{
        ToolRegistration{tool, {tool.schema.id, 1, 1, {}}, factory},
        ToolRegistration{image, {image.schema.id, 1, 1, {}}, factory}
    }));
}

Solution solution_for(char suffix)
{
    const auto node = uuid<NodeId>('1');
    Workflow flow{uuid<WorkflowId>(suffix), "Controlled flow", {key<SchemaId>("test.ui.async.flow"), 1, {}},
        {{node, "Controlled node", ToolConfiguration{tool_type(), 1}}}, {}, {}, {node}};
    return {uuid<SolutionId>(suffix), suffix == '1' ? "First solution" : "Replacement solution",
        solution_format_version, 1, {std::move(flow)}};
}

ExternalFileResource locked_resource(std::string name, char payload, std::uint64_t revision)
{
    const auto digest = QCryptographicHash::hash(QByteArray(1, payload), QCryptographicHash::Sha256);
    return {{PathBase::SolutionDirectory, std::move(name), {}}, ResourceReadPolicy::SnapshotLocked,
        digest.toHex().toStdString(), revision};
}

bool write_byte(const QString& path, char value)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QByteArray(1, value)) == 1;
}

template<class Widget>
Widget& control(EditorWindow& window, const char* object_name)
{
    if (auto* child = window.findChild<Widget*>(QString::fromLatin1(object_name))) { return *child; }
    throw std::runtime_error(object_name);
}

RunWaitResult await(const RunHandle& run, RunWaitTarget target)
{
    return take(run.wait(target, std::chrono::steady_clock::now() + 5s));
}

class RunLifecycleTests final : public QObject {
    Q_OBJECT

private slots:
    void cancel_with_slow_panel_and_resume_after_hiding();
    void admits_multiple_runs_and_cancels_only_the_selected_identity();
    void workspace_cancel_shortcut_keeps_its_flow_data();
    void workspace_cancel_shortcut_keeps_its_flow();
    void replace_and_close_with_pending_physical_exit();
    void edits_preserve_running_capture_and_displayed_image();
    void late_completion_after_window_destruction();
    void trigger_busy_diagnostic_and_cancel_wait_for_cleanup();
};

void RunLifecycleTests::trigger_busy_diagnostic_and_cancel_wait_for_cleanup()
{
    auto draft = solution_for('1');
    auto& flow = draft.workflows.front();
    const auto input = key<InputId>("frame_no");
    flow.interface.inputs.push_back({input, "Frame", DataType::integer()});
    flow.trigger = WorkflowTrigger{TriggerMode::External, key<SchemaId>("ui.test.frames"), input};
    auto driver = std::make_shared<Driver>();
    EditorWindow window(draft, registry_for(driver));
    const DrainOnExit drain{*driver};
    window.show();
    auto& arm = control<QAction>(window, "armWorkflowTrigger");
    auto& send = control<QAction>(window, "sendExternalFrame");
    auto& controller = control<RunController>(window, "runController");
    arm.trigger();
    QTRY_VERIFY(send.isEnabled());
    controller.send_frame(flow.id, 11);
    QTRY_VERIFY(send.isEnabled() && controller.runs().size() == 1 && driver->call(0));
    const auto run = controller.runs().front();
    controller.send_frame(flow.id, 12);
    auto& issues = control<QListWidget>(window, "editorDiagnostics");
    const auto active = QStringLiteral("active run %1").arg(run.identity().run.value());
    QTRY_VERIFY(!issues.findItems(active, Qt::MatchContains).isEmpty());
    const auto message = issues.findItems(active, Qt::MatchContains).front()->text();
    QVERIFY(message.contains(QStringLiteral("frame_no = 12")) && message.contains(QStringLiteral("ui.test.frames / 12")));
    QCOMPARE(controller.runs().size(), 1u);
    control<QAction>(window, "cancelWorkflowTrigger").trigger();
    QTRY_COMPARE(driver->cancellations.load(), 1u);
    QVERIFY(!arm.isEnabled() && !send.isEnabled() && !take(run.inspect()).quiescent);
    controller.send_frame(flow.id, 13);
    release(*driver->call(0));
    QTRY_VERIFY(arm.isEnabled() && controller.can_start());
    QCOMPARE(controller.runs().size(), 1u);
    QCOMPARE(take(run.inspect()).status, ExecutionStatus::Cancelled);
}

void RunLifecycleTests::admits_multiple_runs_and_cancels_only_the_selected_identity()
{
    auto draft = solution_for('1');
    draft.workflows.front().admission = {2, 1, AdmissionOverflow::Reject};
    auto other = solution_for('2').workflows.front();
    other.name = "Independent flow";
    other.nodes.front().id = uuid<NodeId>('2');
    other.entries = {other.nodes.front().id};
    draft.workflows.push_back(other);
    auto driver = std::make_shared<Driver>();
    EditorWindow window(std::move(draft), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& detail_cancel = control<QPushButton>(window, "cancelDetailRun");
    auto& history = control<QComboBox>(window, "runHistory");
    auto& status = control<QLabel>(window, "runStatus");
    controller.set_limits(0, 4);
    window.show();
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0) && start.isEnabled());
    const auto first = controller.runs().front();
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 2 && driver->call(1) && start.isEnabled());
    const auto second = controller.runs().back();
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 3 && start.isEnabled());
    const auto queued = controller.runs().back();
    QVERIFY(queued.admission() == RunAdmission::Queued && !driver->call(2));
    QTRY_COMPARE(status.text(), QStringLiteral("Status: Queued"));
    std::optional<Diagnostic> refusal;
    connect(&controller, &RunController::operation_failed, &window, [&](const Diagnostic& issue) { refusal = issue; });
    start.trigger();
    QTRY_VERIFY(refusal && refusal->code == ErrorCode::RunQueueFull);
    QCOMPARE(controller.runs().size(), 3u);

    auto* view = window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas"));
    auto* scene = qobject_cast<ControlScene*>(view->scene());
    QVERIFY(scene && scene->select_workflow(other.id) && start.isEnabled());
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 4 && driver->call(2));
    const auto independent = controller.runs().back();
    QVERIFY(independent.identity().workflow == other.id && !start.isEnabled());
    const auto other_call = *driver->call(2);
    take(other_call.completion.complete(succeeded(other_call)));
    take(other_call.completion.mark_exited());
    QTRY_VERIFY(controller.runs().size() == 3 && start.isEnabled());
    QVERIFY(take(independent.inspect()).status == ExecutionStatus::Succeeded);
    QVERIFY(take(first.inspect()).status == ExecutionStatus::Running && take(second.inspect()).status == ExecutionStatus::Running);

    // History limit zero still displays every pending/running/cleaning handle.
    history.setCurrentIndex(3);
    QVERIFY(panel.selected_run() && panel.selected_run()->identity() == first.identity() && detail_cancel.isEnabled());
    detail_cancel.click();
    QTRY_VERIFY(take(first.inspect()).status == ExecutionStatus::Cancelled);
    QVERIFY(!take(first.inspect()).quiescent && take(second.inspect()).status == ExecutionStatus::Running);
    QVERIFY(take(queued.inspect()).status == ExecutionStatus::Pending && !driver->call(3));
    release(*driver->call(0));
    QTRY_VERIFY(driver->call(3) && controller.runs().size() == 2);
    QVERIFY(driver->call(3)->identity.invocation.workflow_run == queued.identity().run);
    QVERIFY(!detail_cancel.isEnabled() && panel.selected_run()->identity() == first.identity());
    for (const auto index : {1u, 3u}) {
        const auto call = *driver->call(index);
        take(call.completion.complete(succeeded(call)));
        take(call.completion.mark_exited());
    }
    QTRY_VERIFY(controller.can_start() && controller.runs().empty());
    QVERIFY(take(second.inspect()).status == ExecutionStatus::Succeeded && take(queued.inspect()).status == ExecutionStatus::Succeeded);
    QCOMPARE(driver->cancellations.load(), 1u);
}

void RunLifecycleTests::workspace_cancel_shortcut_keeps_its_flow_data()
{
    QTest::addColumn<bool>("details_visible");
    QTest::addColumn<bool>("details_focused");
    QTest::newRow("details closed") << false << false;
    QTest::newRow("details open") << true << false;
    QTest::newRow("floating details focused") << true << true;
}

void RunLifecycleTests::workspace_cancel_shortcut_keeps_its_flow()
{
    QFETCH(bool, details_visible);
    QFETCH(bool, details_focused);
    auto draft = solution_for('1');
    draft.workflows.front().name = "Flow A";
    auto other = solution_for('2').workflows.front();
    other.name = "Flow B";
    other.nodes.front().id = uuid<NodeId>('2');
    other.entries = {other.nodes.front().id};
    draft.workflows.push_back(std::move(other));
    auto driver = std::make_shared<Driver>();
    EditorWindow window(std::move(draft), registry_for(driver));
    const DrainOnExit drain{*driver};
    window.show();
    auto& controller = control<RunController>(window, "runController");
    auto& selector = control<QTabBar>(window, "workflowSelector");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& cancel = control<QAction>(window, "cancelRun");
    auto& dock = control<QDockWidget>(window, "runDock");
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& detail_cancel = control<QPushButton>(window, "cancelDetailRun");
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0));
    const auto a = controller.runs().front();
    selector.setCurrentIndex(1);
    QTRY_VERIFY(start.isEnabled());
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 2 && driver->call(1));
    const auto b = controller.runs().back();
    selector.setCurrentIndex(0);
    const auto target = QStringLiteral("取消“Flow A”第 %1 轮").arg(a.identity().run.value());
    QTRY_COMPARE(cancel.text(), target);
    QVERIFY(control<QLabel>(window, "workspaceStats").text().contains(QStringLiteral("第 %1 轮").arg(a.identity().run.value())));
    dock.show();
    control<QComboBox>(window, "runHistory").setCurrentIndex(1);
    QTRY_VERIFY(panel.selected_run() && panel.selected_run()->identity() == b.identity());
    QVERIFY(control<QLabel>(window, "runIdentity").text().contains(QStringLiteral("Flow B")));
    QCOMPARE(cancel.text(), target);
    dock.hide();
    QCOMPARE(cancel.text(), target);
    dock.setVisible(details_visible);
    QCOMPARE(cancel.text(), target);
    QVERIFY(cancel.isEnabled());

    QWidget* key_target = details_focused ? static_cast<QWidget*>(&detail_cancel) : static_cast<QWidget*>(&selector);
    key_target->window()->activateWindow();
    key_target->setFocus();
    QTRY_VERIFY(key_target->hasFocus());
    QTest::keyClick(key_target, Qt::Key_F5, Qt::ShiftModifier);
    QTRY_COMPARE(take(a.inspect()).status, ExecutionStatus::Cancelled);
    QCOMPARE(take(b.inspect()).status, ExecutionStatus::Running);
    QTRY_VERIFY(!cancel.isEnabled());

    dock.show();
    QTRY_VERIFY(detail_cancel.isEnabled() && panel.selected_run() && panel.selected_run()->identity() == b.identity());
    QTest::mouseClick(&detail_cancel, Qt::LeftButton);
    QTRY_COMPARE(take(b.inspect()).status, ExecutionStatus::Cancelled);
    QTRY_VERIFY(!detail_cancel.isEnabled());
    release(*driver->call(0));
    release(*driver->call(1));
    QTRY_VERIFY(controller.can_start());
    QCOMPARE(driver->cancellations.load(), 2u);
}

void RunLifecycleTests::cancel_with_slow_panel_and_resume_after_hiding()
{
    auto driver = std::make_shared<Driver>();
    EditorWindow window(solution_for('1'), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& dock = control<QDockWidget>(window, "runDock");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& cancel = control<QAction>(window, "cancelRun");
    auto& status = control<QLabel>(window, "runStatus");
    auto& completion = control<QLabel>(window, "runCompletion");
    auto& cleanup = control<QLabel>(window, "runCleanup");
    auto& notifications = control<QLabel>(window, "runNotifications");
    controller.set_limits(2, 1);
    window.show();
    QVERIFY(start.isEnabled());
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && cancel.isEnabled() && driver->call(0));
    const auto run = controller.runs().front();
    const auto call = *driver->call(0);
    QVERIFY(dock.isVisible());
    cancel.trigger();

    // The GUI event loop stays idle while the engine publishes both the
    // cancelled result and the run's terminal notice into one queue slot.
    const auto terminal = await(run, RunWaitTarget::Completion);
    QVERIFY(terminal.outcome == RunWaitOutcome::Satisfied);
    QVERIFY(terminal.progress.status == ExecutionStatus::Cancelled && !terminal.progress.quiescent);
    QTRY_VERIFY(driver->cancellations.load() == 1);
    QTRY_COMPARE(status.text(), QStringLiteral("Status: Cancelled"));
    QTRY_VERIFY(controller.dropped() > 0);
    QCOMPARE(notifications.text(), QStringLiteral("Dropped notifications: %1").arg(controller.dropped()));
    QCOMPARE(completion.text(), QStringLiteral("Logical completion: Complete"));
    QCOMPARE(cleanup.text(), QStringLiteral("Cleanup: Pending"));
    QVERIFY(!start.isEnabled());

    QVERIFY(dock.close());
    QVERIFY(dock.isHidden());
    release(call);
    const auto finished = await(run, RunWaitTarget::Quiescence);
    QVERIFY(finished.outcome == RunWaitOutcome::Satisfied && finished.progress.quiescent);
    QTRY_VERIFY(start.isEnabled());
    QCOMPARE(cleanup.text(), QStringLiteral("Cleanup: Pending"));
    dock.show();
    QTRY_COMPARE(cleanup.text(), QStringLiteral("Cleanup: Quiescent"));
    QVERIFY(panel.selected_run() && panel.selected_run()->identity() == run.identity());
    const auto retained = take(run.lookup(call.identity.invocation));
    QVERIFY(retained && retained->data().status == ExecutionStatus::Cancelled);

    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 2 && driver->call(1));
    const auto hidden_run = controller.runs().back();
    const auto hidden_call = *driver->call(1);
    QVERIFY(dock.close() && dock.isHidden());
    take(hidden_call.completion.complete(succeeded(hidden_call)));
    take(hidden_call.completion.mark_exited());
    const auto hidden_finished = await(hidden_run, RunWaitTarget::Quiescence);
    QVERIFY(hidden_finished.outcome == RunWaitOutcome::Satisfied && hidden_finished.progress.status == ExecutionStatus::Succeeded);
    QTRY_VERIFY(start.isEnabled());
    dock.show();
    QTRY_COMPARE(status.text(), QStringLiteral("Status: Succeeded"));
    QVERIFY(panel.selected_run() && panel.selected_run()->identity() == hidden_run.identity());
    QVERIFY(control<QLabel>(window, "runJudgement").text() == QStringLiteral("Judgement: OK"));
}

void RunLifecycleTests::replace_and_close_with_pending_physical_exit()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto replacement = solution_for('2');
    const fs::path directory(scratch.path().toStdWString());
    take(write_new_solution(directory, replacement));
    auto driver = std::make_shared<Driver>();
    auto original = solution_for('1');
    original.workflows.front().admission.max_in_flight = 2;
    EditorWindow window(std::move(original), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& identity = control<QLabel>(window, "runIdentity");
    window.show();
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0));
    const auto first = controller.runs().front();
    const auto old_call = *driver->call(0);
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 2 && driver->call(1));
    const auto sibling = controller.runs().back();
    const auto sibling_call = *driver->call(1);

    window.open_document(directory);
    QTRY_VERIFY(!window.file_busy() && window.draft().id == replacement.id);
    take(old_call.completion.complete(succeeded(old_call)));
    const auto before_exit = await(first, RunWaitTarget::Completion);
    QVERIFY(before_exit.outcome == RunWaitOutcome::Satisfied);
    QVERIFY(before_exit.progress.status == ExecutionStatus::Succeeded && !before_exit.progress.quiescent);
    QCoreApplication::processEvents();
    QVERIFY(!start.isEnabled() && controller.has_session_work());
    QVERIFY(!panel.selected_run() && identity.text().isEmpty());

    take(old_call.completion.mark_exited());
    QVERIFY(await(first, RunWaitTarget::Quiescence).progress.quiescent);
    take(sibling_call.completion.complete(succeeded(sibling_call)));
    QVERIFY(await(sibling, RunWaitTarget::Completion).progress.status == ExecutionStatus::Succeeded);
    QCoreApplication::processEvents();
    QVERIFY(!start.isEnabled() && !panel.selected_run());
    take(sibling_call.completion.mark_exited());
    QTRY_VERIFY(start.isEnabled());
    QVERIFY(take(sibling.inspect()).quiescent);
    QVERIFY(!panel.selected_run());
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(2));
    const auto second = controller.runs().front();
    const auto new_call = *driver->call(2);
    QVERIFY(second.identity().snapshot.session != first.identity().snapshot.session);
    QVERIFY(panel.selected_run() && panel.selected_run()->identity() == second.identity());
    QVERIFY(identity.text().contains(QString::fromStdString(second.identity().snapshot.session.to_string())));
    QVERIFY(!identity.text().contains(QString::fromStdString(first.identity().snapshot.session.to_string())));

    QVERIFY(!window.close());
    take(new_call.completion.complete(succeeded(new_call)));
    const auto closing = await(second, RunWaitTarget::Completion);
    QVERIFY(closing.outcome == RunWaitOutcome::Satisfied);
    QVERIFY(closing.progress.status == ExecutionStatus::Succeeded && !closing.progress.quiescent);
    QCoreApplication::processEvents();
    QVERIFY(window.isVisible() && controller.has_session_work());
    take(new_call.completion.mark_exited());
    QTRY_VERIFY(!window.isVisible() && !controller.has_session_work());
    QVERIFY(take(second.inspect()).quiescent);
    QCOMPARE(driver->cancellations.load(), 0U);
}

void RunLifecycleTests::edits_preserve_running_capture_and_displayed_image()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto first_path = scratch.filePath(QStringLiteral("first.bin"));
    QVERIFY(write_byte(first_path, 17));
    QVERIFY(write_byte(scratch.filePath(QStringLiteral("second.bin")), 31));

    auto solution = solution_for('1');
    const auto gate = uuid<NodeId>('1');
    const auto successor = uuid<NodeId>('2');
    const auto resource = uuid<ResourceId>('1');
    const auto setting = key<ParameterId>("setting");
    auto& flow = solution.workflows.front();
    flow.nodes.push_back({successor, "Resource-backed image", ToolConfiguration{image_tool_type(), 1},
        {{setting, {ConfigValue::integer(7)}}}, {{key<SchemaId>("payload"), {resource, ResourceKind::Binary, 1}}}});
    flow.control_links.push_back({uuid<ControlLinkId>('1'), gate, {}, successor});
    solution.resources = {{resource, "Snapshot-locked byte", ResourceKind::Binary, locked_resource("first.bin", 17, 1)}};
    solution.coordinates = {{frame(), CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
    const fs::path directory(scratch.path().toStdWString());
    take(write_new_solution(directory, solution));

    auto driver = std::make_shared<Driver>();
    EditorWindow window(solution_for('2'), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
    window.findChild<QDockWidget*>(QStringLiteral("runDock"))->show();
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& dock = control<QDockWidget>(window, "runDock");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& nodes = control<QTreeWidget>(window, "runNodes");
    auto& open_image = control<QPushButton>(window, "showRunImage");
    auto& images = control<ImagePanel>(window, "imagePanel");
    auto& provenance = control<QLabel>(window, "imageProvenance");
    controller.set_limits(1, controller.observation_capacity());
    window.show();
    window.open_document(directory);
    QTRY_VERIFY(!window.file_busy() && window.draft().id == solution.id && start.isEnabled());

    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0));
    const auto first = controller.runs().front().identity();
    auto* canvas = window.findChild<QGraphicsView*>(QStringLiteral("workflowCanvas"));
    QVERIFY(canvas);
    auto* scene = qobject_cast<ControlScene*>(canvas->scene());
    QVERIFY(scene);
    QVERIFY(scene->edit_draft(QStringLiteral("Change setting and resource during run"), [](Solution& draft) -> Result<std::monostate> {
        draft.resources.front().location = locked_resource("second.bin", 31, 2);
        auto& edited = draft.workflows.front().nodes.back();
        edited.resources.front().resource.revision = 2;
        edited.bindings.front().sources.front() = ConfigValue::integer(11);
        return std::monostate{};
    }));
    QVERIFY(write_byte(first_path, 99));
    QVERIFY(!start.isEnabled());
    {
        const auto run = controller.runs().front();
        const auto call = *driver->call(0);
        take(call.completion.complete(succeeded(call)));
        take(call.completion.mark_exited());
        const auto finished = await(run, RunWaitTarget::Quiescence);
        QVERIFY(finished.outcome == RunWaitOutcome::Satisfied);
        QCOMPARE(finished.progress.status, ExecutionStatus::Succeeded);
        const auto trace = std::ranges::find(finished.progress.nodes, successor, &RunNodeProgress::node);
        QVERIFY(trace != finished.progress.nodes.end() && trace->resources.size() == 1);
        QVERIFY(trace->resources.front().reference.revision == 1);
    }
    QTRY_VERIFY(start.isEnabled() && nodes.topLevelItemCount() == 2);
    nodes.setCurrentItem(nodes.topLevelItem(1));
    QVERIFY(panel.selected_invocation() && panel.selected_invocation()->node == successor);
    QVERIFY(open_image.isEnabled());
    open_image.click();
    const std::array first_pixels{std::byte{7}, std::byte{17}, std::byte{17}, std::byte{7}};
    QVERIFY(images.frame() && std::ranges::equal(images.frame()->copy_pixels(), first_pixels));
    const auto first_label = provenance.text();
    QVERIFY(!first_label.isEmpty());

    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 2 && driver->call(1));
    const auto second = controller.runs().back();
    QVERIFY(second.identity().snapshot.session == first.snapshot.session);
    QVERIFY(second.identity().snapshot.snapshot > first.snapshot.snapshot);
    QVERIFY(dock.close() && dock.isHidden());
    const auto call = *driver->call(1);
    take(call.completion.complete(succeeded(call)));
    take(call.completion.mark_exited());
    const auto finished = await(second, RunWaitTarget::Quiescence);
    QVERIFY(finished.outcome == RunWaitOutcome::Satisfied);
    QCOMPARE(finished.progress.status, ExecutionStatus::Succeeded);
    const auto trace = std::ranges::find(finished.progress.nodes, successor, &RunNodeProgress::node);
    QVERIFY(trace != finished.progress.nodes.end() && trace->resources.size() == 1);
    QVERIFY(trace->resources.front().reference.revision == 2);
    QTRY_VERIFY(start.isEnabled() && controller.runs().size() == 1);
    QVERIFY(controller.runs().front().identity() == second.identity());

    dock.show();
    QTRY_VERIFY(panel.selected_run() && panel.selected_run()->identity() == second.identity());
    QVERIFY(images.frame() && std::ranges::equal(images.frame()->copy_pixels(), first_pixels));
    QCOMPARE(provenance.text(), first_label);
    auto& view = control<ImageView>(window, "imageView");
    const auto pixel = view.pixel_at(view.mapFromScene(QPointF{1, 0}));
    QVERIFY(pixel && pixel->samples[0] == 17);

    nodes.setCurrentItem(nodes.topLevelItem(1));
    QVERIFY(open_image.isEnabled());
    open_image.click();
    const std::array second_pixels{std::byte{11}, std::byte{31}, std::byte{31}, std::byte{11}};
    QVERIFY(images.frame() && std::ranges::equal(images.frame()->copy_pixels(), second_pixels));
    QVERIFY(provenance.text() != first_label);
}

void RunLifecycleTests::late_completion_after_window_destruction()
{
    auto driver = std::make_shared<Driver>();
    auto window = std::make_unique<EditorWindow>(solution_for('1'), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(*window, "runController");
    const QPointer<RunPanel> panel(&control<RunPanel>(*window, "runPanel"));
    window->show();
    control<QAction>(*window, "runWorkflow").trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0));
    const auto run = controller.runs().front();
    const auto call = *driver->call(0);
    window.reset();
    QVERIFY(panel.isNull());
    take(call.completion.complete(succeeded(call)));
    take(call.completion.mark_exited());
    const auto finished = await(run, RunWaitTarget::Quiescence);
    QVERIFY(finished.outcome == RunWaitOutcome::Satisfied && finished.progress.quiescent);
    QCOMPARE(finished.progress.status, ExecutionStatus::Succeeded);
    const auto retained = take(run.lookup(call.identity.invocation));
    QVERIFY(retained && retained->data().status == ExecutionStatus::Succeeded);
    QCoreApplication::processEvents();
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::RunLifecycleTests)
#include "run_lifecycle_tests.moc"
