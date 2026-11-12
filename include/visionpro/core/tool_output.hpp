#pragma once

#include <visionpro/core/value.hpp>

#include <variant>

namespace visionpro::core {

enum class OutputAbsenceReason {
    NotSelected, Disabled, ToolDidNotProduce, Failed, Cancelled, TimedOut, ConditionNotMet
};
struct NotProduced { OutputAbsenceReason reason; };
struct InvalidOutput { Diagnostic diagnostic; };
using OutputContent = std::variant<DataValue, NotProduced, InvalidOutput>;

// Tool omissions are distinct from control closure or a failed invocation.
[[nodiscard]] constexpr bool is_tool_output_absence(OutputAbsenceReason reason) noexcept
{
    return reason == OutputAbsenceReason::ToolDidNotProduce || reason == OutputAbsenceReason::ConditionNotMet;
}

} // namespace visionpro::core
