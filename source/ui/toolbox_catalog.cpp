#include "toolbox_catalog.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

#include <algorithm>
#include <array>

namespace visionpro::ui {
namespace {

using Category = ToolboxCategory;

struct SectionStyle {
    const char* name;
    QRgb ink;
};

constexpr std::array sections{
    SectionStyle{"图像采集", 0xff276290}, SectionStyle{"定位", 0xff137065},
    SectionStyle{"测量", 0xff1e6593}, SectionStyle{"识别", 0xff704893},
    SectionStyle{"缺陷检测", 0xffa14d37}, SectionStyle{"边缘学习", 0xff476f37},
    SectionStyle{"深度学习", 0xff684ba1}, SectionStyle{"标定", 0xff8a561b},
    SectionStyle{"运算", 0xff735395}, SectionStyle{"图像处理", 0xff355e9d},
    SectionStyle{"颜色处理", 0xff8c456b}, SectionStyle{"拆分组合", 0xff276f76},
    SectionStyle{"图形生成", 0xff377344}, SectionStyle{"逻辑工具", 0xff675196},
    SectionStyle{"通信", 0xff267084}
};
static_assert(sections.size() == static_cast<std::size_t>(Category::Count));

enum class Glyph {
    Image, Camera, Frame, Target, Blobs, Distance, Calibration, Coordinates,
    Threshold, Levels, Color, Range, Calculation, Text, And, Branch, Indexed,
    Merge, Send, Receive, Read, Write, Group, Loop, File, Counter, Forward, Generic
};

struct Tool {
    std::string_view type;
    Category category;
    const char* name;
    Glyph glyph;
    const char* mark = "";
};

constexpr Tool tools[] = {
    {"visionpro.image.read_file", Category::Acquisition, "本地图像", Glyph::Image},
    {"visionpro.camera.acquire", Category::Acquisition, "相机采集", Glyph::Camera},
    {"visionpro.camera.frame", Category::Acquisition, "触发图像", Glyph::Frame},
    {"visionpro.locate.template_translation", Category::Location, "模板定位", Glyph::Target},
    {"visionpro.image.blob", Category::Location, "连通域分析", Glyph::Blobs},
    {"visionpro.measure.distance", Category::Measurement, "两点测距", Glyph::Distance},
    {"visionpro.calibration.fit", Category::Calibration, "平面标定", Glyph::Calibration},
    {"visionpro.geometry.map_point", Category::Calculation, "点坐标转换", Glyph::Coordinates},
    {"visionpro.calc.int64", Category::Calculation, "整数计算", Glyph::Calculation, "±"},
    {"visionpro.calc.int64_accumulate", Category::Calculation, "整数累加", Glyph::Calculation, "Σ"},
    {"visionpro.sim.add", Category::Calculation, "模拟加法", Glyph::Calculation, "+"},
    {"visionpro.image.threshold", Category::ImageProcessing, "图像二值化", Glyph::Threshold},
    {"visionpro.image.class_map", Category::ImageProcessing, "灰度分类", Glyph::Levels},
    {"visionpro.measure.color_mean", Category::Color, "颜色测量", Glyph::Color},
    {"visionpro.list.int64_at", Category::SplitCombine, "整数列表取项", Glyph::Indexed, "1"},
    {"visionpro.list.blob_at", Category::SplitCombine, "连通域取项", Glyph::Indexed, "B"},
    {"visionpro.shared.join_dimensions", Category::SplitCombine, "工件尺寸汇合", Glyph::Merge},
    {"visionpro.sim.collect", Category::SplitCombine, "模拟集合", Glyph::Indexed, "+"},
    {"visionpro.judge.float_range", Category::Logic, "浮点范围判断", Glyph::Range, "x.y"},
    {"visionpro.judge.integer_range", Category::Logic, "整数范围判断", Glyph::Range, "123"},
    {"visionpro.logic.and", Category::Logic, "逻辑与", Glyph::And},
    {"visionpro.logic.condition", Category::Logic, "条件分支", Glyph::Branch},
    {"visionpro.format.float64_list", Category::Logic, "浮点数格式化", Glyph::Text, ".0"},
    {"visionpro.format.int64_list", Category::Logic, "整数格式化", Glyph::Text, "12"},
    {"visionpro.shared.global_read_int64", Category::Logic, "读取全局整数", Glyph::Read, "V"},
    {"visionpro.shared.global_write_int64", Category::Logic, "写入全局整数", Glyph::Write, "V"},
    {"visionpro.shared.global_read_many_int64", Category::Logic, "批量读取整数", Glyph::Read, "Vn"},
    {"visionpro.sim.file_byte", Category::Logic, "模拟读取字节", Glyph::File},
    {"visionpro.sim.select", Category::Logic, "模拟分支", Glyph::Branch, "S"},
    {"visionpro.sim.deliver", Category::Logic, "模拟传递", Glyph::Forward},
    {"visionpro.sim.call_count", Category::Logic, "模拟调用计数", Glyph::Counter},
    {"@group", Category::Logic, "工具组", Glyph::Group},
    {"@loop", Category::Logic, "循环", Glyph::Loop},
    {"visionpro.text.encode_utf8", Category::Communication, "UTF-8 编码", Glyph::Text, "U8"},
    {"visionpro.tcp.send", Category::Communication, "TCP 发送", Glyph::Send, "TCP"},
    {"visionpro.tcp.receive", Category::Communication, "TCP 接收", Glyph::Receive, "TCP"},
    {"visionpro.plc.write", Category::Communication, "PLC 写入", Glyph::Write, "PLC"},
    {"visionpro.plc.read", Category::Communication, "PLC 读取", Glyph::Read, "PLC"},
    {"visionpro.shared.queue_send_fragment", Category::Communication, "发送测量片段", Glyph::Send, "F"},
    {"visionpro.shared.queue_receive_fragment", Category::Communication, "接收测量片段", Glyph::Receive, "F"},
    {"visionpro.shared.queue_send_dimensions", Category::Communication, "发送尺寸", Glyph::Send, "W/H"},
    {"visionpro.shared.queue_receive_dimensions", Category::Communication, "接收尺寸", Glyph::Receive, "W/H"},
    {"visionpro.shared.queue_send_text", Category::Communication, "发送文本", Glyph::Send, "Aa"},
    {"visionpro.shared.queue_receive_text", Category::Communication, "接收文本", Glyph::Receive, "Aa"}
};

void symbol(QPainter& painter, const QRectF& bounds, const char* text, int pixels)
{
    auto font = painter.font();
    font.setPixelSize(pixels);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.drawText(bounds, Qt::AlignCenter, QString::fromUtf8(text));
}

void draw_glyph(QPainter& p, Glyph glyph, const char* mark, const QColor& ink)
{
    // All motifs use the same 32-unit canvas, stroke width and optical margins.
    switch (glyph) {
    case Glyph::Image:
        p.drawRoundedRect(QRectF(6, 8, 20, 16), 2, 2);
        p.drawEllipse(QPointF(11, 12), 1.2, 1.2);
        p.drawPolyline(QPolygonF{{7, 21}, {13, 16}, {16, 19}, {20, 14}, {25, 20}});
        break;
    case Glyph::Camera:
        p.drawPolyline(QPolygonF{{8, 11}, {11, 11}, {13, 8}, {19, 8}, {21, 11}, {25, 11}, {25, 23}, {7, 23}, {7, 11}, {8, 11}});
        p.drawEllipse(QPointF(16, 17), 3.7, 3.7);
        break;
    case Glyph::Frame:
        p.drawRoundedRect(QRectF(7, 8, 18, 16), 2, 2);
        p.drawPolyline(QPolygonF{{9, 18}, {13, 18}, {15, 12}, {17, 21}, {19, 15}, {23, 15}});
        break;
    case Glyph::Target:
        p.drawEllipse(QPointF(16, 16), 7, 7);
        p.drawLine(16, 5, 16, 12); p.drawLine(16, 20, 16, 27);
        p.drawLine(5, 16, 12, 16); p.drawLine(20, 16, 27, 16);
        break;
    case Glyph::Blobs:
        p.setBrush(ink);
        p.drawEllipse(QRectF(6, 8, 8, 10)); p.drawEllipse(QRectF(15, 15, 10, 8));
        p.drawEllipse(QRectF(19, 7, 4, 4));
        break;
    case Glyph::Distance:
        p.drawLine(8, 21, 24, 11);
        p.drawLine(6, 18, 10, 24); p.drawLine(22, 8, 26, 14);
        p.drawLine(14, 15, 16, 18); p.drawLine(18, 13, 20, 16);
        break;
    case Glyph::Calibration:
        p.drawRect(QRectF(7, 7, 18, 18));
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                if ((x + y) % 2 == 0) { p.fillRect(QRectF(7 + 6*x, 7 + 6*y, 6, 6), ink); }
            }
        }
        break;
    case Glyph::Coordinates:
        p.drawPolyline(QPolygonF{{8, 8}, {8, 24}, {25, 24}});
        p.drawPolyline(QPolygonF{{12, 20}, {22, 10}, {22, 15}});
        p.drawLine(17, 10, 22, 10);
        break;
    case Glyph::Threshold:
        p.drawRect(QRectF(7, 7, 18, 18));
        p.fillRect(QRectF(7, 7, 9, 18), ink);
        break;
    case Glyph::Levels:
        p.drawPolyline(QPolygonF{{6, 23}, {12, 23}, {12, 17}, {18, 17}, {18, 11}, {25, 11}});
        break;
    case Glyph::Color:
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(179, 65, 80)); p.drawEllipse(QRectF(10, 6, 12, 12));
        p.setBrush(QColor(30, 136, 104, 224)); p.drawEllipse(QRectF(6, 14, 12, 12));
        p.setBrush(QColor(39, 116, 179, 224)); p.drawEllipse(QRectF(15, 14, 12, 12));
        break;
    case Glyph::Range:
        p.drawPolyline(QPolygonF{{10, 9}, {7, 9}, {7, 23}, {10, 23}});
        p.drawPolyline(QPolygonF{{22, 9}, {25, 9}, {25, 23}, {22, 23}});
        symbol(p, QRectF(7, 9, 18, 14), mark, 9);
        break;
    case Glyph::Calculation:
        symbol(p, QRectF(4, 2, 24, 27), mark, 23);
        break;
    case Glyph::Text:
        p.drawLine(7, 23, 25, 23);
        symbol(p, QRectF(5, 5, 22, 17), mark, 14);
        break;
    case Glyph::And: {
        QPainterPath gate;
        gate.moveTo(10, 9); gate.lineTo(16, 9); gate.cubicTo(26, 9, 26, 23, 16, 23);
        gate.lineTo(10, 23); gate.closeSubpath();
        p.drawPath(gate);
        p.drawLine(6, 12, 10, 12); p.drawLine(6, 20, 10, 20); p.drawLine(24, 16, 27, 16);
        break;
    }
    case Glyph::Branch:
        p.drawPolyline(QPolygonF{{8, 23}, {8, 16}, {16, 16}, {16, 7}});
        p.drawPolyline(QPolygonF{{16, 16}, {24, 16}, {24, 23}});
        p.drawEllipse(QPointF(16, 7), 1.6, 1.6);
        if (*mark) { symbol(p, QRectF(11, 16, 10, 12), mark, 9); }
        break;
    case Glyph::Indexed:
        p.drawLine(7, 9, 25, 9); p.drawLine(7, 15, 13, 15); p.drawLine(7, 21, 13, 21);
        p.drawRoundedRect(QRectF(15, 13, 10, 11), 1, 1);
        symbol(p, QRectF(15, 13, 10, 11), mark, 9);
        break;
    case Glyph::Merge:
        p.drawPolyline(QPolygonF{{8, 8}, {8, 12}, {16, 17}, {16, 25}});
        p.drawPolyline(QPolygonF{{24, 8}, {24, 12}, {16, 17}});
        p.drawPolyline(QPolygonF{{12, 21}, {16, 25}, {20, 21}});
        break;
    case Glyph::Send:
    case Glyph::Receive:
    case Glyph::Read:
    case Glyph::Write: {
        // The arrow shows direction while the label identifies the transport or payload.
        const bool outgoing = glyph == Glyph::Send || glyph == Glyph::Read;
        p.drawLine(9, 10, 23, 10);
        if (outgoing) { p.drawPolyline(QPolygonF{{19, 6}, {23, 10}, {19, 14}}); }
        else { p.drawPolyline(QPolygonF{{13, 6}, {9, 10}, {13, 14}}); }
        if (glyph == Glyph::Read || glyph == Glyph::Write) {
            p.drawRoundedRect(QRectF(6.5, 16, 19, 9), 2, 2);
        }
        symbol(p, QRectF(6, 15, 20, 11), mark, 8);
        break;
    }
    case Glyph::Group:
        p.drawRoundedRect(QRectF(6, 7, 20, 17), 2, 2);
        p.drawRect(QRectF(9, 12, 5, 5)); p.drawRect(QRectF(18, 15, 5, 5));
        p.drawLine(14, 15, 18, 17);
        break;
    case Glyph::Loop:
        p.drawArc(QRectF(7, 7, 18, 18), -30*16, 285*16);
        p.drawPolyline(QPolygonF{{20, 18}, {24, 21}, {26, 16}});
        break;
    case Glyph::File:
        p.drawPolyline(QPolygonF{{9, 7}, {19, 7}, {24, 12}, {24, 25}, {9, 25}, {9, 7}});
        symbol(p, QRectF(10, 13, 13, 10), "01", 9);
        break;
    case Glyph::Counter:
        p.drawRoundedRect(QRectF(6, 9, 20, 14), 2, 2);
        symbol(p, QRectF(6, 9, 20, 14), "01", 11);
        break;
    case Glyph::Forward:
        p.drawPolyline(QPolygonF{{7, 20}, {12, 15}, {24, 15}});
        p.drawPolyline(QPolygonF{{19, 10}, {24, 15}, {19, 20}});
        break;
    case Glyph::Generic:
        p.drawRoundedRect(QRectF(9, 10, 14, 12), 2, 2);
        p.drawLine(5, 16, 9, 16); p.drawLine(23, 16, 27, 16);
        break;
    }
}

