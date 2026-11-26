#include "control_scene.hpp"
#include "editor_window.hpp"
#include "image_panel.hpp"
#include "image_source_fields.hpp"
#include "image_source_strip.hpp"
#include "module_history_panel.hpp"
#include "run_controller.hpp"
#include "run_image.hpp"
#include "tool_catalog.hpp"
#include "workspace_appearance.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/shared_data.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDockWidget>
#include <QFileDialog>
#include <QDir>
#include <QFontInfo>
#include <QGraphicsPixmapItem>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollArea>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStyleHints>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
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

double contrast_ratio(const QColor& first, const QColor& second)
{
    const auto linear = [](double component) {
        return component <= .04045 ? component / 12.92 : std::pow((component + .055) / 1.055, 2.4);
    };
    const auto luminance = [&](const QColor& color) {
        return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
    };
    const auto a = luminance(first), b = luminance(second);
    return (std::max(a, b) + .05) / (std::min(a, b) + .05);
}

void verify_workspace_heading_contrast(EditorWindow& window)
{
    const auto& colors = workspace_colors();
    const auto screenshot = window.grab().toImage();
    const auto scale = screenshot.devicePixelRatio();
    const auto verify_text = [&](QWidget& widget, int text_start, int text_top, const QString& label) {
        const auto area = workspace_rect(widget, window);
        const auto pixels = screenshot.copy(QRect(area.topLeft() * scale, area.size() * scale));
        const auto background = pixels.pixelColor(pixels.width() - qCeil(4 * scale), pixels.height() / 2);
        QCOMPARE(background.lightness() < 128, workspace_is_dark());
        // Sample the label area, excluding icons and the focus ring.
        int visible_ink = 0;
        for (int y = qCeil(text_top * scale); y < pixels.height() - qCeil(3 * scale); ++y) {
            for (int x = qCeil(text_start * scale); x < pixels.width() - qCeil(8 * scale); ++x) {
                const auto pixel = pixels.pixelColor(x, y);
                if (std::max({std::abs(pixel.red() - colors.text.red()), std::abs(pixel.green() - colors.text.green()),
                    std::abs(pixel.blue() - colors.text.blue())}) <= 24 && contrast_ratio(pixel, background) >= 4.5) {
                    if (++visible_ink >= 3) { return; }
                }
            }
        }
        QFAIL(qPrintable(QStringLiteral("No legible text ink for %1 on %2").arg(label, background.name())));
    };
    const auto headers = window.findChildren<QWidget*>(QStringLiteral("workspacePanelHeader"));
    QCOMPARE(headers.size(), 2);
    for (auto* header : headers) {
        auto* title = header->findChild<QLabel*>();
        QVERIFY(title && title->isVisible());
        verify_text(*title, 0, 3, title->text());
    }
    auto* search = control<QToolButton>(window, "searchToolbox");
    QVERIFY(search->isVisible());
    verify_text(*search, search->width() / 2, 3, search->text());
    int categories = 0;
    for (auto* button : window.findChildren<QToolButton*>()) {
        const auto category = button->property("toolCategory").toString();
        if (category != QStringLiteral("图像采集") && category != QStringLiteral("测量")) { continue; }
        QVERIFY(!button->isChecked());
        verify_text(*button, 3, button->height() - button->fontMetrics().height() - 2, category);
        ++categories;
    }
    QCOMPARE(categories, 2);
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

QDialog* visible_properties(EditorWindow& window)
{
    auto* dialog = window.findChild<QDialog*>(QStringLiteral("propertiesDialog"));
    return dialog && dialog->isVisible() ? dialog : nullptr;
}

void set_upper_limit(QDialog& inspector)
{
    QLineEdit* edit = nullptr;
    for (auto* candidate : inspector.findChildren<QLineEdit*>(QStringLiteral("inlineConstant"))) {
        if (candidate->isVisible() && candidate->property("bindingTarget") == QStringLiteral("maximum")) {
            edit = candidate;
            break;
        }
    }
    QVERIFY(edit);
    auto* scroll = inspector.findChild<QScrollArea*>();
    QVERIFY(scroll);
    scroll->ensureWidgetVisible(edit);
    QTest::mouseClick(edit, Qt::LeftButton);
    QTRY_VERIFY(edit->hasFocus());
    QTest::keySequence(edit, QKeySequence::SelectAll);
    QTest::keyClicks(edit, "50");
    QTest::keyClick(edit, Qt::Key_Return);
    QVERIFY(!QApplication::activeModalWidget());
}

class WorkspaceTests final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void appearance_preferences();
    void toolbox_category_grid_and_search();
    void remembers_and_resets_workspace_layout();
    void workspace_visual_states();
    void toolbar_icons_keep_transparent_margins();
    void five_lane_selection_and_history();
    void panels_float_and_redock();
    void properties_dialog_reuses_selection_and_clears_on_replacement();
    void format_image_sources_data();
    void format_image_sources();
    void image_source_workbench();
    void image_output_keeps_exact_history_and_result_fields();
    void recovers_image_view_after_failed_measurement();
    void flow_image_remembers_skipped_source();
    void pages_large_output_collection();
    void loop_invocation_history();
    void edits_image_result_variables();
    void restricts_image_result_variables_to_direct_group();

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

void WorkspaceTests::panels_float_and_redock()
{
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    EditorWindow window(sample.solution(), registry);
    window.resize(1280, 720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* history = control<QDockWidget>(window, "moduleHistoryDock");
    auto* diagnostics = control<QDockWidget>(window, "diagnosticsDock");
    auto* image = control<QWidget>(window, "imagePanel");
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* canvas_host = qobject_cast<QMainWindow*>(diagnostics->parentWidget());
    QVERIFY(canvas_host);
    QVERIFY(!history->visibleRegion().isEmpty() && diagnostics->isHidden());
    QTest::mouseClick(control<QToolButton>(window, "openDiagnostics"), Qt::LeftButton);
    QVERIFY(!diagnostics->isFloating() && diagnostics->features().testFlag(QDockWidget::DockWidgetMovable));
    auto* undock = diagnostics->findChild<QAbstractButton*>(QStringLiteral("qt_dockwidget_floatbutton"));
    QVERIFY(undock && undock->isVisible());
    QTest::mouseClick(undock, Qt::LeftButton);
    QTRY_VERIFY(diagnostics->isFloating());
    diagnostics->setFloating(false);
    diagnostics->raise();
    QTRY_VERIFY(!diagnostics->isFloating() && !diagnostics->visibleRegion().isEmpty());
    QCOMPARE(canvas_host->dockWidgetArea(diagnostics), Qt::BottomDockWidgetArea);
    QVERIFY(workspace_rect(*canvas, window).bottom() < workspace_rect(*diagnostics, window).top());
    QVERIFY(workspace_rect(*image, window).bottom() < workspace_rect(*history, window).top());
    QVERIFY(diagnostics->close());
    QTest::mouseClick(control<QToolButton>(window, "openDiagnostics"), Qt::LeftButton);
    QTRY_VERIFY(!history->visibleRegion().isEmpty() && !diagnostics->visibleRegion().isEmpty());
    QCOMPARE(window.size(), QSize(1280, 720));
}

void WorkspaceTests::properties_dialog_reuses_selection_and_clears_on_replacement()
{
    const auto registry = take(app::make_tool_registry());
    const auto package = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String());
    const auto sample = take(infrastructure::load_solution(package, registry));
    EditorWindow window(sample.solution(), registry);
    window.resize(1440, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* image = control<QWidget>(window, "imagePanel");
    auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
    const auto& flow = *find_workflow(sample.solution(), inspection_id);
    const auto node_name = [&](NodeId id) { return QString::fromStdString(find_node(flow, id)->name); };
    const auto name_field = [](QDialog* dialog, const char* field) {
        auto* edit = dialog ? dialog->findChild<QAbstractButton*>(QString::fromLatin1(field)) : nullptr;
        return edit && edit->isVisible() ? edit->toolTip() : QString{};
    };
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, judge_id));
    QApplication::processEvents();
    QVERIFY(!visible_properties(window));
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, judge_id));
    QTRY_VERIFY(visible_properties(window));
    QPointer<QDialog> properties = visible_properties(window);
    QVERIFY(properties->isWindow());
    QCOMPARE(properties->windowModality(), Qt::NonModal);
    QTRY_COMPARE(name_field(properties, "renameNode"), node_name(judge_id));
    QVERIFY(!table->visibleRegion().isEmpty() && !image->visibleRegion().isEmpty());
    QVERIFY(properties->close());
    QTRY_VERIFY(!visible_properties(window));
    QVERIFY(!table->visibleRegion().isEmpty() && !image->visibleRegion().isEmpty());

    auto* toolbar = control<QToolBar>(window, "workspaceToolbar");
    auto* show = toolbar->widgetForAction(control<QAction>(window, "showProperties"));
    QVERIFY(show && show->isVisible());
    QTest::mouseClick(show, Qt::LeftButton);
    QTRY_VERIFY(visible_properties(window));
    properties = visible_properties(window);
    QTRY_COMPARE(name_field(properties, "renameNode"), node_name(judge_id));
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, measure_id));
    QApplication::processEvents();
    QCOMPARE(name_field(properties, "renameNode"), node_name(judge_id));
    set_upper_limit(*properties);
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
    QVERIFY(scene.undoStack().canUndo());
    scene.undoStack().undo();
    properties->activateWindow();
    QTRY_VERIFY(properties->isActiveWindow());
    auto* debug = control<QAbstractButton>(*properties, "debugConfiguredNode");
    debug->setFocus();
    QTimer input;
    connect(&input, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        input.stop();
        auto* value = dialog->findChild<QLineEdit*>(QStringLiteral("runInputValue.value"));
        if (!value) { dialog->reject(); return; }
        value->setText(QStringLiteral("50"));
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    input.start(15);
    QTest::keyClick(debug, Qt::Key_F5, Qt::ControlModifier);
    auto* controller = window.findChild<RunController*>();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && !controller->runs().empty(), 10000);
    const auto* target = std::get_if<NodeTarget>(&controller->runs().back().request().target());
    QVERIFY(target && target->workflow == inspection_id && target->node == judge_id);
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, measure_id));
    QTRY_COMPARE(name_field(properties, "renameNode"), node_name(measure_id));
    QCOMPARE(visible_properties(window), properties.data());
    QCOMPARE(window.findChildren<QDialog*>(QStringLiteral("propertiesDialog")).size(), 1);

    control<QAction>(window, "workflowProperties")->trigger();
    QTRY_COMPARE(name_field(properties, "renameWorkflow"), QString::fromStdString(flow.name));
    QVERIFY(name_field(properties, "renameNode").isEmpty());
    const auto previous_id = window.draft().id;
    window.open_document(package.parent_path() / "s6-calculation");
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy() && window.draft().id != previous_id, 10000);
    QVERIFY(!visible_properties(window));
    control<QAction>(window, "workflowProperties")->trigger();
    QTRY_VERIFY(visible_properties(window));
    QTRY_COMPARE(name_field(visible_properties(window), "renameWorkflow"),
        QString::fromStdString(window.draft().workflows.front().name));
    QVERIFY(!table->visibleRegion().isEmpty() && !image->visibleRegion().isEmpty());
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
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (qEnvironmentVariableIsSet("VISIONPRO_TEST_REOPEN_SETTINGS")) {
        QCOMPARE(appearance.mode(), AppearanceMode::Dark);
        QVERIFY(dark->isChecked() && !light->isChecked() && !system->isChecked());
        QCOMPARE(window.palette().window().color(), QColor("#20252b"));
        verify_workspace_heading_contrast(window);
        return;
    }

    QCOMPARE(appearance.mode(), AppearanceMode::System);
    QVERIFY(system->isChecked() && !light->isChecked() && !dark->isChecked());
    QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("VisionPro"), QStringLiteral("VisionPro"));
    const auto key = QStringLiteral("appearance/mode");
    const auto hints = QApplication::styleHints();
    const auto platform_scheme = hints->colorScheme();
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
    control<QAction>(window, "workflowProperties")->trigger();
    QTRY_VERIFY(visible_properties(window));
    auto* inspector = visible_properties(window);
    auto* run = control<QAction>(window, "runWorkflow");
    auto tiles = window.findChildren<QToolButton*>();
    const auto tile = std::ranges::find_if(tiles, [](const auto* button) { return button->property("toolType").isValid(); });
    QVERIFY(tile != tiles.end());
    const auto icon_pixels = [](const QIcon& icon) { return icon.pixmap(QSize(24, 24)).toImage(); };

    light->trigger();
    QTRY_COMPARE(dialog.palette().window().color(), QColor("#f3f4f5"));
    verify_workspace_heading_contrast(window);
    QVERIFY(light->isChecked() && !dark->isChecked() && !system->isChecked());
    QCOMPARE(settings.value(key).toString(), QStringLiteral("light"));
    const auto light_run = icon_pixels(run->icon());
    const auto light_tile = icon_pixels((*tile)->icon());
    dark->trigger();
    QTRY_COMPARE(dialog.palette().window().color(), QColor("#20252b"));
    verify_workspace_heading_contrast(window);
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
    auto* toolbox = control<QWidget>(window, "toolList");
    auto* drawer = control<QWidget>(window, "toolDrawer");
    auto* rail = control<QWidget>(window, "toolCategoryRail");
    auto* search = control<QLineEdit>(window, "toolSearch");
    auto* search_button = control<QToolButton>(window, "searchToolbox");
    QVERIFY(drawer->isHidden() && rail->isVisible());
    QCOMPARE(toolbox->width(), rail->width());

    QList<QToolButton*> headers;
    for (auto* button : toolbox->findChildren<QToolButton*>()) {
        if (button->property("toolCategory").isValid() && button->isVisible()) { headers.append(button); }
    }
    std::ranges::sort(headers, {}, [toolbox](QWidget* button) { return button->mapTo(toolbox, QPoint{}).y(); });
    const QStringList expected{QStringLiteral("图像采集"), QStringLiteral("定位"), QStringLiteral("测量"),
        QStringLiteral("标定"), QStringLiteral("运算"), QStringLiteral("图像处理"), QStringLiteral("颜色处理"),
        QStringLiteral("拆分组合"), QStringLiteral("逻辑工具"), QStringLiteral("通信")};
    QStringList titles;
    QList<QImage> glyphs;
    for (auto* header : headers) {
        QVERIFY(header->isVisible() && header->isCheckable() && !header->isChecked());
        QVERIFY(!header->icon().isNull());
        const auto glyph = header->icon().pixmap(QSize{20, 20}).toImage();
        QVERIFY(!glyphs.contains(glyph));
        glyphs.append(glyph);
        titles.append(header->property("toolCategory").toString());
    }
    QCOMPARE(titles, expected);
    auto* acquisition = headers.at(expected.indexOf(QStringLiteral("图像采集")));
    auto* location = headers.at(expected.indexOf(QStringLiteral("定位")));
    auto* logic = headers.at(expected.indexOf(QStringLiteral("逻辑工具")));
    window.activateWindow();
    search_button->setFocus();
    QTRY_VERIFY(search_button->hasFocus());
    for (int tab = 0; tab < 8 && !acquisition->hasFocus(); ++tab) {
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    }
    QVERIFY(acquisition->hasFocus());
    auto& appearance = WorkspaceAppearance::instance();
    const auto previous_mode = appearance.mode();
    for (const auto mode : {AppearanceMode::Light, AppearanceMode::Dark}) {
        appearance.setMode(mode);
        for (const bool expanded : {false, true}) {
            QCOMPARE(acquisition->isChecked(), expanded);
            QCOMPARE(drawer->isVisible(), expanded);
            const auto pixels = window.grab(workspace_rect(*acquisition, window)).toImage();
            const auto x = pixels.width() / 2;
            const auto background = pixels.pixelColor(qRound(5 * pixels.devicePixelRatio()), pixels.height() / 2);
            for (const auto offset : {0.5, 1.5}) {
                const auto ring = pixels.pixelColor(x, qFloor(offset * pixels.devicePixelRatio()));
                QCOMPARE(ring, workspace_colors().accent);
                QVERIFY(contrast_ratio(ring, background) >= 3.0);
            }
            QTest::keyClick(acquisition, Qt::Key_Space);
        }
    }
    appearance.setMode(previous_mode);
    QVERIFY(!acquisition->isChecked() && drawer->isHidden());
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    const auto closed_canvas = workspace_rect(*canvas, window);
    acquisition->click();
    QCOMPARE(workspace_rect(*canvas, window), closed_canvas);
    QVERIFY(!location->isChecked() && acquisition->isChecked() && drawer->isVisible());
    location->click();
    QVERIFY(location->isChecked() && !acquisition->isChecked());

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
    QVERIFY(location->isVisible() && location->isChecked());
    QVERIFY(acquisition->isVisible());
    QVERIFY(std::ranges::none_of(tiles, [](const auto* tile) { return tile->isVisible(); }));
    search->setText(QStringLiteral("逻辑工具"));
    QVERIFY(logic->isVisible() && !logic->isChecked());
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
    QVERIFY(!acquisition->isChecked() && location->isChecked() && !logic->isChecked());
    QVERIFY(std::ranges::all_of(headers, [](const auto* header) { return header->isVisible(); }));
    location->click();
    QVERIFY(!location->isChecked() && drawer->isHidden());
    QCOMPARE(toolbox->width(), rail->width());
    search_button->click();
    QVERIFY(search->hasFocus() && drawer->isVisible());
    QTest::keyClick(search, Qt::Key_Escape);
    QVERIFY(drawer->isHidden() && search_button->hasFocus());
}

