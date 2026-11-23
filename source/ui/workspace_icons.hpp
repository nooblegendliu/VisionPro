#pragma once

#include <QIcon>

namespace visionpro::ui {

enum class WorkspaceIcon { New, Open, Save, Undo, Redo, Parent, Run, Repeat, Stop, Properties, Details };

QIcon workspace_icon(WorkspaceIcon glyph);

} // namespace visionpro::ui
