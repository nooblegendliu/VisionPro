#include "workspace_toolbox.hpp"
#include "canvas_overview.hpp"
#include "workspace_icons.hpp"
#include "workspace_appearance.hpp"
#include "toolbox_catalog.hpp"
#include "workspace_style.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
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

QString category_style()
{
    const auto& colors = workspace_colors();
    return QStringLiteral(
        "QToolButton { padding: 0; color: %1; background: transparent; border: 2px solid transparent; }"
        "QToolButton:hover { background: %2; }"
        "QToolButton:checked { background: %2; border-left-color: %3; }"
        "QToolButton:focus { border: 2px solid %3; }")
        .arg(colors.text.name(), colors.button.name(), colors.accent.name());
}

class ToolTile final : public QToolButton {
    Q_OBJECT
public:
    explicit ToolTile(QWidget* parent) : QToolButton(parent)
    {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::StrongFocus);
        setIconSize({workspace_size::tool_icon, workspace_size::tool_icon});
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override { return {72, iconSize().height() + 2 * fontMetrics().lineSpacing() + 12}; }
    QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }
    bool is_dragging() const { return dragging_; }

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
            painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), workspace_size::radius, workspace_size::radius);
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
    : QWidget(parent), selection_(new QButtonGroup(this)), drawer_(new QWidget(this))
{
    setObjectName(QStringLiteral("toolList"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto* rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("toolCategoryRail"));
    rail->setFixedWidth(rail_width);
    auto* rail_layout = new QVBoxLayout(rail);
    rail_layout->setContentsMargins(0, 0, 0, 0);
    rail_layout->setSpacing(0);
    row->addWidget(rail);
    search_button_ = new QToolButton(rail);
    search_button_->setObjectName(QStringLiteral("searchToolbox"));
    search_button_->setIcon(workspace_icon(WorkspaceIcon::Search));
    search_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    search_button_->setText(tr("搜索"));
    search_button_->setToolTip(tr("搜索全部节点"));
    search_button_->setAccessibleName(search_button_->toolTip());
    search_button_->setFocusPolicy(Qt::StrongFocus);
    search_button_->installEventFilter(this);
    search_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    search_button_->setMinimumHeight(workspace_size::header_height);
    rail_layout->addWidget(search_button_);

    const auto scroll = [](QWidget* parent) {
        auto* area = new QScrollArea(parent);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setFocusPolicy(Qt::NoFocus);
        return area;
    };
    auto* categories = scroll(rail);
    rail_layout->addWidget(categories, 1);
    auto* category_list = new QWidget;
    auto* rail_buttons = new QVBoxLayout(category_list);
    rail_buttons->setContentsMargins(0, 0, 0, 0);
    rail_buttons->setSpacing(0);
    categories->setWidget(category_list);
    drawer_->setObjectName(QStringLiteral("toolDrawer"));
    drawer_->hide();
    row->addWidget(drawer_, 1);
    auto* drawer_layout = new QVBoxLayout(drawer_);
    drawer_layout->setContentsMargins(5, 4, 4, 0);
    drawer_layout->setSpacing(4);
    search_ = new QLineEdit(drawer_);
    search_->setObjectName(QStringLiteral("toolSearch"));
    search_->setPlaceholderText(tr("搜索节点…"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(tr("搜索节点"));
    search_->installEventFilter(this);
    auto* search_row = new QHBoxLayout;
    search_row->setSpacing(workspace_size::small_gap);
    search_row->addWidget(search_, 1);
    pin_ = new QToolButton(drawer_);
    pin_->setObjectName(QStringLiteral("pinToolDrawer"));
    pin_->setText(tr("固定"));
    pin_->setCheckable(true);
    pin_->setToolTip(tr("固定或取消固定工具抽屉"));
    pin_->setAccessibleName(pin_->toolTip());
    pin_->installEventFilter(this);
    search_row->addWidget(pin_);
    drawer_layout->addLayout(search_row);
    connect(pin_, &QToolButton::toggled, this, [this](bool pinned) {
        emit drawerChanged(pinned && !drawer_->isHidden());
        position_drawer();
    });
    auto* tools = scroll(drawer_);
    drawer_layout->addWidget(tools, 1);
    auto* content = new QWidget;
    auto* list = new QVBoxLayout(content);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(2);
    tools->setWidget(content);
    const auto header_style = category_style();
    for (int i = 0; i < static_cast<int>(ToolboxCategory::Count); ++i) {
        const auto category = static_cast<ToolboxCategory>(i);
        const auto title = toolbox_category_name(category);
        auto* header = new QToolButton(category_list);
        header->setText(title);
        header->setAccessibleName(title);
        header->setToolTip(title);
        header->setProperty("toolCategory", title);
        header->setIcon(workspace_category_icon(category));
        header->setIconSize({18, 18});
        header->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        header->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        header->setFocusPolicy(Qt::StrongFocus);
        header->installEventFilter(this);
        header->setCheckable(true);
        header->setStyleSheet(header_style);
        header->setFont(workspace_font(WorkspaceText::Secondary));
        header->setFixedHeight(header->fontMetrics().height() + 25);
        rail_buttons->addWidget(header);
        auto* label = new QLabel(title, content);
        label->setFont(workspace_font(WorkspaceText::Category));
        list->addWidget(label);
        auto* panel = new QWidget(content);
        auto* grid = new QGridLayout(panel);
        grid->setContentsMargins(1, 1, 1, 1);
        grid->setSpacing(2);
        for (int column = 0; column < columns; ++column) { grid->setColumnStretch(column, 1); }
        list->addWidget(panel);
        sections_.push_back({header, label, grid, {}});
        connect(header, &QToolButton::clicked, this, [this, i] {
            const auto index = static_cast<std::size_t>(i);
            const bool closing = category_ == index;
            category_ = closing ? std::nullopt : std::optional{index};
            browsing_all_ = false;
            const QSignalBlocker block(search_);
            search_->clear();
            update_sections();
        });
    }
    rail_buttons->addStretch();
    empty_ = new QLabel(content);
    list->addWidget(empty_);
    list->addStretch();
    connect(search_, &QLineEdit::textChanged, this, &WorkspaceToolbox::filter);
    connect(search_button_, &QToolButton::clicked, this, [this] {
        browsing_all_ = !browsing_all_;
        category_.reset();
        const QSignalBlocker block(search_);
        search_->clear();
        update_sections();
        if (browsing_all_) { search_->setFocus(Qt::ShortcutFocusReason); }
    });
    const auto add = [this](std::string_view type, const QString& original) {
        const auto entry = toolbox_entry(type, original);
        auto& section = sections_.at(static_cast<size_t>(entry.category));
        auto* tile = new ToolTile(section.grid->parentWidget());
        tile->installEventFilter(this);
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
    update_sections();
    connect(selection_, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool checked) {
        if (checked) { emit selectionChanged(); }
    });
    connect(&WorkspaceAppearance::instance(), &WorkspaceAppearance::colorsChanged, this, [this] {
        const auto style = category_style();
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            sections_[i].header->setStyleSheet(style);
            sections_[i].header->setIcon(workspace_category_icon(static_cast<ToolboxCategory>(i)));
        }
        search_button_->setIcon(workspace_icon(WorkspaceIcon::Search));
        for (auto* tile : selection_->buttons()) {
            const auto type = tile->property("toolType").toString().toStdString();
            tile->setIcon(toolbox_entry(type, tile->text()).icon);
        }
    });
}

QString WorkspaceToolbox::selected_type() const
{
    const auto* selected = selection_->checkedButton();
    return selected && selected->isVisibleTo(this) ? selected->property("toolType").toString() : QString{};
}

bool WorkspaceToolbox::eventFilter(QObject* watched, QEvent* event)
{
    if (slot_ && (watched == slot_ || watched == slot_->parentWidget())) {
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show) {
            position_drawer();
        }
        return false;
    }
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && !drawer_->isHidden()) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier) {
            if (event->type() == QEvent::ShortcutOverride) {
                // Only claim Escape when a toolbox control receives it, including during a tile drag.
                event->accept();
                return true;
            }
            if (const auto* tile = qobject_cast<ToolTile*>(watched); tile && tile->is_dragging()) {
                return false;
            }
            auto* focus = category_ ? sections_[*category_].header : search_button_;
            category_.reset();
            browsing_all_ = false;
            const QSignalBlocker block(search_);
            search_->clear();
            update_sections();
            focus->setFocus(Qt::ShortcutFocusReason);
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void WorkspaceToolbox::filter(const QString& query)
{
    if (query != search_->text()) {
        search_->setText(query);
        return;
    }
    update_sections();
}

void WorkspaceToolbox::attach_to_slot(QWidget& slot)
{
    slot_ = &slot;
    slot_->installEventFilter(this);
    slot_->parentWidget()->installEventFilter(this);
    setAutoFillBackground(true);
    position_drawer();
}

void WorkspaceToolbox::position_drawer()
{
    if (!slot_) { return; }
    const auto origin = slot_->mapTo(parentWidget(), QPoint{});
    const int width = pinned() || drawer_->isHidden() ? slot_->width() : drawer_width_;
    setGeometry(QRect(origin, QSize(std::min(width, parentWidget()->width() - origin.x()), slot_->height())));
    raise();
}

bool WorkspaceToolbox::pinned() const { return pin_->isChecked(); }

void WorkspaceToolbox::set_pinned(bool pinned)
{
    category_.reset();
    browsing_all_ = pinned;
    search_->clear();
    pin_->setChecked(pinned);
    update_sections();
}

void WorkspaceToolbox::set_drawer_width(int width) { drawer_width_ = std::max(240, width); }

void WorkspaceToolbox::set_developer_tools(bool visible)
{
    developer_tools_ = visible;
    update_sections();
}

void WorkspaceToolbox::update_sections()
{
    const auto term = search_->text().trimmed();
    const bool searching = !term.isEmpty();
    const bool open = searching || browsing_all_ || category_.has_value();
    bool found = false;
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        auto& section = sections_[i];
        const bool selected = category_ == i;
        const bool by_name = section.header->text().contains(term, Qt::CaseInsensitive);
        section.header->setChecked(selected);
        int shown = 0;
        bool available = false;
        for (auto* tile : section.tiles) {
            const bool offered = developer_tools_ || !tile->property("toolType").toString().startsWith(QStringLiteral("visionpro.sim."));
            available |= offered;
            const bool visible = offered && (by_name || tile->toolTip().contains(term, Qt::CaseInsensitive));
            section.grid->removeWidget(tile);
            tile->setVisible(visible);
            if (visible) {
                section.grid->addWidget(tile, shown / columns, shown % columns);
                ++shown;
            }
        }
        section.header->setVisible(available);
        const bool show_section = available && (searching || browsing_all_ ? shown > 0 : selected);
        section.label->setVisible(show_section);
        section.grid->parentWidget()->setVisible(show_section && shown > 0);
        found |= show_section && shown > 0;
    }
    empty_->setText(searching ? tr("没有匹配的节点") : tr("此分类暂无可用节点"));
    empty_->setVisible(!found);
    const bool was_open = !drawer_->isHidden();
    drawer_->setVisible(open);
    if (was_open != open) {
        emit drawerChanged(open && pinned());
        position_drawer();
    }
    emit selectionChanged();
}