void WorkspaceTests::workspace_visual_states()
{
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    EditorWindow window(sample.solution(), registry);
    window.resize(1440, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QString output = qEnvironmentVariable("VISIONPRO_UI_STATE_SCREENSHOTS");
    const auto capture = [&](const QString& state) {
        if (output.isEmpty()) { return; }
        const auto file = QDir(output).filePath(QStringLiteral("ui4-%1-%2.png").arg(QGuiApplication::platformName(), state));
        const auto image = window.grab();
        QVERIFY2(image.save(file), qPrintable(file));
        qInfo() << state << "platform" << QGuiApplication::platformName() << "window" << window.size()
                << "image" << image.size() << "DPR" << image.devicePixelRatio()
                << "screen" << window.screen()->geometry() << "logical DPI" << window.screen()->logicalDotsPerInch()
                << "font" << QFontInfo(window.font()).family();
    };
    const auto* drawer = control<QWidget>(window, "toolDrawer");
    const auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    QVERIFY(drawer->isHidden() && canvas->isVisible());
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
    const auto& graph = static_cast<const ControlGraphModel&>(scene.graphModel());
    QRectF previous;
    for (int lane = 0; lane < 5; ++lane) {
        const auto node = graph.canvas_id(fixture_id<NodeId>(std::to_string(6920 + lane)));
        auto* card = scene.nodeGraphicsObject(node);
        QVERIFY(card);
        const QRectF body(card->pos(), scene.nodeGeometry().size(node));
        if (lane > 0) { QVERIFY(body.left() - previous.right() >= 4); }
        previous = body;
    }
    capture(QStringLiteral("default"));
    const auto initial = workspace_rect(*canvas, window);
    auto* search = control<QToolButton>(window, "searchToolbox");
    search->click();
    QTRY_VERIFY(drawer->isVisible());
    QCOMPARE(workspace_rect(*canvas, window), initial);
    capture(QStringLiteral("drawer"));
    search->click();
    auto* diagnostics = control<QDockWidget>(window, "diagnosticsDock");
    control<QToolButton>(window, "openDiagnostics")->click();
    QTRY_VERIFY(diagnostics->isVisible());
    capture(QStringLiteral("diagnostics"));
    diagnostics->close();
    window.resize(1920, 1080);
    QTRY_COMPARE(window.size(), QSize(1920, 1080));
    capture(QStringLiteral("fullhd"));
}

void WorkspaceTests::remembers_and_resets_workspace_layout()
{
    QTemporaryDir temp;
    QSettings settings(temp.filePath(QStringLiteral("layout.ini")), QSettings::IniFormat);
    const auto registry = take(app::make_tool_registry());
    const auto sample = take(infrastructure::load_solution(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), registry));
    int pinned_width = 0;
    QSize stored_size;
    {
        EditorWindow first(sample.solution(), registry, nullptr, &settings);
        stored_size = QSize(1280, 720).boundedTo(first.screen()->availableGeometry().size() - QSize(16, 48));
        first.resize(stored_size);
        first.show();
        QVERIFY(QTest::qWaitForWindowExposed(&first));
        auto* splitter = control<QSplitter>(first, "workspaceSplitter");
        const int rail_width = splitter->sizes().front();
        control<QToolButton>(first, "searchToolbox")->click();
        QCOMPARE(splitter->sizes().front(), rail_width);
        control<QToolButton>(first, "pinToolDrawer")->click();
        QVERIFY(splitter->sizes().front() > rail_width);
        splitter->setSizes({320, 470, 480});
        pinned_width = splitter->sizes().front();
        control<QAction>(first, "showResultPanel")->setChecked(false);
        QVERIFY(first.close());
    }
    QCOMPARE(settings.value("workspace/layout/version").toInt(), 1);
    {
        EditorWindow reopened(sample.solution(), registry, nullptr, &settings);
        reopened.show();
        QVERIFY(QTest::qWaitForWindowExposed(&reopened));
        QCOMPARE(reopened.size(), stored_size);
        auto* splitter = control<QSplitter>(reopened, "workspaceSplitter");
        QCOMPARE(splitter->sizes().front(), pinned_width);
        QVERIFY(control<QToolButton>(reopened, "pinToolDrawer")->isChecked());
        QVERIFY(control<QDockWidget>(reopened, "moduleHistoryDock")->isHidden());
        control<QAction>(reopened, "resetWorkspaceLayout")->trigger();
        QVERIFY(!control<QToolButton>(reopened, "pinToolDrawer")->isChecked());
        QVERIFY(splitter->sizes().front() < pinned_width);
        QVERIFY(control<QDockWidget>(reopened, "moduleHistoryDock")->isVisible());
        QVERIFY(!control<QAction>(reopened, "undoDraft")->isEnabled());
        QVERIFY(reopened.close());
    }
    for (const int version : {1, 99}) {
        settings.setValue("workspace/layout/version", version);
        settings.setValue("workspace/layout/pinned", true);
        settings.setValue("workspace/layout/columns", QByteArray("corrupt"));
        EditorWindow recovered(sample.solution(), registry, nullptr, &settings);
        QVERIFY(!control<QToolButton>(recovered, "pinToolDrawer")->isChecked());
        QVERIFY(control<QAction>(recovered, "showResultPanel")->isChecked());
    }
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
    auto* source = control<QComboBox>(*image, "workspaceImageSource");
    auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
    auto* follow = control<QCheckBox>(window, "followLatest");
    auto* selector = control<QTabBar>(window, "workflowSelector");
    auto* flow_bar = control<QToolBar>(window, "workflowBar");
    auto* toolbar = control<QToolBar>(window, "workspaceToolbar");
    auto* tools = control<QWidget>(window, "nodeToolbox");
    auto* run = control<QAction>(window, "runWorkflow");
    auto* run_solution = control<QAction>(window, "runSolution");
    QVERIFY(flow_bar->isAncestorOf(selector));
    for (const auto& flow : window.draft().workflows) {
        int matching_tabs = 0;
        for (int row = 0; row < selector->count(); ++row) {
            if (selector->tabData(row).toString() == QString::fromStdString(flow.id.to_string())) { ++matching_tabs; }
        }
        QCOMPARE(matching_tabs, 1);
    }
    auto* run_button = control<QToolBar>(window, "workflowRunBar")->widgetForAction(run);
    QVERIFY(run_button && run_button->isVisible() && !toolbar->widgetForAction(run));
    QVERIFY(toolbar->widgetForAction(run_solution) && toolbar->widgetForAction(run_solution)->isVisible());
    QVERIFY(!window.draft().graph && !run_solution->isEnabled());
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
        QTRY_VERIFY2(rect(*canvas).right() <= rect(*image).left(),
            qPrintable(QStringLiteral("Canvas ends at %1, image starts at %2 at window width %3")
                .arg(rect(*canvas).right()).arg(rect(*image).left()).arg(size.width())));
        QVERIFY(rect(*stats).right() <= rect(*table).left());
        QVERIFY(rect(*image).bottom() <= rect(*table).top());
        const int statistics_width = stats->fontMetrics().horizontalAdvance(stats->text());
        QTRY_VERIFY2(statistics_width <= stats->contentsRect().width(),
            qPrintable(QStringLiteral("%1 needs %2 px, allocated %3 at window width %4 (column %5, minimum %6, hint %7)")
                .arg(stats->text()).arg(statistics_width).arg(stats->contentsRect().width()).arg(size.width())
                .arg(control<QWidget>(window, "flowWorkspace")->width())
                .arg(control<QWidget>(window, "flowWorkspace")->minimumWidth())
                .arg(control<QWidget>(window, "flowWorkspace")->minimumSizeHint().width())));
        const auto headings = window.findChildren<QWidget*>(QStringLiteral("workspacePanelHeader"));
        QCOMPARE(headings.size(), 2);
        for (const auto* heading : headings) {
            const auto* title = heading->findChild<QLabel*>();
            QVERIFY(title && title->font().pointSizeF() > table->font().pointSizeF());
            QVERIFY(title->fontMetrics().height() <= title->height());
            QVERIFY(title->fontMetrics().horizontalAdvance(title->text()) <= title->width());
        }
        auto* search = control<QToolButton>(window, "searchToolbox");
        QVERIFY(search->isVisible() && search->fontMetrics().height() <= search->height());
        QVERIFY(search->fontMetrics().horizontalAdvance(search->text()) + search->iconSize().width() <= search->width());
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
    QTest::mouseClick(run_button, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto first = controller->runs().front();
    QCOMPARE(first.identity().workflow, inspection_id);
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == judge_id);
    QTRY_VERIFY(image->frame() && table->topLevelItemCount() == 1);
    QVERIFY(!visible_properties(window));
    QCOMPARE(table->topLevelItem(0)->text(2), QStringLiteral("NG"));
    QCOMPARE(table->topLevelItem(0)->text(1), QStringLiteral("成功"));
    QCOMPARE(table->topLevelItem(0)->text(4), QStringLiteral("50"));
    QCOMPARE(table->topLevelItem(0)->text(5), QStringLiteral("[10, 40]"));
    auto* result_tabs = control<QTabBar>(window, "workspaceResultTabs");
    auto* current_result = control<QLabel>(window, "currentDetectionResult");
    auto* origin = control<QLabel>(window, "resultContext");
    result_tabs->setCurrentIndex(0);
    QVERIFY(current_result->isVisible() && current_result->text().contains(QStringLiteral("判定 NG")));
    QVERIFY(!origin->text().contains(QStringLiteral("此前配置")));
    result_tabs->setCurrentIndex(1);
    QVERIFY(table->topLevelItem(0)->textAlignment(4) & Qt::AlignRight);
    QVERIFY(table->visualItemRect(table->topLevelItem(0)).height() >= std::max(28, table->fontMetrics().height() + 8));
    const auto name = QString::fromStdString(find_node(*find_workflow(window.draft(), inspection_id), measure_id)->name);
    QVERIFY(source->currentText().contains(name) || control<QLabel>(*image, "imageProvenance")->text().contains(name));
    compare_view_with_run(*image, first);
    compare_cli(package, QStringLiteral("NG"));
    auto* image_pages = control<QTabBar>(window,"workspaceOutputTabs");
    image_pages->setCurrentIndex(1);
    auto* compare = control<QCheckBox>(window,"flowImageComparison");
    compare->click();
    for (auto* base : {control<QComboBox>(window,"comparisonSource1"),control<QComboBox>(window,"comparisonSource2")}) {
        const auto camera = base->findText("Read camera / ",Qt::MatchStartsWith);
        QVERIFY(camera > 0);
        base->setCurrentIndex(camera);
    }
    auto* menu = control<QToolButton>(window,"comparisonGraphics1")->menu();
    for (const auto& lane : {QStringLiteral("Measure lane 1 / ROI 掩膜"),QStringLiteral("Measure lane 2 / ROI 掩膜")}) {
        const auto options = menu->actions();
        const auto found = std::ranges::find_if(options,[&](QAction* action){ return action->text() == lane; });
        QVERIFY2(found != options.end(),qPrintable(lane));
        (*found)->trigger();
    }
    const auto masks = [&](const char* id) {
        auto* panel = control<ImagePanel>(window,id);
        const auto items = panel->findChild<ImageView*>()->scene()->items();
        return std::ranges::count_if(items,[](const QGraphicsItem* item) {
            const auto* pixels = qgraphicsitem_cast<const QGraphicsPixmapItem*>(item);
            return pixels && pixels->zValue() == 1 && !pixels->pixmap().isNull();
        });
    };
    QCOMPARE(masks("comparisonImage1"),2);
    QCOMPARE(masks("comparisonImage2"),0);
    compare->click();
    image_pages->setCurrentIndex(0);

    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualItemRect(table->topLevelItem(0)).center());
    QTRY_VERIFY(!follow->isChecked());
    verify_layout({1280, 720});
    auto* diagnostics = control<QDockWidget>(window, "diagnosticsDock");
    QTest::mouseClick(control<QToolButton>(window, "openDiagnostics"), Qt::LeftButton);
    QTRY_VERIFY(!diagnostics->visibleRegion().isEmpty() && !diagnostics->isFloating());
    QVERIFY(workspace_rect(*diagnostics, window).top() > workspace_rect(*canvas, window).bottom());
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    QTRY_VERIFY(visible_properties(window));
    auto* properties = visible_properties(window);
    QVERIFY(!table->visibleRegion().isEmpty());
    QVERIFY(image->frame() && window.size() == QSize(1280, 720));
    set_upper_limit(*properties);
    const auto dialog_screenshot = qEnvironmentVariable("VISIONPRO_PROPERTIES_DIALOG_SCREENSHOT");
    if (!dialog_screenshot.isEmpty()) { QVERIFY2(properties->grab().save(dialog_screenshot), qPrintable(dialog_screenshot)); }
    QVERIFY(properties->close());
    QTRY_VERIFY(!visible_properties(window));
    QVERIFY(!table->visibleRegion().isEmpty() && table->topLevelItemCount() == 1);
    QTRY_VERIFY(origin->text().contains(QStringLiteral("此前配置")));
    QCOMPARE(table->topLevelItem(0)->text(5), QStringLiteral("[10, 40]"));
    compare_view_with_run(*image, first);
    diagnostics->close();
    window.save_document();
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    QVERIFY(run->isEnabled());
    window.activateWindow();
    canvas->setFocus();
    QTRY_VERIFY(canvas->hasFocus());
    QTest::keyClick(canvas, Qt::Key_F5);
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2, 10000);
    const auto latest = controller->runs().back();
    QTest::mouseClick(control<QToolButton>(window, "openWorkspaceLog"), Qt::LeftButton);
    auto* events = control<QPlainTextEdit>(window, "workspaceEventHistory");
    QTRY_VERIFY(events->isVisible());
    for (const auto& finished : {first, latest}) {
        const auto message = QStringLiteral("第 %1 轮 · 成功").arg(finished.identity().run.value());
        QCOMPARE(events->toPlainText().count(message), 1);
    }
    events->window()->close();
    QTRY_VERIFY(!table->visibleRegion().isEmpty());
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

    auto* result_view = control<ImageView>(*image, "imageView");
    for (int step = 0; step < 8; ++step) { QTest::mouseClick(control<QToolButton>(*image, "imageZoomIn"), Qt::LeftButton); }
    result_view->centerOn(25.5, 9.5);
    const auto detail_zoom = result_view->transform();
    const auto center = [&] { return result_view->mapToScene(result_view->viewport()->rect().center()); };
    const auto detail_center = center();
    QVERIFY(QLineF(detail_center, QPointF(25.5, 9.5)).length() < 0.2);
    follow->click();
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == latest.identity());
    QTRY_VERIFY(stats->text().contains(round_text(latest)));
    result_tabs->setCurrentIndex(0);
    QVERIFY(current_result->text().contains(QStringLiteral("判定 OK")));
    const auto badge = scene.node_badge(graph.canvas_id(judge_id));
    QCOMPARE(badge.status, ExecutionStatus::Succeeded);
    QCOMPARE(badge.judgement, Judgement::OK);
    QVERIFY(badge.text.contains(QStringLiteral("成功")) && badge.text.contains(QStringLiteral("OK")));
    QVERIFY(!origin->text().contains(QStringLiteral("此前配置")));
    result_tabs->setCurrentIndex(1);
    QCOMPARE(round_label->text(), QStringLiteral("最新 · ") + round_text(latest));
    QVERIFY(stats->text().contains(QStringLiteral("5 OK / 0 NG")));
    compare_view_with_run(*image, latest);
    QCOMPARE(result_view->transform(), detail_zoom);
    QVERIFY(QLineF(center(), detail_center).length() < 0.2);
    QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, table->visualItemRect(table->topLevelItem(1)).center());
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == first.identity());
    compare_view_with_run(*image, first);
    QCOMPARE(result_view->transform(), detail_zoom);
    QVERIFY(QLineF(center(), detail_center).length() < 0.2);
    follow->click();
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == latest.identity());
    result_view->fit_image();
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
        const auto tab = selector->grab(selector->tabRect(selector->currentIndex())).toImage();
        const auto dpr = tab.devicePixelRatio();
        QCOMPARE(tab.pixelColor(qRound(8 * dpr), qRound(5 * dpr)).lightness() < 128, workspace_is_dark());
        QCOMPARE(tab.pixelColor(tab.width() / 2, tab.height() - qCeil(dpr)), workspace_colors().accent);
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

