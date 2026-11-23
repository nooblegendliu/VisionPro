#include "control_graph_model.hpp"
#include "control_scene.hpp"
#include "simulation.hpp"
#include "workspace_appearance.hpp"
#include "workspace_style.hpp"
#include "workspace_toolbox.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <QtNodes/GraphicsView>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QFontMetrics>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainterPath>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace {

using namespace visionpro;

template<class T>
T take(core::Result<T> result)
{
    if (const auto* diagnostic = std::get_if<core::Diagnostic>(&result)) {
        throw std::runtime_error(diagnostic->message);
    }
    return std::get<T>(std::move(result));
}

template<core::UuidIdentity Id>
Id uuid(char suffix)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(core::IdFactory::parse<Id>(text));
}

template<core::SchemaIdentity Id>
Id key(std::string_view text)
{
    return take(core::SchemaIdFactory::parse<Id>(text));
}

struct Fixture {
    const core::WorkflowId workflow_id = uuid<core::WorkflowId>('1');
    const core::NodeId route = uuid<core::NodeId>('1');
    const core::NodeId first = uuid<core::NodeId>('2');
    const core::NodeId second = uuid<core::NodeId>('3');
    const core::NodeId join = uuid<core::NodeId>('4');
    const core::NodeId group = uuid<core::NodeId>('5');
    const core::NodeId finish = uuid<core::NodeId>('6');
    const core::NodeId inside = uuid<core::NodeId>('7');
    const core::NodeId nested = uuid<core::NodeId>('8');
    const core::NodeId leaf = uuid<core::NodeId>('9');
    const core::NodeId spare = uuid<core::NodeId>('a');
    const core::ControlOutputId left = key<core::ControlOutputId>("left");
    const core::ControlOutputId right = key<core::ControlOutputId>("right");
    const core::ControlOutputId done = key<core::ControlOutputId>("done");
    const workflow::OutputReference data_source{workflow_id, first, key<core::OutputId>("value"), {}};
    const core::ToolRegistry registry = take(core::ToolRegistry::create(tools::simulation_registrations()));

    workflow::Solution solution() const
    {
        const auto add_type = key<core::ToolTypeId>("visionpro.sim.add");
        const auto make_add = [&](const core::NodeId& id, std::string name) {
            return workflow::Node{id, std::move(name), workflow::ToolConfiguration{add_type, 1}, {
                {key<core::InputId>("left"), {core::ConfigValue::integer(1)}},
                {key<core::InputId>("right"), {core::ConfigValue::integer(2)}}}};
        };
        auto finish_node = make_add(finish, "Finish");
        finish_node.bindings.front().sources = {data_source};

        const workflow::Node route_node{route, "Route",
            workflow::ToolConfiguration{key<core::ToolTypeId>("visionpro.sim.select"), 1},
            {{key<core::InputId>("mask"), {core::ConfigValue::integer(3)}}}};
        const workflow::Node group_node{group, "Group", workflow::GroupConfiguration{
            {key<core::SchemaId>("canvas.group"), 1, {}, {}, {{done, "Completed"}}}, {inside}, {}, {{done, inside, {}}}}};
        const workflow::Node nested_node{nested, "Nested Group", workflow::GroupConfiguration{
            {key<core::SchemaId>("canvas.nested"), 1}, {leaf}, {}, {}}};
        const workflow::Workflow flow{workflow_id, "Canvas tests", {key<core::SchemaId>("canvas.workflow"), 1},
            {route_node, make_add(first, "Inspect"), make_add(second, "Inspect"), make_add(join, "Join"),
                group_node, finish_node, make_add(inside, "Inside"), nested_node,
                make_add(leaf, "Leaf"), make_add(spare, "Spare")},
            {{uuid<core::ControlLinkId>('1'), route, left, first},
                {uuid<core::ControlLinkId>('2'), route, left, second},
                {uuid<core::ControlLinkId>('3'), route, right, second},
                {uuid<core::ControlLinkId>('4'), first, {}, join},
                {uuid<core::ControlLinkId>('5'), second, {}, join},
                {uuid<core::ControlLinkId>('6'), join, {}, finish},
                {uuid<core::ControlLinkId>('7'), group, done, finish},
                {uuid<core::ControlLinkId>('8'), inside, {}, nested}},
            {{group, inside}, {group, nested}, {nested, leaf}}, {route, group, spare}};
        return {uuid<core::SolutionId>('1'), "Canvas test solution", 1, 1, {flow}, {}, {}, {}, {},
            {{workflow_id, {{route, 0, 40}, {first, 210, -40}, {second, 210, 75}, {spare, 210, 190},
                {join, 435, 75}, {finish, 650, 75}, {group, 435, 200}, {nested, 250, 0}}}}};
    }
};

