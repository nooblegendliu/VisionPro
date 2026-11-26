#pragma once

#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_session.hpp>

namespace visionpro::infrastructure {

inline constexpr std::uint32_t solution_format_version = 1;
inline constexpr std::uint32_t solution_graph_format_version = workflow::solution_graph_manifest_version;
[[nodiscard]] constexpr bool supports_solution_format(std::uint32_t version) noexcept
{ return version == solution_format_version || version == solution_graph_format_version; }
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
    [[nodiscard]] bool matches_manifest(std::string_view text) const noexcept { return manifest_ == text; }
    [[nodiscard]] bool same_manifest(const SolutionDocument& other) const noexcept { return manifest_ == other.manifest_; }
    [[nodiscard]] std::span<const NamedResourceRoot> resource_roots() const noexcept { return roots_; }
    [[nodiscard]] core::Result<std::monostate> flush_image_archive() const { return resources_->flush_image_archive(); }
private:
    friend core::Result<SolutionDocument> load_solution(const std::filesystem::path&, const core::ToolRegistry&,
        std::span<const NamedResourceRoot>);
    friend core::Result<LoadedSolutionExecution> open_solution_execution(const SolutionDocument&,
        const core::ToolRegistry&, workflow::WorkflowEngineOptions);
    friend core::Result<workflow::ConfigSnapshot> activate_solution(workflow::WorkflowSession&, const SolutionDocument&);

    SolutionDocument(workflow::Solution solution, std::filesystem::path directory,
        std::vector<core::Diagnostic> diagnostics, bool complete, std::shared_ptr<FileResourceProvider> resources,
        std::string manifest, std::span<const NamedResourceRoot> roots)
        : solution_(std::move(solution)), directory_(std::move(directory)), diagnostics_(std::move(diagnostics)),
          complete_(complete), resources_(std::move(resources)), manifest_(std::move(manifest)),
          roots_(roots.begin(), roots.end()) {}

    workflow::Solution solution_;
    std::filesystem::path directory_;
    std::vector<core::Diagnostic> diagnostics_;
    bool complete_;
    std::shared_ptr<FileResourceProvider> resources_;
    std::string manifest_;
    std::vector<NamedResourceRoot> roots_;
};

// Reads solution.json under the explicit absolute directory. Known-format
// drafts retain diagnostics. Invalid or unsupported representations fail.
[[nodiscard]] core::Result<SolutionDocument> load_solution(const std::filesystem::path& directory,
    const core::ToolRegistry& registry, std::span<const NamedResourceRoot> roots = {});
// Obtains the exact committed manifest as the next save baseline.
[[nodiscard]] core::Result<SolutionDocument> reload_saved_solution(const std::filesystem::path& directory,
    const workflow::Solution& saved, const core::ToolRegistry& registry, std::span<const NamedResourceRoot> roots = {});

// Publishes a new manifest. Local inputs with explicit image embedding are included;
// the caller supplies other package resources. Relative paths use the destination base.
// Use the provenance overload for a configuration edited from a loaded file.
[[nodiscard]] core::Result<std::monostate> write_new_solution(const std::filesystem::path& directory,
    const workflow::Solution& solution);
[[nodiscard]] core::Result<std::monostate> write_new_solution(const std::filesystem::path& directory,
    const SolutionDocument& source, const workflow::Solution& edited);

enum class SolutionTransfer { SaveAs, IndependentCopy };
struct SolutionTransferResult {
    workflow::Solution solution;
    std::vector<std::filesystem::path> external_dependencies;
    std::vector<core::SchemaId> unresolved_roots;
};

// Publishes a new package without replacing an existing manifest. A loaded
// source provides path provenance; otherwise destination and roots are the first bases.
[[nodiscard]] core::Result<SolutionTransferResult> transfer_solution(const std::filesystem::path& destination,
    const workflow::Solution& draft, SolutionTransfer operation, const SolutionDocument* source = nullptr,
    std::span<const NamedResourceRoot> roots = {});

// Creates a separate package file for an explicitly copied external resource.
// The application may use the returned relative path in its draft.
[[nodiscard]] core::Result<core::ResourcePath> copy_external_resource_into_solution(const SolutionDocument& source,
    const core::ExternalFileResource& resource);

struct ManagedResourceWrite {
    core::ResourceId resource;
    std::span<const std::byte> bytes;
};

struct SolutionCleanupReport {
    std::size_t removed{};
    std::size_t deferred{};
    std::vector<core::Diagnostic> diagnostics;
};

struct SolutionSaveResult {
    workflow::Solution solution;
    SolutionCleanupReport cleanup;
};

// Replaces an existing manifest if it still matches the loaded source.
// Each explicit write assigns its managed resource a new path, revision and
// digest. Bindings with a pinned revision remain explicit in the edited model.
// A returned SolutionSaveResult means the manifest committed, even if cleanup
// reports pending resources or failures. Reload for the next save base.
[[nodiscard]] core::Result<SolutionSaveResult> save_solution(const SolutionDocument& source,
    const workflow::Solution& edited, std::span<const ManagedResourceWrite> writes = {});

// Rechecks obsolete managed files from a previously loaded revision against the
// current manifest and retries cleanup under the same package lock.
[[nodiscard]] core::Result<SolutionCleanupReport> cleanup_solution_resources(const SolutionDocument& previous);

// Scoped execution of a decoded model. Submission still validates the selected
// workflow/target and its physical resources before admitting any work.
[[nodiscard]] core::Result<LoadedSolutionExecution> open_solution_execution(const SolutionDocument& document,
    const core::ToolRegistry& registry, workflow::WorkflowEngineOptions options = {});

// Prepares file resources for the draft using the source document's directory
// and named roots. The selected run and its capture are published together.
[[nodiscard]] workflow::RunSubmission publish_solution_run(workflow::WorkflowSession& session,
    const workflow::ConfigSnapshot& base, const SolutionDocument& source, const workflow::Solution& draft,
    const workflow::RunArguments& run, std::shared_ptr<ImageArchivePool> archives = {});

// Activation requires a complete candidate and follows the session's strict
// validation and drain protocol. Loading failure never changes the session.
[[nodiscard]] core::Result<workflow::ConfigSnapshot> activate_solution(workflow::WorkflowSession& session,
    const SolutionDocument& document);
[[nodiscard]] core::Result<workflow::ConfigSnapshot> load_and_activate_solution(workflow::WorkflowSession& session,
    const std::filesystem::path& directory, const core::ToolRegistry& registry,
    std::span<const NamedResourceRoot> roots = {});

} // namespace visionpro::infrastructure