void WorkspaceTests::image_output_keeps_exact_history_and_result_fields()
{
    const auto registry = take(app::make_tool_registry());
    Workflow flow{inspection_id,"成图流程",{source_key<SchemaId>("test.image.output"),1}};
    Solution solution{fixture_id<SolutionId>("6271"),"输出图像",infrastructure::solution_format_version,1,{flow}};
    EditorWindow window(std::move(solution),registry);
    window.resize(1150,720);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto& scene = dynamic_cast<ControlScene&>(*control<QGraphicsView>(window,"workflowCanvas")->scene());
    QVERIFY(scene.add_tool(source_key<ToolTypeId>("visionpro.image.source"),QPointF(0,0)));
    const auto source = scene.selected_nodes().front();
    QTemporaryDir pictures;
    QImage original(11,7,QImage::Format_RGB32);
    original.fill(QColor(221, 23, 13));
    const auto path = pictures.filePath("part.png");
    QVERIFY(original.save(path));
    control<ImageSourceStrip>(window,"imageSourceStrip")->add_files({path});
    QVERIFY(scene.add_tool(source_key<ToolTypeId>("visionpro.image.output"),QPointF(220,0)));
    const auto output = scene.selected_nodes().front();
    const auto image = source_key<OutputId>("image");
    QVERIFY(scene.edit_draft("订阅成图底图",[&](Solution& draft) -> Result<std::monostate> {
        auto& f = draft.workflows.front();
        f.entries = {source};
        f.control_links.push_back({take(IdFactory::generate<ControlLinkId>()),source,{},output});
        auto& node = *std::ranges::find(f.nodes,output,&Node::id);
        node.bindings.push_back({source_key<InputId>("image"),{OutputReference{f.id,source,image,{}}}});
        set_source_value(node,"text",take(ConfigValue::string("X")));
        set_source_value(node,"font_size",ConfigValue::integer(7));
        return std::monostate{};
    }));
    QVERIFY(scene.reveal_node(output));
    auto* start = control<QAction>(window,"runWorkflow");
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    auto* panel = control<ImagePanel>(window,"imagePanel");
    QTRY_VERIFY(start->isEnabled());
    start->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && history->selected_invocation() && panel->frame(),10000);
    const auto first = *history->selected_run();
    const auto record = take(first.lookup(*history->selected_invocation()));
    QVERIFY(record && record->data().identity.invocation.node == output);
    const auto pixels = std::get<DataValue>(record->output(image)->content).as_image()->image;
    QVERIFY(pixels == *panel->frame());
    auto* tabs = control<QTabBar>(window,"workspaceOutputTabs");
    tabs->setCurrentIndex(2);
    auto* tree = control<QTreeWidget>(window,"moduleOutputTree");
    QTRY_VERIFY(tree->topLevelItemCount() == 1 && tree->topLevelItem(0)->text(0) == QStringLiteral("输出图像"));
    const auto output_row = [&](QString id) -> QTreeWidgetItem* {
        auto* root = tree->topLevelItem(0);
        if (!root) { return nullptr; }
        for (int i = 0; i < root->childCount(); ++i) {
            auto* row = root->child(i);
            if (row->data(0,Qt::UserRole) == id) { return row; }
        }
        return nullptr;
    };
    QVERIFY(output_row("image") && output_row("module_status") && output_row("original_path") && output_row("big_file_path"));
    QCOMPARE(output_row("image")->childCount(),4);
    QCOMPARE(output_row("image")->child(1)->text(2),QStringLiteral("11"));
    QCOMPARE(output_row("image")->child(3)->text(2),QStringLiteral("35127316（RGB24）"));
    QVERIFY(output_row("module_status")->text(2).startsWith("1"));
    const auto canvas_output = static_cast<const ControlGraphModel&>(scene.graphModel()).canvas_id(output);
    QTRY_VERIFY(scene.node_badge(canvas_output).completion == 1);
    QCOMPARE(scene.node_badge(canvas_output).judgement,Judgement::NotEvaluated);
    QVERIFY(window.findChild<QPushButton*>("mapResultVariable.rendered_path"));
    QVERIFY(output_row("original_big_file_path") && output_row("original_big_file_path")->text(0) == QStringLiteral("原图大文件路径"));
    QVERIFY(output_row("rendered_big_file_path") && output_row("rendered_big_file_path")->text(0) == QStringLiteral("渲染图大文件路径"));
    tabs->setCurrentIndex(1);
    auto* compare = control<QCheckBox>(window,"flowImageComparison");
    compare->click();
    auto* left = control<QComboBox>(window,"comparisonSource1");
    auto* right = control<QComboBox>(window,"comparisonSource2");
    const auto channel_index = [&](QComboBox* select,NodeId node) {
        const auto label = QString::fromStdString(find_node(window.draft().workflows.front(),node)->name) + " / ";
        return select->findText(label,Qt::MatchStartsWith);
    };
    QTRY_VERIFY(channel_index(left,source)>0 && channel_index(right,output)>0);
    left->setCurrentIndex(channel_index(left,source));
    right->setCurrentIndex(channel_index(right,output));
    auto* left_view = control<ImagePanel>(window,"comparisonImage1");
    auto* right_view = control<ImagePanel>(window,"comparisonImage2");
    QTRY_VERIFY(left_view->frame() && right_view->frame() && *right_view->frame() == pixels);
    const auto first_left = *left_view->frame();
    QVERIFY(take(to_display_image(first_left)).convertToFormat(QImage::Format_RGB32)
        != take(to_display_image(pixels)).convertToFormat(QImage::Format_RGB32));
    tabs->setCurrentIndex(0);
    QVERIFY(scene.edit_draft("关闭结果图输出",[&](Solution& draft) -> Result<std::monostate> {
        auto& nodes = draft.workflows.front().nodes;
        set_source_value(*std::ranges::find(nodes,output,&Node::id),"output_enabled",ConfigValue::boolean(false));
        return std::monostate{};
    }));
    QTRY_VERIFY(start->isEnabled());
    start->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2 && !panel->frame(),10000);
    tabs->setCurrentIndex(1);
    QTRY_VERIFY(left_view->frame() && !right_view->frame());
    QCOMPARE(left->currentIndex(),channel_index(left,source));
    QCOMPARE(right->currentIndex(),channel_index(right,output));
    history->browse_run(first);
    QTRY_VERIFY(left_view->frame() && *left_view->frame() == first_left && right_view->frame() && *right_view->frame() == pixels);
    QCOMPARE(controller->runs().size(),2U);
    compare->click();
    QVERIFY(control<QComboBox>(window,"flowImageChannel")->isVisible());
    tabs->setCurrentIndex(0);
    QTRY_VERIFY(panel->frame() && *panel->frame() == pixels);
    QCOMPARE(QDir(pictures.path()).entryList(QDir::Files).size(),1);
    const auto package_dir = pictures.filePath("packages");
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        dialog->findChild<QCheckBox*>("archiveEnabled")->setChecked(true);
        auto* base = dialog->findChild<QComboBox*>();
        base->setCurrentIndex(base->findData(static_cast<int>(PathBase::Absolute)));
        dialog->findChild<QLineEdit*>("archiveDirectory")->setText(package_dir);
        dialog->findChild<QLineEdit*>("archivePackageSize")->setText("2");
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    control<QAction>(window,"imageArchiveSettings")->trigger();
    QVERIFY(window.draft().image_archive && window.draft().image_archive->images_per_file == 2);
    QVERIFY(scene.edit_draft("启用原图归档",[&](Solution& draft) -> Result<std::monostate> {
        auto& nodes = draft.workflows.front().nodes;
        auto& target = *std::ranges::find(nodes,output,&Node::id);
        set_source_value(target,"save_enabled",ConfigValue::boolean(true));
        set_source_value(target,"save_original",ConfigValue::boolean(true));
        set_source_value(target,"original_prefix",take(ConfigValue::string("工件")));
        return std::monostate{};
    }));
    const auto packages = [&] { return QDir(package_dir).entryList({"*.dat"},QDir::Files); };
    for (int cycle=1; cycle<=3; ++cycle) {
        QTRY_VERIFY(start->isEnabled());
        start->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == static_cast<std::size_t>(cycle+2),10000);
        QCOMPARE(packages().size(),cycle/2);
    }
    auto* flush = control<QAction>(window,"flushImageArchive");
    QTRY_VERIFY(flush->isEnabled());
    flush->trigger();
    QTRY_COMPARE(packages().size(),2);
    QVERIFY(scene.edit_draft("输入无效文字颜色",[&](Solution& draft)->Result<std::monostate> {
        auto& nodes = draft.workflows.front().nodes;
        auto& target = *std::ranges::find(nodes,output,&Node::id);
        set_source_value(target,"output_enabled",ConfigValue::boolean(true));
        set_source_value(target,"save_enabled",ConfigValue::boolean(false));
        set_source_value(target,"text_color",take(ConfigValue::string("bad-color")));
        return std::monostate{};
    }));
    QTRY_VERIFY(start->isEnabled());
    start->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size()==6,10000);
    history->browse_run(controller->runs().back());
    QTRY_VERIFY(scene.node_badge(canvas_output).completion == 0);
    QCOMPARE(scene.node_badge(canvas_output).judgement,Judgement::NotEvaluated);
}

