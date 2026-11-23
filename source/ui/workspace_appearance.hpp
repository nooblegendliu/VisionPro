#pragma once

#include <QColor>
#include <QObject>

namespace visionpro::ui {

enum class AppearanceMode { System, Light, Dark };

struct WorkspaceColors {
    QColor window, surface, text, secondary, button, border, highlight, highlightedText, accent;
    QColor canvas, grid, node, nodeText, nodeBorder, nodePortFilled, selection, connection, connectionSelected;
    QColor positive, negative;
};

// Shared by all windows. Only the user's choice is persisted.
class WorkspaceAppearance final : public QObject {
    Q_OBJECT
public:
    static WorkspaceAppearance& instance();
    AppearanceMode mode() const { return mode_; }
    bool isDark() const { return dark_; }
    void setMode(AppearanceMode mode);

signals:
    void modeChanged(AppearanceMode mode);
    void colorsChanged();

private:
    explicit WorkspaceAppearance(QObject* parent);
    void initialize();
    void requestScheme();
    void applyScheme(Qt::ColorScheme scheme);

    AppearanceMode mode_{AppearanceMode::System};
    bool dark_{};
    bool paletteApplied_{};
    bool platformDefaultDark_{};
};

const WorkspaceColors& workspace_colors();
bool workspace_is_dark();

} // namespace visionpro::ui
