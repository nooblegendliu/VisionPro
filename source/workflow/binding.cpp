#include <visionpro/workflow/binding.hpp>

#include <set>
#include <type_traits>

namespace visionpro::workflow {
namespace {
core::Diagnostic invalid(std::string message, const BindingTarget& target)
{
    core::Diagnostic result{core::ErrorCode::InvalidBinding, core::Severity::Error, {}, std::move(message)};
    std::visit([&](auto id) {
        if constexpr (std::is_same_v<decltype(id), core::InputId>) { result.location.input = id; }
        else { result.location.parameter = id; }
    }, target);
    return result;
}
bool valid(SystemField field)
{
    switch (field) {
    case SystemField::ExecutionStatus: case SystemField::Judgement:
    case SystemField::Diagnostics: case SystemField::Elapsed: return true;
    }
    return false;
}
} // namespace

core::BindingSourceKind source_kind(const BindingSource& source) noexcept
{
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::ConfigValue>) { return core::BindingSourceKind::Constant; }
        else if constexpr (std::is_same_v<T, OutputReference>) { return core::BindingSourceKind::RunOutput; }
        else if constexpr (std::is_same_v<T, ScopedInputReference>) { return core::BindingSourceKind::PublicInput; }
        else if constexpr (std::is_same_v<T, core::ResourceReference>) { return core::BindingSourceKind::Resource; }
        else { return core::BindingSourceKind::GlobalRead; }
    }, source);
}

core::Result<std::monostate> validate_binding_shape(const Binding& binding)
{
    const auto fail = [&](std::string message) { return invalid(std::move(message), binding.target); };
    if (binding.sources.empty()) { return fail("A configured binding must declare its source; omission is not a default or None."); }
    switch (binding.cardinality) {
    case core::BindingCardinality::SingleSource:
    case core::BindingCardinality::CollectionSource:
        if (binding.sources.size() != 1) { return fail("Single/collection bindings require exactly one source."); }
        break;
    case core::BindingCardinality::MultipleSources: break;
    default: return fail("Unknown binding cardinality.");
    }
    switch (binding.conversion) {
    case core::ConversionKind::Exact: case core::ConversionKind::WrapOptional:
    case core::ConversionKind::Int64ToFloat64Explicit: break;
    default: return fail("Unknown conversion policy.");
    }
    std::set<MissingReason> reasons;
    for (const auto reason : binding.availability.accepted_absence) {
        if ((reason < MissingReason::NotSelected || reason > MissingReason::ConditionNotMet)
            || !reasons.insert(reason).second) { return fail("Accepted absence reasons must be recognized and unique."); }
    }
    for (const auto& source : binding.sources) {
        if (const auto* output = std::get_if<OutputReference>(&source)) {
            if (const auto* field = std::get_if<SystemField>(&output->output); field && !valid(*field)) {
                return fail("Unknown system result field.");
            }
        }
        if (const auto* resource = std::get_if<core::ResourceReference>(&source)) {
            if (auto checked = core::validate_resource_reference(*resource);
                const auto* error = std::get_if<core::Diagnostic>(&checked)) {
                auto result = fail(error->message);
                result.location.resource = resource->id;
                return result;
            }
        }
    }
    return std::monostate{};
}

} // namespace visionpro::workflow