const workflow::Workflow& draft_workflow(const ui::ControlGraphModel& graph)
{
    return graph.draft().workflows.front();
}

const workflow::NodePlacement* placement(const ui::ControlGraphModel& graph, const core::NodeId& node)
{
    const auto& layouts = graph.draft().layouts;
    const auto layout = std::ranges::find(layouts, draft_workflow(graph).id, &workflow::WorkflowLayout::workflow);
    if (layout == layouts.end()) { return nullptr; }
    const auto found = std::ranges::find(layout->nodes, node, &workflow::NodePlacement::node);
    return found == layout->nodes.end() ? nullptr : &*found;
}

void drag(QtNodes::GraphicsView& view, const QPoint& from, const QPoint& to)
{
    auto* target = view.viewport();
    QTest::mousePress(target, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(target, (from + to) / 2);
    QTest::mouseMove(target, to);
    QTest::mouseRelease(target, Qt::LeftButton, Qt::NoModifier, to);
}

QPoint view_port_position(QtNodes::GraphicsView& view, ui::ControlScene& scene,
    QtNodes::NodeId node, QtNodes::PortType port_type, QtNodes::PortIndex port)
{
    const auto* item = scene.nodeGraphicsObject(node);
    return view.mapFromScene(scene.nodeGeometry().portScenePosition(node, port_type, port, item->sceneTransform()));
}

class CanvasTests final : public QObject {
    Q_OBJECT

    QTemporaryDir settings_;

private Q_SLOTS:
    void initTestCase();
    void projects_control_graph();
    void repaints_existing_nodes_and_connections();
    void measures_chinese_titles_and_branch_labels();
    void reveals_unused_branch_when_wiring();
    void replays_edits_across_group_scopes();
    void protects_existing_links_with_shared_endpoints();
    void handles_mouse_gestures();
};

void CanvasTests::initTestCase()
{
    QVERIFY(settings_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
}

void CanvasTests::repaints_existing_nodes_and_connections()
{
    auto& appearance = ui::WorkspaceAppearance::instance();
    appearance.setMode(ui::AppearanceMode::Light);
    const Fixture fixture;
    ui::ControlGraphModel graph(fixture.solution(), fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    ui::WorkspaceToolbox toolbox(fixture.registry);
    ui::WorkflowCanvas view(&scene, &toolbox, [](const QString&, QPointF) {});
    view.resize(1000, 550);
    view.show();
    QCoreApplication::processEvents();
    view.restore_view(1.2, scene.itemsBoundingRect().center());
    const auto center = view.view_center();
    const auto group = graph.canvas_id(fixture.group);
    const QtNodes::ConnectionId id{group, 1, graph.canvas_id(fixture.finish), 0};
    auto* card = scene.nodeGraphicsObject(group);
    auto* wire = scene.connectionGraphicsObject(id);
    QVERIFY(card && wire);
    const auto position = card->pos() + QPointF(-10, 0);
    QVERIFY(scene.move_nodes(std::array{ui::CanvasMove{group, position}}));
    card->setSelected(true);
    wire->setSelected(true);

    const auto contains_color = [](const QImage& image, QColor color) {
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y) == color) { return true; }
            }
        }
        return false;
    };
    const auto [c1, c2] = wire->pointsC1C2();
    QPainterPath path(wire->out());
    path.cubicTo(c1, c2, wire->in());
    const auto wire_point = view.mapFromScene(wire->mapToScene(path.pointAtPercent(0.55)));
    const QRect wire_rect(wire_point - QPoint(5, 5), QSize(11, 11));
    const QPoint card_point = view.mapFromScene(card->mapToScene(QPointF(8, 6)));
    const QRect card_rect(card_point - QPoint(2, 2), QSize(5, 5));
    QVERIFY(view.viewport()->rect().contains(card_rect) && view.viewport()->rect().contains(wire_rect));
    for (const auto mode : {ui::AppearanceMode::Light, ui::AppearanceMode::Dark, ui::AppearanceMode::Light}) {
        appearance.setMode(mode);
        const auto& colors = ui::workspace_colors();
        QTRY_VERIFY(contains_color(view.viewport()->grab(card_rect).toImage(), colors.node));
        QTRY_VERIFY(contains_color(view.viewport()->grab(wire_rect).toImage(), colors.connectionSelected));
        const auto background = view.viewport()->grab(QRect(3, 3, 72, 72)).toImage();
        QVERIFY(contains_color(background, colors.grid));
        QVERIFY(contains_color(background, colors.canvas));
        QCOMPARE(scene.nodeGraphicsObject(group), card);
        QCOMPARE(scene.connectionGraphicsObject(id), wire);
        QVERIFY(card->isSelected() && wire->isSelected());
        QCOMPARE(card->pos(), position);
        QCOMPARE(view.getScale(), 1.2);
        QCOMPARE(view.view_center(), center);
        QCOMPARE(scene.undoStack().index(), 1);
    }
    appearance.setMode(ui::AppearanceMode::System);
}

