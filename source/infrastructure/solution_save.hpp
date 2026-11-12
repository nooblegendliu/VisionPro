#pragma once

#include <visionpro/infrastructure/solution_file.hpp>

#include <functional>

namespace visionpro::infrastructure::detail {

enum class SaveStage { BeforeLock, ResourceWrite, ManifestWrite, Commit, Cleanup };
using SaveCheckpoint = std::function<std::optional<core::Diagnostic>(SaveStage)>;

// Testable transaction entry. Production uses the same path with no checkpoint.
[[nodiscard]] core::Result<SolutionSaveResult> save_solution(const SolutionDocument& source,
    const workflow::Solution& edited, std::span<const ManagedResourceWrite> writes, const SaveCheckpoint& checkpoint);

} // namespace visionpro::infrastructure::detail