QIcon make_icon(Category category, Glyph glyph, const char* mark)
{
    const QColor ink(sections[static_cast<std::size_t>(category)].ink);
    QPixmap image(96, 96);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(3, 3);
    const QColor background((ink.red() + 8*255) / 9, (ink.green() + 8*255) / 9, (ink.blue() + 8*255) / 9);
    painter.setBrush(background);
    painter.setPen(QPen(ink.lighter(155), 0.8));
    painter.drawRoundedRect(QRectF(1, 1, 30, 30), 6, 6);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    draw_glyph(painter, glyph, mark, ink);
    painter.end();
    return QIcon(image);
}

} // namespace

QString toolbox_category_name(ToolboxCategory category)
{
    const auto index = static_cast<std::size_t>(category);
    return index < sections.size() ? QCoreApplication::translate("WorkspaceToolbox", sections[index].name) : QString{};
}

ToolboxEntry toolbox_entry(std::string_view type, const QString& original)
{
    const auto tool = std::ranges::find(tools, type, &Tool::type);
    if (tool == std::end(tools)) {
        return {Category::Logic, original, make_icon(Category::Logic, Glyph::Generic, "")};
    }
    return {tool->category, QCoreApplication::translate("WorkspaceToolbox", tool->name),
        make_icon(tool->category, tool->glyph, tool->mark)};
}

} // namespace visionpro::ui
