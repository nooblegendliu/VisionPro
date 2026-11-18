#include "loop_execution.hpp"
#include "runtime_values.hpp"

#include <algorithm>
#include <limits>

namespace visionpro::workflow::detail {
namespace {
using namespace core;

Diagnostic failure(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Result<std::int64_t> checked_add(std::int64_t left, std::int64_t right)
{
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
        || (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        return failure(ErrorCode::ConstraintViolation, "Loop integer addition exceeds Int64.");
    }
    return left + right;
}
} // namespace

LoopExecution::LoopExecution(const GroupConfiguration& configuration, InvocationKey identity,
    std::int64_t start, std::int64_t end, InputId index_id, std::vector<RunInput> inputs)
    : group(&configuration), call(identity), index(start), end_(end), index_id_(std::move(index_id)), inputs_(std::move(inputs)) {}

Result<LoopExecution> LoopExecution::create(const GroupConfiguration& group, const InvocationKey& call, std::vector<RunInput> inputs)
{
    const auto input = [&](std::string_view name) -> const std::int64_t* {
        const auto found = std::ranges::find_if(inputs, [&](const auto& value) { return value.id.text() == name; });
        return found == inputs.end() ? nullptr : found->value.as_int64();
    };
    const auto* start = input("start");
    const auto* end = input("end");
    const auto counter = std::ranges::find_if(group.interface.inputs, [](const auto& port) { return port.id.text() == "index"; });
    if (!start || !end || counter == group.interface.inputs.end() || !group.loop || group.loop->step == 0) {
        return failure(ErrorCode::InvalidRunRequest, "A loop requires Int64 start, end and index ports and a nonzero step.");
    }
    const auto first = *start;
    const auto last = *end;
    LoopExecution loop(group, call, first, last, counter->id, std::move(inputs));
    for (const auto& spec : group.loop->exports) {
        Accumulator accumulator{spec.output, {}, {}};
        if (spec.initial) {
            auto value = materialize_config_value(*spec.initial);
            if (const auto* bad = std::get_if<Diagnostic>(&value)) { return *bad; }
            accumulator.single = std::get<DataValue>(value);
        }
        loop.accumulators_.push_back(std::move(accumulator));
    }
    return loop;
}
bool LoopExecution::in_range() const noexcept
{
    return (group->loop->inclusive_end && index == end_) || (group->loop->step > 0 ? index < end_ : index > end_);
}
Result<bool> LoopExecution::advance()
{
    if (index == end_) { return false; }
    auto next = checked_add(index, group->loop->step);
    if (const auto* bad = std::get_if<Diagnostic>(&next)) { return *bad; }
    index = std::get<std::int64_t>(next);
    return in_range();
}
std::vector<RunInput> LoopExecution::body_inputs() const
{
    auto result = inputs_;
    result.push_back({index_id_, DataValue::integer(index)});
    return result;
}
Result<std::monostate> LoopExecution::collect(const LoopExport& spec, const DataValue& value)
{
    auto& accumulator = *std::ranges::find(accumulators_, spec.output, &Accumulator::id);
    switch (spec.mode) {
    case LoopExportMode::First:
        if (!accumulator.single) { accumulator.single = value; }
        break;
    case LoopExportMode::Last: accumulator.single = value; break;
    case LoopExportMode::Collect: accumulator.values.push_back(value); break;
    case LoopExportMode::Sum:
        if (const auto* integer = value.as_int64(); integer && accumulator.single && accumulator.single->as_int64()) {
            auto sum = checked_add(*accumulator.single->as_int64(), *integer);
            if (const auto* bad = std::get_if<Diagnostic>(&sum)) { return *bad; }
            accumulator.single = DataValue::integer(std::get<std::int64_t>(sum));
        } else if (const auto* real = value.as_float64(); real && accumulator.single && accumulator.single->as_float64()) {
            auto sum = DataValue::real(*accumulator.single->as_float64() + *real);
            if (const auto* bad = std::get_if<Diagnostic>(&sum)) { return *bad; }
            accumulator.single = std::get<DataValue>(sum);
        } else { return failure(ErrorCode::TypeMismatch, "Sum requires a matching numeric value and initial value."); }
        break;
    }
    return std::monostate{};
}
Result<std::vector<OutputSlot>> LoopExecution::outputs() const
{
    std::vector<OutputSlot> slots;
    for (const auto& spec : group->loop->exports) {
        const auto& type = std::ranges::find(group->interface.outputs, spec.output, &OutputDescriptor::id)->type;
        const auto& accumulator = *std::ranges::find(accumulators_, spec.output, &Accumulator::id);
        auto value = accumulator.single;
        if (spec.mode == LoopExportMode::Collect) {
            auto list = DataValue::list(*type.element_type(), accumulator.values);
            if (const auto* bad = std::get_if<Diagnostic>(&list)) { return *bad; }
            value = std::get<DataValue>(list);
        } else if (type.kind() == TypeKind::Optional) {
            if (!value) { value = DataValue::none(*type.element_type()); }
            else if (value->type() != type) { value = DataValue::some(*value); }
        }
        if (!value) { return failure(ErrorCode::MissingRequiredValue, "Empty loop has no value for a required export."); }
        slots.push_back({spec.output, type, *value});
    }
    return slots;
}

} // namespace visionpro::workflow::detail
