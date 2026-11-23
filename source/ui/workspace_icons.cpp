#include "workspace_icons.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace visionpro::ui {
namespace {

// The toolbar and menus request the same icon at different sizes and pixel densities.
class WorkspaceIconEngine final : public QIconEngine {
public:
    explicit WorkspaceIconEngine(WorkspaceIcon glyph) : glyph_(glyph) {}
    QIconEngine* clone() const override { return new WorkspaceIconEngine(glyph_); }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap image(size * scale);
        image.fill(Qt::transparent);
        image.setDevicePixelRatio(scale);
        QPainter painter(&image);
        paint(&painter, QRect(QPoint{}, size), mode, state);
        return image;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        const auto role = mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::ButtonText;
        const auto ink = QApplication::palette().color(group, role);
        const int side = qMin(rect.width(), rect.height());
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->translate(rect.x() + (rect.width() - side) / 2.0, rect.y() + (rect.height() - side) / 2.0);
        painter->scale(side / 20.0, side / 20.0);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        draw_glyph(*painter);
        painter->restore();
    }

private:
    void draw_glyph(QPainter& p) const
    {
        switch (glyph_) {
        case WorkspaceIcon::New:
            p.drawPolygon(QPolygonF{{4, 2}, {11, 2}, {16, 7}, {16, 18}, {4, 18}});
            p.drawPolyline(QPolygonF{{11, 2}, {11, 7}, {16, 7}});
            break;
        case WorkspaceIcon::Open:
            p.drawPolyline(QPolygonF{{2, 16}, {2, 4}, {8, 4}, {10, 6}, {16, 6}, {16, 8}});
            p.drawPolygon(QPolygonF{{2, 16}, {5, 9}, {18, 9}, {15, 16}});
            break;
        case WorkspaceIcon::Save:
            p.drawPolygon(QPolygonF{{3, 3}, {14, 3}, {17, 6}, {17, 17}, {3, 17}});
            p.drawPolyline(QPolygonF{{6, 3}, {6, 7}, {12, 7}, {12, 3}});
            p.drawPolyline(QPolygonF{{6, 17}, {6, 11}, {14, 11}, {14, 17}});
            break;
        case WorkspaceIcon::Undo:
        case WorkspaceIcon::Redo: {
            if (glyph_ == WorkspaceIcon::Redo) { p.translate(20, 0); p.scale(-1, 1); }
            p.drawPolyline(QPolygonF{{7, 4}, {3, 8}, {7, 12}});
            QPainterPath turn({3, 8});
            turn.lineTo(11, 8);
            turn.quadTo(17, 8, 17, 15);
            p.drawPath(turn);
            break;
        }
        case WorkspaceIcon::Parent:
            p.drawPolyline(QPolygonF{{16, 15}, {8, 15}, {8, 4}});
            p.drawPolyline(QPolygonF{{4, 8}, {8, 4}, {12, 8}});
            break;
        case WorkspaceIcon::Run:
            p.drawPolygon(QPolygonF{{5, 3}, {17, 10}, {5, 17}});
            break;
        case WorkspaceIcon::Repeat:
            p.drawPolyline(QPolygonF{{3, 10}, {3, 6}, {16, 6}});
            p.drawPolyline(QPolygonF{{13, 3}, {16, 6}, {13, 9}});
            p.drawPolyline(QPolygonF{{17, 10}, {17, 14}, {4, 14}});
            p.drawPolyline(QPolygonF{{7, 11}, {4, 14}, {7, 17}});
            break;
        case WorkspaceIcon::Stop:
            p.drawRoundedRect(QRectF(4, 4, 12, 12), 1, 1);
            break;
        case WorkspaceIcon::Properties:
            p.drawLine(3, 6, 7, 6); p.drawLine(11, 6, 17, 6);
            p.drawLine(3, 14, 11, 14); p.drawLine(15, 14, 17, 14);
            p.drawEllipse(QPointF(9, 6), 2, 2); p.drawEllipse(QPointF(13, 14), 2, 2);
            break;
        case WorkspaceIcon::Details:
            p.drawRoundedRect(QRectF(3, 3, 14, 14), 1, 1);
            p.drawLine(6, 7, 14, 7); p.drawLine(6, 10, 14, 10); p.drawLine(6, 13, 11, 13);
            break;
        }
    }
    WorkspaceIcon glyph_;
};

} // namespace

QIcon workspace_icon(WorkspaceIcon glyph) { return QIcon(new WorkspaceIconEngine(glyph)); }

} // namespace visionpro::ui
