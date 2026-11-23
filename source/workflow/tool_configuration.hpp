#pragma once

#include <visionpro/core/tool_execution.hpp>
#include <visionpro/workflow/snapshot.hpp>

namespace visionpro::workflow::detail {

// Fix schema parameters and resource revisions without constructing an instance or acquiring a lease.
[[nodiscard]] core::Result<core::ToolExecutionConfiguration> project_tool_configuration(
    const ConfigSnapshot& snapshot, core::WorkflowId workflow, const Node& node);

} // namespace visionpro::workflow::detail
