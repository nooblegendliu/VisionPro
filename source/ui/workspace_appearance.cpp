#include "workspace_appearance.hpp"

#include <QApplication>
#include <QEvent>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStyleHints>
#include <QToolTip>

namespace visionpro::ui {
namespace {

const WorkspaceColors light{
    .window = 0xf3f4f5, .surface = 0xffffff, .text = 0x172634, .secondary = 0x506370,
    .button = 0xeff2f4, .border = 0xc2cbd1, .highlight = 0xc4e4ea, .highlightedText = 0x143b44,
    .accent = 0x006d75, .canvas = 0x32393e, .grid = 0x3b4247, .node = 0xe9edf1,
    .nodeText = 0x172634, .nodeBorder = 0xa7b3bb, .nodePortFilled = 0x006d75, .selection = 0x009c9b,
    .connection = 0x7bd3cf, .connectionSelected = 0xb9fff8, .positive = 0x167045, .negative = 0xaa3028,
    .disabled = 0x697984, .warning = 0x855307, .error = 0xb32c30,
};

const WorkspaceColors dark{
    .window = 0x20252b, .surface = 0x292f36, .text = 0xe6ebef, .secondary = 0xb4bfc8,
    .button = 0x343c45, .border = 0x49535e, .highlight = 0x284952, .highlightedText = 0xedf6f8,
    .accent = 0x80d4d0, .canvas = 0x1c2228, .grid = 0x2b343d, .node = 0x34414b,
    .nodeText = 0xe6ebef, .nodeBorder = 0x768794, .nodePortFilled = 0x80d4d0, .selection = 0x80d4d0,
    .connection = 0x80d4d0, .connectionSelected = 0xbefff5, .positive = 0x89d5a7, .negative = 0xff9d93,
    .disabled = 0x88949e, .warning = 0xf1c779, .error = 0xffaaa5,
};

QSettings user_settings()
{
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope,
        QStringLiteral("VisionPro"), QStringLiteral("VisionPro"));
}

QPalette widget_palette(const WorkspaceColors& c, bool isDark)
{
    QPalette palette;
    palette.setColor(QPalette::Window, c.window);
    palette.setColor(QPalette::Base, c.surface);
    palette.setColor(QPalette::AlternateBase, c.button);
    palette.setColor(QPalette::Button, c.button);
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText}) {
        palette.setColor(role, c.text);
        palette.setColor(QPalette::Disabled, role, c.disabled);
    }
    palette.setColor(QPalette::PlaceholderText, c.secondary);
    palette.setColor(QPalette::Light, c.button.lighter(isDark ? 135 : 108));
    palette.setColor(QPalette::Midlight, c.border);
    palette.setColor(QPalette::Mid, c.border.darker(115));
    palette.setColor(QPalette::Dark, c.border.darker(135));
    palette.setColor(QPalette::Shadow, c.window.darker(175));
    palette.setColor(QPalette::Highlight, c.highlight);
    palette.setColor(QPalette::HighlightedText, c.highlightedText);
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Accent, c.accent);
    palette.setColor(QPalette::Link, c.accent);
    palette.setColor(QPalette::LinkVisited, isDark ? QColor(0xcfb4e5) : QColor(0x704881));
    palette.setColor(QPalette::ToolTipBase, c.surface);
    palette.setColor(QPalette::ToolTipText, c.text);
    return palette;
}

} // namespace

WorkspaceAppearance& WorkspaceAppearance::instance()
{
    static QPointer<WorkspaceAppearance> current;
    if (!current) {
        current = new WorkspaceAppearance(qApp);
        current->initialize();
    }
    return *current;
}

WorkspaceAppearance::WorkspaceAppearance(QObject* parent) : QObject(parent) {}

void WorkspaceAppearance::initialize()
{
    const auto saved = user_settings().value(QStringLiteral("appearance/mode")).toString();
    if (saved == QStringLiteral("dark")) { mode_ = AppearanceMode::Dark; }
    else if (saved == QStringLiteral("light")) { mode_ = AppearanceMode::Light; }

    platformDefaultDark_ = QApplication::palette().window().color().lightness() < 128;
    // Fusion consistently uses the application palette for menus, editors and scrollbars.
    QApplication::setStyle(QStringLiteral("Fusion"));
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, &WorkspaceAppearance::applyScheme);
    // Windows resets Qt's separate tooltip palette after delivering the scheme signal.
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [] {
        QToolTip::setPalette(QApplication::palette());
    }, Qt::QueuedConnection);
    requestScheme();
}

void WorkspaceAppearance::setMode(AppearanceMode mode)
{
    if (mode == mode_) { return; }
    mode_ = mode;
    const auto value = mode == AppearanceMode::System ? QStringLiteral("system")
        : mode == AppearanceMode::Dark ? QStringLiteral("dark") : QStringLiteral("light");
    user_settings().setValue(QStringLiteral("appearance/mode"), value);
    requestScheme();
    emit modeChanged(mode_);
}

void WorkspaceAppearance::requestScheme()
{
    // Native file and color pickers can use the OS appearance despite an app override.
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, mode_ != AppearanceMode::System);
    auto* hints = qApp->styleHints();
    switch (mode_) {
    case AppearanceMode::System: hints->unsetColorScheme(); break;
    case AppearanceMode::Light: hints->setColorScheme(Qt::ColorScheme::Light); break;
    case AppearanceMode::Dark: hints->setColorScheme(Qt::ColorScheme::Dark); break;
    }
    applyScheme(hints->colorScheme());
    QToolTip::setPalette(QApplication::palette());
}

void WorkspaceAppearance::applyScheme(Qt::ColorScheme scheme)
{
    const bool isDark = mode_ == AppearanceMode::System
        ? (scheme == Qt::ColorScheme::Unknown ? platformDefaultDark_ : scheme == Qt::ColorScheme::Dark)
        : mode_ == AppearanceMode::Dark;
    if (paletteApplied_ && dark_ == isDark) { return; }
    dark_ = isDark;
    paletteApplied_ = true;
    const auto palette = widget_palette(dark_ ? dark : light, dark_);
    QApplication::setPalette(palette);
    // Qt posts this event to each window. Let its controls adopt the palette
    // before repainting cached nodes, tool icons and existing history rows.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::ApplicationPaletteChange);
    emit colorsChanged();
}

bool workspace_is_dark() { return WorkspaceAppearance::instance().isDark(); }
const WorkspaceColors& workspace_colors() { return workspace_is_dark() ? dark : light; }

} // namespace visionpro::ui
