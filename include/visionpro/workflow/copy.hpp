#pragma once

#include <visionpro/workflow/model.hpp>

#include <map>
#include <span>

namespace visionpro::workflow {

enum class ExternalReferencePolicy { Preserve, Reject };
struct IdentityRemap {
    std::map<core::SolutionId, core::SolutionId> solutions;
    std::map<core::WorkflowId, core::WorkflowId> workflows;
    std::map<core::NodeId, core::NodeId> nodes;
    std::map<core::ControlLinkId, core::ControlLinkId> control_links;
    std::map<core::ResourceId, core::ResourceId> resources;
    std::map<core::VariableId, core::VariableId> variables;
    std::map<core::QueueId, core::QueueId> queues;
    std::map<core::RegionId, core::RegionId> regions;
};
struct ControlExportReference { core::WorkflowId workflow; ControlOutputMapping mapping; };
using ExternalCopyReference = std::variant<OutputReference, ScopedInputReference,
    core::ResourceReference, GlobalReference, Containment, ControlExportReference>;
struct PreservedReference {
    // Source node identity when applicable; other references retain their own
    // structured location (for example Containment carries its original child).
    std::optional<core::NodeId> owner;
    ExternalCopyReference reference;
};
struct CopyReport {
    std::vector<PreservedReference> preserved_references;
    // Original boundary edges are deliberately not inserted into the copied fragment.
    std::vector<ControlLink> omitted_control_links;
};
struct SubgraphCopy {
    core::WorkflowId workflow;
    std::vector<Node> nodes;
    std::vector<ControlLink> control_links;
    std::vector<Containment> containment;
    IdentityRemap identities;
    CopyReport report;
};
struct SolutionCopy {
    Solution solution;
    IdentityRemap identities;
    CopyReport report;
};

// Produces a detached fragment for insertion into the SAME workflow. Selecting a
// Group includes all descendants. Parent relationships outside the selection are
// retained only by an explicit Preserve policy. Workflow entries/exports/layout
// are not implicitly duplicated; boundary control links are reported and omitted.
[[nodiscard]] core::Result<SubgraphCopy> clone_subgraph(const Workflow& workflow,
    std::span<const core::NodeId> selected, ExternalReferencePolicy external_policy);
// Configuration-only copy: remaps owned identities/references and layout; does
// not copy resource files, execute nodes or transfer runtime state/results.
[[nodiscard]] core::Result<SolutionCopy> clone_solution(const Solution& solution,
    ExternalReferencePolicy external_policy);

} // namespace visionpro::workflow
