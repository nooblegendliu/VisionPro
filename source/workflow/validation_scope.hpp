#pragma once

#include <visionpro/workflow/run.hpp>
#include <visionpro/workflow/validation.hpp>

namespace visionpro::workflow::detail {

core::Result<ValidationIndex> validate_execution_scope(const RunRequest& request);

} // namespace visionpro::workflow::detail
