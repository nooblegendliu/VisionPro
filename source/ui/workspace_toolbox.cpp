#include "workspace_toolbox.hpp"
#include "toolbox_catalog.hpp"
#include "workspace_style.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>

namespace visionpro::ui {
namespace {

const QString mime_type = QStringLiteral("application/x-visionpro-tool-type");
constexpr int columns = 3;

class ToolTile final : public QToolButton {
    Q_OBJECT
public:
    explicit ToolTile(QWidget* parent) : QToolButton(parent)
    {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::StrongFocus);
        setIconSize({24, 24});
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return {72, iconSize().height() + 2 * fontMetrics().lineSpacing() + 12}; }
    QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }

signals:
    void activated();
    void dropped(QPoint global_position);

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text.clear();
        option.icon = QIcon{};
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        if (isChecked()) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(option.palette.highlight());
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 3, 3);
        }

        const int side = iconSize().height();
        icon().paint(&painter, QRect((width() - side) / 2, 4, side, side), Qt::AlignCenter,
            isEnabled() ? QIcon::Normal : QIcon::Disabled);
        const auto label = rect().adjusted(2, side + 7, -2, -1);
        painter.setPen(option.palette.color(isChecked() ? QPalette::HighlightedText : QPalette::ButtonText));
        painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWrapAnywhere, text());
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) { press_point_ = event->position().toPoint(); }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (press_point_ && event->buttons().testFlag(Qt::LeftButton)
            && (event->position().toPoint() - *press_point_).manhattanLength() >= QApplication::startDragDistance()) {
            press_point_.reset();
            setChecked(true);
            setDown(false);
            dragging_ = true;
            setCursor(Qt::DragCopyCursor);
        }
        if (dragging_) { event->accept(); }
        else { QToolButton::mouseMoveEvent(event); }
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        press_point_.reset();
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            unsetCursor();
            emit dropped(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            press_point_.reset();
            setChecked(true);
            emit activated();
            event->accept();
        } else { QToolButton::mouseDoubleClickEvent(event); }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Escape && dragging_) {
            cancel_drag();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            setChecked(true);
            if (!event->isAutoRepeat()) { emit activated(); }
            event->accept();
        } else { QToolButton::keyPressEvent(event); }
    }

private:
    void cancel_drag()
    {
        press_point_.reset();
        dragging_ = false;
        unsetCursor();
        setDown(false);
    }

    bool event(QEvent* event) override
    {
        if (dragging_ && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowDeactivate || (event->type() == QEvent::EnabledChange && !isEnabled()))) {
            cancel_drag();
        }
        return QToolButton::event(event);
    }

    std::optional<QPoint> press_point_;
    bool dragging_{};
};

} // namespace

WorkspaceToolbox::WorkspaceToolbox(const core::ToolRegistry& registry, QWidget* parent)
    : QScrollArea(parent), selection_(new QButtonGroup(this))
{
    setObjectName(QStringLiteral("toolList"));
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(2);
    for (int i = 0; i < static_cast<int>(ToolboxCategory::Count); ++i) {
        const auto category = static_cast<ToolboxCategory>(i);
        auto* header = new QToolButton(content);
        const auto title = toolbox_category_name(category);
        header->setText(title);
        header->setProperty("toolCategory", title);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        header->setFocusPolicy(Qt::StrongFocus);
        header->setCheckable(true);
        header->setStyleSheet(QStringLiteral("QToolButton { text-align: left; padding: 2px 5px;"
            " border: 0; border-bottom: 1px solid palette(midlight); background: transparent; }"
            "QToolButton:checked, QToolButton:hover { background: palette(alternate-base); }"
            "QToolButton:focus { border: 1px solid palette(highlight); }"));
        header->setFont(workspace_font(WorkspaceText::Category));
        header->setMinimumHeight(std::max(27, header->fontMetrics().height() + 6));
        const bool open = category == ToolboxCategory::Acquisition || category == ToolboxCategory::Location;
        header->setChecked(open);
        header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        list->addWidget(header);

        auto* panel = new QWidget(content);
        auto* grid = new QGridLayout(panel);
        grid->setContentsMargins(1, 1, 1, 1);
        grid->setSpacing(2);
        for (int column = 0; column < columns; ++column) { grid->setColumnStretch(column, 1); }
        list->addWidget(panel);
        panel->setVisible(open);
        sections_.push_back({header, grid, {}});
        connect(header, &QToolButton::toggled, this, [this, header, panel](bool expanded) {
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            panel->setVisible(expanded);
            emit selectionChanged();
        });
    }
    list->addStretch();
    setWidget(content);
    const auto add = [this](std::string_view type, const QString& original) {
        const auto entry = toolbox_entry(type, original);
        auto& section = sections_.at(static_cast<size_t>(entry.category));
        auto* tile = new ToolTile(section.grid->parentWidget());
        const auto id = QString::fromUtf8(type);
        tile->setText(entry.name);
        tile->setAccessibleName(entry.name);
        tile->setIcon(entry.icon);
        tile->setProperty("toolType", id);
        QStringList tip{entry.name};
        if (!original.isEmpty() && original != entry.name) { tip << original; }
        tip << id;
        tile->setToolTip(tip.join(u'\n'));
        selection_->addButton(tile);
        const int index = static_cast<int>(section.tiles.size());
        section.grid->addWidget(tile, index / columns, index % columns);
        section.tiles.push_back(tile);
        connect(tile, &ToolTile::activated, this, &WorkspaceToolbox::toolActivated);
        connect(tile, &ToolTile::dropped, this, [this, tile](QPoint position) { emit toolDropped(tile, position); });
    };
    for (const auto& tool : registry.descriptors()) { add(tool.id.text(), QString::fromStdString(tool.display_name)); }
    add("@group", {});
    add("@loop", {});
    for (const auto& section : sections_) {
        if (section.header->isChecked() && !section.tiles.empty()) {
            section.tiles.front()->setChecked(true);
            break;
        }
    }
    connect(selection_, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool checked) {
        if (checked) { emit selectionChanged(); }
    });
}

