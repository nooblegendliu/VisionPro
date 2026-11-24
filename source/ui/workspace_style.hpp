#pragma once

#include <QFont>

namespace visionpro::ui {

enum class WorkspaceText { Body, Heading, Category, Secondary, NodeTitle, Technical };

namespace workspace_size {
inline constexpr int small_gap = 4;
inline constexpr int gap = 8;
inline constexpr int toolbar_icon = 18;
inline constexpr int tool_icon = 24;
inline constexpr int control_height = 28;
inline constexpr int header_height = 32;
inline constexpr int row_height = 28;
inline constexpr int splitter = 4;
inline constexpr int radius = 3;
}

QFont workspace_font(WorkspaceText role = WorkspaceText::Body);

} // namespace visionpro::ui
