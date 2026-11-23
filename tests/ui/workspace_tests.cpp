#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "module_history_panel.hpp"
#include "run_controller.hpp"
#include "tool_catalog.hpp"
#include "workspace_appearance.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QGridLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <stdexcept>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
namespace fs = std::filesystem;

template<class T> T take(Result<T> value)
{
    if (auto* error = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(value));
}

template<class T> T fixture_id(std::string_view suffix)
{
    return take(IdFactory::parse<T>("5ca4f87a-ea1e-4d62-89b0-00000000" + std::string(suffix)));
}

template<class T> T* control(QObject& owner, const char* name)
{
    auto* found = owner.findChild<T*>(QString::fromLatin1(name));
    if (!found) { throw std::runtime_error(std::string("Missing UI control: ") + name); }
    return found;
}

const auto judge_id = fixture_id<NodeId>("6934");
const auto measure_id = fixture_id<NodeId>("6924");
const auto reader_id = fixture_id<NodeId>("6911");
const auto inspection_id = fixture_id<WorkflowId>("6910");

QPoint node_position(QGraphicsView& view, const NodeId& node)
{
    auto& scene = dynamic_cast<ControlScene&>(*view.scene());
    const auto& model = static_cast<const ControlGraphModel&>(scene.graphModel());
    auto* item = scene.nodeGraphicsObject(model.canvas_id(node));
    if (!item) { throw std::runtime_error("The fixture node is missing from the canvas."); }
    view.ensureVisible(item);
    return view.mapFromScene(item->sceneBoundingRect().center());
}

QRect workspace_rect(const QWidget& widget, const QWidget& window)
{
    return {widget.mapTo(&window, QPoint{}), widget.size()};
}

QList<QPoint> overlay_members(const ImageView& view)
{
    QList<QPoint> points;
    for (const auto* item : view.scene()->items()) {
        const auto* layer = dynamic_cast<const QGraphicsPixmapItem*>(item);
        if (!layer || layer->zValue() != 1) { continue; }
        const auto mask = layer->pixmap().toImage();
        for (int y = 0; y < mask.height(); ++y) {
            for (int x = 0; x < mask.width(); ++x) {
                if (qAlpha(mask.pixel(x, y))) { points.emplaceBack(x, y); }
            }
        }
    }
    return points;
}

void compare_view_with_run(const ImagePanel& panel, const RunHandle& run, int lane = 5)
{
    const auto progress = take(run.inspect());
    const auto read = std::ranges::find(progress.nodes, reader_id, &RunNodeProgress::node);
    QVERIFY(read != progress.nodes.end() && read->invocation);
    const auto result = take(run.lookup(*read->invocation));
    QVERIFY(result);
    const auto* output = result->output(take(SchemaIdFactory::parse<OutputId>("image")));
    QVERIFY(output);
    const auto* value = std::get_if<DataValue>(&output->content);
    QVERIFY(value && value->as_image() && panel.frame());
    // A visually identical image from another run has a different owned buffer.
    QVERIFY(*panel.frame() == value->as_image()->image);
    const auto* view = panel.findChild<ImageView*>();
    QVERIFY(view);
    const int x = 5 * lane;
    const QList<QPoint> region{{x, 9}, {x + 1, 9}, {x, 10}, {x + 1, 10}};
    QCOMPARE(overlay_members(*view), region);
}

void compare_cli(const fs::path& package, const QString& verdict)
{
    QProcess cli;
    cli.start(QString::fromUtf8(VISIONPRO_TEST_CLI),
        {QStringLiteral("run"), QStringLiteral("--solution"), QString::fromStdU16String((package / "solution.json").u16string()),
            QStringLiteral("--workflow"), QString::fromStdString(inspection_id.to_string())});
    QVERIFY(cli.waitForFinished(10000));
    QCOMPARE(cli.exitStatus(), QProcess::NormalExit);
    QVERIFY2(cli.exitCode() == 0, cli.readAllStandardError().constData());
    const auto report = QJsonDocument::fromJson(cli.readAllStandardOutput()).object();
    QCOMPARE(report.value("status").toString(), QStringLiteral("Succeeded"));
    const auto nodes = report.value("nodes").toArray();
    const auto record = [&](const NodeId& id) {
        for (const auto node : nodes) {
            const auto found = node.toObject();
            if (found.value("id").toString() == QString::fromStdString(id.to_string())) { return found; }
        }
        return QJsonObject{};
    };
    QCOMPARE(record(judge_id).value("judgement").toString(), verdict);
    const auto outputs = record(measure_id).value("outputs").toArray();
    QJsonValue blue;
    for (const auto entry : outputs) {
        const auto output = entry.toObject();
        if (output.value("id").toString() == QStringLiteral("blue")) { blue = output.value("value"); }
    }
    QVERIFY(blue.isDouble());
    QCOMPARE(blue.toDouble(), 50.0);
}