QString WorkspaceToolbox::selected_type() const
{
    const auto* selected = selection_->checkedButton();
    return selected && selected->isVisibleTo(widget()) ? selected->property("toolType").toString() : QString{};
}

void WorkspaceToolbox::filter(const QString& query)
{
    const auto term = query.trimmed();
    const bool searching = !term.isEmpty();
    QSignalBlocker changes(this);
    for (auto& section : sections_) {
        if (searching && !filtering_) { section.expanded_before_search = section.header->isChecked(); }
        const bool category_matches = section.header->text().contains(term, Qt::CaseInsensitive);
        int shown = 0;
        for (auto* tile : section.tiles) {
            const bool matches = category_matches || tile->toolTip().contains(term, Qt::CaseInsensitive);
            section.grid->removeWidget(tile);
            tile->setVisible(matches);
            if (matches) {
                section.grid->addWidget(tile, shown / columns, shown % columns);
                ++shown;
            }
        }
        const bool visible = category_matches || shown > 0;
        section.header->setVisible(visible);
        if (searching) { section.header->setChecked(visible); }
        else if (filtering_) { section.header->setChecked(section.expanded_before_search); }
    }
    filtering_ = searching;
    changes.unblock();
    emit selectionChanged();
}

bool WorkspaceToolbox::accepts_drag(const QObject* source, const QMimeData* payload) const
{
    const auto* tile = qobject_cast<const QAbstractButton*>(source);
    return tile && tile->group() == selection_ && tile->isEnabled() && tile->isVisibleTo(widget())
        && payload && payload->hasFormat(mime_type)
        && payload->data(mime_type) == tile->property("toolType").toString().toUtf8();
}

WorkflowCanvas::WorkflowCanvas(QtNodes::BasicGraphicsScene* scene, WorkspaceToolbox* toolbox, AddTool add)
    : QtNodes::GraphicsView(scene), toolbox_(toolbox), add_(std::move(add)), zoom_controls_(new QFrame(this))
{
    setAcceptDrops(true);
    setBackgroundBrush(QColor("#32393e"));
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    zoom_controls_->setAutoFillBackground(true);
    zoom_controls_->setFrameShape(QFrame::StyledPanel);
    auto* row = new QHBoxLayout(zoom_controls_);
    row->setContentsMargins(3, 1, 3, 1);
    row->setSpacing(1);
    const auto button = [&](const char* name, const QString& text, const QString& description) {
        auto* item = new QToolButton(zoom_controls_);
        item->setObjectName(QString::fromLatin1(name));
        item->setText(text);
        item->setAccessibleName(description);
        item->setToolTip(description);
        item->setAutoRaise(true);
        row->addWidget(item);
        return item;
    };
    auto* smaller = button("canvasZoomOut", QStringLiteral("−"), tr("缩小流程画布"));
    auto* scale = new QLabel(QStringLiteral("100%"), zoom_controls_);
    scale->setObjectName(QStringLiteral("canvasZoomPercent"));
    scale->setAlignment(Qt::AlignCenter);
    scale->setMinimumWidth(scale->fontMetrics().horizontalAdvance(QStringLiteral("200%")));
    row->addWidget(scale);
    auto* larger = button("canvasZoomIn", QStringLiteral("+"), tr("放大流程画布"));
    auto* actual = button("canvasActualSize", QStringLiteral("1:1"), tr("恢复 100% 编辑倍率"));
    auto* fit = button("canvasFitAll", tr("全图"), tr("适应全图"));
    const auto zoom_at_center = [this](bool in) {
        const auto center = view_center();
        if (in) { scaleUp(); } else { scaleDown(); }
        centerOn(center);
    };
    connect(smaller, &QToolButton::clicked, this, [zoom_at_center] { zoom_at_center(false); });
    connect(larger, &QToolButton::clicked, this, [zoom_at_center] { zoom_at_center(true); });
    connect(actual, &QToolButton::clicked, this, [this] {
        restore_view(1.0, view_center());
    });
    connect(fit, &QToolButton::clicked, this, &WorkflowCanvas::fit_workflow);
    connect(this, &WorkflowCanvas::scaleChanged, scale, [scale](double factor) {
        scale->setText(QObject::tr("%1%").arg(factor < 0.01 ? QStringLiteral("<1") : QString::number(qRound(100 * factor))));
    });
    connect(toolbox_, &WorkspaceToolbox::toolDropped, this, [this](const QObject* source, QPoint global_position) {
        auto* target = QApplication::widgetAt(global_position);
        if (!target || (target != viewport() && !viewport()->isAncestorOf(target))) { return; }
        QMimeData payload;
        payload.setData(mime_type, source->property("toolType").toString().toUtf8());
        drop_tool(source, &payload, viewport()->mapFromGlobal(global_position));
    });
}

