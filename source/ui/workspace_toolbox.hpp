#pragma once

#include <QtNodes/GraphicsView>
#include <QPointer>
#include <QWidget>

#include <visionpro/core/tool_registry.hpp>

#include <functional>
#include <optional>
#include <vector>

class QButtonGroup;
class QFrame;
class QGridLayout;
class QLabel;
class QLineEdit;
class QMimeData;
class QToolButton;

namespace visionpro::ui {

class WorkspaceToolbox final : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceToolbox(const core::ToolRegistry& registry, QWidget* parent = nullptr);
    static constexpr int rail_width = 76;
    QString selected_type() const;
    void filter(const QString& query);
    void attach_to_slot(QWidget& slot);
    bool pinned() const;
    bool docked() const { return pinned() && !drawer_->isHidden(); }
    void set_pinned(bool pinned);
    void set_developer_tools(bool visible);
    int drawer_width() const { return drawer_width_; }
    void set_drawer_width(int width);
    bool accepts_drag(const QObject* source, const QMimeData* payload) const;

signals:
    void selectionChanged();
    void toolActivated();
    void toolDropped(const QObject* source, QPoint global_position);
    void drawerChanged(bool docked);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    struct Section {
        QToolButton* header;
        QLabel* label;
        QGridLayout* grid;
        std::vector<QToolButton*> tiles;
    };
    void update_sections();
    void position_drawer();
    std::vector<Section> sections_;
    QButtonGroup* selection_;
    QWidget* drawer_;
    QToolButton* search_button_;
    QToolButton* pin_{};
    QPointer<QWidget> slot_;
    int drawer_width_{294};
    bool developer_tools_{};
    QLineEdit* search_;
    QLabel* empty_;
    std::optional<std::size_t> category_;
    bool browsing_all_{};
};

class CanvasOverview;

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
    CanvasOverview* overview_;
};

} // namespace visionpro::ui