void set_upper_limit(EditorWindow& window)
{
    QPushButton* edit = nullptr;
    for (auto* candidate : window.findChildren<QPushButton*>(QStringLiteral("editBinding"))) {
        if (candidate->isVisible() && candidate->property("bindingTarget") == QStringLiteral("maximum")) { edit = candidate; break; }
    }
    QVERIFY(edit);
    bool changed = false;
    QTimer::singleShot(0, &window, [&] {
        auto* binding = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!binding) { return; }
        auto* set = binding->findChild<QPushButton*>(QStringLiteral("setBindingSource"));
        auto* buttons = binding->findChild<QDialogButtonBox*>();
        if (!set || !buttons) { binding->reject(); return; }
        QTimer::singleShot(0, binding, [&] {
            auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (auto* value = qobject_cast<QInputDialog*>(modal)) {
                value->setTextValue(QStringLiteral("50"));
                value->accept();
                changed = true;
            } else if (modal) {
                modal->reject();
            }
        });
        set->click();
        buttons->button(changed ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel)->click();
    });
    edit->click();
    QVERIFY(changed);
}

class WorkspaceTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void appearance_preferences();
    void toolbox_category_grid_and_search();
    void toolbar_icons_keep_transparent_margins();
    void five_lane_selection_and_history();
    void format_image_sources_data();
    void format_image_sources();
    void loop_invocation_history();

private:
    QTemporaryDir settings_directory_;
};

void WorkspaceTests::initTestCase()
{
    QVERIFY(settings_directory_.isValid());
    const auto directory = qEnvironmentVariable("VISIONPRO_TEST_REOPEN_SETTINGS", settings_directory_.path());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory);
}

