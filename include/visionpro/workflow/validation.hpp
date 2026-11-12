#pragma once

#include <visionpro/workflow/model.hpp>

namespace visionpro::workflow {

struct DataDependency {
    core::NodeId producer;
    core::NodeId consumer;
    BindingTarget target;
    std::size_t source_index;
    bool inherited_region{false};
};
struct ScopeDependencies {
    core::WorkflowId workflow;
    std::optional<core::NodeId> group;
    std::vector<core::NodeId> reachable;
    std::vector<ControlLink> control_links;
    std::vector<DataDependency> data;
};
// Detached, derived IDs only. This is not a schedule or proof of runtime availability.
struct ValidationIndex { std::vector<ScopeDependencies> scopes; };

// Checks all configured nodes, including disconnected drafts. Reachability comes
// only from each scope's explicit entries; bindings never activate producers.
// Nonempty support enables backend Schema/capability checks. Empty support means
// metadata-only validation, not a claim that an execution backend supports it.
[[nodiscard]] core::Result<ValidationIndex> validate_solution(const Solution& solution,
    std::span<const core::ToolDescriptor> tools,
    std::span<const core::SchemaSupport> support = {});

// Fatal catalog/identity errors use the diagnostic branch. Local issues remain
// editable; the report includes the first issue per workflow and any layout issue.
[[nodiscard]] core::Result<std::vector<core::Diagnostic>> diagnose_solution(const Solution& solution,
    std::span<const core::ToolDescriptor> tools);

// Stable public system-result projection, shared with the resolver.
// Elapsed is an Int64 count of nanoseconds. Diagnostics is List<Record>.
[[nodiscard]] core::Result<core::DataType> system_field_type(SystemField field);

} // namespace visionpro::workflow
