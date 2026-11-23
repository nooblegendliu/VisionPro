#pragma once

#include <QtNodes/GraphicsView>
#include <QScrollArea>

#include <visionpro/core/tool_registry.hpp>

#include <functional>
#include <optional>
#include <vector>

class QButtonGroup;
class QGridLayout;
class QMimeData;
class QToolButton;

namespace visionpro::ui {

class WorkspaceToolbox final : public QScrollArea {
    Q_OBJECT
public:
    explicit WorkspaceToolbox(const core::ToolRegistry& registry, QWidget* parent = nullptr);
    QString selected_type() const;
    void filter(const QString& query);
    bool accepts_drag(const QObject* source, const QMimeData* payload) const;

signals:
    void selectionChanged();
    void toolActivated();
    void toolDropped(const QObject* source, QPoint global_position);

private:
    struct Section {
        QToolButton* header;
        QGridLayout* grid;
        std::vector<QToolButton*> tiles;
        bool expanded_before_search{};
    };
    std::vector<Section> sections_;
    QButtonGroup* selection_;
    bool filtering_{};
};

class WorkflowCanvas final : public QtNodes::GraphicsView {
public:
    using AddTool = std::function<void(const QString&, QPointF)>;
    WorkflowCanvas(QtNodes::BasicGraphicsScene* scene, WorkspaceToolbox* toolbox, AddTool add);
    bool drop_tool(const QObject* source, const QMimeData* payload, QPoint viewport_position);
    void open_at_actual_size(const QRectF& entry);
    QPointF view_center() const;
    void restore_view(qreal scale, QPointF center);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& exposed) override;

private:
    bool accepts(const QDropEvent& event) const;
    void fit_workflow();
    void place_zoom_controls();
    WorkspaceToolbox* toolbox_;
    AddTool add_;
    std::optional<QRectF> pending_entry_;
    QFrame* zoom_controls_;
};

} // namespace visionpro::ui
