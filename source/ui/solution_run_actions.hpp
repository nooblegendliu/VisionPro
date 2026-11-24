#pragma once

#include <functional>

class QAction;
class QMenu;

namespace visionpro::workflow { struct Solution; }

namespace visionpro::ui {
class RunController;
struct RunDocument;

struct SolutionRunActions {
    QAction* start{};
    QAction* stop{};
    QAction* report{};
};
SolutionRunActions add_solution_run_actions(QMenu& menu, RunController& controller,
    std::function<const workflow::Solution*()> current_draft, std::function<RunDocument()> current_document);
}