void WorkspaceTests::appearance_preferences()
{
    auto& appearance = WorkspaceAppearance::instance();
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    EditorWindow window(sample.solution(), registry);
    auto* system = control<QAction>(window, "appearanceSystem");
    auto* light = control<QAction>(window, "appearanceLight");
    auto* dark = control<QAction>(window, "appearanceDark");
    if (qEnvironmentVariableIsSet("VISIONPRO_TEST_REOPEN_SETTINGS")) {
        QCOMPARE(appearance.mode(), AppearanceMode::Dark);
        QVERIFY(dark->isChecked() && !light->isChecked() && !system->isChecked());
        QCOMPARE(window.palette().window().color(), QColor("#20252b"));
        return;
    }

    QCOMPARE(appearance.mode(), AppearanceMode::System);
    QVERIFY(system->isChecked() && !light->isChecked() && !dark->isChecked());
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("VisionPro"), QStringLiteral("VisionPro"));
    const auto key = QStringLiteral("appearance/mode");
    const auto hints = QApplication::styleHints();
    const auto platform_scheme = hints->colorScheme();
    window.show();
    QVERIFY(window.windowHandle());
    const bool windows = QApplication::platformName() == QStringLiteral("windows");
    const auto change_platform_scheme = [&](Qt::ColorScheme scheme) {
        // Windows supports process-local platform overrides. Offscreen has no system preference.
        if (windows) { hints->setColorScheme(scheme); }
        else { hints->colorSchemeChanged(scheme); }
    };
    change_platform_scheme(Qt::ColorScheme::Dark);
    QTRY_COMPARE(window.palette().window().color(), QColor("#20252b"));
    change_platform_scheme(Qt::ColorScheme::Light);
    QTRY_COMPARE(window.palette().window().color(), QColor("#f3f4f5"));
    QCOMPARE(appearance.mode(), AppearanceMode::System);
    if (windows) { hints->unsetColorScheme(); }
    else { hints->colorSchemeChanged(platform_scheme); }

    QDialog dialog(&window);
    dialog.show();
    auto* inspector = control<QDockWidget>(window, "inspectorDock");
    inspector->setFloating(true);
    inspector->show();
    auto* run = control<QAction>(window, "runWorkflow");
    auto tiles = window.findChildren<QToolButton*>();
    const auto tile = std::ranges::find_if(tiles, [](const auto* button) { return button->property("toolType").isValid(); });
    QVERIFY(tile != tiles.end());
    const auto icon_pixels = [](const QIcon& icon) { return icon.pixmap(QSize(24, 24)).toImage(); };

    light->trigger();
    QTRY_COMPARE(dialog.palette().window().color(), QColor("#f3f4f5"));
    QVERIFY(light->isChecked() && !dark->isChecked() && !system->isChecked());
    QCOMPARE(settings.value(key).toString(), QStringLiteral("light"));
    const auto light_run = icon_pixels(run->icon());
    const auto light_tile = icon_pixels((*tile)->icon());
    dark->trigger();
    QTRY_COMPARE(dialog.palette().window().color(), QColor("#20252b"));
    QTRY_COMPARE(inspector->palette().window().color(), dialog.palette().window().color());
    QDialog new_dialog(&window);
    QCOMPARE(new_dialog.palette().window().color(), dialog.palette().window().color());
    auto* appearance_menu = qobject_cast<QMenu*>(dark->parent());
    QVERIFY(appearance_menu);
    QCOMPARE(appearance_menu->palette().window().color(), dialog.palette().window().color());
    QTRY_COMPARE(QToolTip::palette().toolTipBase().color(), workspace_colors().surface);
    QCOMPARE(QToolTip::palette().toolTipText().color(), workspace_colors().text);
    QFileDialog file_picker(&window);
    file_picker.show();
    QTRY_VERIFY(file_picker.findChild<QDialogButtonBox*>() && file_picker.findChild<QDialogButtonBox*>()->isVisible());
    QCOMPARE(file_picker.palette().base().color(), workspace_colors().surface);
    file_picker.reject();
    QTRY_VERIFY(icon_pixels(run->icon()) != light_run);
    QTRY_VERIFY(icon_pixels((*tile)->icon()) != light_tile);
    QVERIFY(dark->isChecked() && !light->isChecked() && !system->isChecked());
    hints->colorSchemeChanged(Qt::ColorScheme::Light);
    QApplication::processEvents();
    QCOMPARE(appearance.mode(), AppearanceMode::Dark);
    QCOMPARE(window.palette().window().color(), QColor("#20252b"));
    QCOMPARE(settings.value(key).toString(), QStringLiteral("dark"));

    QProcess reopened;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("VISIONPRO_TEST_REOPEN_SETTINGS"), settings_directory_.path());
    reopened.setProcessEnvironment(environment);
    reopened.setProcessChannelMode(QProcess::MergedChannels);
    reopened.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-platform"), QApplication::platformName(), QStringLiteral("appearance_preferences")});
    QVERIFY2(reopened.waitForFinished(15000), qPrintable(reopened.errorString()));
    QCOMPARE(reopened.exitStatus(), QProcess::NormalExit);
    QVERIFY2(reopened.exitCode() == 0, reopened.readAll().constData());

    system->trigger();
    QCOMPARE(appearance.mode(), AppearanceMode::System);
    QVERIFY(system->isChecked() && !dark->isChecked() && !light->isChecked());
    QCOMPARE(settings.value(key).toString(), QStringLiteral("system"));
}

void WorkspaceTests::toolbar_icons_keep_transparent_margins()
{
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    EditorWindow window(sample.solution(), registry);
    const auto icon = control<QAction>(window, "runWorkflow")->icon();
    for (const qreal scale : {1.0, 1.25, 1.5}) {
        for (auto mode : {QIcon::Normal, QIcon::Disabled}) {
            const auto pixels = icon.pixmap(QSize(18, 18), scale, mode).toImage();
            QVERIFY(!pixels.isNull());
            QCOMPARE(pixels.size(), QSize(18, 18) * scale);
            QCOMPARE(qAlpha(pixels.pixel(0, 0)), 0);
            int ink = 0;
            for (int y = 0; y < pixels.height(); ++y) {
                for (int x = 0; x < pixels.width(); ++x) { ink += qAlpha(pixels.pixel(x, y)) > 0; }
            }
            QVERIFY(ink > 0 && ink < pixels.width() * pixels.height() / 2);
        }
    }
}