void CanvasTests::measures_chinese_titles_and_branch_labels()
{
    const Fixture fixture;
    auto solution = fixture.solution();
    auto& group = *std::ranges::find(solution.workflows.front().nodes, fixture.group, &workflow::Node::id);
    group.name = "第五路检测工件定位误差范围最终判定";
    group.enabled = false;
    std::get<workflow::GroupConfiguration>(group.configuration).interface.control_outputs.front().display_name
        = "允许范围内";
    ui::ControlGraphModel graph(std::move(solution), fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    const auto node = graph.canvas_id(fixture.group);
    const auto title = graph.nodeData(node, QtNodes::NodeRole::Caption).toString();
    const QFontMetrics title_font(ui::workspace_font(ui::WorkspaceText::NodeTitle));
    const QFontMetrics branch_font(ui::workspace_font(ui::WorkspaceText::Secondary));
    for (const auto direction : {Qt::Horizontal, Qt::Vertical}) {
        scene.setOrientation(direction);
        const auto& geometry = scene.nodeGeometry();
        const QRectF card(QPointF{}, geometry.size(node));
        const auto title_box = geometry.captionRect(node);
        QVERIFY(card.contains(title_box));
        QVERIFY(title.contains(QStringLiteral("已禁用")));
        QVERIFY(title_box.height() > title_font.height());
        const auto label = graph.portData(node, QtNodes::PortType::Out, 1, QtNodes::PortRole::Caption).toString();
        QCOMPARE(label, QStringLiteral("允许范围内"));
        const auto label_box = QRectF(branch_font.tightBoundingRect(label))
            .translated(geometry.portTextPosition(node, QtNodes::PortType::Out, 1));
        QVERIFY(card.contains(label_box));
        QVERIFY(!title_box.intersects(label_box));
        QCOMPARE(geometry.checkPortHit(node, QtNodes::PortType::Out,
            geometry.portPosition(node, QtNodes::PortType::Out, 1)), QtNodes::PortIndex{1});
    }
}

void CanvasTests::projects_control_graph()
{
    const Fixture fixture;
    ui::ControlGraphModel graph(fixture.solution(), fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    const auto route = graph.canvas_id(fixture.route);
    const auto first = graph.canvas_id(fixture.first);
    const auto second = graph.canvas_id(fixture.second);
    const auto join = graph.canvas_id(fixture.join);
    const auto group = graph.canvas_id(fixture.group);
    const auto finish = graph.canvas_id(fixture.finish);

    QVERIFY(first != second);
    QCOMPARE(graph.allNodeIds().size(), std::size_t{7});
    QVERIFY(scene.nodeGraphicsObject(group));
    QVERIFY(!scene.nodeGraphicsObject(graph.canvas_id(fixture.inside)));
    QVERIFY(scene.nodeGeometry().size(group).isValid());
    QVERIFY(!scene.nodeGeometry().size(group).isEmpty());
    const auto style = QJsonDocument::fromVariant(graph.nodeData(group, QtNodes::NodeRole::Style))
        .object().value(QStringLiteral("NodeStyle")).toObject();
    const QColor title(style.value(QStringLiteral("FontColor")).toString());
    const QColor fill(style.value(QStringLiteral("GradientColor0")).toString());
    QVERIFY(title.isValid() && fill.isValid() && std::abs(fill.lightness() - title.lightness()) > 128);
    QCOMPARE(graph.layout_orientation(), Qt::Horizontal);
    QCOMPARE(graph.nodeData(route, QtNodes::NodeRole::OutPortCount).toUInt(), 3u);
    QCOMPARE(graph.nodeData(group, QtNodes::NodeRole::OutPortCount).toUInt(), 2u);
    QCOMPARE(graph.portData(route, QtNodes::PortType::Out, 1, QtNodes::PortRole::Caption).toString(), QStringLiteral("Left"));
    QCOMPARE(graph.portData(route, QtNodes::PortType::Out, 2, QtNodes::PortRole::Caption).toString(), QStringLiteral("Right"));
    QCOMPARE(graph.portData(group, QtNodes::PortType::Out, 1, QtNodes::PortRole::Caption).toString(), QStringLiteral("Completed"));
    QCOMPARE(graph.connections(route, QtNodes::PortType::Out, 1).size(), std::size_t{2});
    QCOMPARE(graph.connections(join, QtNodes::PortType::In, 0).size(), std::size_t{2});

    for (const auto& link : std::array{QtNodes::ConnectionId{route, 1, first, 0},
        QtNodes::ConnectionId{route, 1, second, 0}, QtNodes::ConnectionId{route, 2, second, 0},
        QtNodes::ConnectionId{first, 0, join, 0}, QtNodes::ConnectionId{second, 0, join, 0},
        QtNodes::ConnectionId{group, 1, finish, 0}}) {
        QVERIFY(scene.connectionGraphicsObject(link));
    }
    QVERIFY(!scene.connectionGraphicsObject({first, 0, finish, 0}));
    QVERIFY(!scene.connectionGraphicsObject({group, 0, graph.canvas_id(fixture.inside), 0}));
}

void CanvasTests::reveals_unused_branch_when_wiring()
{
    using QtNodes::PortRole;
    using QtNodes::PortType;
    const Fixture fixture;
    auto solution = fixture.solution();
    std::erase_if(solution.workflows.front().control_links, [&](const auto& link) {
        return link.source == fixture.route && link.output == fixture.right;
    });
    ui::ControlGraphModel graph(std::move(solution), fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    QtNodes::GraphicsView view(&scene);
    view.resize(1000, 550);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.resetTransform();
    view.centerOn(scene.itemsBoundingRect().center());
    const auto route = graph.canvas_id(fixture.route);
    const auto first = graph.canvas_id(fixture.first);
    QVERIFY(graph.portData(first, PortType::In, 0, PortRole::Caption).toString().isEmpty());
    QVERIFY(graph.portData(first, PortType::Out, 0, PortRole::Caption).toString().isEmpty());

    auto* item = scene.nodeGraphicsObject(route);
    const auto baseline = scene.nodeGeometry().portTextPosition(route, PortType::Out, 2);
    const auto label = graph.portData(route, PortType::Out, 2, PortRole::Caption).toString();
    QCOMPARE(label, QStringLiteral("Right"));
    // Compare just the pending branch's label at the current font and layout.
    const auto label_rect = QFontMetrics(ui::workspace_font(ui::WorkspaceText::Secondary)).tightBoundingRect(label)
        .translated(view.mapFromScene(item->mapToScene(baseline)));
    QVERIFY(view.viewport()->rect().contains(label_rect));
    const auto label_pixels = [&] { return view.viewport()->grab(label_rect).toImage(); };
    QTest::mouseMove(view.viewport(), QPoint(2, 2));
    const auto unused = label_pixels();
    QVERIFY(!unused.isNull());
    QTest::mouseMove(view.viewport(), view.mapFromScene(item->sceneBoundingRect().center()));
    QTRY_VERIFY(item->nodeState().hovered());
    QVERIFY(label_pixels() != unused);

    const QtNodes::ConnectionId added{route, 2, first, 0};
    drag(view, view_port_position(view, scene, route, PortType::Out, 2),
        view_port_position(view, scene, first, PortType::In, 0));
    QTest::mouseMove(view.viewport(), QPoint(2, 2));
    QTRY_VERIFY(!item->nodeState().hovered());
    QVERIFY(scene.connectionGraphicsObject(added));
    QVERIFY(label_pixels() != unused);
    QCOMPARE(graph.connections(route, PortType::Out, 1).size(), std::size_t{2});
    scene.undoStack().undo();
    QVERIFY(!scene.connectionGraphicsObject(added));
    QCOMPARE(label_pixels(), unused);
}

void CanvasTests::replays_edits_across_group_scopes()
{
    const Fixture fixture;
    const auto solution = fixture.solution();
    ui::ControlGraphModel graph(solution, fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    auto& history = scene.undoStack();
    const auto route = graph.canvas_id(fixture.route);
    const auto first = graph.canvas_id(fixture.first);
    const auto group = graph.canvas_id(fixture.group);
    const auto inside = graph.canvas_id(fixture.inside);
    const auto nested = graph.canvas_id(fixture.nested);
    const auto leaf = graph.canvas_id(fixture.leaf);
    const auto& links = draft_workflow(graph).control_links;

    QVERIFY(!scene.connect_control({route, 1, first, 0}));
    QVERIFY(!scene.connect_control({group, 1, inside, 0}));
    QCOMPARE(history.count(), 0);
    const QtNodes::ConnectionId added{route, 2, first, 0};
    QVERIFY(scene.connect_control(added));
    const auto find_link = [&]() {
        return std::ranges::find_if(links, [&](const workflow::ControlLink& link) {
            return link.source == fixture.route && link.output == fixture.right && link.target == fixture.first;
        });
    };
    QVERIFY(find_link() != links.end());
    const auto added_id = find_link()->id;
    QVERIFY(scene.connectionGraphicsObject(added));
    const QPointF original = scene.nodeGraphicsObject(first)->pos();
    const QPointF moved = original + QPointF{55, 20};
    QVERIFY(scene.move_nodes(std::array{ui::CanvasMove{first, moved}}));
    QCOMPARE(scene.nodeGraphicsObject(first)->pos(), moved);

    QVERIFY(scene.open_group(group));
    QVERIFY(graph.scope() == fixture.group);
    QCOMPARE(graph.allNodeIds().size(), std::size_t{2});
    QVERIFY(scene.connectionGraphicsObject({inside, 0, nested, 0}));
    QVERIFY(!scene.connectionGraphicsObject(added));
    QVERIFY(!scene.open_group(inside));
    QVERIFY(!placement(graph, fixture.inside));
    const QPointF moved_inside = scene.nodeGraphicsObject(inside)->pos() + QPointF{45, 55};
    QVERIFY(scene.move_nodes(std::array{ui::CanvasMove{inside, moved_inside}}));
    QVERIFY(scene.open_group(nested));
    QVERIFY(graph.scope() == fixture.nested);
    QCOMPARE(graph.allNodeIds().size(), std::size_t{1});
    QVERIFY(scene.nodeGraphicsObject(leaf));
    QVERIFY(!scene.nodeGraphicsObject(inside));
    QVERIFY(!scene.connectionGraphicsObject({inside, 0, nested, 0}));
    QCOMPARE(history.count(), 3);

    history.undo();
    QVERIFY(!placement(graph, fixture.inside));
    history.undo();
    QVERIFY(placement(graph, fixture.first));
    QCOMPARE(QPointF(placement(graph, fixture.first)->x, placement(graph, fixture.first)->y), original);
    history.undo();
    QVERIFY(find_link() == links.end());
    QVERIFY(scene.nodeGraphicsObject(leaf));
    QVERIFY(graph.scope() == fixture.nested);

    history.redo();
    history.redo();
    history.redo();
    QVERIFY(find_link() != links.end());
    QVERIFY(find_link()->id == added_id);
    QVERIFY(scene.parent_scope());
    QCOMPARE(scene.nodeGraphicsObject(inside)->pos(), moved_inside);
    QVERIFY(scene.parent_scope());
    QVERIFY(!scene.parent_scope());
    QVERIFY(!graph.scope());
    QCOMPARE(graph.canvas_id(fixture.first), first);
    QCOMPARE(scene.nodeGraphicsObject(first)->pos(), moved);
    QVERIFY(scene.connectionGraphicsObject(added));
    QVERIFY(scene.remove_control(added));
    QVERIFY(!scene.connectionGraphicsObject(added));
    QVERIFY(find_link() == links.end());
    history.undo();
    QVERIFY(find_link() != links.end());
    QVERIFY(find_link()->id == added_id);
    QVERIFY(scene.connectionGraphicsObject(added));

    const auto& draft = draft_workflow(graph);
    QVERIFY(std::ranges::equal(draft.containment, solution.workflows.front().containment,
        [](const workflow::Containment& a, const workflow::Containment& b) {
            return a.parent == b.parent && a.child == b.child;
        }));
    const auto* finish = workflow::find_node(draft, fixture.finish);
    QVERIFY(finish && !finish->bindings.empty() && !finish->bindings.front().sources.empty());
    const auto* source = std::get_if<workflow::OutputReference>(&finish->bindings.front().sources.front());
    QVERIFY(source && *source == fixture.data_source);
}

void CanvasTests::protects_existing_links_with_shared_endpoints()
{
    const Fixture fixture;
    auto solution = fixture.solution();
    auto& initial_links = solution.workflows.front().control_links;
    auto duplicate = initial_links.front();
    duplicate.id = uuid<core::ControlLinkId>('a');
    initial_links.push_back(duplicate);

    ui::ControlGraphModel graph(solution, fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    const auto first = graph.canvas_id(fixture.first);
    const QtNodes::ConnectionId link{graph.canvas_id(fixture.route), 1, first, 0};
    QCOMPARE(graph.connections(first, QtNodes::PortType::In, 0).size(), std::size_t{1});
    QVERIFY(scene.connectionGraphicsObject(link));
    const auto& links = draft_workflow(graph).control_links;
    QCOMPARE(links.size(), initial_links.size());
    const QSignalSpy failures(&scene, &ui::ControlScene::edit_failed);
    QVERIFY(!scene.remove_control(link));
    QCOMPARE(failures.size(), qsizetype{1});
    QCOMPARE(scene.undoStack().count(), 0);
    QVERIFY(std::ranges::equal(links, initial_links, {}, &workflow::ControlLink::id, &workflow::ControlLink::id));
    QVERIFY(scene.connectionGraphicsObject(link));
}

void CanvasTests::handles_mouse_gestures()
{
    const Fixture fixture;
    ui::ControlGraphModel graph(fixture.solution(), fixture.workflow_id, fixture.registry);
    ui::ControlScene scene(graph);
    QtNodes::GraphicsView view(&scene);
    view.resize(1000, 550);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.resetTransform();
    view.centerOn(scene.itemsBoundingRect().center());

    const auto route = graph.canvas_id(fixture.route);
    const auto join = graph.canvas_id(fixture.join);
    const auto out = view_port_position(view, scene, route, QtNodes::PortType::Out, 2);
    const auto in = view_port_position(view, scene, join, QtNodes::PortType::In, 0);
    QVERIFY(view.viewport()->rect().contains(out));
    QVERIFY(view.viewport()->rect().contains(in));
    drag(view, out, in);
    const QtNodes::ConnectionId added{route, 2, join, 0};
    const auto* link = scene.connectionGraphicsObject(added);
    QVERIFY(link);
    QCOMPARE(graph.connections(route, QtNodes::PortType::Out, 2).size(), std::size_t{2});
    QCOMPARE(graph.connections(join, QtNodes::PortType::In, 0).size(), std::size_t{3});

    const auto* item = scene.nodeGraphicsObject(join);
    const auto original = item->pos();
    const auto start = view.mapFromScene(item->mapToScene(item->boundingRect().center()));
    const QPoint offset{50, 38};
    drag(view, start, start + offset);
    QCOMPARE(item->pos(), original + QPointF(offset));
    QVERIFY(placement(graph, fixture.join));
    QCOMPARE(QPointF(placement(graph, fixture.join)->x, placement(graph, fixture.join)->y), item->pos());
    QCOMPARE(link->mapToScene(link->in()), scene.nodeGeometry().portScenePosition(join, QtNodes::PortType::In,
        0, item->sceneTransform()));
    auto& history = scene.undoStack();
    QCOMPARE(history.count(), 2);
    history.undo();
    QCOMPARE(item->pos(), original);
    history.undo();
    QVERIFY(!scene.connectionGraphicsObject(added));
    QCOMPARE(graph.connections(join, QtNodes::PortType::In, 0).size(), std::size_t{2});
}

} // namespace

QTEST_MAIN(CanvasTests)

#include "canvas_tests.moc"
