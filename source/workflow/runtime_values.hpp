#pragma once

#include <visionpro/workflow/model.hpp>

namespace visionpro::workflow::detail {

// Pure exact materialization: no lookup, conversion, resource loading or execution.
[[nodiscard]] core::Result<core::DataValue> materialize_config_value(const core::ConfigValue& value);
[[nodiscard]] core::Result<std::monostate> validate_runtime_value(const core::DataType& type,
    const core::ValueConstraints& constraints, const core::DataValue& value);
[[nodiscard]] core::Result<std::monostate> validate_global_read_snapshot(const Solution& solution,
    const core::SolutionSessionId& session, std::span<const core::GlobalVariableEntry> globals);

// Checks actual payload domains and nested references without interpreting ROI geometry.
// Source runtime values must be self-contained. Only configuration expressions whose
// inherited inputs are separately frozen by the resolver may opt into inherited regions.
// Semantics apply to the value/Optional payload, not to unrelated Record/List members.
[[nodiscard]] core::Result<std::monostate> validate_runtime_domain(const core::DataValue& value,
    const Solution& solution, const core::ValueSemantics& semantics,
    const core::ToolDescriptor* tool = nullptr, bool allow_inherited_regions = false);

} // namespace visionpro::workflow::detail