void WorkspaceTests::toolbox_category_grid_and_search()
{
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    EditorWindow window(sample.solution(), registry);
    window.resize(1440, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* toolbox = control<QScrollArea>(window, "toolList");
    auto* search = control<QLineEdit>(window, "toolSearch");

    QList<QToolButton*> headers;
    for (auto* button : toolbox->findChildren<QToolButton*>()) {
        if (button->property("toolCategory").isValid()) { headers.append(button); }
    }
    std::ranges::sort(headers, {}, [toolbox](QWidget* button) { return button->mapTo(toolbox, QPoint{}).y(); });
    const QStringList expected{QStringLiteral("图像采集"), QStringLiteral("定位"), QStringLiteral("测量"),
        QStringLiteral("识别"), QStringLiteral("缺陷检测"), QStringLiteral("边缘学习"), QStringLiteral("深度学习"),
        QStringLiteral("标定"), QStringLiteral("运算"), QStringLiteral("图像处理"), QStringLiteral("颜色处理"),
        QStringLiteral("拆分组合"), QStringLiteral("图形生成"), QStringLiteral("逻辑工具"), QStringLiteral("通信")};
    QStringList titles;
    for (auto* header : headers) {
        QVERIFY(header->isVisible() && header->isCheckable());
        titles.append(header->property("toolCategory").toString());
    }
    QCOMPARE(titles, expected);
    auto* acquisition = headers.at(expected.indexOf(QStringLiteral("图像采集")));
    auto* recognition = headers.at(expected.indexOf(QStringLiteral("识别")));
    auto* logic = headers.at(expected.indexOf(QStringLiteral("逻辑工具")));
    QVERIFY(acquisition->isChecked());
    acquisition->click();
    QVERIFY(!recognition->isChecked());
    recognition->click();
    QVERIFY(recognition->isChecked());

    QList<QAbstractButton*> tiles;
    QSet<QString> offered;
    for (auto* button : toolbox->findChildren<QAbstractButton*>()) {
        const auto id = button->property("toolType").toString();
        if (id.isEmpty()) { continue; }
        QVERIFY(!button->text().isEmpty() && !button->icon().isNull());
        QVERIFY(!offered.contains(id));
        offered.insert(id);
        tiles.append(button);
    }
    QSet<QString> registered{QStringLiteral("@group"), QStringLiteral("@loop")};
    for (const auto& descriptor : registry.descriptors()) { registered.insert(QString::fromUtf8(descriptor.id.text())); }
    QCOMPARE(offered, registered);

    search->setText(QStringLiteral("识别"));
    QVERIFY(recognition->isVisible() && recognition->isChecked());
    QVERIFY(!acquisition->isVisible());
    QVERIFY(std::ranges::none_of(tiles, [](const auto* tile) { return tile->isVisible(); }));
    search->setText(QStringLiteral("逻辑工具"));
    QVERIFY(logic->isVisible() && logic->isChecked());
    QList<QAbstractButton*> visible;
    std::ranges::copy_if(tiles, std::back_inserter(visible), [](const auto* tile) { return tile->isVisible(); });
    QVERIFY(visible.size() > 3);
    auto* grid = qobject_cast<QGridLayout*>(visible.front()->parentWidget()->layout());
    QVERIFY(grid);
    for (qsizetype i = 0; i < visible.size(); ++i) {
        const auto* cell = grid->itemAtPosition(int(i / 3), int(i % 3));
        QVERIFY(cell && visible.contains(qobject_cast<QAbstractButton*>(cell->widget())));
    }
    QVERIFY(!grid->itemAtPosition(0, 3));
    search->setText(QStringLiteral("@loop"));
    QTRY_VERIFY(grid->itemAtPosition(0, 0) && grid->itemAtPosition(0, 0)->widget());
    QCOMPARE(grid->itemAtPosition(0, 0)->widget()->property("toolType").toString(), QStringLiteral("@loop"));
    search->clear();
    QVERIFY(!acquisition->isChecked() && recognition->isChecked() && !logic->isChecked());
    QVERIFY(std::ranges::all_of(headers, [](const auto* header) { return header->isVisible(); }));
    recognition->click();
    QVERIFY(!recognition->isChecked());
}

void WorkspaceTests::five_lane_selection_and_history()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto package = fs::path(scratch.path().toStdU16String()) / "inspection";
    fs::copy(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), package, fs::copy_options::recursive);
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    EditorWindow window(document.solution(), registry);
    window.resize(1440, 800);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);

    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* stats = control<QLabel>(window, "workspaceStats");
    auto* image = control<ImagePanel>(window, "imagePanel");
    auto* source = control<QComboBox>(window, "workspaceImageSource");
    auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
    auto* follow = control<QCheckBox>(window, "followLatest");
    auto* selector = control<QComboBox>(window, "workflowSelector");
    auto* toolbar = control<QToolBar>(window, "workspaceToolbar");
    auto* tools = control<QWidget>(window, "nodeToolbox");
    auto* run = control<QAction>(window, "runWorkflow");
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    QVERIFY(history && controller && canvas->scene());
    const auto verify_layout = [&](const QSize& size) {
        window.resize(size);
        QTRY_COMPARE(window.size(), size);
        QApplication::processEvents();
        const auto rect = [&](const QWidget& widget) { return workspace_rect(widget, window); };
        QVERIFY(toolbar->isVisible() && tools->isVisible() && canvas->isVisible());
        QVERIFY(stats->isVisible() && image->isVisible() && table->isVisible());
        QVERIFY(rect(*toolbar).bottom() <= rect(*canvas).top());
        QVERIFY(rect(*tools).right() <= rect(*canvas).left());
        QVERIFY(rect(*canvas).bottom() <= rect(*stats).top());
        QVERIFY(rect(*canvas).right() <= rect(*image).left());
        QVERIFY(rect(*stats).right() <= rect(*table).left());
        QVERIFY(rect(*image).bottom() <= rect(*table).top());
        QVERIFY(stats->fontMetrics().horizontalAdvance(stats->text()) <= stats->contentsRect().width());
        const auto headings = window.findChildren<QWidget*>(QStringLiteral("workspacePanelHeader"));
        QCOMPARE(headings.size(), 3);
        for (const auto* heading : headings) {
            const auto* title = heading->findChild<QLabel*>();
            QVERIFY(title && title->font().pointSizeF() > table->font().pointSizeF());
            QVERIFY(title->fontMetrics().height() <= title->height());
            QVERIFY(title->fontMetrics().horizontalAdvance(title->text()) <= title->width());
        }
        if (table->topLevelItemCount()) {
            for (int column : {0, 1, 2, 4, 5}) {
                QVERIFY(!table->isColumnHidden(column));
                QVERIFY(table->columnWidth(column) >= table->fontMetrics().horizontalAdvance(table->topLevelItem(0)->text(column)));
            }
        }
    };
    verify_layout({1280, 720});
    verify_layout({1440, 800});
    QVERIFY(!image->frame() && table->topLevelItemCount() == 0);
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
    const auto& graph = static_cast<const ControlGraphModel&>(scene.graphModel());
    const auto style = QJsonDocument::fromVariant(graph.nodeData(graph.canvas_id(judge_id), QtNodes::NodeRole::Style))
        .object().value("NodeStyle").toObject();
    QVERIFY(style.contains("FontColor") && style.value("FontColor") != style.value("GradientColor0"));
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    QCOMPARE(table->headerItem()->text(4), QStringLiteral("实测"));
    QCOMPARE(table->headerItem()->text(5), QStringLiteral("允许范围"));
    QVERIFY(!table->isColumnHidden(4) && !table->isColumnHidden(5) && table->isColumnHidden(3));
    QVERIFY(run->isEnabled());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto first = controller->runs().front();
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == judge_id);
    QTRY_VERIFY(image->frame() && table->topLevelItemCount() == 1);
    QCOMPARE(table->topLevelItem(0)->text(2), QStringLiteral("NG"));
    QCOMPARE(table->topLevelItem(0)->text(1), QStringLiteral("成功"));
    QCOMPARE(table->topLevelItem(0)->text(4), QStringLiteral("50"));
    QCOMPARE(table->topLevelItem(0)->text(5), QStringLiteral("[10, 40]"));
    QVERIFY(table->topLevelItem(0)->textAlignment(4) & Qt::AlignRight);
    QVERIFY(table->visualItemRect(table->topLevelItem(0)).height() >= std::max(28, table->fontMetrics().height() + 8));
    const auto name = QString::fromStdString(find_node(*find_workflow(window.draft(), inspection_id), measure_id)->name);
    QVERIFY(source->currentText().contains(name) || control<QLabel>(*image, "imageProvenance")->text().contains(name));
    compare_view_with_run(*image, first);
    compare_cli(package, QStringLiteral("NG"));

    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->topLevelItem(0)).center());
    QTRY_VERIFY(!follow->isChecked());
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    auto* inspector = control<QDockWidget>(window, "inspectorDock");
    QTRY_VERIFY(inspector->isVisible());
    set_upper_limit(window);
    inspector->hide();
    window.save_document();
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    QVERIFY(run->isEnabled());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2, 10000);
    const auto latest = controller->runs().back();
    QTRY_COMPARE(table->topLevelItemCount(), 2);
    QVERIFY(history->selected_run() && history->selected_run()->identity() == first.identity());
    QCOMPARE(table->topLevelItem(0)->text(2), QStringLiteral("OK"));
    QCOMPARE(table->topLevelItem(0)->text(4), QStringLiteral("50"));
    QCOMPARE(table->topLevelItem(0)->text(5), QStringLiteral("[10, 50]"));
    QCOMPARE(table->topLevelItem(1)->text(2), QStringLiteral("NG"));
    QCOMPARE(table->topLevelItem(1)->text(4), QStringLiteral("50"));
    QCOMPARE(table->topLevelItem(1)->text(5), QStringLiteral("[10, 40]"));
    const auto round_text = [](const RunHandle& handle) {
        return QStringLiteral("第 %1 轮").arg(handle.identity().run.value());
    };
    QVERIFY(stats->text().contains(round_text(first)) && stats->text().contains(QStringLiteral("历史")));
    auto* round_label = control<QLabel>(*history, "historyRound");
    QCOMPARE(round_label->text(), QStringLiteral("历史 · ") + round_text(first));
    QVERIFY(history->statistics_details().contains(QStringLiteral("快照 %1").arg(first.identity().snapshot.snapshot.value())));
    QVERIFY(history->statistics_details().contains(QStringLiteral("已派发 %1 次").arg(take(first.inspect()).call_order.size())));
    QVERIFY(!stats->text().contains(QStringLiteral("已派发")) && !stats->text().contains(QStringLiteral("算法耗时")));
    QVERIFY(stats->text().contains(QStringLiteral("4 OK / 1 NG")));
    compare_view_with_run(*image, first);
    compare_cli(package, QStringLiteral("OK"));

    follow->click();
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == latest.identity());
    QTRY_VERIFY(stats->text().contains(round_text(latest)));
    QCOMPARE(round_label->text(), QStringLiteral("最新 · ") + round_text(latest));
    QVERIFY(stats->text().contains(QStringLiteral("5 OK / 0 NG")));
    compare_view_with_run(*image, latest);
    verify_layout({1280, 720});
    const auto compact_screenshot = qEnvironmentVariable("VISIONPRO_WORKSPACE_COMPACT_SCREENSHOT");
    if (!compact_screenshot.isEmpty()) { QVERIFY2(window.grab().save(compact_screenshot), qPrintable(compact_screenshot)); }
    verify_layout({1440, 800});
    control<QToolButton>(window, "canvasFitAll")->click();
    const auto visible_scene = canvas->mapToScene(canvas->viewport()->rect()).boundingRect();
    for (int lane = 0; lane < 5; ++lane) {
        const auto id = fixture_id<NodeId>(std::to_string(6930 + lane));
        const auto* item = scene.nodeGraphicsObject(graph.canvas_id(id));
        QVERIFY(item && visible_scene.contains(item->sceneBoundingRect()));
    }
    const auto screenshot = qEnvironmentVariable("VISIONPRO_WORKSPACE_SCREENSHOT");
    if (!screenshot.isEmpty()) { QVERIFY2(window.grab().save(screenshot), qPrintable(screenshot)); }
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->topLevelItem(1)).center());
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == first.identity());
    compare_view_with_run(*image, first);
    QVERIFY(stats->text().contains(round_text(first)));

    const auto selected_call = history->selected_invocation();
    const auto selected_nodes = scene.selected_nodes();
    const auto transform = canvas->transform();
    const auto undo_position = scene.undoStack().index();
    const auto revision = window.draft().revision;
    const auto stats_text = stats->text();
    auto* displayed_view = image->findChild<ImageView*>();
    QVERIFY(displayed_view);
    const auto image_layers = [&] {
        QList<QImage> layers;
        for (const auto* item : displayed_view->scene()->items()) {
            if (const auto* layer = dynamic_cast<const QGraphicsPixmapItem*>(item)) { layers.append(layer->pixmap().toImage()); }
        }
        return layers;
    };
    const auto pixels = image_layers();
    QCOMPARE(pixels.size(), 2);
    const auto verify_theme = [&](const char* action, const char* screenshot_variable) {
        control<QAction>(window, action)->trigger();
        QTRY_COMPARE(canvas->backgroundBrush().color(), workspace_colors().canvas);
        QTRY_COMPARE(window.palette().window().color(), workspace_colors().window);
        QTRY_COMPARE(toolbar->palette().buttonText().color(), workspace_colors().text);
        QTRY_COMPARE(table->viewport()->palette().base().color(), workspace_colors().surface);
        const auto table_background = [&] {
            const auto viewport = table->viewport()->rect();
            return table->viewport()->grab(QRect(viewport.center().x(), viewport.bottom() - 4, 1, 1))
                .toImage().pixelColor(0, 0);
        };
        QTRY_COMPARE(table_background(), workspace_colors().surface);
        QTRY_COMPARE(table->topLevelItem(0)->foreground(2).color(), workspace_colors().positive);
        QTRY_COMPARE(table->topLevelItem(1)->foreground(2).color(), workspace_colors().negative);
        QCOMPARE(table->currentItem(), table->topLevelItem(1));
        // A selected item can have the right ForegroundRole while Qt paints HighlightedText instead.
        const auto verdict_rect = table->visualRect(table->indexFromItem(table->currentItem(), 2));
        QVERIFY(table->viewport()->rect().contains(verdict_rect));
        const auto cell = table->viewport()->grab(verdict_rect).toImage();
        const auto ink = workspace_colors().negative;
        bool visible_ink = false;
        for (int y = 0; y < cell.height() && !visible_ink; ++y) {
            for (int x = 0; x < cell.width(); ++x) {
                const auto pixel = cell.pixelColor(x, y);
                if (std::max({qAbs(pixel.red() - ink.red()), qAbs(pixel.green() - ink.green()), qAbs(pixel.blue() - ink.blue())}) < 40) {
                    visible_ink = true;
                    break;
                }
            }
        }
        QVERIFY(visible_ink);
        QVERIFY(!follow->isChecked() && history->selected_run() && history->selected_run()->identity() == first.identity());
        QVERIFY(history->selected_invocation() == selected_call);
        QCOMPARE(stats->text(), stats_text);
        QCOMPARE(window.draft().revision, revision);
        QVERIFY(!window.isWindowModified());
        QCOMPARE(canvas->scene(), &scene);
        QVERIFY(scene.selected_nodes() == selected_nodes);
        QCOMPARE(canvas->transform(), transform);
        QCOMPARE(scene.undoStack().index(), undo_position);
        compare_view_with_run(*image, first);
        QVERIFY(image_layers() == pixels);
        const auto path = qEnvironmentVariable(screenshot_variable);
        if (!path.isEmpty()) { QVERIFY2(window.grab().save(path), qPrintable(path)); }
    };
    verify_theme("appearanceDark", "VISIONPRO_WORKSPACE_DARK_SCREENSHOT");
    verify_theme("appearanceLight", "VISIONPRO_WORKSPACE_LIGHT_SCREENSHOT");
    control<QAction>(window, "appearanceSystem")->trigger();

    controller->set_limits(1, controller->observation_capacity());
    QCOMPARE(controller->runs().size(), std::size_t{1});
    QVERIFY(history->selected_run() && history->selected_run()->identity() == first.identity());
    QCOMPARE(table->topLevelItemCount(), 2);
    compare_view_with_run(*image, first);

    // The OK formatting node was skipped in the pinned NG run.
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, fixture_id<NodeId>("6942")));
    QTRY_VERIFY(!image->frame());
    QVERIFY(history->selected_run() && history->selected_run()->identity() == first.identity());
    QCOMPARE(selector->count(), 2);
    selector->setCurrentIndex(1);
    QTRY_VERIFY(!history->selected_run() && table->topLevelItemCount() == 0 && !image->frame());
    selector->setCurrentIndex(0);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == latest.identity());
    QCOMPARE(table->topLevelItemCount(), 1);
    compare_view_with_run(*image, latest);
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* reopened_canvas = control<QGraphicsView>(window, "workflowCanvas");
    QTest::mouseClick(reopened_canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*reopened_canvas, judge_id));
    QTRY_VERIFY(controller->runs().empty() && table->topLevelItemCount() == 0 && !image->frame());
    QVERIFY(!history->selected_run() && !history->selected_invocation());
    qInfo().noquote() << "Six-region workspace at 1280x720 and 1440x800; GUI/CLI: lane 5 B=50; upper 40 -> NG, upper 50 -> OK."
        << "Both displayed runs use their own input buffer and the translated 2x2 ROI at (25,9).";
}

