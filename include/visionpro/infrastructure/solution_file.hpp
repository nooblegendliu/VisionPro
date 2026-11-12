#pragma once

#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_session.hpp>

namespace visionpro::infrastructure {

inline constexpr std::uint32_t solution_format_version = 1;
inline constexpr std::string_view solution_manifest_name = "solution.json";

struct LoadedSolutionExecution {
    workflow::ConfigSnapshot snapshot;
    std::unique_ptr<workflow::WorkflowEngine> engine;
};

// An owned, diagnosed configuration and its resolved file resources. Copy the
// configuration for editing and pass this document as provenance when saving it.
class SolutionDocument final {
public:
    [[nodiscard]] const workflow::Solution& solution() const noexcept { return solution_; }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool can_save() const noexcept { return complete_; }
private:
    friend core::Result<SolutionDocument> load_solution(const std::filesystem::path&, const core::ToolRegistry&,
        std::span<const NamedResourceRoot>);
    friend core::Result<LoadedSolutionExecution> open_solution_execution(const SolutionDocument&,
        const core::ToolRegistry&, workflow::WorkflowEngineOptions);
    friend core::Result<workflow::ConfigSnapshot> activate_solution(workflow::WorkflowSession&, const SolutionDocument&);

    SolutionDocument(workflow::Solution solution, std::filesystem::path directory,
        std::vector<core::Diagnostic> diagnostics, bool complete, std::shared_ptr<FileResourceProvider> resources)
        : solution_(std::move(solution)), directory_(std::move(directory)), diagnostics_(std::move(diagnostics)),
          complete_(complete), resources_(std::move(resources)) {}

    workflow::Solution solution_;
    std::filesystem::path directory_;
    std::vector<core::Diagnostic> diagnostics_;
    bool complete_;
    std::shared_ptr<FileResourceProvider> resources_;
};

// Reads solution.json under the explicit absolute directory. Known-format
// drafts retain diagnostics. Invalid or unsupported representations fail.
[[nodiscard]] core::Result<SolutionDocument> load_solution(const std::filesystem::path& directory,
    const core::ToolRegistry& registry, std::span<const NamedResourceRoot> roots = {});

// Publishes a new manifest only. The caller supplies any package resources.
// Use the provenance overload for a configuration edited from a loaded file.
[[nodiscard]] core::Result<std::monostate> write_new_solution(const std::filesystem::path& directory,
    const workflow::Solution& solution);
[[nodiscard]] core::Result<std::monostate> write_new_solution(const std::filesystem::path& directory,
    const SolutionDocument& source, const workflow::Solution& edited);

// Scoped execution of a decoded model. Submission still validates the selected
// workflow/target and its physical resources before admitting any work.
[[nodiscard]] core::Result<LoadedSolutionExecution> open_solution_execution(const SolutionDocument& document,
    const core::ToolRegistry& registry, workflow::WorkflowEngineOptions options = {});

// Activation requires a complete candidate and follows the session's strict
// validation and drain protocol. Loading failure never changes the session.
[[nodiscard]] core::Result<workflow::ConfigSnapshot> activate_solution(workflow::WorkflowSession& session,
    const SolutionDocument& document);
[[nodiscard]] core::Result<workflow::ConfigSnapshot> load_and_activate_solution(workflow::WorkflowSession& session,
    const std::filesystem::path& directory, const core::ToolRegistry& registry,
    std::span<const NamedResourceRoot> roots = {});

} // namespace visionpro::infrastructure
