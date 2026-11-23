#pragma once

#include <QFont>

namespace visionpro::ui {

enum class WorkspaceText { Body, Heading, Category, Secondary, NodeTitle };

QFont workspace_font(WorkspaceText role = WorkspaceText::Body);

} // namespace visionpro::ui