void WorkspaceTests::format_image_sources_data()
{
    QTest::addColumn<bool>("expect_ok");
    QTest::newRow("Format NG") << false;
    QTest::newRow("Format OK") << true;
}

void WorkspaceTests::format_image_sources()
{
    QFETCH(bool, expect_ok);
    const auto package = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String());
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* source = control<QComboBox>(window, "workspaceImageSource");
    auto* panel = control<ImagePanel>(window, "imagePanel");
    auto* provenance = control<QLabel>(*panel, "imageProvenance");
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    QVERIFY(history && controller);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    if (expect_ok) {
        QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
        auto* inspector = control<QDockWidget>(window, "inspectorDock");
        QTRY_VERIFY(inspector->isVisible());
        set_upper_limit(window);
        inspector->hide();
    }
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto run = controller->runs().front();
    QTRY_VERIFY(panel->frame());
    compare_view_with_run(*panel, run);

    const auto formatter = fixture_id<NodeId>(expect_ok ? "6942" : "6944");
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, formatter));
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == formatter);
    const auto consumer = *history->selected_invocation();
    QTRY_COMPARE(source->count(), 5);
    QCOMPARE(source->currentIndex(), -1);
    QVERIFY(!panel->frame() && source->isVisible());
    QVERIFY(provenance->text().contains(QStringLiteral("请选择")));
    const auto progress = take(run.inspect());
    for (int lane = 1; lane <= 5; ++lane) {
        const auto name = QStringLiteral("Measure lane %1").arg(lane);
        const int index = source->findText(name, Qt::MatchContains);
        QVERIFY2(index >= 0, qPrintable(name));
        source->setCurrentIndex(index);
        QTRY_VERIFY(panel->frame());
        compare_view_with_run(*panel, run, lane);
        const auto id = fixture_id<NodeId>(std::to_string(6919 + lane));
        const auto call = std::ranges::find(progress.nodes, id, &RunNodeProgress::node);
        QVERIFY(call != progress.nodes.end() && call->invocation);
        QVERIFY(provenance->text().contains(name));
        QVERIFY(provenance->text().endsWith(QStringLiteral("调用 %1").arg(call->invocation->invocation.value())));
        QVERIFY(history->selected_invocation() == consumer);
    }
    qInfo() << (expect_ok ? "Format OK" : "Format NG") << "requires an explicit source; all five translated ROIs and producer calls match.";
}