void WorkspaceTests::image_source_workbench()
{
    const auto registry = take(app::make_tool_registry());
    Workflow flow{inspection_id, "取像流程", {source_key<SchemaId>("test.image.source"), 1}};
    Solution solution{fixture_id<SolutionId>("6171"), "图像源", infrastructure::solution_format_version, 1, {std::move(flow)}};
    EditorWindow window(std::move(solution), registry);
    window.resize(1280, 760);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
    QVERIFY(scene.add_tool(source_key<ToolTypeId>("visionpro.image.source"), QPointF(100, 90)));
    QCOMPARE(scene.selected_nodes().size(), 1U);
    const auto selected = scene.selected_nodes().front();
    const auto* node = find_node(window.draft().workflows.front(), selected);
    QVERIFY(is_image_source(node) && source_value(*node,"frame_origin"));
    QCOMPARE(QString::fromStdString(node->name), QStringLiteral("图像源"));
    auto* strip = control<ImageSourceStrip>(window, "imageSourceStrip");
    auto* thumbs = control<QListWidget>(*strip, "imageSourceThumbnails");
    auto* auto_next = control<QCheckBox>(*strip, "imageSourceAutoNext");
    QTRY_VERIFY(strip->isVisible());
    auto* add_images = control<QPushButton>(*strip, "addSourceImages");
    auto* add_folder = control<QPushButton>(*strip, "addSourceFolder");
    QVERIFY(add_images->isVisible() && add_folder->isVisible());
    QCOMPARE(add_images->geometry().center().y(), add_folder->geometry().center().y());
    QVERIFY(add_images->geometry().right() < add_folder->geometry().left());
    QTemporaryDir pictures;
    QVERIFY(pictures.isValid());
    QStringList paths;
    for (int i = 0; i < 2; ++i) {
        QImage picture(9+i, 6, QImage::Format_RGB888);
        picture.fill(i ? Qt::blue : Qt::red);
        const auto path = pictures.filePath(QStringLiteral("sample-%1.png").arg(i));
        QVERIFY(picture.save(path));
        paths << path;
    }
    strip->add_files(paths);
    QTRY_COMPARE(thumbs->count(), 2);
    QTRY_VERIFY(!thumbs->item(0)->icon().isNull());
    thumbs->item(0)->setCheckState(Qt::Unchecked);
    auto* range = control<QComboBox>(*strip, "imageSourceRange");
    range->setCurrentIndex(1);
    emit range->activated(1);
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, selected));
    QTRY_VERIFY(visible_properties(window));
    auto* properties = visible_properties(window);
    QCOMPARE(properties->windowModality(), Qt::NonModal);
    auto* initial_sn = control<QWidget>(*properties, "source_sn_initial");
    QPushButton* subscribe_sn = nullptr;
    for (auto* button : properties->findChildren<QPushButton*>("editBinding")) {
        if (button->property("bindingTarget").toString() == "sn_subscription") { subscribe_sn = button; break; }
    }
    QVERIFY(subscribe_sn && subscribe_sn->isVisible() && initial_sn->isVisible());
    QCOMPARE(initial_sn->parentWidget(), subscribe_sn->parentWidget());
    QVERIFY(initial_sn->geometry().right() < subscribe_sn->geometry().left());
    QVERIFY(initial_sn->geometry().contains(QPoint(initial_sn->x(),subscribe_sn->geometry().center().y())));
    auto active_check = [&] {
        for (auto* box : properties->findChildren<QCheckBox*>("source_auto_next")) { if (box->isVisible()) { return box; } }
        return static_cast<QCheckBox*>(nullptr);
    };
    QTRY_VERIFY(active_check());
    QVERIFY(auto_next->isChecked() && active_check()->isChecked());
    active_check()->click();
    QTRY_VERIFY(!auto_next->isChecked());
    auto_next->click();
    QTRY_VERIFY(active_check() && active_check()->isChecked());
    const auto properties_image = qEnvironmentVariable("VISIONPRO_IMAGE_SOURCE_PARAMETERS_SCREENSHOT");
    if (!properties_image.isEmpty()) { QVERIFY(properties->grab().save(properties_image)); }
    QVERIFY(properties->close());
    QTRY_VERIFY(strip->isVisible() && !visible_properties(window));
    control<QAction>(window, "runWorkflow")->trigger();
    auto* history = window.findChild<ModuleHistoryPanel*>();
    QTRY_VERIFY_WITH_TIMEOUT(history->selected_invocation(),10000);
    const auto result = take(history->selected_run()->lookup(*history->selected_invocation()));
    QVERIFY(result);
    const auto* image = result->output(source_key<OutputId>("image"));
    QVERIFY(image && std::holds_alternative<DataValue>(image->content));
    QCOMPARE(std::get<DataValue>(image->content).as_image()->image.width(), 10);
    auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
    QCOMPARE(table->headerItem()->text(0), QStringLiteral("执行序号"));
    QVERIFY(table->topLevelItem(0)->text(1).contains(u'.'));
    auto* tabs = control<QTabBar>(window, "workspaceOutputTabs");
    QCOMPARE(tabs->tabText(2), QStringLiteral("模块结果"));
    tabs->setCurrentIndex(2);
    auto* outputs = control<QTreeWidget>(window, "moduleOutputTree");
    QTRY_COMPARE(outputs->topLevelItemCount(), 1);
    auto* root = outputs->topLevelItem(0);
    QCOMPARE(root->text(0), QStringLiteral("图像源"));
    QCOMPARE(root->text(2), QStringLiteral("第 1 轮"));
    QCOMPARE(root->childCount(), 6);
    QCOMPARE(root->child(0)->text(0), QStringLiteral("耗时（ms）"));
    auto* image_row = root->child(1);
    QCOMPARE(image_row->data(0, Qt::UserRole).toString(), QStringLiteral("image"));
    QCOMPARE(image_row->childCount(), 4);
    QCOMPARE(image_row->child(3)->text(2), QStringLiteral("17301505（Mono8）"));
    QCOMPARE(root->child(5)->text(0), QStringLiteral("SN 码"));
    outputs->setCurrentItem(image_row);
    const auto expression = QStringLiteral("run.lookup_output(call, \"%1\", \"image\")").arg(QString::fromStdString(selected.to_string()));
    QTRY_COMPARE(control<QLabel>(window,"sourceOutputReference")->text(), expression);
    control<QPushButton>(window, "copySourceReference")->click();
    QCOMPARE(QApplication::clipboard()->text(), expression);
    image_row->setExpanded(true);
    const auto shot = qEnvironmentVariable("VISIONPRO_IMAGE_SOURCE_WORKBENCH_SCREENSHOT");
    if (!shot.isEmpty()) { QVERIFY(window.grab().save(shot)); }
    outputs->setCurrentItem(image_row->child(0));
    QTest::keyClick(outputs, Qt::Key_Return);
    QTRY_COMPARE(tabs->currentIndex(), 0);
    QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, selected));
    QTRY_VERIFY(visible_properties(window));
    auto active_combo = [&] {
        for (auto* box : properties->findChildren<QComboBox*>("source_source_type")) { if (box->isVisible()) { return box; } }
        return static_cast<QComboBox*>(nullptr);
    };
    QTRY_VERIFY(active_combo());
    auto* choose = active_combo();
    const int sdk = choose->findData("SDK");
    QVERIFY(sdk >= 0);
    choose->setCurrentIndex(sdk);
    emit choose->activated(sdk);
    QTRY_VERIFY(!strip->isVisible());
    QVERIFY(source_enum(*find_node(window.draft().workflows.front(),selected),"source_type","Local") == "SDK");
    auto* input_tabs = control<QTabBar>(*properties, "moduleConfigTabs");
    QTRY_VERIFY(input_tabs->isVisible() && input_tabs->tabText(1) == QStringLiteral("SDK 接入"));
    QVERIFY(control<QWidget>(*properties, "source_sn_initial")->isVisible());
    auto* flow_input = control<QPushButton>(*properties, "createSourceFlowInput");
    QVERIFY(!flow_input->isVisible());
    input_tabs->setCurrentIndex(1);
    QPushButton* expose = nullptr;
    QTRY_VERIFY((expose = properties->findChild<QPushButton*>("createSourceFlowInput")) && expose->isVisible());
    expose->click();
    QCOMPARE(window.draft().workflows.front().interface.inputs.size(), 1U);
    auto* controller = window.findChild<RunController*>();
    for (const auto* command : {"debugSelectedNode","runWorkflow"}) {
        auto* run_action = control<QAction>(window, command);
        QTRY_VERIFY_WITH_TIMEOUT(controller->can_submit(window.draft().workflows.front()) && run_action->isEnabled(), 10000);
        const auto before = controller->runs().size();
        const bool debug = std::string_view(command) == "debugSelectedNode";
        bool supplied = false;
        QTimer input_timer;
        connect(&input_timer, &QTimer::timeout, &window, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) { return; }
            input_timer.stop();
            if (debug) {
                auto* kind = dialog->findChild<QComboBox*>("sdkDebugImageSource");
                if (!kind || kind->findData(true) < 0) { dialog->reject(); return; }
                kind->setCurrentIndex(kind->findData(true));
            } else {
                auto* file = dialog->findChild<QLineEdit*>("runInputFile.image");
                if (!file || dialog->findChild<QComboBox*>("sdkDebugImageSource")) { dialog->reject(); return; }
                file->setText(paths.front());
            }
            supplied = true;
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        });
        input_timer.start(5);
        run_action->trigger();
        QVERIFY2(supplied, command);
        QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == before + 1,10000);
        const auto& run = controller->runs().back();
        const auto progress = take(run.inspect());
        QVERIFY(progress.status == ExecutionStatus::Succeeded);
        const auto record = take(run.lookup(*progress.nodes.front().invocation));
        QVERIFY(record);
        const auto* frame = record->output(source_key<OutputId>("image"));
        QVERIFY(frame);
        const auto& pixels = std::get<DataValue>(frame->content).as_image()->image;
        QCOMPARE(pixels.width(),debug ? 2U : 9U);
        QCOMPARE(pixels.height(),debug ? 2U : 6U);
        if (debug) { QCOMPARE(pixels.format().order, ChannelOrder::Mono); }
        QCOMPARE(std::holds_alternative<NodeTarget>(run.request().target()),debug);
    }
    tabs->setCurrentIndex(2);
    QTRY_COMPARE(outputs->topLevelItemCount(), 1);
    QTRY_COMPARE(outputs->topLevelItem(0)->childCount(), 4);
    QCOMPARE(outputs->topLevelItem(0)->child(3)->text(0), QStringLiteral("SN 码"));
    QVERIFY(outputs->findItems(QStringLiteral("帧号"), Qt::MatchExactly | Qt::MatchRecursive).empty());
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
    window.resize(1440, 800);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* panel = control<ImagePanel>(window, "imagePanel");
    auto* source = control<QComboBox>(*panel, "workspaceImageSource");
    auto* thumbnails = control<QListView>(*panel, "workspaceImageThumbnails");
    auto* provenance = control<QLabel>(*panel, "imageProvenance");
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    QVERIFY(history && controller);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
    if (expect_ok) {
        QTest::mouseDClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, judge_id));
        QTRY_VERIFY(visible_properties(window));
        auto* properties = visible_properties(window);
        set_upper_limit(*properties);
        QVERIFY(properties->close());
    }
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto run = controller->runs().front();
    QTRY_VERIFY(panel->frame());
    compare_view_with_run(*panel, run);
    QVERIFY(!thumbnails->isVisible());

    const auto formatter = fixture_id<NodeId>(expect_ok ? "6942" : "6944");
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, Qt::NoModifier, node_position(*canvas, formatter));
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == formatter);
    const auto consumer = *history->selected_invocation();
    QTRY_COMPARE(source->count(), 5);
    QVERIFY(thumbnails->isVisible());
    QCOMPARE(thumbnails->model()->rowCount(), 5);
    QCOMPARE(source->currentIndex(), -1);
    QVERIFY(!panel->frame() && source->isVisible());
    QVERIFY(provenance->text().contains(QStringLiteral("请选择")));
    const auto progress = take(run.inspect());
    const auto record = take(run.lookup(consumer));
    QVERIFY(record);
    const auto previews = workspace_image_sources(run, *record);
    QCOMPARE(previews.size(), std::size_t(5));
    const auto first_preview = take(workspace_thumbnail(run, previews.front(), window.draft()));
    const auto last_preview = take(workspace_thumbnail(run, previews.back(), window.draft()));
    QVERIFY(!first_preview.isNull() && first_preview.width() <= 80 && first_preview.height() <= 48);
    QCOMPARE(first_preview.size(), last_preview.size());
    QVERIFY(first_preview != last_preview);
    for (int lane = 1; lane <= 5; ++lane) {
        const auto name = QStringLiteral("Measure lane %1").arg(lane);
        const int index = source->findText(name, Qt::MatchContains);
        QVERIFY2(index >= 0, qPrintable(name));
        if (lane % 2 == 1) {
            const auto thumbnail = thumbnails->model()->index(index, 0);
            thumbnails->scrollTo(thumbnail);
            QTRY_VERIFY(thumbnails->viewport()->rect().contains(thumbnails->visualRect(thumbnail).center()));
            QTest::mouseClick(thumbnails->viewport(), Qt::LeftButton, {}, thumbnails->visualRect(thumbnail).center());
        } else {
            source->setCurrentIndex(index);
        }
        QTRY_COMPARE(source->currentIndex(), index);
        QCOMPARE(thumbnails->currentIndex().row(), index);
        QTRY_VERIFY(panel->frame());
        compare_view_with_run(*panel, run, lane);
        const auto id = fixture_id<NodeId>(std::to_string(6919 + lane));
        const auto call = std::ranges::find(progress.nodes, id, &RunNodeProgress::node);
        QVERIFY(call != progress.nodes.end() && call->invocation);
        QVERIFY(provenance->text().contains(name));
        QVERIFY(provenance->text().endsWith(QStringLiteral("调用 %1").arg(call->invocation->invocation.value())));
        QVERIFY(history->selected_invocation() == consumer);
    }
    if (!expect_ok) {
        const auto screenshot = qEnvironmentVariable("VISIONPRO_IMAGE_SOURCES_SCREENSHOT");
        if (!screenshot.isEmpty()) { QVERIFY2(window.grab().save(screenshot), qPrintable(screenshot)); }
    }
    auto* follow = control<QCheckBox>(window, "followLatest");
    if (expect_ok) {
        auto* table = control<QTreeWidget>(window, "moduleHistoryTable");
        QTest::mouseClick(table->viewport(), Qt::LeftButton, {}, table->visualItemRect(table->topLevelItem(0)).center());
    }
    QCOMPARE(follow->isChecked(), !expect_ok);
    auto* view = control<ImageView>(*panel, "imageView");
    view->zoom_by(3);
    view->centerOn(25.5, 9.5);
    const auto magnified = view->transform();
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2, 10000);
    if (expect_ok) { follow->click(); }
    const auto latest = controller->runs().back();
    QTRY_VERIFY(history->selected_run() && history->selected_run()->identity() == latest.identity());
    QVERIFY(source->currentText().contains(QStringLiteral("Measure lane 5")));
    compare_view_with_run(*panel, latest, 5);
    QCOMPARE(view->transform(), magnified);
    source->setCurrentIndex(source->findText(QStringLiteral("Measure lane 1"), Qt::MatchContains));
    compare_view_with_run(*panel, latest, 1);
    QVERIFY(view->transform() != magnified);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, measure_id));
    QTRY_VERIFY(!thumbnails->isVisible());
    QCOMPARE(source->count(), 1);
    qInfo() << (expect_ok ? "Format OK" : "Format NG") << "keeps the selected display source across runs.";
}

