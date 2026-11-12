#pragma once

#include <visionpro/workflow/model.hpp>

#include <map>

namespace visionpro::workflow::detail {

using Parents = std::map<core::NodeId, core::NodeId>;

core::Result<Parents> validate_containment(const Workflow& workflow);
core::Result<std::monostate> validate_workflow_interface_shape(const Workflow& workflow, const Parents& parents);
core::Result<std::monostate> validate_node_shape(const Workflow& workflow, const Parents& parents,
    const Node& node, bool independent = false);
core::Result<std::monostate> validate_workflow_shape(const Workflow& workflow);
core::Result<std::monostate> validate_layouts(const Solution& solution);

} // namespace visionpro::workflow::detail