void WorkspaceTests::loop_invocation_history()
{
    const auto package = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()).parent_path() / "s6-loop";
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
    auto* follow = control<QCheckBox>(window, "followLatest");
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    QVERIFY(history && controller);
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto run = controller->runs().front();
    const auto progress = take(run.inspect());
    QCOMPARE(progress.iterations.size(), std::size_t{2});
    const auto loop = fixture_id<NodeId>("6104");
    const auto child = fixture_id<NodeId>("6105");
    const auto& scene = dynamic_cast<const ControlScene&>(*canvas->scene());
    const auto& graph = static_cast<const ControlGraphModel&>(scene.graphModel());
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, loop));
    QTRY_VERIFY(graph.scope() == loop);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, child));
    QTRY_COMPARE(table->topLevelItemCount(), 2);
    const auto select_iteration = [&](int iteration_index, int expected_area) {
        QVERIFY(!table->isColumnHidden(3) && table->isColumnHidden(4) && table->isColumnHidden(5));
        auto* item = table->topLevelItem(1 - iteration_index);
        QCOMPARE(item->text(0), QStringLiteral("#%1 · 迭代 %2").arg(run.identity().run.value()).arg(iteration_index));
        QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(item).center());
        QTRY_VERIFY(!follow->isChecked() && history->selected_invocation() && history->selected_run());
        QVERIFY(history->selected_run()->identity() == run.identity());
        const auto invocation = *history->selected_invocation();
        QVERIFY(invocation.node == child);
        const auto iteration = std::ranges::find(progress.iterations, iteration_index, &RunLoopIteration::index);
        QVERIFY(iteration != progress.iterations.end() && invocation.scope == iteration->scope);
        const auto record = take(run.lookup(invocation));
        QVERIFY(record && record->data().identity.invocation == invocation);
        const auto* output = record->output(take(SchemaIdFactory::parse<OutputId>("item")));
        QVERIFY(output);
        const auto* value = std::get_if<DataValue>(&output->content);
        const auto* area = value ? value->field(take(SchemaIdFactory::parse<FieldId>("area_pixels"))) : nullptr;
        QVERIFY(area && area->as_int64());
        QCOMPARE(*area->as_int64(), expected_area);
        QVERIFY(table->currentItem()->text(3).contains(QStringLiteral("area_pixels=%1").arg(expected_area)));
    };
    select_iteration(0, 6);
    const auto first = history->selected_invocation();
    select_iteration(1, 9);
    QVERIFY(first && history->selected_invocation() && first->invocation != history->selected_invocation()->invocation);
    select_iteration(0, 6);
    QVERIFY(history->selected_invocation() == first);
    qInfo() << "One loop run: selected iterations 0, 1, 0 and matched their distinct invocation scopes and Blob areas 6, 9, 6.";
}

} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::WorkspaceTests)
#include "workspace_tests.moc"
