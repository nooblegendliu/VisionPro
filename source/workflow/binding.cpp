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

core::Result<IndexSource> make_index_source(const BindingSource& source)
{
    const auto invalid = [](std::string message) -> core::Result<IndexSource> {
        return core::Diagnostic{core::ErrorCode::InvalidSelector, core::Severity::Error, {}, std::move(message)};
    };
    return std::visit([&](const auto& ref) -> core::Result<IndexSource> {
        if constexpr (requires { ref.selector; }) {
            FixedSelector path;
            for (const auto& step : ref.selector) {
                if (const auto* field = std::get_if<FieldSelection>(&step)) { path.emplace_back(*field); }
                else if (const auto* element = std::get_if<ElementSelection>(&step)) { path.emplace_back(*element); }
                else { return invalid("An index source cannot itself depend on a dynamic index."); }
            }
            using T = std::decay_t<decltype(ref)>;
            if constexpr (std::same_as<T, OutputReference>) { return IndexSource{IndexOutputReference{ref.workflow, ref.node, ref.output, std::move(path)}}; }
            else if constexpr (std::same_as<T, ScopedInputReference>) { return IndexSource{IndexInputReference{ref.workflow, ref.group, ref.input, std::move(path)}}; }
            else { return IndexSource{IndexGlobalReference{ref.variable, ref.required_revision, std::move(path)}}; }
        } else { return invalid("A dynamic index requires an output, public input or global reference."); }
    }, source);
}

BindingSource index_binding_source(const IndexSource& index)
{
    return std::visit([](const auto& ref) -> BindingSource {
        Selector path;
        for (const auto& step : ref.selector) {
            std::visit([&](const auto& value) { path.emplace_back(value); }, step);
        }
        using T = std::decay_t<decltype(ref)>;
        if constexpr (std::same_as<T, IndexOutputReference>) { return OutputReference{ref.workflow, ref.node, ref.output, std::move(path)}; }
        else if constexpr (std::same_as<T, IndexInputReference>) { return ScopedInputReference{ref.workflow, ref.group, ref.input, std::move(path)}; }
        else { return GlobalReference{ref.variable, ref.required_revision, std::move(path)}; }
    }, index);
}

const Selector* binding_selector(const BindingSource& source) noexcept
{
    return std::visit([](const auto& ref) -> const Selector* {
        if constexpr (requires { ref.selector; }) { return &ref.selector; }
        else { return nullptr; }
    }, source);
}

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
        if (const auto* path = binding_selector(source)) {
            for (const auto& step : *path) {
                const auto* dynamic = std::get_if<DynamicElementSelection>(&step);
                const auto* ref = dynamic ? std::get_if<IndexOutputReference>(&dynamic->source) : nullptr;
                const auto* field = ref ? std::get_if<SystemField>(&ref->output) : nullptr;
                if (field && !valid(*field)) { return fail("Unknown system result field in a dynamic index."); }
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