void WorkspaceTests::recovers_image_view_after_failed_measurement()
{
    const auto package = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String());
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    EditorWindow window(document.solution(), registry);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto* panel = control<ImagePanel>(window, "imagePanel");
    auto* view = control<ImageView>(*panel, "imageView");
    auto* pixel = control<QLabel>(*panel, "imagePixel");
    auto* controller = window.findChild<RunController*>();
    auto* history = window.findChild<ModuleHistoryPanel*>();
    QVERIFY(controller && history);
    QTest::mouseClick(canvas->viewport(), Qt::LeftButton, {}, node_position(*canvas, measure_id));
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    QTRY_VERIFY(panel->frame());
    view->zoom_by(5);
    view->centerOn(25.5, 9.5);
    const auto center = [&] { return view->mapToScene(view->viewport()->rect().center()); };
    QTest::keyClick(view, Qt::Key_Right);
    QVERIFY(!pixel->text().isEmpty());
    const auto detail_center = center();
    const auto detail_zoom = view->transform();

    // Move this invocation's ROI outside the frame. The failed call is published with no image.
    auto rejected = window.draft();
    auto flow = std::ranges::find(rejected.workflows, inspection_id, &Workflow::id);
    QVERIFY(flow != rejected.workflows.end());
    auto measure = std::ranges::find(flow->nodes, measure_id, &Node::id);
    QVERIFY(measure != flow->nodes.end());
    auto binding = std::ranges::find(measure->bindings, BindingTarget{take(SchemaIdFactory::parse<ParameterId>("roi"))}, &Binding::target);
    QVERIFY(binding != measure->bindings.end());
    auto roi = *std::get<ConfigValue>(binding->sources.at(0)).as_region2d();
    std::get<AxisAlignedRect>(std::get<RegionPrimitive>(roi.source).geometry).x = 10000;
    binding->sources[0] = take(ConfigValue::region2d(roi));
    controller->start(std::move(rejected), {document, package, {}},
        {WorkflowTarget{inspection_id}, {}, controller->runs().front().request().trigger()}, {});
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2, 10000);
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == measure_id);
    const auto failed = take(controller->runs().back().lookup(*history->selected_invocation()));
    QVERIFY(failed && failed->data().status == ExecutionStatus::Failed);
    auto* tabs = control<QTabBar>(window, "workspaceOutputTabs");
    tabs->setCurrentIndex(2);
    auto* outputs = control<QTreeWidget>(window, "moduleOutputTree");
    QCOMPARE(outputs->topLevelItemCount(), 5);
    for (int row = 0; row < outputs->topLevelItemCount(); ++row) {
        const auto status = outputs->topLevelItem(row)->text(2);
        QVERIFY2(status.contains(QStringLiteral("失败")) || status.contains(QStringLiteral("未产出"))
            || status.contains(QStringLiteral("无效")), qPrintable(status));
    }
    tabs->setCurrentIndex(0);
    QTRY_VERIFY(!panel->frame());
    QVERIFY(pixel->text().isEmpty() && overlay_members(*view).isEmpty());
    for (const auto* item : view->scene()->items()) {
        if (const auto* layer = dynamic_cast<const QGraphicsPixmapItem*>(item)) { QVERIFY(layer->pixmap().isNull()); }
    }

    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 3, 10000);
    QTRY_VERIFY(panel->frame());
    compare_view_with_run(*panel, controller->runs().back());
    QCOMPARE(view->transform(), detail_zoom);
    QVERIFY(QLineF(center(), detail_center).length() < 0.2);
}

