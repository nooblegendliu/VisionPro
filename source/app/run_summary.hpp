#pragma once

#include <visionpro/workflow/run_handle.hpp>

#include <nlohmann/json.hpp>

namespace visionpro::app {

[[nodiscard]] nlohmann::json diagnostics_json(std::span<const core::Diagnostic> diagnostics);
[[nodiscard]] core::Result<nlohmann::json> summarize_run(const workflow::RunHandle& handle,
    const workflow::RunProgress& progress, std::span<const core::Diagnostic> load_diagnostics);

} // namespace visionpro::app
