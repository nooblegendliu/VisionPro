#include "canvas_example.hpp"
#include "control_scene.hpp"
#include "editor_window.hpp"

#include <simulation.hpp>

#include <QtNodes/GraphicsView>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
namespace fs = std::filesystem;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

ToolRegistry registry() { return take(ToolRegistry::create(tools::simulation_registrations())); }

ControlScene& scene(EditorWindow& window)
{
    const auto* view = qobject_cast<QtNodes::GraphicsView*>(window.centralWidget());
    return *qobject_cast<ControlScene*>(view->scene());
}

ControlGraphModel& model(EditorWindow& window) { return static_cast<ControlGraphModel&>(scene(window).graphModel()); }

QAction& action(EditorWindow& window, const char* object_name)
{ return *window.findChild<QAction*>(QString::fromLatin1(object_name)); }

QString path_text(const fs::path& path) { return QString::fromStdWString(path.native()); }

QByteArray read_manifest(const fs::path& directory)
{
    QFile file(path_text(directory / solution_manifest_name));
    if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Could not read the test manifest."); }
    return file.readAll();
}

void write_manifest(const fs::path& directory, const QByteArray& bytes)
{
    fs::create_directories(directory);
    QFile file(path_text(directory / solution_manifest_name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size()) {
        throw std::runtime_error("Could not write the test manifest.");
    }
}

class EditorTests final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void new_document_creates_empty_workflow();
    void workflow_and_clipboard_share_history();
    void locates_cross_scope_link();
    void file_operations_use_the_editor_draft();
    void imports_reusable_subgraphs_as_undoable_edits();
    void saved_canvas_runs_in_cli_process();
};

void EditorTests::imports_reusable_subgraphs_as_undoable_edits()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const fs::path root(scratch.path().toStdWString());
    const auto initial = canvas_example();
    const auto& flow = initial.workflows.front();
    const auto original_group = flow.containment.front().parent;
    const auto catalog = registry();
    EditorWindow window(initial, catalog);
    window.show();
    const auto host = root / "host";
    window.transfer_document(host, SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    const auto original = read_manifest(host);
    auto& history = scene(window).undoStack();
    QVERIFY(scene(window).reveal_node(original_group));
    QVERIFY(action(window, "exportGroup").isEnabled() && action(window, "exportWorkflow").isEnabled());
    const auto group_package = root / L"组包";
    window.export_subgraph(group_package, original_group);
    QTRY_VERIFY(!window.file_busy());
    const auto workflow_package = root / L"流程包";
    window.export_subgraph(workflow_package);
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(history.count(), 0);

    window.import_subgraph(group_package);
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(history.count(), 1);
    const auto first = scene(window).selected_nodes().at(0);
    QVERIFY(first != original_group);
    window.import_subgraph(group_package);
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(history.count(), 2);
    const auto second = scene(window).selected_nodes().at(0);
    QVERIFY(second != first && second != original_group);
    QVERIFY(std::get<GroupConfiguration>(find_node(window.draft().workflows.front(), first)->configuration).entries
        != std::get<GroupConfiguration>(find_node(window.draft().workflows.front(), second)->configuration).entries);

    window.import_subgraph(root / "missing");
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(history.index(), 2);
    QVERIFY(scene(window).selected_nodes() == std::vector{second});
    QCOMPARE(read_manifest(host), original);
    history.undo();
    QVERIFY(!find_node(window.draft().workflows.front(), second));
    history.redo();
    QVERIFY(find_node(window.draft().workflows.front(), second));
    window.import_subgraph(workflow_package);
    QTRY_VERIFY(!window.file_busy());
    QCOMPARE(window.draft().workflows.size(), 2u);
    QVERIFY(model(window).workflow_id() == window.draft().workflows.back().id && model(window).workflow_id() != flow.id);
    window.save_document();
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(history.isClean());
    const auto saved = take(load_solution(host, catalog));
    QCOMPARE(saved.solution().workflows.size(), 2u);
    QVERIFY(find_node(saved.solution().workflows.front(), first) && find_node(saved.solution().workflows.front(), second));
}

void EditorTests::new_document_creates_empty_workflow()
{
    const auto initial = canvas_example();
    EditorWindow window(initial, registry());
    window.show();
    QTimer enter_name;
    bool entered = false;
    connect(&enter_name, &QTimer::timeout, &window, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            entered = true;
            dialog->setTextValue(QStringLiteral("New workflow document"));
            dialog->accept();
        }
    });
    QTimer finish;
    finish.setSingleShot(true);
    connect(&finish, &QTimer::timeout, &window, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) { dialog->reject(); }
    });
    enter_name.start(10);
    finish.start(4000);
    action(window, "newSolution").trigger();
    enter_name.stop();
    QVERIFY(entered);
    QVERIFY(window.draft().id != initial.id);
    QCOMPARE(QString::fromStdString(window.draft().name), QStringLiteral("New workflow document"));
    QCOMPARE(window.draft().workflows.size(), std::size_t{1});
    QVERIFY(window.draft().workflows.front().nodes.empty());
    QVERIFY(scene(window).add_tool(registry().descriptors().front().id, QPointF(20, 20)));
    QCOMPARE(window.draft().workflows.front().nodes.size(), std::size_t{1});
    QVERIFY(window.isWindowModified());
}

