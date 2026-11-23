#include "workspace_style.hpp"

#include <QFontDatabase>
#include <QStringList>

#include <algorithm>

namespace visionpro::ui {

QFont workspace_font(WorkspaceText role)
{
    auto font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#ifdef Q_OS_WIN
    static const auto chinese_ui = QStringLiteral("Microsoft YaHei UI");
    static const bool available = QFontDatabase::hasFamily(chinese_ui);
    if (available) { font.setFamilies({chinese_ui, font.family()}); }
#endif
    const auto body = std::max(10.0, font.pointSizeF());
    font.setPointSizeF(body);
    font.setWeight(QFont::Normal);
    switch (role) {
    case WorkspaceText::Heading:
        font.setPointSizeF(body + 1);
        [[fallthrough]];
    case WorkspaceText::NodeTitle: font.setWeight(QFont::DemiBold); break;
    case WorkspaceText::Category: font.setWeight(QFont::Medium); break;
    case WorkspaceText::Secondary: font.setPointSizeF(body - 1); break;
    case WorkspaceText::Body: break;
    }
    return font;
}

} // namespace visionpro::ui
