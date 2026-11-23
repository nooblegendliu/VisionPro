#pragma once

#include <visionpro/workflow/workflow_session.hpp>

#include <nlohmann/json.hpp>

namespace visionpro::app {

[[nodiscard]] nlohmann::json diagnostics_json(std::span<const core::Diagnostic> diagnostics);
[[nodiscard]] nlohmann::json reported_duration(std::optional<double> duration, core::TimingUnit unit);
[[nodiscard]] core::Result<nlohmann::json> summarize_run(const workflow::RunHandle& handle,
    const workflow::RunProgress& progress, std::span<const core::Diagnostic> load_diagnostics);
[[nodiscard]] core::Result<nlohmann::json> summarize_solution(const workflow::SolutionRunReport& run,
    const workflow::Solution& solution, std::span<const core::Diagnostic> load_diagnostics);

} // namespace visionpro::app