void WorkspaceTests::pages_large_output_collection()
{
    const auto node = fixture_id<NodeId>("6170");
    std::vector<ConfigValue> values;
    for (int i = 0; i < 150; ++i) { values.push_back(ConfigValue::integer(i)); }
    auto list = take(ConfigValue::list(DataType::integer(), values));
    Node formatter{node, "长列表", ToolConfiguration{take(SchemaIdFactory::parse<ToolTypeId>("visionpro.format.int64_list")), 1},
        {{take(SchemaIdFactory::parse<InputId>("list")), {std::move(list)}}}, {}};
    Workflow flow{inspection_id, "列表输出", {take(SchemaIdFactory::parse<SchemaId>("test.lists")), 1, {}, {}, {}},
        {std::move(formatter)}, {}, {}, {node}, {}};
    Solution draft{fixture_id<SolutionId>("6171"), "列表", 1, 1, {std::move(flow)}};
    const auto registry = take(app::make_tool_registry());
    EditorWindow window(draft, registry);
    window.show();
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    QVERIFY(dynamic_cast<ControlScene&>(*canvas->scene()).reveal_node(node));
    auto* controller = window.findChild<RunController*>();
    control<QAction>(window, "runWorkflow")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    control<QTabBar>(window, "workspaceOutputTabs")->setCurrentIndex(2);
    auto* outputs = control<QTreeWidget>(window, "moduleOutputTree");
    QTRY_COMPARE(outputs->topLevelItemCount(), 3);
    auto* collection = outputs->topLevelItem(2);
    QCOMPARE(collection->data(0, Qt::UserRole).toString(), QStringLiteral("used_values"));
    QCOMPARE(collection->childCount(), 0);
    collection->setExpanded(true);
    QCOMPARE(collection->childCount(), 65);
    const auto more = [&](int index) {
        auto* item = collection->child(index);
        QVERIFY(item->text(0).contains(QStringLiteral("显示更多")));
        outputs->setCurrentItem(item);
        QTest::keyClick(outputs, Qt::Key_Return);
    };
    more(64);
    QCOMPARE(collection->childCount(), 129);
    more(128);
    QCOMPARE(collection->childCount(), 150);
    QCOMPARE(collection->child(149)->text(2), QStringLiteral("149"));
}

