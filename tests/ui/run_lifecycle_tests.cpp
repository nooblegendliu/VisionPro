#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QTemporaryDir>
#include <QtTest>

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

class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Driver> driver) : driver_(std::move(driver)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
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
    return take(ToolRegistry::create(std::array{
        ToolRegistration{tool, {tool.schema.id, 1, 1, {}}, std::make_shared<Factory>(driver)}
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
    void replace_and_close_with_pending_physical_exit();
};

void RunLifecycleTests::cancel_with_slow_panel_and_resume_after_hiding()
{
    auto driver = std::make_shared<Driver>();
    EditorWindow window(solution_for('1'), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
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
    EditorWindow window(solution_for('1'), registry_for(driver));
    const DrainOnExit drain{*driver};
    auto& controller = control<RunController>(window, "runController");
    auto& panel = control<RunPanel>(window, "runPanel");
    auto& start = control<QAction>(window, "runWorkflow");
    auto& identity = control<QLabel>(window, "runIdentity");
    window.show();
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(0));
    const auto first = controller.runs().front();
    const auto old_call = *driver->call(0);

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
    QTRY_VERIFY(start.isEnabled());
    QVERIFY(take(first.inspect()).quiescent);
    QVERIFY(!panel.selected_run());
    start.trigger();
    QTRY_VERIFY(controller.runs().size() == 1 && driver->call(1));
    const auto second = controller.runs().front();
    const auto new_call = *driver->call(1);
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

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::RunLifecycleTests)
#include "run_lifecycle_tests.moc"
