#pragma once

#include <visionpro/infrastructure/solution_file.hpp>

namespace visionpro::infrastructure {

inline constexpr std::string_view subgraph_manifest_name = "subgraph.json";

struct SubgraphImport {
    workflow::Solution solution;
    core::WorkflowId workflow;
    std::optional<core::NodeId> group;
};
struct SubgraphHostCapabilities {
    bool node_state{true};
    bool file_leases{true};
    bool shared_data{true};
};

// Exports the public definition. Parent control connections and input bindings
// belong to the calling host, including solution-level workflow links and port
// mappings. Importing never enrolls the copied workflow in a host graph.
// Regular files are copied into the package.
[[nodiscard]] core::Result<std::monostate> export_subgraph_package(const std::filesystem::path& directory,
    const workflow::Solution& draft, core::WorkflowId workflow, std::optional<core::NodeId> group,
    const core::ToolRegistry& registry, const SolutionDocument* source = nullptr);

// Returns a detached edit after all validation and file staging succeeds.
// File-backed imports require a host document for the destination asset base.
[[nodiscard]] core::Result<SubgraphImport> import_subgraph_package(const std::filesystem::path& directory,
    const workflow::Solution& draft, std::optional<core::WorkflowId> target_workflow,
    std::optional<core::NodeId> parent, const core::ToolRegistry& registry, const SolutionDocument* host = nullptr,
    SubgraphHostCapabilities capabilities = {});

} // namespace visionpro::infrastructure
