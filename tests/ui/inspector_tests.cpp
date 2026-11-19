#include "canvas_example.hpp"
#include "control_scene.hpp"
#include "editor_inspector.hpp"
#include "editor_window.hpp"
#include "run_controller.hpp"
#include "tool_catalog.hpp"

#include <simulation.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QGraphicsView>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <functional>
#include <initializer_list>
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

template<SchemaIdentity Id> Id key(std::string_view text)
{ return take(SchemaIdFactory::parse<Id>(text)); }

ToolRegistry registry()
{
    auto registrations = tools::simulation_registrations();
    const auto add = key<ToolTypeId>("visionpro.sim.add");
    const auto registration = std::ranges::find_if(registrations,
        [&](const auto& item) { return item.descriptor.id == add; });
    if (registration == registrations.end()) { throw std::runtime_error("The add tool is missing."); }
    const auto object = take(DataType::record(key<SchemaId>("test.inspector.object"), 1,
        {{key<FieldId>("area_pixels"), DataType::integer()}}));
    registration->descriptor.outputs.push_back({key<OutputId>("objects"), "Objects", DataType::list(object),
        {}, false, {OutputAbsenceReason::ToolDidNotProduce}});
    return take(ToolRegistry::create(registrations));
}

const Node& node_named(const Workflow& flow, std::string_view name)
{
    const auto node = std::ranges::find(flow.nodes, name, &Node::name);
    if (node == flow.nodes.end()) { throw std::runtime_error("The example node is missing."); }
    return *node;
}

const Binding* binding_for(const ControlGraphModel& model, NodeId node, const InputId& target)
{
    const auto* selected = find_node(model.draft().workflows.front(), node);
    if (!selected) { return nullptr; }
    const auto binding = std::ranges::find(selected->bindings, BindingTarget{target}, &Binding::target);
    return binding == selected->bindings.end() ? nullptr : &*binding;
}

const GroupConfiguration& group_config(const ControlGraphModel& model, NodeId group)
{
    const auto* node = find_node(model.draft().workflows.front(), group);
    if (!node) { throw std::runtime_error("The example Group is missing."); }
    return std::get<GroupConfiguration>(node->configuration);
}

QPushButton* binding_button(const QWidget& inspector, const QString& target)
{
    for (auto* button : inspector.findChildren<QPushButton*>(QStringLiteral("editBinding"))) {
        if (button->property("bindingTarget").toString() == target) { return button; }
    }
    return nullptr;
}

QPushButton* accept_button(QDialog& dialog)
{
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    return buttons ? buttons->button(QDialogButtonBox::Ok) : nullptr;
}

bool select_choice(QComboBox& combo, const QString& label, bool partial = false)
{
    for (int index = 0; index < combo.count(); ++index) {
        const auto item = combo.itemText(index);
        if (partial ? item.contains(label) : item == label) {
            combo.setCurrentIndex(index);
            return true;
        }
    }
    return false;
}

using DialogStep = std::function<QPushButton*(QDialog&)>;

QString exercise_dialog(QWidget& inspector, QPushButton& start, std::initializer_list<DialogStep> steps)
{
    if (auto* scroll = inspector.findChild<QScrollArea*>()) { scroll->ensureWidgetVisible(&start); }
    const auto* next = steps.begin();
    QString stalled_at;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&] {
        poll.stop();
        if (const auto* modal = QApplication::activeModalWidget()) { stalled_at = modal->windowTitle(); }
        for (auto* window : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QDialog*>(window); dialog && dialog->isVisible() && dialog->isModal()) {
                dialog->reject();
            }
        }
    });
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || next == steps.end()) { return; }
        if (auto* button = (*next)(*dialog)) {
            ++next;
            QTimer::singleShot(0, button, [button] { QTest::mouseClick(button, Qt::LeftButton); });
        }
    });
    poll.start(10);
    timeout.start(5000);
    QTest::mouseClick(&start, Qt::LeftButton);
    if (next == steps.end() && timeout.isActive()) { return {}; }
    return QStringLiteral("Stopped at dialog step %1 of %2 (%3).")
        .arg(next - steps.begin() + 1).arg(steps.size()).arg(stalled_at);
}

class InspectorTests final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commits_binding_and_group_outlet_through_dialogs();
    void edits_loop_settings_and_collects_a_selected_field();
    void edits_and_reloads_dynamic_indices_and_format_options();
    void resets_only_the_selected_session_accumulator();
};

