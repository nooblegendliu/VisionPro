#pragma once

#include "json_codec.hpp"

#include <visionpro/workflow/model.hpp>

namespace visionpro::infrastructure::detail {

[[nodiscard]] Json encode_solution(const workflow::Solution& solution);
[[nodiscard]] workflow::Solution decode_solution(const Reader& reader);

} // namespace visionpro::infrastructure::detail
