#pragma once

#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/trigger_control.hpp>
#include <nlohmann/json.hpp>

namespace visionpro::app {

struct TriggerSequence {
    std::vector<std::int64_t> frames;
    std::vector<std::chrono::milliseconds> advances;
    std::optional<workflow::TriggerStopMode> stop;
};

struct TriggerReplay {
    nlohmann::json report;
    std::vector<core::Diagnostic> failures;
};

[[nodiscard]] core::Result<TriggerReplay> replay_triggers(const infrastructure::SolutionDocument& document,
    const core::ToolRegistry& registry, core::WorkflowId workflow, const TriggerSequence& sequence);

} // namespace visionpro::app