ControlScene& scene(EditorWindow& window)
{
    return *qobject_cast<ControlScene*>(qobject_cast<QGraphicsView*>(window.centralWidget())->scene());
}

QWidget& properties(EditorWindow& window)
{
    return *window.findChild<QDockWidget*>(QStringLiteral("inspectorDock"))->widget();
}

NodeResult result_for(const RunHandle& run, const NodeId& node)
{
    const auto progress = take(run.inspect());
    const auto found = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    if (found == progress.nodes.end() || !found->invocation) { throw std::runtime_error("Missing invocation"); }
    return take(run.lookup(*found->invocation)).value();
}

void InspectorTests::edits_and_reloads_dynamic_indices_and_format_options()
{
    const auto flow_id = take(IdFactory::generate<WorkflowId>());
    const auto formatter = take(IdFactory::generate<NodeId>());
    const auto index = take(IdFactory::generate<NodeId>());
    const auto consumer = take(IdFactory::generate<NodeId>());
    const auto left = key<InputId>("left");
    const auto right = key<InputId>("right");
    const auto value = key<OutputId>("value");
    const auto output = key<OutputId>("used_values");
    const auto calc = ToolConfiguration{key<ToolTypeId>("visionpro.calc.int64"), 1};
    const std::vector list{ConfigValue::integer(6), ConfigValue::integer(9)};
    Workflow flow{flow_id, "Selector", {key<SchemaId>("test.selector"), 1},
        {{formatter, "Format", ToolConfiguration{key<ToolTypeId>("visionpro.format.int64_list"), 1},
            {{key<InputId>("list"), {take(ConfigValue::list(DataType::integer(), list))}},
             {key<InputId>("values"), {ConfigValue::integer(10), ConfigValue::integer(20)}, BindingCardinality::MultipleSources}}},
         {index, "Index", calc, {{left, {ConfigValue::integer(1)}}, {right, {ConfigValue::integer(0)}}}},
         {consumer, "Consumer", calc, {{left, {OutputReference{flow_id, formatter, output, {ElementSelection{0}}}}},
            {right, {ConfigValue::integer(0)}}}}},
        {{take(IdFactory::generate<ControlLinkId>()), formatter, {}, index},
         {take(IdFactory::generate<ControlLinkId>()), index, {}, consumer}}, {}, {formatter}};
    const Solution draft{take(IdFactory::generate<SolutionId>()), "Selector draft", infrastructure::solution_format_version, 1, {flow}};
    const auto catalog = take(app::make_tool_registry());
    EditorWindow window(draft, catalog);
    window.show();
    QVERIFY(scene(window).reveal_node(formatter));
    auto& inspector = properties(window);
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("indices")));
    const auto set = [](QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); };
    const auto finish = [&](QDialog& dialog) { return set(dialog) ? accept_button(dialog) : nullptr; };
    const auto edit_literal = [&](const QString& target, const QString& literal, bool integer_list) {
        auto* entry = binding_button(inspector, target);
        if (!entry) { return QStringLiteral("Missing binding button: ") + target; }
        return exercise_dialog(inspector, *entry, {set, [&](QDialog& dialog) -> QPushButton* {
            auto* input = qobject_cast<QInputDialog*>(&dialog);
            if (!input || (integer_list && input->objectName() != QStringLiteral("integerListEditor"))) { return nullptr; }
            input->setTextValue(literal);
            return accept_button(dialog);
        }, finish});
    };
    for (const auto& [target, literal] : {std::pair{QStringLiteral("indices"), QStringLiteral("[0,99,1,2,3]")},
              std::pair{QStringLiteral("skip_out_of_range"), QStringLiteral("1")},
              std::pair{QStringLiteral("format"), QStringLiteral("<{value}>")}}) {
        const auto edited = edit_literal(target, literal, target != QStringLiteral("format"));
        QVERIFY2(edited.isEmpty(), qPrintable(edited));
        QCoreApplication::processEvents();
    }
    const auto reordered = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("values")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* sources = dialog.findChild<QListWidget*>(QStringLiteral("bindingSources"));
            if (!sources || sources->count() != 2) { return nullptr; }
            sources->setCurrentRow(1);
            return dialog.findChild<QPushButton*>(QStringLiteral("earlierBindingSource"));
        }, finish});
    QVERIFY2(reordered.isEmpty(), qPrintable(reordered));
    QVERIFY(scene(window).reveal_node(consumer));
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("left")));
    const auto dynamic = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("left")), {
        set, [](QDialog& dialog) -> QPushButton* {
            auto* kind = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return kind && select_choice(*kind, QStringLiteral("Node output")) ? accept_button(dialog) : nullptr;
        }, [](QDialog& dialog) -> QPushButton* {
            auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            if (!path || !source || path->text() != QStringLiteral("0") || !source->currentText().contains(QStringLiteral("used_values"))) { return nullptr; }
            return dialog.findChild<QPushButton*>(QStringLiteral("chooseIndexSource"));
        }, [](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            return source && select_choice(*source, QStringLiteral("Index ["), true) ? accept_button(dialog) : nullptr;
        }, [](QDialog& dialog) -> QPushButton* {
            auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            return path && path->text() == QStringLiteral("@1") ? accept_button(dialog) : nullptr;
        }, finish});
    QVERIFY2(dynamic.isEmpty(), qPrintable(dynamic));

    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto directory = std::filesystem::path(scratch.path().toStdWString()) / L"edited";
    window.transfer_document(directory, infrastructure::SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    EditorWindow reopened(canvas_example(), catalog);
    reopened.show();
    reopened.open_document(directory);
    QTRY_VERIFY(!reopened.file_busy() && reopened.draft().id == draft.id);
    const auto* stored = find_node(reopened.draft().workflows.front(), consumer);
    QVERIFY(stored);
    const OutputReference expected{flow_id, formatter, output, {DynamicElementSelection{IndexOutputReference{flow_id, index, value, {}}}}};
    QVERIFY(stored->bindings.front().sources == std::vector<BindingSource>{expected});
    QVERIFY(scene(reopened).reveal_node(consumer));
    QTRY_VERIFY(binding_button(properties(reopened), QStringLiteral("left")) &&
        binding_button(properties(reopened), QStringLiteral("left"))->toolTip().contains(QStringLiteral("Index [")));

    auto* controller = reopened.findChild<RunController*>();
    reopened.findChild<QAction*>(QStringLiteral("runWorkflow"))->trigger();
    QTRY_VERIFY(controller->runs().size() == 1 && controller->can_start());
    const auto& handle = controller->runs().back();
    const auto progress = take(handle.inspect());
    QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
    const auto formatted = result_for(handle, formatter);
    QCOMPARE(*std::get<DataValue>(formatted.output(key<OutputId>("text"))->content).as_string(), std::string("<6>,<9>,<20>,<10>"));
    const auto result = result_for(handle, consumer);
    QCOMPARE(*std::get<DataValue>(result.output(value)->content).as_int64(), 9);
    auto* tree = reopened.findChild<QTreeWidget*>(QStringLiteral("runNodes"));
    QTRY_COMPARE(tree->topLevelItemCount(), 3);
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        auto* item = tree->topLevelItem(row);
        if (item->toolTip(0) == QString::fromStdString(consumer.to_string())) { tree->setCurrentItem(item); }
    }
    auto* details = reopened.findChild<QPlainTextEdit*>(QStringLiteral("runDetails"));
    QVERIFY(details->toPlainText().contains(QStringLiteral("Index at selector step 1 = 1")));
}

