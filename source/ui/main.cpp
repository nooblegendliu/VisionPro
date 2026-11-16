#include "canvas_example.hpp"
#include "control_scene.hpp"

#include <simulation.hpp>

#include <QtNodes/GraphicsView>
#include <QAction>
#include <QApplication>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QUndoStack>

#include <stdexcept>

namespace {

using namespace visionpro;

int show_canvas(QApplication& application)
{
    auto registered = core::ToolRegistry::create(tools::simulation_registrations());
    if (const auto* failure = std::get_if<core::Diagnostic>(&registered)) {
        throw std::runtime_error(failure->message);
    }
    auto draft = ui::canvas_example();
    const auto workflow_id = draft.workflows.front().id;
    ui::ControlGraphModel model(std::move(draft), workflow_id, std::get<core::ToolRegistry>(registered));
    ui::ControlScene scene(model);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("VisionPro - Workflow Canvas"));
    auto* view = new QtNodes::GraphicsView(&scene, &window);
    // QtNodes' clipboard/deletion shortcuts carry only canvas addresses.
    // Expose the domain commands and their shared undo stack through the window.
    for (auto* action : view->actions()) {
        if (action != view->clearSelectionAction()) { view->removeAction(action); }
    }
    window.setCentralWidget(view);
    const auto frame_canvas = [&scene, view] {
        const auto bounds = scene.itemsBoundingRect().adjusted(-25, -25, 25, 25);
        view->resetTransform();
        if (bounds.width() > view->viewport()->width() || bounds.height() > view->viewport()->height()) {
            view->fitInView(bounds, Qt::KeepAspectRatio);
        }
        view->centerOn(bounds.center());
    };

    auto* toolbar = window.addToolBar(QStringLiteral("Workflow"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* up = toolbar->addAction(QStringLiteral("Parent Group"));
    auto* path = new QLabel(model.scope_path(), toolbar);
    toolbar->addWidget(path);
    toolbar->addSeparator();
    auto* undo = scene.undoStack().createUndoAction(toolbar, QStringLiteral("Undo"));
    auto* redo = scene.undoStack().createRedoAction(toolbar, QStringLiteral("Redo"));
    undo->setShortcuts(QKeySequence::Undo);
    redo->setShortcuts(QKeySequence::Redo);
    toolbar->addAction(undo);
    toolbar->addAction(redo);
    auto* remove = toolbar->addAction(QStringLiteral("Remove Link"));
    remove->setShortcuts(QKeySequence::Delete);
    remove->setEnabled(false);
    up->setEnabled(false);

    auto* status = window.statusBar();
    status->addWidget(new QLabel(
        QStringLiteral("Drag an output to an input to connect. Double-click a Group to open it."), status));
    QObject::connect(&scene, &ui::ControlScene::edit_failed, &window, [status](const QString& message) {
        status->showMessage(message, 7000);
    });
    QObject::connect(up, &QAction::triggered, &window, [&scene] { scene.parent_scope(); });
    QObject::connect(&scene, &QtNodes::BasicGraphicsScene::nodeDoubleClicked, &window,
        [&window, &scene](QtNodes::NodeId node) {
            // Navigation replaces the graphics item that emitted this event.
            QTimer::singleShot(0, &window, [&scene, node] { scene.open_group(node); });
        });
    QObject::connect(&model, &QtNodes::AbstractGraphModel::modelReset, &window, [&] {
        path->setText(model.scope_path());
        up->setEnabled(model.scope().has_value());
        frame_canvas();
    });
    QObject::connect(&scene, &QGraphicsScene::selectionChanged, &window, [&] {
        remove->setEnabled(scene.selected_control().has_value());
    });
    QObject::connect(remove, &QAction::triggered, &window, [&] {
        if (const auto selected = scene.selected_control()) { scene.remove_control(*selected); }
    });

    window.resize(1200, 650);
    window.show();
    frame_canvas();
    return application.exec();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("VisionPro"));
    try {
        return show_canvas(application);
    } catch (const std::exception& failure) {
        QMessageBox::critical(nullptr, QStringLiteral("VisionPro"), QString::fromUtf8(failure.what()));
        return 1;
    }
}
