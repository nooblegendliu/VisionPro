#pragma once

#include <QIcon>

namespace visionpro::ui {

enum class WorkspaceIcon { New, Open, Save, Undo, Redo, Parent, Run, RunSolution, Repeat, Stop, Properties, Details, Search, Link, Camera };
enum class ToolboxCategory;

QIcon workspace_icon(WorkspaceIcon glyph);
QIcon workspace_category_icon(ToolboxCategory category);

} // namespace visionpro::ui