void EditorTests::workflow_and_clipboard_share_history()
{
    const auto initial = canvas_example();
    const auto& original = initial.workflows.front();
    const auto route = original.nodes[0].id;
    const auto left = original.nodes[1].id;
    EditorWindow window(initial, registry());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    QVERIFY(scene(window).reveal_node(route));
    scene(window).nodeGraphicsObject(model(window).canvas_id(left))->setSelected(true);
    QVERIFY(action(window, "copyNodes").isEnabled());
    action(window, "copyNodes").trigger();
    action(window, "pasteNodes").trigger();
    QCOMPARE(window.draft().workflows.front().nodes.size(), original.nodes.size() + 2);
    QCOMPARE(window.draft().workflows.front().control_links.size(), original.control_links.size() + 1);
    const auto& pasted = window.draft().workflows.front();
    const auto copied_edge = pasted.control_links.back();
    QVERIFY(copied_edge.id != original.control_links.front().id && copied_edge.source != route && copied_edge.target != left);
    QCOMPARE(copied_edge.output->text(), original.control_links.front().output->text());
    QCOMPARE(window.draft().layouts.front().nodes.size(), initial.layouts.front().nodes.size() + 2);

    action(window, "removeNodes").trigger();
    QCOMPARE(window.draft().workflows.front().nodes.size(), original.nodes.size());
    action(window, "undoDraft").trigger();
    QVERIFY(find_node(window.draft().workflows.front(), copied_edge.target));
    QVERIFY(scene(window).add_workflow("Another workflow"));
    QCOMPARE(window.draft().workflows.size(), std::size_t{2});
    const auto new_flow = window.draft().workflows.back().id;
    QVERIFY(model(window).workflow_id() == new_flow);
    QVERIFY(scene(window).add_tool(registry().descriptors().front().id, QPointF(30, 40)));
    QCOMPARE(window.draft().workflows.back().nodes.size(), std::size_t{1});
    QVERIFY(scene(window).select_workflow(original.id));
    action(window, "undoDraft").trigger();
    QCOMPARE(window.draft().workflows.back().nodes.size(), std::size_t{0});
    action(window, "undoDraft").trigger();
    QCOMPARE(window.draft().workflows.size(), std::size_t{1});
    action(window, "undoDraft").trigger();
    QCOMPARE(window.draft().workflows.front().nodes.size(), original.nodes.size());
    QVERIFY(!window.isWindowModified());
    action(window, "redoDraft").trigger();
    QVERIFY(find_node(window.draft().workflows.front(), copied_edge.target));
}

void EditorTests::locates_cross_scope_link()
{
    const auto initial = canvas_example();
    const auto& original = initial.workflows.front();
    const auto moved_node = original.nodes[1].id;
    const auto group = original.nodes[5].id;
    EditorWindow window(initial, registry());
    window.show();
    QVERIFY(scene(window).edit_draft(QStringLiteral("Move node into Group"), [&](Solution& candidate) -> Result<std::monostate> {
        candidate.workflows.front().containment.push_back({group, moved_node});
        return std::monostate{};
    }));
    auto* problems = window.findChild<QListWidget*>(QStringLiteral("editorDiagnostics"));
    QTRY_VERIFY(problems->count() == 1 && problems->item(0)->text().contains(QStringLiteral("Group boundary")));
    problems->setCurrentRow(0);
    QVERIFY(action(window, "removeDiagnosedLink").isEnabled());
    action(window, "undoDraft").trigger();
    QVERIFY(!action(window, "removeDiagnosedLink").isEnabled());
    QTRY_VERIFY(problems->count() == 0);
    action(window, "redoDraft").trigger();
    QTRY_VERIFY(problems->count() == 1 && problems->item(0)->text().contains(QStringLiteral("Group boundary")));
    problems->setCurrentRow(0);
    Q_EMIT problems->itemDoubleClicked(problems->item(0));
    QVERIFY(model(window).scope() == group);
    QVERIFY(scene(window).selected_nodes() == std::vector<NodeId>{moved_node});
    QVERIFY(action(window, "removeDiagnosedLink").isEnabled());
    action(window, "removeDiagnosedLink").trigger();
    QCOMPARE(window.draft().workflows.front().control_links.size(), original.control_links.size() - 1);
    action(window, "undoDraft").trigger();
    QCOMPARE(window.draft().workflows.front().control_links.size(), original.control_links.size());
}