void WorkspaceTests::flow_image_remembers_skipped_source()
{
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto package = fs::path(scratch.path().toStdU16String()) / "inspection";
    fs::copy(fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()), package, fs::copy_options::recursive);
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    auto draft = document.solution();
    auto& flow = draft.workflows.front();
    auto branch_measure = *find_node(flow, measure_id);
    branch_measure.id = fixture_id<NodeId>("6981");
    branch_measure.name = "NG 复核";
    flow.nodes.push_back(branch_measure);
    flow.control_links.push_back({fixture_id<ControlLinkId>("6982"), fixture_id<NodeId>("6944"), {}, branch_measure.id});
    take(infrastructure::save_solution(document, draft));
    EditorWindow window(draft, registry);
    window.show();
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    auto* canvas = control<QGraphicsView>(window, "workflowCanvas");
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
    auto* history = window.findChild<ModuleHistoryPanel*>();
    auto* controller = window.findChild<RunController*>();
    auto* tabs = control<QTabBar>(window, "workspaceOutputTabs");
    auto* channels = control<QComboBox>(window, "flowImageChannel");
    auto* run = control<QAction>(window, "runWorkflow");
    QVERIFY(scene.reveal_node(measure_id));
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto first = controller->runs().front();
    tabs->setCurrentIndex(2);
    auto* outputs = control<QTreeWidget>(window, "moduleOutputTree");
    QTRY_COMPARE(outputs->topLevelItemCount(), 5);
    QCOMPARE(outputs->topLevelItem(0)->data(0, Qt::UserRole).toString(), QStringLiteral("blue"));
    QCOMPARE(outputs->topLevelItem(0)->text(2), QStringLiteral("50"));
    QVERIFY(outputs->topLevelItem(0)->text(0) != outputs->topLevelItem(0)->data(0, Qt::UserRole).toString());
    tabs->setCurrentIndex(1);
    auto* flow_image = control<ImagePanel>(window, "flowImagePanel");
    auto* provenance = control<QLabel>(*flow_image, "imageProvenance");
    QCOMPARE(channels->currentIndex(), 0);
    const int channel = channels->findText(QStringLiteral("NG 复核 / "), Qt::MatchStartsWith);
    QVERIFY(channel > 0);
    channels->setCurrentIndex(channel);
    QTRY_VERIFY(flow_image->frame());
    const auto original_frame = *flow_image->frame();
    const auto original_label = channels->currentText();
    QVERIFY(original_frame.format().order == ChannelOrder::Mono);
    QVERIFY(scene.reveal_node(judge_id));
    QVERIFY(flow_image->frame() && *flow_image->frame() == original_frame);
    tabs->setCurrentIndex(0);
    tabs->setCurrentIndex(1);
    QVERIFY(flow_image->frame() && *flow_image->frame() == original_frame);
    auto* flows = control<QTabBar>(window, "workflowSelector");
    flows->setCurrentIndex(1);
    QCOMPARE(channels->currentIndex(), 0);
    QVERIFY(!flow_image->frame());
    channels->setCurrentIndex(1);
    flows->setCurrentIndex(0);
    QCOMPARE(channels->currentText(), original_label);
    QTRY_VERIFY(flow_image->frame() && *flow_image->frame() == original_frame);
    QVERIFY(scene.reveal_node(judge_id));
    auto accepted = window.draft();
    auto& nodes = accepted.workflows.front().nodes;
    auto& judge = *std::ranges::find(nodes, judge_id, &Node::id);
    auto& maximum = *std::ranges::find(judge.bindings,
        BindingTarget{take(SchemaIdFactory::parse<ParameterId>("maximum"))}, &Binding::target);
    maximum.sources[0] = take(ConfigValue::real(50));
    controller->start(std::move(accepted), {document, package, {}},
        {WorkflowTarget{flow.id}, {}, first.request().trigger()}, {});
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 2, 10000);
    QTRY_VERIFY(!flow_image->frame());
    QCOMPARE(channels->currentText(), original_label);
    QVERIFY(provenance->text().contains(QStringLiteral("跳过")));
    QVERIFY(control<ImagePanel>(window, "imagePanel")->frame());
    history->browse_run(first);
    QTRY_VERIFY(flow_image->frame() && *flow_image->frame() == original_frame);
    window.open_document(package);
    QTRY_VERIFY_WITH_TIMEOUT(!window.file_busy(), 10000);
    tabs->setCurrentIndex(1);
    QCOMPARE(channels->currentIndex(), 0);
    QVERIFY(!flow_image->frame());
}

void WorkspaceTests::loop_invocation_history()
{
    const auto original = fs::path(QString::fromUtf8(VISIONPRO_S69_DIRECTORY).toStdU16String()).parent_path() / "s6-loop";
    QTemporaryDir scratch;
    QVERIFY(scratch.isValid());
    const auto package = fs::path(scratch.path().toStdU16String()) / "loop";
    fs::copy(original, package, fs::copy_options::recursive);
    const auto registry = take(app::make_tool_registry());
    const auto document = take(infrastructure::load_solution(package, registry));
    const auto loop = fixture_id<NodeId>("6104");
    const auto child = fixture_id<NodeId>("6105");
    const auto peer = fixture_id<NodeId>("6198");
    const auto gate = fixture_id<NodeId>("6196");
    const auto conditional = fixture_id<NodeId>("6197");
    auto draft = document.solution();
    auto& flow = draft.workflows.front();
    auto second = *find_node(flow, child);
    second.id = peer;
    second.name = "复核组件";
    flow.nodes.push_back(std::move(second));
    flow.containment.push_back({loop, peer});
    flow.control_links.push_back({fixture_id<ControlLinkId>("6199"), child, {}, peer});
    const auto index = take(SchemaIdFactory::parse<InputId>("index"));
    flow.nodes.push_back({gate, "按迭代选择", ToolConfiguration{take(SchemaIdFactory::parse<ToolTypeId>("visionpro.sim.select")), 1},
        {{take(SchemaIdFactory::parse<InputId>("mask")), {ScopedInputReference{flow.id, loop, index, {}}}}}, {}});
    auto conditional_peer = *find_node(flow, child);
    conditional_peer.id = conditional;
    conditional_peer.name = "仅第二次执行";
    flow.nodes.push_back(std::move(conditional_peer));
    flow.containment.push_back({loop, gate});
    flow.containment.push_back({loop, conditional});
    flow.control_links.push_back({fixture_id<ControlLinkId>("6194"), peer, {}, gate});
    flow.control_links.push_back({fixture_id<ControlLinkId>("6195"), gate, take(SchemaIdFactory::parse<ControlOutputId>("left")), conditional});
    const auto placement = *find_placement(draft, flow.id, child);
    draft.layouts.front().nodes.push_back({peer, placement.x + 280, placement.y});
    take(infrastructure::save_solution(document, draft));
    EditorWindow window(draft, registry);
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
    auto& scene = dynamic_cast<ControlScene&>(*canvas->scene());
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
    QVERIFY(scene.reveal_node(peer));
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == peer);
    QVERIFY(history->selected_run()->identity() == run.identity() && history->selected_invocation()->scope == first->scope);
    auto* tabs = control<QTabBar>(window, "workspaceOutputTabs");
    tabs->setCurrentIndex(2);
    auto* outputs = control<QTreeWidget>(window, "moduleOutputTree");
    QTRY_VERIFY(outputs->topLevelItemCount() == 1);
    auto* blob = outputs->topLevelItem(0);
    QCOMPARE(blob->data(0, Qt::UserRole).toString(), QStringLiteral("item"));
    QCOMPARE(blob->childCount(), 0);
    blob->setExpanded(true);
    bool found_area = false;
    for (int row = 0; row < blob->childCount(); ++row) {
        if (blob->child(row)->data(0, Qt::UserRole).toString() == QStringLiteral("area_pixels")) {
            found_area = true;
            QCOMPARE(blob->child(row)->text(2), QStringLiteral("6"));
        }
    }
    QVERIFY(found_area);
    control<QTabBar>(window, "workspaceResultTabs")->setCurrentIndex(0);
    QVERIFY(control<QLabel>(window, "currentDetectionResult")->text().contains(QStringLiteral("迭代 0")));
    QVERIFY(scene.remove_selected_nodes());
    QVERIFY(scene.selected_nodes().empty());
    QVERIFY(!find_node(window.draft().workflows.front(), peer));
    QVERIFY(history->selected_invocation() && history->selected_invocation()->node == peer);
    QVERIFY(control<QLabel>(window, "resultContext")->text().contains(QStringLiteral("此前配置")));
    QCOMPARE(outputs->topLevelItemCount(), 1);
    scene.undoStack().undo();
    QVERIFY(find_node(window.draft().workflows.front(), peer));
    QVERIFY(scene.reveal_node(conditional));
    QTRY_VERIFY(!history->selected_invocation());
    QVERIFY(history->selected_run()->identity() == run.identity() && history->selected_scope() == first->scope);
    QVERIFY(history->selection_message().contains(QStringLiteral("跳过")));
    QCOMPARE(outputs->topLevelItemCount(), 1);
    QVERIFY(outputs->topLevelItem(0)->text(2).contains(QStringLiteral("跳过")));
    QVERIFY(!control<ImagePanel>(window, "imagePanel")->frame());
    control<QTabBar>(window, "workspaceResultTabs")->setCurrentIndex(1);
    QVERIFY(scene.reveal_node(child));
    select_iteration(1, 9);
    QVERIFY(scene.reveal_node(conditional));
    QTRY_VERIFY(history->selected_invocation() && history->selected_invocation()->node == conditional);
    QVERIFY(history->selected_invocation()->scope != first->scope);
}

QPushButton* mapping_button(EditorWindow& window, const QString& field)
{
    auto* tree = control<QTreeWidget>(window, "moduleOutputTree");
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == field) {
            return qobject_cast<QPushButton*>(tree->itemWidget(*it, 4));
        }
    }
    return nullptr;
}