bool WorkspaceToolbox::accepts_drag(const QObject* source, const QMimeData* payload) const
{
    const auto* tile = qobject_cast<const QAbstractButton*>(source);
    return tile && tile->group() == selection_ && tile->isEnabled() && tile->isVisibleTo(this)
        && payload && payload->hasFormat(mime_type)
        && payload->data(mime_type) == tile->property("toolType").toString().toUtf8();
}

WorkflowCanvas::WorkflowCanvas(QtNodes::BasicGraphicsScene* scene, WorkspaceToolbox* toolbox, AddTool add)
    : QtNodes::GraphicsView(scene), toolbox_(toolbox), add_(std::move(add)), zoom_controls_(new QFrame(this)),
      overview_(new CanvasOverview(*this, *scene))
{
    setAcceptDrops(true);
    setBackgroundBrush(workspace_colors().canvas);
    connect(&WorkspaceAppearance::instance(), &WorkspaceAppearance::colorsChanged, this, [this] {
        setBackgroundBrush(workspace_colors().canvas);
    });
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
    overview_->place_above(bar, area);
}

void WorkflowCanvas::drawBackground(QPainter* painter, const QRectF& exposed)
{
    QGraphicsView::drawBackground(painter, exposed);
    qreal step = 24.0;
    while (step * getScale() < 14.0) { step *= 4; }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(QPen(workspace_colors().grid, 0));
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
