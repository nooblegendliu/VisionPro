#pragma once

#include "run_controller.hpp"

#include <functional>

class QMenu;

namespace visionpro::ui {
void add_solution_run_actions(QMenu& menu, RunController& controller,
    std::function<const workflow::Solution*()> current_draft, std::function<RunDocument()> current_document);
}
