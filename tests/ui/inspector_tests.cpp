#include "canvas_example.hpp"
#include "control_scene.hpp"
#include "editor_inspector.hpp"

#include <simulation.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
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
};

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