bool edit_mapping(EditorWindow& window, const QString& field, VariableScope scope, const QString& choice,
    const QString& name = {}, const std::function<bool(const QComboBox&, const QComboBox&)>& check = {})
{
    auto* button = mapping_button(window, field);
    if (!button || !button->isEnabled()) { return false; }
    bool accepted = false;
    QTimer dispatch;
    dispatch.setSingleShot(true);
    QObject::connect(&dispatch, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) { return; }
        auto* scopes = dialog->findChild<QComboBox*>("resultVariableScope");
        auto* targets = dialog->findChild<QComboBox*>("resultVariableTarget");
        if (!scopes || !targets) { dialog->reject(); return; }
        scopes->setCurrentIndex(scopes->findData(static_cast<int>(scope)));
        if (check && !check(*scopes, *targets)) { dialog->reject(); return; }
        const int index = targets->findData(choice);
        if (index < 0) { dialog->reject(); return; }
        targets->setCurrentIndex(index);
        if (choice == QStringLiteral("new")) { control<QLineEdit>(*dialog, "resultVariableName")->setText(name); }
        auto* ok = dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
        if (!ok->isEnabled()) { dialog->reject(); return; }
        accepted = true;
        ok->click();
    });
    dispatch.start(0);
    button->click();
    return accepted;
}

void WorkspaceTests::edits_image_result_variables()
{
    const auto registry = take(app::make_tool_registry());
    const auto alternate = fixture_id<VariableId>("6174");
    Workflow flow{inspection_id, "取像流程", {source_key<SchemaId>("test.result.variables"), 1}};
    Solution solution{fixture_id<SolutionId>("6173"), "变量测试", infrastructure::solution_format_version, 1, {flow}};
    solution.variables.push_back({alternate, "备用编号", DataType::integer(), ConfigValue::integer(-10)});
    EditorWindow window(std::move(solution), registry);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto& scene = dynamic_cast<ControlScene&>(*control<QGraphicsView>(window, "workflowCanvas")->scene());
    QVERIFY(scene.add_tool(source_key<ToolTypeId>("visionpro.image.source"), {120, 80}));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("part.png");
    QImage picture(9, 6, QImage::Format_RGB888);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(path));
    control<ImageSourceStrip>(window, "imageSourceStrip")->add_files({path});
    control<QTabBar>(window, "workspaceOutputTabs")->setCurrentIndex(2);
    QTRY_VERIFY(mapping_button(window, "image_width"));
    QVERIFY(edit_mapping(window, "image_width", VariableScope::Global, {}, {}, [](const auto& scopes, const auto&) {
        auto* dialog = scopes.window();
        return control<QLabel>(*dialog, "resultVariableField")->text() == QStringLiteral("图像 · 宽度 · Int64")
            && control<QLineEdit>(*dialog, "resultVariableName")->text() == QStringLiteral("图像 · 宽度");
    }));
    QVERIFY(edit_mapping(window, "sn", VariableScope::Global, "new", "采集编号"));
    QVERIFY(edit_mapping(window, "file_name", VariableScope::Local, "new", "本轮文件名"));
    QVERIFY(edit_mapping(window, "image", VariableScope::Local, "new", "本轮原图"));
    const auto& draft = window.draft();
    QCOMPARE(draft.workflows.front().nodes.size(), 1U);
    QVERIFY(draft.workflows.front().control_links.empty());
    QCOMPARE(draft.workflows.front().nodes.front().result_variables.size(), 3U);
    QCOMPARE(draft.variables.size(), 2U);
    QCOMPARE(draft.workflows.front().local_variables.size(), 2U);
    const auto sn = draft.variables.back().id;
    const auto file = draft.workflows.front().local_variables.front().id;
    const auto image = draft.workflows.front().local_variables.back().id;
    QVERIFY(draft.workflows.front().local_variables.back().type == DataType::optional(DataType::image()));
    QTRY_VERIFY(mapping_button(window, "sn")->text().contains(QStringLiteral("采集编号")));
    auto* controller = window.findChild<RunController*>();
    auto* run = control<QAction>(window, "runWorkflow");
    QTRY_VERIFY(run->isEnabled());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto first_run = controller->runs().back();
    const auto first = take(first_run.inspect());
    QCOMPARE(first.status, ExecutionStatus::Succeeded);
    const auto global = take(controller->shared_preview());
    QVERIFY(global);
    const auto sampled = take(global->data->read(sn));
    QCOMPARE(sampled.revision, 1U);
    QVERIFY(sampled.current_value.as_int64() && *sampled.current_value.as_int64() == 1);
    const auto filename = std::ranges::find(first.local_variables, file, &GlobalVariableEntry::variable);
    QVERIFY(filename != first.local_variables.end() && filename->current_value.as_string());
    QCOMPARE(*filename->current_value.as_string(), std::string("part.png"));
    const auto frame = std::ranges::find(first.local_variables, image, &GlobalVariableEntry::variable);
    QVERIFY(frame != first.local_variables.end() && frame->current_value.optional_value());
    const auto* pixels = frame->current_value.optional_value()->as_image();
    QVERIFY(pixels && pixels->image.width() == 9 && pixels->image.height() == 6);
    const auto screenshot = qEnvironmentVariable("VISIONPRO_RESULT_VARIABLES_SCREENSHOT");
    if (!screenshot.isEmpty()) { QVERIFY(window.grab().save(screenshot)); }
    const auto alt = QString::fromStdString(alternate.to_string());
    const auto original = QString::fromStdString(sn.to_string());
    QVERIFY(edit_mapping(window, "sn", VariableScope::Global, alt, {},
        [&](const auto&, const auto& targets) { return targets.currentData().toString() == original; }));
    QVERIFY(edit_mapping(window, "file_name", VariableScope::Local, {}));
    QTRY_VERIFY(mapping_button(window, "sn")->text().contains(QStringLiteral("备用编号")));
    auto* removed = mapping_button(window, "file_name");
    QVERIFY(removed->text().isEmpty() && !removed->icon().isNull());
    QVERIFY(removed->accessibleName().contains(QStringLiteral("绑定全局或局部变量")));
    QTRY_VERIFY(run->isEnabled());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() >= 1 &&
        controller->runs().back().identity() != first_run.identity(), 10000);
    const auto after = take(controller->runs().back().inspect());
    QCOMPARE(after.status, ExecutionStatus::Succeeded);
    const auto updated = take(controller->shared_preview());
    QVERIFY(updated);
    const auto second = take(updated->data->read(alternate));
    QVERIFY(second.current_value.as_int64() && *second.current_value.as_int64() >= 1 && second.revision == 1);
    const auto unbound = std::ranges::find(after.local_variables, file, &GlobalVariableEntry::variable);
    QVERIFY(unbound != after.local_variables.end() && unbound->revision == 0);
    QCOMPARE(*unbound->current_value.as_string(), std::string{});
}

void WorkspaceTests::restricts_image_result_variables_to_direct_group()
{
    const auto registry = take(app::make_tool_registry());
    const auto flow_variable = fixture_id<VariableId>("6176"), outer_variable = fixture_id<VariableId>("6177");
    Workflow flow{inspection_id, "取像流程", {source_key<SchemaId>("test.group.variables"), 1}};
    flow.local_variables.push_back({flow_variable, "流程编号", DataType::integer(), ConfigValue::integer(-1)});
    EditorWindow window({fixture_id<SolutionId>("6175"), "嵌套组", infrastructure::solution_format_version, 1, {flow}}, registry);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto& scene = dynamic_cast<ControlScene&>(*control<QGraphicsView>(window, "workflowCanvas")->scene());
    const auto& model = static_cast<const ControlGraphModel&>(scene.graphModel());
    QVERIFY(scene.add_group("外组", {120, 80}));
    const auto outer = scene.selected_nodes().front();
    QVERIFY(scene.edit_draft("设置外组变量", [&](Solution& draft) -> Result<std::monostate> {
        auto& config = std::get<GroupConfiguration>(draft.workflows.front().nodes.front().configuration);
        config.local_variables.push_back({outer_variable, "外组编号", DataType::integer(), ConfigValue::integer(-2)});
        return std::monostate{};
    }));
    QVERIFY(scene.open_group(model.canvas_id(outer)));
    QVERIFY(scene.add_group("内组", {120, 80}));
    const auto inner = scene.selected_nodes().front();
    QVERIFY(scene.open_group(model.canvas_id(inner)));
    QVERIFY(scene.add_tool(source_key<ToolTypeId>("visionpro.image.source"), {120, 80}));
    control<QTabBar>(window, "workspaceOutputTabs")->setCurrentIndex(2);
    QTRY_VERIFY(mapping_button(window, "sn"));
    QVERIFY(edit_mapping(window, "sn", VariableScope::Local, "new", "内组编号", [&](const auto& scopes, const auto& targets) {
        return scopes.currentText().contains(QStringLiteral("内组"))
            && targets.findData(QString::fromStdString(flow_variable.to_string())) < 0
            && targets.findData(QString::fromStdString(outer_variable.to_string())) < 0;
    }));
    const auto& updated_flow = window.draft().workflows.front();
    const auto& own = std::get<GroupConfiguration>(find_node(updated_flow, inner)->configuration).local_variables;
    QCOMPARE(own.size(), 1U);
    QCOMPARE(own.front().name, std::string("内组编号"));
    QCOMPARE(updated_flow.local_variables.size(), 1U);
    QCOMPARE(std::get<GroupConfiguration>(find_node(updated_flow, outer)->configuration).local_variables.size(), 1U);
    const auto local_id = own.front().id;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QImage picture(5, 3, QImage::Format_Grayscale8);
    picture.fill(64);
    const auto path = directory.filePath("group-input.png");
    QVERIFY(picture.save(path));
    control<ImageSourceStrip>(window, "imageSourceStrip")->add_files({path});
    auto* controller = window.findChild<RunController*>();
    auto* run = control<QAction>(window, "runWorkflow");
    QTRY_VERIFY(run->isEnabled());
    run->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(controller->can_start() && controller->runs().size() == 1, 10000);
    const auto result = take(controller->runs().front().inspect());
    QCOMPARE(result.status, ExecutionStatus::Succeeded);
    QCOMPARE(result.local_variables.size(), 1U);
    QCOMPARE(result.local_variables.front().revision, 0U);
    const auto outer_values = std::ranges::find(result.group_local_variables, outer, &GroupLocalVariables::group);
    const auto inner_values = std::ranges::find(result.group_local_variables, inner, &GroupLocalVariables::group);
    QVERIFY(outer_values != result.group_local_variables.end() && inner_values != result.group_local_variables.end());
    QCOMPARE(outer_values->variables.front().revision, 0U);
    QCOMPARE(inner_values->variables.size(), 1U);
    const auto& written = inner_values->variables.front();
    QVERIFY(written.variable == local_id && written.revision == 1 && written.current_value.as_int64());
    QCOMPARE(*written.current_value.as_int64(), 1);
}
} // namespace
} // namespace visionpro::ui

QTEST_MAIN(visionpro::ui::WorkspaceTests)
#include "workspace_tests.moc"