void WorkflowCanvas::open_at_actual_size(const QRectF& entry)
{
    if (!isVisible()) {
        pending_entry_ = entry;
        return;
    }
    restore_view(1.0, scene()->itemsBoundingRect().center());
    if (!entry.isEmpty()) { ensureVisible(entry, 24, 24); }
}

QPointF WorkflowCanvas::view_center() const
{
    return viewportTransform().inverted().map(QRectF(viewport()->rect()).center());
}

void WorkflowCanvas::restore_view(qreal scale, QPointF center)
{
    pending_entry_.reset();
    // An explicit overview of a large graph may fall below the usual wheel limit.
    setScaleRange(std::min(scale, 0.3), 2.0);
    setupScale(scale);
    centerOn(center);
}

void WorkflowCanvas::fit_workflow()
{
    const QRectF bounds = scene()->itemsBoundingRect().adjusted(-18, -18, 18, 18);
    const int footer = zoom_controls_->sizeHint().height() + 8;
    const qreal scale = std::min({1.0, viewport()->width() / bounds.width(),
        (viewport()->height() - footer) / bounds.height()});
    if (scale > 0.0) { restore_view(scale, bounds.center() + QPointF(0, footer / (2 * scale))); }
}

void WorkflowCanvas::showEvent(QShowEvent* event)
{
    // QtNodes fits the whole scene on every show, discarding the user's zoom.
    QGraphicsView::showEvent(event);
    place_zoom_controls();
    if (const auto entry = pending_entry_) { open_at_actual_size(*entry); }
}

void WorkflowCanvas::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    place_zoom_controls();
}

void WorkflowCanvas::place_zoom_controls()
{
    // Viewport children scroll with the scene; this sibling stays in view coordinates.
    const QRect area = viewport()->geometry().adjusted(8, 8, -8, -8);
    QRect bar(QPoint{}, zoom_controls_->sizeHint());
    bar.moveBottomRight(area.bottomRight());
    zoom_controls_->setGeometry(bar);
    zoom_controls_->raise();
}

void WorkflowCanvas::drawBackground(QPainter* painter, const QRectF& exposed)
{
    QGraphicsView::drawBackground(painter, exposed);
    qreal step = 24.0;
    while (step * getScale() < 14.0) { step *= 4; }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(QPen(QColor("#3b4247"), 0));
    for (qreal x = std::ceil(exposed.left() / step) * step; x < exposed.right(); x += step) {
        painter->drawLine(QLineF(x, exposed.top(), x, exposed.bottom()));
    }
    for (qreal y = std::ceil(exposed.top() / step) * step; y < exposed.bottom(); y += step) {
        painter->drawLine(QLineF(exposed.left(), y, exposed.right(), y));
    }
    painter->restore();
}

bool WorkflowCanvas::accepts(const QDropEvent& event) const
{
    return toolbox_->accepts_drag(event.source(), event.mimeData());
}

void WorkflowCanvas::dragEnterEvent(QDragEnterEvent* event)
{
    if (accepts(*event)) { event->acceptProposedAction(); }
    else { event->ignore(); }
}

void WorkflowCanvas::dragMoveEvent(QDragMoveEvent* event)
{
    if (accepts(*event)) { event->acceptProposedAction(); }
    else { event->ignore(); }
}

bool WorkflowCanvas::drop_tool(const QObject* source, const QMimeData* payload, QPoint viewport_position)
{
    if (!toolbox_->accepts_drag(source, payload)) { return false; }
    add_(QString::fromUtf8(payload->data(mime_type)), mapToScene(viewport_position));
    return true;
}

void WorkflowCanvas::dropEvent(QDropEvent* event)
{
    if (drop_tool(event->source(), event->mimeData(), event->position().toPoint())) { event->acceptProposedAction(); }
    else { event->ignore(); }
}

} // namespace visionpro::ui

#include "workspace_toolbox.moc"
