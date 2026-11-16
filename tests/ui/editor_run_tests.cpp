#include "control_scene.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "run_panel.hpp"
#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity Id> Id key(std::string_view name) { return take(SchemaIdFactory::parse<Id>(name)); }

template<UuidIdentity Id> Id id(char suffix = '1')
{
    std::string text = "5fba0000-0000-4000-8000-000000000001";
    text.back() = suffix;
    return take(IdFactory::parse<Id>(text));
}

ControlScene& scene(EditorWindow& window)
{
    return *qobject_cast<ControlScene*>(qobject_cast<QGraphicsView*>(window.centralWidget())->scene());
}

QAction& action(EditorWindow& window, const char* name) { return *window.findChild<QAction*>(QString::fromLatin1(name)); }

template<class Fill> bool submit_dialog(QAction& entry, const Fill& fill)
{
    bool accepted = false;
    QTimer interaction;
    interaction.setSingleShot(true);
    QObject::connect(&interaction, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        if (buttons && fill(*dialog, *buttons->button(QDialogButtonBox::Ok))) {
            buttons->button(QDialogButtonBox::Ok)->click();
            accepted = dialog->result() == QDialog::Accepted;
        }
        if (!accepted) { dialog->reject(); }
    });
    interaction.start(0);
    entry.trigger();
    return accepted;
}

std::int64_t integer_output(const RunHandle& run, const NodeId& node, const OutputId& port)
{
    const auto progress = take(run.inspect());
    const auto invocation = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    const auto result = take(run.lookup(invocation->invocation.value())).value();
    return *std::get<DataValue>(result.output(port)->content).as_int64();
}

class Counter final : public ToolInstance {
public:
    explicit Counter(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto state = take(invocation.state->read());
        auto total = DataValue::integer(*state.as_int64() + *invocation.inputs.front().value.as_int64());
        auto written = invocation.state->write(total);
        if (auto* error = std::get_if<Diagnostic>(&written)) { return std::move(*error); }
        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("total"), std::move(total)}}, {}}};
    }
};

class CounterFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Counter>(configuration)}; }
};

ToolRegistry counter_registry()
{
    const auto integer = DataType::integer();
    InputDescriptor delta{key<InputId>("delta"), "Delta", integer};
    delta.constraints.integer_range = IntegerRange{0, 100};
    delta.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput};
    ToolDescriptor descriptor{key<ToolTypeId>("test.editor.counter"), "Counter", 1,
        {key<SchemaId>("test.editor.counter.schema"), 1}, {}, {delta},
        {{key<OutputId>("total"), "Total", integer}}};
    descriptor.execution.effects = ToolEffects::State;
    descriptor.state = NodeStateDefinition{integer, ConfigValue::integer(0), StateScope::SolutionSession,
        StateResetPolicy::DomainStart, StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted,
        StateConcurrencyPolicy::Serialized};
    const ToolRegistration registration{descriptor, {descriptor.schema.id, 1, 1, {}}, std::make_shared<CounterFactory>()};
    return take(ToolRegistry::create(std::span{&registration, 1}));
}

Solution counter_fixture()
{
    const auto delta = key<InputId>("delta");
    const auto flow_id = id<WorkflowId>();
    const auto first = id<NodeId>('1');
    const auto second = id<NodeId>('2');
    const ToolConfiguration counter{key<ToolTypeId>("test.editor.counter"), 1};
    const InputDescriptor public_delta{delta, "Delta", DataType::integer(), false, ConfigValue::integer(2)};
    Workflow flow{flow_id, "Counters", {key<SchemaId>("test.editor.flow"), 1, {public_delta}},
        {{first, "First counter", counter, {{delta, {ScopedInputReference{flow_id, {}, delta, {}}}}}},
            {second, "Second counter", counter, {{delta, {ConfigValue::integer(1)}}}}},
        {{id<ControlLinkId>(), first, {}, second}}, {}, {first}};
    return {id<SolutionId>(), "Counter draft", infrastructure::solution_format_version, 1, {std::move(flow)}};
}

class EditorRunTests final : public QObject {
    Q_OBJECT

private slots:
    void publishes_draft_and_isolates_debug();
    void runs_unsaved_absolute_resources();
};