void InspectorTests::resets_only_the_selected_session_accumulator()
{
    const auto catalog = take(app::make_tool_registry());
    const ToolConfiguration accumulator{key<ToolTypeId>("visionpro.calc.int64_accumulate"), 1};
    const auto flow_id = take(IdFactory::generate<WorkflowId>());
    const auto local = take(IdFactory::generate<NodeId>());
    const auto selected = take(IdFactory::generate<NodeId>());
    const auto other = take(IdFactory::generate<NodeId>());
    const auto value = key<InputId>("value");
    const auto output = key<OutputId>("value");
    const auto mode = key<ParameterId>("mode");
    const auto initial = key<ParameterId>("initial");
    const auto* descriptor = catalog.find_descriptor(accumulator.type);
    QVERIFY(descriptor);
    const auto parameter = std::ranges::find(descriptor->parameters, mode, &ParameterDescriptor::id);
    QVERIFY(parameter != descriptor->parameters.end());
    const auto session = take(ConfigValue::enumeration(parameter->type, key<EnumMemberId>("Session")));
    Workflow flow{flow_id, "Counters", {key<SchemaId>("test.accumulators"), 1},
        {{local, "Per run", accumulator, {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(10)}}}},
         {selected, "First session", accumulator,
            {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(20)}}, {mode, {session}}}},
         {other, "Second session", accumulator,
            {{value, {ConfigValue::integer(1)}}, {initial, {ConfigValue::integer(30)}}, {mode, {session}}}}},
        {{take(IdFactory::generate<ControlLinkId>()), local, {}, selected},
         {take(IdFactory::generate<ControlLinkId>()), selected, {}, other}}, {}, {local}};
    Solution draft{take(IdFactory::generate<SolutionId>()), "Accumulators", infrastructure::solution_format_version, 1, {flow}};
    EditorWindow window(std::move(draft), catalog);
    window.show();
    auto* run = window.findChild<QAction*>(QStringLiteral("runWorkflow"));
    auto* reset = window.findChild<QAction*>(QStringLiteral("resetSelectedState"));
    auto* controller = window.findChild<RunController*>();
    const auto verify_run = [&](std::size_t index, int first_expected, int other_expected) {
        run->trigger();
        QTRY_VERIFY(controller->runs().size() == index + 1 && controller->can_start());
        const auto& handle = controller->runs().at(index);
        const auto progress = take(handle.inspect());
        QVERIFY2(progress.status == ExecutionStatus::Succeeded, progress.diagnostics.empty() ? "" : progress.diagnostics.front().message.c_str());
        const auto total = [&](const NodeId& node) {
            const auto result = result_for(handle, node);
            return *std::get<DataValue>(result.output(output)->content).as_int64();
        };
        QCOMPARE(total(local), 11);
        QCOMPARE(total(selected), first_expected);
        QCOMPARE(total(other), other_expected);
    };
    QVERIFY(scene(window).reveal_node(selected));
    QVERIFY(!reset->isEnabled());
    verify_run(0, 21, 31);
    verify_run(1, 22, 32);
    QVERIFY(scene(window).reveal_node(local));
    QVERIFY(!reset->isEnabled());
    QVERIFY(scene(window).reveal_node(selected));
    QTRY_VERIFY(reset->isEnabled());
    auto& inspector = properties(window);
    auto rename_selected = [&]() -> QPushButton* {
        auto* rename = inspector.findChild<QPushButton*>(QStringLiteral("renameNode"));
        return rename && rename->text() == QStringLiteral("First session") ? rename : nullptr;
    };
    QTRY_VERIFY(rename_selected());
    const auto renamed = exercise_dialog(inspector, *rename_selected(), {[](QDialog& dialog) -> QPushButton* {
        auto* input = qobject_cast<QInputDialog*>(&dialog);
        if (!input) { return nullptr; }
        input->setTextValue(QStringLiteral("Renamed accumulator"));
        return accept_button(dialog);
    }});
    QVERIFY2(renamed.isEmpty(), qPrintable(renamed));
    verify_run(2, 23, 33);
    QVERIFY(scene(window).reveal_node(selected));
    QTRY_VERIFY(reset->isEnabled());
    const auto undo_index = scene(window).undoStack().index();
    reset->trigger();
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Reset session state for Renamed accumulator")));
    QCOMPARE(scene(window).undoStack().index(), undo_index);
    verify_run(3, 21, 34);
    const auto prior = result_for(controller->runs().at(2), selected);
    QCOMPARE(*std::get<DataValue>(prior.output(output)->content).as_int64(), 23);
}