void EditorTests::file_operations_use_the_editor_draft()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const fs::path root(scratch.path().toStdWString());
    const auto first = root / L"first";
    const auto other = root / L"other";
    const auto independent = root / L"independent";
    const auto initial = canvas_example();
    const auto flow_id = initial.workflows.front().id;
    auto catalog = registry();
    EditorWindow window(initial, catalog);
    window.show();

    window.transfer_document(first, SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    const auto original = take(load_solution(first, catalog));
    QVERIFY(original.solution().id == initial.id);
    QVERIFY(!window.isWindowModified());
    QVERIFY(!action(window, "saveSolution").isEnabled());

    const auto moved = original.solution().workflows.front().nodes[4].id;
    const CanvasMove placement{model(window).canvas_id(moved), QPointF(400, 240)};
    QVERIFY(scene(window).move_nodes(std::span{&placement, 1}));
    action(window, "saveSolution").trigger();
    QTRY_VERIFY(!window.file_busy());
    QTRY_COMPARE(window.draft().revision, original.solution().revision + 1);
    auto updated = take(load_solution(first, catalog));
    const auto stored_position = std::ranges::find(updated.solution().layouts.front().nodes, moved, &NodePlacement::node);
    QCOMPARE(QPointF(stored_position->x, stored_position->y), placement.position);
    action(window, "undoDraft").trigger();
    QCOMPARE(window.draft().revision, updated.solution().revision);
    QVERIFY(window.isWindowModified());
    action(window, "redoDraft").trigger();
    QVERIFY(!window.isWindowModified());

    window.transfer_document(other, SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    const auto save_as = take(load_solution(other, catalog));
    QVERIFY(save_as.solution().id == initial.id && save_as.solution().workflows.front().id == flow_id);
    window.transfer_document(independent, SolutionTransfer::IndependentCopy);
    QTRY_VERIFY(!window.file_busy());
    const auto duplicated = take(load_solution(independent, catalog));
    QVERIFY(duplicated.solution().id == window.draft().id && duplicated.solution().id != initial.id);
    const auto& duplicated_flow = duplicated.solution().workflows.front();
    QVERIFY(duplicated_flow.id != flow_id);
    const auto& shared_input = std::get<OutputReference>(duplicated_flow.nodes[4].bindings.front().sources.front());
    QVERIFY(shared_input.workflow == duplicated_flow.id && shared_input.node == duplicated_flow.nodes[1].id);
    QCOMPARE(duplicated.solution().layouts.front().nodes[4].x, placement.position.x());

    const auto broken = root / L"broken";
    write_manifest(broken, "{broken");
    window.open_document(broken);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(window.draft().id == duplicated.solution().id);

    const auto future = root / L"future";
    auto serialized = read_manifest(first);
    const auto opening = serialized.indexOf('{');
    QVERIFY(opening >= 0);
    serialized.insert(opening + 1, "\n\"future_feature\": true,");
    write_manifest(future, serialized);
    window.open_document(future);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(window.draft().id == initial.id);
    QVERIFY(!action(window, "saveSolutionAs").isEnabled());
    QVERIFY(!scene(window).add_workflow("Would discard unknown fields"));
    QCOMPARE(read_manifest(future), serialized);
}

void EditorTests::saved_canvas_runs_in_cli_process()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const fs::path directory(scratch.path().toStdWString());
    auto configured = canvas_example();
    auto& workflow = configured.workflows.front();
    const auto id = workflow.id;
    workflow.nodes.erase(workflow.nodes.begin() + 5, workflow.nodes.end());
    workflow.control_links.erase(workflow.control_links.begin() + 6, workflow.control_links.end());
    workflow.containment.clear();
    auto& placements = configured.layouts.front().nodes;
    placements.erase(placements.begin() + 5, placements.end());
    EditorWindow window(std::move(configured), registry());
    window.show();
    window.transfer_document(directory, SolutionTransfer::SaveAs);
    QTRY_VERIFY(!window.file_busy());
    QVERIFY(fs::exists(directory / solution_manifest_name));

    QProcess cli;
    cli.start(QStringLiteral(VISIONPRO_TEST_CLI), {QStringLiteral("run"), QStringLiteral("--solution"),
        path_text(directory / solution_manifest_name), QStringLiteral("--workflow"), QString::fromStdString(id.to_string())});
    QVERIFY(cli.waitForFinished(10000));
    const auto report = cli.readAllStandardOutput();
    QVERIFY2(cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0, cli.readAllStandardError().constData());
    QCOMPARE(QJsonDocument::fromJson(report).object().value(QStringLiteral("status")).toString(), QStringLiteral("Succeeded"));
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::EditorTests)
#include "editor_tests.moc"
