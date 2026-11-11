#pragma once

#include <visionpro/workflow/model.hpp>

#include <functional>

namespace visionpro::workflow::detail {

// Validates references carried inside configuration values without loading resources.
// The caller resolves inherited outputs against its node/scope/dependency context.
[[nodiscard]] core::Result<std::monostate> validate_config_references(
    const core::ConfigValue& value, const Solution& solution,
    const core::RegionCapabilities* capabilities,
    const std::function<core::Result<std::monostate>(const core::InheritedRegion&)>& inherited);

// Optional payloads retain their spatial semantics. Composite values and transforms
// have no single frame/unit; transforms explicitly describe two endpoint frames.
[[nodiscard]] core::ValueSemantics config_value_semantics(const core::ConfigValue& value);

} // namespace visionpro::workflow::detail