void InspectorTests::edits_loop_settings_and_collects_a_selected_field()
{
    const auto draft = canvas_example();
    const auto tools = registry();
    ControlGraphModel model(draft, draft.workflows.front().id, tools);
    ControlScene scene(model);
    EditorInspector inspector(model, scene, tools);
    inspector.show();
    QVERIFY(scene.add_group("Count objects", {}, ControlScene::GroupKind::Loop));
    const auto loop_id = scene.selected_nodes().at(0);
    QVERIFY(scene.open_group(model.canvas_id(loop_id)));
    QVERIFY(scene.add_tool(key<ToolTypeId>("visionpro.sim.add"), {}));
    const auto child = scene.selected_nodes().at(0);
    QVERIFY(group_config(model, loop_id).entries == std::vector{child});
    QVERIFY(scene.parent_scope());
    QVERIFY(scene.reveal_node(loop_id));
    QTRY_VERIFY(inspector.findChild<QPushButton*>(QStringLiteral("editLoopSettings")));
    QVERIFY(binding_button(inspector, QStringLiteral("start")) && binding_button(inspector, QStringLiteral("end")));
    QVERIFY(!binding_button(inspector, QStringLiteral("index")));
    const auto settings = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("editLoopSettings")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* step = dialog.findChild<QLineEdit*>(QStringLiteral("loopStep"));
            auto* end = dialog.findChild<QCheckBox*>(QStringLiteral("loopInclusiveEnd"));
            auto* interval = dialog.findChild<QLineEdit*>(QStringLiteral("loopInterval"));
            auto* stop = dialog.findChild<QComboBox*>(QStringLiteral("loopBreakSource"));
            if (!step || !end || !interval || !stop || stop->count() != 2) { return nullptr; }
            step->setText(QStringLiteral("-2"));
            end->setChecked(true);
            interval->setText(QStringLiteral("17"));
            stop->setCurrentIndex(1);
            return accept_button(dialog);
        }});
    QVERIFY2(settings.isEmpty(), qPrintable(settings));
    const auto edited = *group_config(model, loop_id).loop;
    QVERIFY(edited.step == -2 && edited.inclusive_end && edited.interval_ms == 17 && edited.break_source == child);
    QCoreApplication::processEvents();
    auto* kind = inspector.findChild<QComboBox*>(QStringLiteral("publicPortKind"));
    QVERIFY(kind && select_choice(*kind, QStringLiteral("Output")));
    const auto add = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* id = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortId"));
            if (!id) { return nullptr; }
            id->setText(QStringLiteral("areas"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
            auto* selector = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
            if (!source || !selector || !select_choice(*source, QStringLiteral("Objects"), true)) { return nullptr; }
            selector->setText(QStringLiteral("0/area_pixels"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* mode = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return mode && select_choice(*mode, QStringLiteral("Collect")) ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(add.isEmpty(), qPrintable(add));
    const auto& result = group_config(model, loop_id);
    QCOMPARE(result.interface.outputs.size(), 1u);
    QVERIFY(result.interface.outputs.front().type == DataType::list(DataType::integer()));
    QVERIFY(result.loop->exports.front().mode == LoopExportMode::Collect);
    const OutputReference expected{draft.workflows.front().id, child, key<OutputId>("objects"),
        {ElementSelection{0}, FieldSelection{key<FieldId>("area_pixels")}}};
    QVERIFY(result.outputs.front().source == expected);
    scene.undoStack().undo();
    QVERIFY(group_config(model, loop_id).loop->exports.empty() && group_config(model, loop_id).interface.outputs.empty());
}

void InspectorTests::commits_binding_and_group_outlet_through_dialogs()
{
    const auto draft = canvas_example();
    const auto& flow = draft.workflows.front();
    const auto producer = node_named(flow, "Reference").id;
    const auto consumer = node_named(flow, "Finish").id;
    const auto group = node_named(flow, "Review").id;
    const auto internal = node_named(flow, "Review check").id;
    QVERIFY(std::ranges::none_of(flow.control_links, [&](const auto& link) {
        return link.source == producer && link.target == consumer;
    }));
    const auto tools = registry();
    ControlGraphModel model(draft, flow.id, tools);
    ControlScene scene(model);
    EditorInspector inspector(model, scene, tools);
    inspector.resize(660, 760);
    inspector.show();
    QVERIFY(QTest::qWaitForWindowExposed(&inspector));
    const QSignalSpy failures(&scene, &ControlScene::edit_failed);
    const auto target = key<InputId>("left");
    const auto output = key<OutputId>("objects");
    const auto selected_area = key<FieldId>("area_pixels");
    const auto approved = key<ControlOutputId>("approved");
    auto& history = scene.undoStack();

    QVERIFY(scene.reveal_node(consumer));
    QTRY_VERIFY(binding_button(inspector, QStringLiteral("left")));
    const auto choose_objects = [](QDialog& dialog, const QString& selector) -> QPushButton* {
        auto* source = dialog.findChild<QComboBox*>(QStringLiteral("bindingSource"));
        auto* path = dialog.findChild<QLineEdit*>(QStringLiteral("bindingSelector"));
        if (!source || !path) { return nullptr; }
        for (int index = 0; index < source->count(); ++index) {
            const auto item = source->itemText(index);
            if (!item.startsWith(QStringLiteral("Reference [")) || !item.contains(QStringLiteral("Objects"))) { continue; }
            source->setCurrentIndex(index);
            if (!path->isEnabled()) { return nullptr; }
            path->setText(selector);
            return accept_button(dialog);
        }
        return nullptr;
    };
    bool reported_bad_field = false;
    const auto binding_result = exercise_dialog(inspector, *binding_button(inspector, QStringLiteral("left")), {
        [](QDialog& dialog) { return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")); },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return choices && select_choice(*choices, QStringLiteral("Node output")) ? accept_button(dialog) : nullptr;
        },
        [&](QDialog& dialog) { return choose_objects(dialog, QStringLiteral("0/unknown_field")); },
        [&](QDialog& dialog) -> QPushButton* {
            auto* warning = qobject_cast<QMessageBox*>(&dialog);
            if (!warning) { return nullptr; }
            reported_bad_field = warning->text().contains(QStringLiteral("field"), Qt::CaseInsensitive);
            return qobject_cast<QPushButton*>(warning->button(QMessageBox::Ok));
        },
        [&](QDialog& dialog) { return choose_objects(dialog, QStringLiteral("0/area_pixels")); },
        [](QDialog& dialog) -> QPushButton* {
            return dialog.findChild<QPushButton*>(QStringLiteral("setBindingSource")) ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(binding_result.isEmpty(), qPrintable(binding_result));
    QVERIFY(reported_bad_field);
    const OutputReference expected{flow.id, producer, output, {ElementSelection{0}, FieldSelection{selected_area}}};
    const auto* binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources.size() == 1);
    const auto* reference = std::get_if<OutputReference>(&binding->sources.front());
    QVERIFY(reference && *reference == expected);
    QCOMPARE(history.count(), 1);

    QVERIFY(scene.reveal_node(group));
    QTRY_VERIFY(inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")));
    auto* kinds = inspector.findChild<QComboBox*>(QStringLiteral("publicPortKind"));
    QVERIFY(kinds && select_choice(*kinds, QStringLiteral("Control outlet")));
    const auto port_result = exercise_dialog(inspector, *inspector.findChild<QPushButton*>(QStringLiteral("addPublicPort")), {
        [](QDialog& dialog) -> QPushButton* {
            auto* id = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortId"));
            auto* name = dialog.findChild<QLineEdit*>(QStringLiteral("publicPortName"));
            if (!id || !name) { return nullptr; }
            id->setText(QStringLiteral("approved"));
            name->setText(QStringLiteral("Approved"));
            return accept_button(dialog);
        },
        [](QDialog& dialog) -> QPushButton* {
            auto* choices = dialog.findChild<QComboBox*>(QStringLiteral("choice"));
            return choices && select_choice(*choices, QStringLiteral("Review check ["), true)
                ? accept_button(dialog) : nullptr;
        }});
    QVERIFY2(port_result.isEmpty(), qPrintable(port_result));
    const auto& edited_group = group_config(model, group);
    const auto mapping = std::ranges::find(edited_group.control_exports, approved, &ControlOutputMapping::output);
    QVERIFY(mapping != edited_group.control_exports.end() && mapping->source == internal && !mapping->source_output);
    QCOMPARE(edited_group.interface.control_outputs.size(), std::size_t{2});
    QCOMPARE(model.nodeData(model.canvas_id(group), QtNodes::NodeRole::OutPortCount).toUInt(), 3u);
    const auto shows_approved = [&] {
        const auto* ports = inspector.findChild<QListWidget*>(QStringLiteral("publicPorts"));
        return ports && !ports->findItems(QStringLiteral("Approved [approved]"), Qt::MatchContains).empty();
    };
    QTRY_VERIFY(shows_approved());
    QCOMPARE(history.count(), 2);
    QCOMPARE(failures.count(), 0);
    QCOMPARE(model.draft().workflows.front().control_links.size(), flow.control_links.size());
    const auto diagnostics = take(diagnose_solution(model.draft(), tools.descriptors()));
    QVERIFY2(diagnostics.empty(), diagnostics.empty() ? "" : diagnostics.front().message.c_str());

    history.undo();
    QCOMPARE(group_config(model, group).control_exports.size(), std::size_t{1});
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{expected});
    history.undo();
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{ConfigValue::integer(1)});
    QVERIFY(history.isClean());
    history.redo();
    history.redo();
    QCOMPARE(group_config(model, group).control_exports.size(), std::size_t{2});
    binding = binding_for(model, consumer, target);
    QVERIFY(binding && binding->sources == std::vector<BindingSource>{expected});
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::InspectorTests)
#include "inspector_tests.moc"