void EditorRunTests::publishes_draft_and_isolates_debug()
{
    EditorWindow window(counter_fixture(), counter_registry());
    window.show();
    auto& control = *window.findChild<RunController*>();
    auto& panel = *window.findChild<RunPanel*>();
    auto& workflow_action = action(window, "runWorkflow");
    auto& debug_action = action(window, "debugSelectedNode");
    auto* history = window.findChild<QComboBox*>(QStringLiteral("runHistory"));
    const auto with_default = [](QDialog& dialog, QPushButton&) {
        return dialog.objectName() == QStringLiteral("runInputsDialog");
    };
    const auto delta = key<InputId>("delta");
    const auto total = key<OutputId>("total");
    const auto first = id<NodeId>('1');
    const auto second = id<NodeId>('2');

    QVERIFY(workflow_action.isEnabled());
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_COMPARE(control.runs().size(), 1u);
    QTRY_VERIFY(control.can_start());
    const auto initial = control.runs().front();
    QCOMPARE(initial.request().input(delta)->origin, InputOrigin::Default);
    QCOMPARE(integer_output(initial, first, total), 2);
    QCOMPARE(integer_output(initial, second, total), 1);

    QVERIFY(scene(window).reveal_node(first) && debug_action.isEnabled());
    bool missing_rejected = false;
    QVERIFY(submit_dialog(debug_action, [&](QDialog& dialog, QPushButton& ok) {
        auto* field = dialog.findChild<QLineEdit*>(QStringLiteral("runInputValue.delta"));
        auto* error = dialog.findChild<QLabel*>(QStringLiteral("runInputError"));
        if (!field || !error) { return false; }
        ok.click();
        missing_rejected = dialog.isVisible() && !error->text().isEmpty() && control.runs().size() == 1;
        field->setText(QStringLiteral("9"));
        return true;
    }));
    QVERIFY(missing_rejected);
    QTRY_COMPARE(control.runs().size(), 2u);
    QTRY_VERIFY(control.can_start());
    const auto debug = control.runs().back();
    QVERIFY(debug.request().debug_state() == DebugStatePolicy::Isolated);
    QCOMPARE(debug.request().input(delta)->origin, InputOrigin::Provided);
    QCOMPARE(*debug.request().input(delta)->value.as_int64(), 9);
    const auto debug_progress = take(debug.inspect());
    QVERIFY(debug_progress.call_order == std::vector{first});
    QCOMPARE(integer_output(debug, first, total), 9);
    QCOMPARE(initial.identity().snapshot.session, debug.identity().snapshot.session);
    QVERIFY(scene(window).undoStack().isClean());
    const auto& persisted = window.draft().workflows.front();
    QCOMPARE(*persisted.interface.inputs.front().default_value->as_int64(), 2);
    QVERIFY(std::holds_alternative<ScopedInputReference>(persisted.nodes.front().bindings.front().sources.front()));

    history->setCurrentIndex(2);
    QVERIFY(panel.selected_run()->identity() == initial.identity());
    QVERIFY(submit_dialog(action(window, "runDisplayLimits"), [](QDialog& dialog, QPushButton&) {
        auto* size = dialog.findChild<QSpinBox*>(QStringLiteral("runHistoryLimit"));
        if (!size) { return false; }
        size->setValue(1);
        return true;
    }));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(panel.selected_run()->identity() == initial.identity());

    QVERIFY(scene(window).edit_draft(QStringLiteral("Change workflow default"), [](Solution& draft) -> Result<std::monostate> {
        draft.workflows.front().interface.inputs.front().default_value = ConfigValue::integer(3);
        return std::monostate{};
    }));
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_VERIFY(control.can_start() && control.runs().front().identity() != debug.identity());
    const auto latest = control.runs().front();
    QCOMPARE(latest.identity().snapshot.session, initial.identity().snapshot.session);
    QVERIFY(latest.identity().snapshot.snapshot != initial.identity().snapshot.snapshot);
    QCOMPARE(integer_output(latest, first, total), 5);
    QCOMPARE(integer_output(latest, second, total), 2);
    QCOMPARE(*initial.request().input(delta)->value.as_int64(), 2);
    QVERIFY(panel.selected_run()->identity() == initial.identity());
    history->setCurrentIndex(0);
    QVERIFY(panel.selected_run()->identity() == latest.identity());

    QVERIFY(scene(window).edit_draft(QStringLiteral("Remove required binding"), [](Solution& draft) -> Result<std::monostate> {
        draft.workflows.front().nodes.front().bindings.clear();
        return std::monostate{};
    }));
    QVERIFY(submit_dialog(workflow_action, with_default));
    QTRY_VERIFY(control.can_start() && !window.statusBar()->currentMessage().startsWith(QStringLiteral("Preparing")));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(control.runs().front().identity() == latest.identity());
}

void EditorRunTests::runs_unsaved_absolute_resources()
{
    const QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QFile payload(temporary.filePath(QStringLiteral("input.bin")));
    QVERIFY(payload.open(QIODevice::WriteOnly));
    QCOMPARE(payload.write(QByteArray(1, char{42})), 1);
    payload.close();
    const auto resource = id<ResourceId>();
    const auto node = id<NodeId>();
    const Node reader{node, "Read byte", ToolConfiguration{key<ToolTypeId>("visionpro.sim.file_byte"), 1}, {},
        {{key<SchemaId>("payload"), {resource, ResourceKind::Binary, {}}}}};
    Workflow workflow{id<WorkflowId>(), "Resource workflow", {key<SchemaId>("test.editor.read"), 1}, {reader}, {}, {}, {node}};
    Solution solution{id<SolutionId>(), "Unsaved resources", infrastructure::solution_format_version, 1, {std::move(workflow)},
        {{resource, "Input bytes", ResourceKind::Binary,
            ExternalFileResource{{PathBase::Absolute, payload.fileName().toStdString(), {}}, ResourceReadPolicy::PerInvocation, {}, {}}}}};
    EditorWindow window(std::move(solution), take(app::make_tool_registry()));
    window.show();
    auto& control = *window.findChild<RunController*>();
    auto& run = action(window, "runWorkflow");
    run.trigger();
    QTRY_COMPARE(control.runs().size(), 1u);
    QTRY_VERIFY(control.can_start());
    const auto first = control.runs().front();
    QCOMPARE(integer_output(first, node, key<OutputId>("value")), 42);

    QVERIFY(scene(window).edit_draft(QStringLiteral("Use solution-relative bytes"), [](Solution& draft) -> Result<std::monostate> {
        auto& file = std::get<ExternalFileResource>(draft.resources.front().location);
        file.path = {PathBase::SolutionDirectory, "input.bin", {}};
        return std::monostate{};
    }));
    run.trigger();
    QTRY_VERIFY(control.can_start());
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("explicit solution directory")));
    QCOMPARE(control.runs().size(), 1u);
    QVERIFY(control.runs().front().identity() == first.identity());
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::EditorRunTests)
#include "editor_run_tests.moc"
