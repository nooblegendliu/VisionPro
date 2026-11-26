#include "workspace_icons.hpp"
#include "toolbox_catalog.hpp"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <variant>

namespace visionpro::ui {
namespace {

// The toolbar and menus request the same icon at different sizes and pixel densities.
class WorkspaceIconEngine final : public QIconEngine {
public:
    using Glyph = std::variant<WorkspaceIcon, ToolboxCategory>;
    explicit WorkspaceIconEngine(Glyph glyph) : glyph_(glyph) {}
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
        if (const auto* category = std::get_if<ToolboxCategory>(&glyph_)) { draw_category(*painter, *category); }
        else { draw_glyph(*painter, std::get<WorkspaceIcon>(glyph_)); }
        painter->restore();
    }

private:
    static void draw_category(QPainter& p, ToolboxCategory category)
    {
        switch (category) {
        case ToolboxCategory::Acquisition:
            p.drawPolygon(QPolygonF{{2, 6}, {6, 6}, {8, 3}, {12, 3}, {14, 6}, {18, 6}, {18, 16}, {2, 16}});
            p.drawEllipse(QPointF(10, 11), 3, 3); break;
        case ToolboxCategory::Location:
            p.drawEllipse(QPointF(10, 10), 6, 6);
            p.drawLine(10, 1, 10, 7); p.drawLine(10, 13, 10, 19); p.drawLine(1, 10, 7, 10); p.drawLine(13, 10, 19, 10); break;
        case ToolboxCategory::Measurement:
            p.translate(10, 10); p.rotate(-35); p.drawRect(QRectF(-8,-3.5,16,7));
            for (int x : {-5, -2, 1, 4}) { p.drawLine(QPointF(x,-3), QPointF(x,0)); } break;
        case ToolboxCategory::Recognition:
            for (int x : {3, 6, 10, 12, 16}) { p.drawLine(x, 4, x, 15); } p.drawLine(3,18,16,18); break;
        case ToolboxCategory::Defect:
            p.drawEllipse(QPointF(8,8), 5.5,5.5); p.drawLine(12,12,18,18);
            p.drawPolyline(QPolygonF{{8,4}, {6,8}, {9,8}, {7,12}}); break;
        case ToolboxCategory::EdgeLearning:
            p.drawRoundedRect(QRectF(4,4,12,12), 2,2); p.drawPolyline(QPolygonF{{6,12}, {9,8}, {11,11}, {14,7}});
            for (int x : {7,13}) { p.drawLine(x,1,x,3); p.drawLine(x,17,x,19); } break;
        case ToolboxCategory::DeepLearning:
            for (int y : {4,15}) { p.drawLine(5,y,10,10); p.drawLine(10,10,15,y); p.drawEllipse(QPointF(4,y),2,2); p.drawEllipse(QPointF(16,y),2,2); }
            p.drawEllipse(QPointF(10,10),2,2); break;
        case ToolboxCategory::Calibration:
            p.drawRect(QRectF(3,3,14,14));
            p.fillRect(QRectF(3,3,7,7),p.pen().color()); p.fillRect(QRectF(10,10,7,7),p.pen().color()); break;
        case ToolboxCategory::Calculation:
            p.drawPolyline(QPolygonF{{16,3}, {5,3}, {11,10}, {5,17}, {16,17}}); break;
        case ToolboxCategory::ImageProcessing:
            p.drawRoundedRect(QRectF(2,3,16,14),1,1); p.drawPolyline(QPolygonF{{2,14},{7,9},{11,13},{15,8},{18,11}});
            p.drawEllipse(QPointF(6,7),1,1); break;
        case ToolboxCategory::Color:
            p.drawEllipse(QPointF(10,6.5),4.5,4.5); p.drawEllipse(QPointF(6,13),4.5,4.5); p.drawEllipse(QPointF(14,13),4.5,4.5); break;
        case ToolboxCategory::SplitCombine:
            p.drawPolyline(QPolygonF{{2,10},{8,10},{13,5},{18,5}}); p.drawPolyline(QPolygonF{{8,10},{13,15},{18,15}});
            p.drawLine(15,2,18,5); p.drawLine(15,18,18,15); break;
        case ToolboxCategory::Geometry:
            p.drawPolygon(QPolygonF{{7,3},{2,15},{12,15}}); p.drawEllipse(QPointF(14,12),4,4); break;
        case ToolboxCategory::Logic:
            p.drawPolygon(QPolygonF{{10,2},{15,7},{10,12},{5,7}}); p.drawPolyline(QPolygonF{{5,7},{2,7},{2,17},{6,17}});
            p.drawPolyline(QPolygonF{{15,7},{18,7},{18,17},{14,17}}); break;
        case ToolboxCategory::Communication:
            p.drawLine(2,6,17,6); p.drawPolyline(QPolygonF{{13,2},{17,6},{13,10}});
            p.drawLine(18,14,3,14); p.drawPolyline(QPolygonF{{7,10},{3,14},{7,18}}); break;
        case ToolboxCategory::Count: break;
        }
    }

    static void draw_glyph(QPainter& p, WorkspaceIcon glyph)
    {
        switch (glyph) {
        case WorkspaceIcon::Camera: draw_category(p, ToolboxCategory::Acquisition); break;
        case WorkspaceIcon::Link:
            p.translate(10, 10); p.rotate(-45);
            p.drawRoundedRect(QRectF(-9, -3, 8, 6), 3, 3);
            p.drawRoundedRect(QRectF(1, -3, 8, 6), 3, 3);
            p.drawLine(-4, 0, 4, 0); break;
        case WorkspaceIcon::Search:
            p.drawEllipse(QPointF(8.5,8.5),5.5,5.5); p.drawLine(13,13,18,18); break;
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
            if (glyph == WorkspaceIcon::Redo) { p.translate(20, 0); p.scale(-1, 1); }
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
        case WorkspaceIcon::RunSolution:
            p.drawRoundedRect(QRectF(2, 2, 13, 12), 1, 1);
            p.drawPolygon(QPolygonF{{6, 5}, {11, 8}, {6, 11}});
            p.drawPolyline(QPolygonF{{5, 17}, {18, 17}, {18, 5}});
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
    Glyph glyph_;
};

} // namespace

QIcon workspace_icon(WorkspaceIcon glyph) { return QIcon(new WorkspaceIconEngine(glyph)); }
QIcon workspace_category_icon(ToolboxCategory category) { return QIcon(new WorkspaceIconEngine(category)); }

} // namespace visionpro::ui
