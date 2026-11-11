#include <visionpro/core/tool_descriptor.hpp>
#include <visionpro/core/text.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace visionpro::core {
namespace {

using Check = Result<std::monostate>;
using SchemaDefinitions = std::map<std::pair<SchemaId, std::uint32_t>, DataType>;

Diagnostic error(ErrorCode code, std::string message, DiagnosticLocation location = {})
{
    return {code, Severity::Error, std::move(location), std::move(message)};
}

Check consistent_schema(const DataType& type, SchemaDefinitions& definitions, DiagnosticLocation where)
{
    if (type.schema_id()) {
        const auto [entry, inserted] = definitions.emplace(
            std::pair{*type.schema_id(), type.schema_version()}, type);
        if (!inserted) {
            if (entry->second != type) {
                where.schema = *type.schema_id();
                return error(ErrorCode::InvalidSchema,
                    "One Schema identity/version cannot describe different type structures in the same tool.", std::move(where));
            }
            // An identical definition and all of its nested types were already checked.
            return std::monostate{};
        }
    }
    if (const auto* element = type.element_type()) {
        if (auto checked = consistent_schema(*element, definitions, where); std::holds_alternative<Diagnostic>(checked)) {
            return checked;
        }
    }
    for (const auto& field : type.fields()) {
        auto field_location = where;
        field_location.path.emplace_back(field.id);
        if (auto checked = consistent_schema(field.type, definitions, std::move(field_location)); std::holds_alternative<Diagnostic>(checked)) {
            return checked;
        }
    }
    return std::monostate{};
}

template <class T>
bool contains(const std::vector<T>& values, const T& item)
{
    return std::find(values.begin(), values.end(), item) != values.end();
}

template <class T>
bool duplicates(const std::vector<T>& values)
{
    return std::set<T>(values.begin(), values.end()).size() != values.size();
}

bool valid(BindingSourceKind source)
{
    switch (source) {
    case BindingSourceKind::Constant:
    case BindingSourceKind::RunOutput:
    case BindingSourceKind::PublicInput:
    case BindingSourceKind::Resource:
    case BindingSourceKind::GlobalRead: return true;
    }
    return false;
}

bool valid(ConversionKind conversion)
{
    switch (conversion) {
    case ConversionKind::Exact:
    case ConversionKind::WrapOptional:
    case ConversionKind::Int64ToFloat64Explicit: return true;
    }
    return false;
}

Check semantics(const ValueSemantics& value)
{
    switch (value.unit) {
    case MeasurementUnit::Unspecified:
    case MeasurementUnit::Dimensionless:
    case MeasurementUnit::Pixel:
    case MeasurementUnit::Millimetre:
    case MeasurementUnit::Second:
    case MeasurementUnit::Radian:
    case MeasurementUnit::Degree: return std::monostate{};
    }
    return error(ErrorCode::InvalidDescriptor, "Unrecognized measurement unit.");
}

Check label(const std::string& text)
{
    if (!is_valid_utf8(text)) {
        return error(ErrorCode::InvalidDescriptor, "Display text must be valid UTF-8.");
    }
    return std::monostate{};
}

const DataType& unwrapped(const DataType& type)
{
    const DataType* current = &type;
    while (current->kind() == TypeKind::Optional) {
        current = current->element_type();
    }
    return *current;
}

const ConfigValue* unwrapped(const ConfigValue& value)
{
    const ConfigValue* current = &value;
    while (current && current->type().kind() == TypeKind::Optional) {
        current = current->optional_value();
    }
    return current;
}

Check bounds(const ConfigValue& value, const ValueConstraints& limits)
{
    // None has no numeric/length payload. An explicit allowed_values set may still exclude it.
    if (const auto* payload = unwrapped(value)) {
        if (limits.integer_range) {
            const auto* number = payload->as_int64();
            if (!number || *number < limits.integer_range->minimum || *number > limits.integer_range->maximum) {
                return error(ErrorCode::ConstraintViolation, "Integer is outside the declared inclusive range.");
            }
        }
        if (limits.real_range) {
            const auto* number = payload->as_float64();
            if (!number || !std::isfinite(*number) || *number < limits.real_range->minimum || *number > limits.real_range->maximum) {
                return error(ErrorCode::ConstraintViolation, "Float64 is outside the declared finite inclusive range.");
            }
        }
        if (limits.max_length) {
            std::size_t length = 0;
            if (const auto* text = payload->as_string()) { length = text->size(); }
            else if (const auto* bytes = payload->as_binary()) { length = bytes->size(); }
            else if (const auto* elements = payload->elements()) { length = elements->size(); }
            else { return error(ErrorCode::InvalidDescriptor, "Length constraints require String, Binary or List."); }
            if (length > *limits.max_length) {
                return error(ErrorCode::ConstraintViolation, "Value exceeds the declared maximum length; it was not truncated.");
            }
        }
    }
    return std::monostate{};
}

Check constraint_schema(const DataType& type, const ValueConstraints& limits)
{
    const auto kind = unwrapped(type).kind();
    if (limits.integer_range && (kind != TypeKind::Int64 || limits.integer_range->minimum > limits.integer_range->maximum)) {
        return error(ErrorCode::InvalidDescriptor, "Integer range requires Int64 and minimum <= maximum.");
    }
    if (limits.real_range && (kind != TypeKind::Float64 || !std::isfinite(limits.real_range->minimum)
        || !std::isfinite(limits.real_range->maximum) || limits.real_range->minimum > limits.real_range->maximum)) {
        return error(ErrorCode::InvalidDescriptor, "Real range requires Float64 and finite ordered bounds.");
    }
    if (limits.max_length && kind != TypeKind::String && kind != TypeKind::Binary && kind != TypeKind::List) {
        return error(ErrorCode::InvalidDescriptor, "Length constraints require String, Binary or List.");
    }
    for (std::size_t index = 0; index < limits.allowed_values.size(); ++index) {
        const auto& allowed = limits.allowed_values[index];
        if (allowed.type() != type) {
            return error(ErrorCode::InvalidDescriptor, "Allowed values must have the exact declared type.");
        }
        if (const auto result = bounds(allowed, limits); std::holds_alternative<Diagnostic>(result)) {
            return error(ErrorCode::InvalidDescriptor, "An allowed value violates another declared constraint.");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (allowed == limits.allowed_values[previous]) {
                return error(ErrorCode::InvalidDescriptor, "Allowed values must not contain duplicates.");
            }
        }
    }
    return std::monostate{};
}

Check value_check(const DataType& type, const ValueConstraints& limits, const ConfigValue& value)
{
    if (value.type() != type) {
        return error(ErrorCode::TypeMismatch, "Configured value must match the exact target type.");
    }
    if (auto checked = bounds(value, limits); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (!limits.allowed_values.empty() && !contains(limits.allowed_values, value)) {
        return error(ErrorCode::ConstraintViolation, "Value is not one of the explicitly allowed values.");
    }
    return std::monostate{};
}

Check policy_check(const DataType& type, const BindingPolicy& policy)
{
    if (policy.allowed_sources.empty() || duplicates(policy.allowed_sources)
        || !std::all_of(policy.allowed_sources.begin(), policy.allowed_sources.end(), [](auto value) { return valid(value); })) {
        return error(ErrorCode::InvalidDescriptor, "Binding sources must be nonempty, known and unique.");
    }
    if (policy.allowed_conversions.empty() || duplicates(policy.allowed_conversions)
        || !std::all_of(policy.allowed_conversions.begin(), policy.allowed_conversions.end(), [](auto value) { return valid(value); })) {
        return error(ErrorCode::InvalidDescriptor, "Conversion policies must be nonempty, known and unique.");
    }
    switch (policy.cardinality) {
    case BindingCardinality::SingleSource: break;
    case BindingCardinality::MultipleSources:
    case BindingCardinality::CollectionSource:
        if (type.kind() != TypeKind::List) {
            return error(ErrorCode::InvalidDescriptor, "Multiple/collection sources require a List target.");
        }
        break;
    default: return error(ErrorCode::InvalidDescriptor, "Unrecognized binding cardinality.");
    }
    switch (policy.time) {
    case BindingTime::SnapshotBuild:
        if (policy.allowed_sources != std::vector{BindingSourceKind::Constant}) {
            return error(ErrorCode::InvalidDescriptor, "Snapshot-time values must be constant.");
        }
        break;
    case BindingTime::BeforeInvocation: break;
    default: return error(ErrorCode::InvalidDescriptor, "Unrecognized binding time.");
    }
    return std::monostate{};
}

DiagnosticLocation location(const ParameterDescriptor& descriptor)
{
    DiagnosticLocation result;
    result.parameter = descriptor.id;
    return result;
}

DiagnosticLocation location(const InputDescriptor& descriptor)
{
    DiagnosticLocation result;
    result.input = descriptor.id;
    return result;
}

Check parameter_flags(const InputDescriptor&) { return std::monostate{}; }
Check parameter_flags(const ParameterDescriptor& descriptor)
{
    if ((!descriptor.subscribable || descriptor.structural)
        && descriptor.binding.allowed_sources != std::vector{BindingSourceKind::Constant}) {
        return error(ErrorCode::InvalidDescriptor, "Non-subscribable and structural parameters accept only constants.");
    }
    if (descriptor.structural && descriptor.binding.time != BindingTime::SnapshotBuild) {
        return error(ErrorCode::InvalidDescriptor, "Structural parameters must be fixed while building the snapshot.");
    }
    return std::monostate{};
}

template <class Target>
Check target_check(const Target& descriptor)
{
    for (auto result : {label(descriptor.display_name), semantics(descriptor.semantics),
        constraint_schema(descriptor.type, descriptor.constraints), policy_check(descriptor.type, descriptor.binding), parameter_flags(descriptor)}) {
        if (auto* diagnostic = std::get_if<Diagnostic>(&result)) {
            diagnostic->location = location(descriptor);
            return result;
        }
    }
    if (descriptor.required == descriptor.default_value.has_value()) {
        return error(ErrorCode::InvalidDescriptor, "Required targets must not have defaults; non-required targets must declare defaults.", location(descriptor));
    }
    if (descriptor.default_value) {
        auto result = value_check(descriptor.type, descriptor.constraints, *descriptor.default_value);
        if (auto* diagnostic = std::get_if<Diagnostic>(&result)) {
            diagnostic->location = location(descriptor);
            return result;
        }
    }
    return std::monostate{};
}

template <class Target>
Result<ConfigValue> configured_value(const Target& descriptor, const std::optional<ConfigValue>& configured)
{
    if (auto checked = target_check(descriptor); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    const auto& selected = configured ? configured : descriptor.default_value;
    if (!selected) {
        return error(ErrorCode::MissingRequiredValue, "Target has no configured source; Optional type does not make its binding optional.", location(descriptor));
    }
    if (auto checked = value_check(descriptor.type, descriptor.constraints, *selected); auto* failure = std::get_if<Diagnostic>(&checked)) {
        failure->location = location(descriptor);
        return *failure;
    }
    return *selected;
}

template <class Target>
Result<ConfigValue> resolved_value(const Target& descriptor, const std::optional<Result<ConfigValue>>& source)
{
    if (!source) { return configured_value(descriptor, std::nullopt); }
    if (const auto* failure = std::get_if<Diagnostic>(&*source)) { return *failure; }
    return configured_value(descriptor, std::get<ConfigValue>(*source));
}

template <class Target>
Check binding_check(const Target& target, BindingSourceKind source, const DataType& source_type, ConversionKind conversion)
{
    if (auto checked = target_check(target); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (!valid(source) || !contains(target.binding.allowed_sources, source)) {
        return error(ErrorCode::DisallowedBindingSource, "This source kind is not permitted by the target.", location(target));
    }
    if (!valid(conversion) || !contains(target.binding.allowed_conversions, conversion)) {
        return error(ErrorCode::UnsupportedConversion, "This conversion was not permitted by the target.", location(target));
    }
    const DataType& expected = target.binding.cardinality == BindingCardinality::MultipleSources
        ? *target.type.element_type() : target.type;
    bool compatible = false;
    switch (conversion) {
    case ConversionKind::Exact: compatible = source_type == expected; break;
    case ConversionKind::WrapOptional:
        compatible = expected.kind() == TypeKind::Optional && source_type == *expected.element_type();
        break;
    case ConversionKind::Int64ToFloat64Explicit:
        compatible = source_type.kind() == TypeKind::Int64 && expected.kind() == TypeKind::Float64;
        break;
    }
    if (!compatible) {
        return error(ErrorCode::UnsupportedConversion, "Source and target types do not satisfy the selected conversion.", location(target));
    }
    return std::monostate{};
}

} // namespace

Result<std::monostate> validate_input_descriptor(const InputDescriptor& descriptor)
{ return target_check(descriptor); }

Result<std::monostate> validate_output_descriptor(const OutputDescriptor& descriptor)
{
    for (auto checked : {label(descriptor.display_name), semantics(descriptor.semantics)}) {
        if (auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.output = descriptor.id;
            return checked;
        }
    }
    return std::monostate{};
}

Result<std::monostate> validate_descriptor(const ToolDescriptor& descriptor)
{
    DiagnosticLocation tool_location;
    tool_location.tool = descriptor.id;
    tool_location.schema = descriptor.schema.id;
    const auto& execution = descriptor.execution;
    if ((execution.mode != ToolExecutionMode::Synchronous && execution.mode != ToolExecutionMode::Asynchronous)
        || (execution.lifetime != ToolInstanceLifetime::Invocation && execution.lifetime != ToolInstanceLifetime::WorkflowRun
            && execution.lifetime != ToolInstanceLifetime::SolutionSession)
        || (execution.reentrancy != ToolReentrancy::Serialized && execution.reentrancy != ToolReentrancy::Reentrant)
        || (execution.effects != ToolEffects::None && execution.effects != ToolEffects::State && execution.effects != ToolEffects::External)
        || (execution.cancellation != ToolCancellation::Unsupported && execution.cancellation != ToolCancellation::Cooperative)
        || (execution.retry != ToolRetry::Never && execution.retry != ToolRetry::Idempotent)) {
        return error(ErrorCode::InvalidDescriptor, "Tool execution policy contains an unknown capability.", tool_location);
    }
    const auto contextualize = [&](Diagnostic diagnostic) -> Check {
        diagnostic.location.tool = descriptor.id;
        diagnostic.location.schema = descriptor.schema.id;
        return diagnostic;
    };
    if (descriptor.config_version == 0 || descriptor.schema.version == 0) {
        return error(ErrorCode::InvalidDescriptor, "Tool configuration and Schema versions must both be nonzero.", tool_location);
    }
    if (!descriptor.schema.complete) {
        return error(ErrorCode::IncompleteSchema, "The complete declared Schema is required; omitted outputs cannot be silently discarded.", tool_location);
    }
    if (duplicates(descriptor.schema.required_capabilities)) {
        return error(ErrorCode::InvalidDescriptor, "Required capabilities must be unique.", tool_location);
    }
    if (const auto checked = label(descriptor.display_name); const auto* failure = std::get_if<Diagnostic>(&checked)) {
        return contextualize(*failure);
    }
    SchemaDefinitions definitions;
    if (descriptor.state) {
        if (const auto checked = validate_node_state_definition(*descriptor.state);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
        if (auto checked = consistent_schema(descriptor.state->schema, definitions, tool_location);
            std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    std::set<SchemaId> resource_slots;
    for (const auto& requirement : descriptor.resources) {
        if (!resource_slots.insert(requirement.slot).second) {
            return error(ErrorCode::InvalidDescriptor, "Resource requirement slots must be unique.", tool_location);
        }
        if (const auto checked = validate_resource_requirement(requirement);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
    }
    if (descriptor.region_capabilities) {
        if (const auto checked = validate_region_capabilities(*descriptor.region_capabilities);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
    }
    for (std::size_t index = 0; index < descriptor.image_formats.size(); ++index) {
        if (const auto checked = bytes_per_pixel(descriptor.image_formats[index]);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
        if (std::find(descriptor.image_formats.begin(), descriptor.image_formats.begin() + static_cast<std::ptrdiff_t>(index),
            descriptor.image_formats[index]) != descriptor.image_formats.begin() + static_cast<std::ptrdiff_t>(index)) {
            return error(ErrorCode::InvalidDescriptor, "Image format declarations must be unique.", tool_location);
        }
    }
    const auto check_targets = [&](const auto& targets) -> Check {
        using Id = decltype(targets.front().id);
        std::set<Id> ids;
        for (const auto& target : targets) {
            if (!ids.insert(target.id).second) {
                return contextualize(error(ErrorCode::InvalidDescriptor, "Duplicate target identity in a tool Schema.", location(target)));
            }
            if (const auto checked = target_check(target); const auto* failure = std::get_if<Diagnostic>(&checked)) {
                return contextualize(*failure);
            }
            auto target_location = location(target);
            target_location.tool = descriptor.id;
            target_location.schema = descriptor.schema.id;
            if (auto checked = consistent_schema(target.type, definitions, std::move(target_location)); std::holds_alternative<Diagnostic>(checked)) {
                return checked;
            }
        }
        return std::monostate{};
    };
    if (auto checked = check_targets(descriptor.parameters); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (auto checked = check_targets(descriptor.inputs); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    std::set<OutputId> outputs;
    for (const auto& output : descriptor.outputs) {
        DiagnosticLocation output_location = tool_location;
        output_location.output = output.id;
        if (!outputs.insert(output.id).second) {
            return error(ErrorCode::InvalidDescriptor, "Duplicate data output identity.", output_location);
        }
        for (auto checked : {label(output.display_name), semantics(output.semantics)}) {
            if (auto* failure = std::get_if<Diagnostic>(&checked)) {
                failure->location = output_location;
                return checked;
            }
        }
        if (auto checked = consistent_schema(output.type, definitions, output_location); std::holds_alternative<Diagnostic>(checked)) {
            return checked;
        }
    }
    std::set<ControlOutputId> controls;
    for (const auto& output : descriptor.control_outputs) {
        if (!controls.insert(output.id).second) {
            return error(ErrorCode::InvalidDescriptor, "Duplicate control output identity.", tool_location);
        }
        if (const auto checked = label(output.display_name); const auto* failure = std::get_if<Diagnostic>(&checked)) {
            return contextualize(*failure);
        }
    }
    return std::monostate{};
}

Result<std::monostate> validate_schema_support(const ToolDescriptor& descriptor, const SchemaSupport& support)
{
    if (auto checked = validate_descriptor(descriptor); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    DiagnosticLocation where;
    where.tool = descriptor.id;
    where.schema = descriptor.schema.id;
    if (support.minimum_version == 0 || support.minimum_version > support.maximum_version || duplicates(support.capabilities)) {
        return error(ErrorCode::InvalidDescriptor, "Schema support requires a nonzero ordered version range and unique capabilities.", where);
    }
    if (support.id != descriptor.schema.id || descriptor.schema.version < support.minimum_version
        || descriptor.schema.version > support.maximum_version) {
        return error(ErrorCode::IncompatibleSchemaVersion, "Schema identity/version is not supported.", where);
    }
    for (const auto& required : descriptor.schema.required_capabilities) {
        if (!contains(support.capabilities, required)) {
            return error(ErrorCode::MissingCapability, "A declared Schema capability is unavailable: " + std::string(required.text()), where);
        }
    }
    return std::monostate{};
}

Result<std::monostate> validate_image_support(const ToolDescriptor& descriptor, const ImageHandle& image)
{
    if (auto checked = validate_descriptor(descriptor); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (!contains(descriptor.image_formats, image.format())) {
        DiagnosticLocation where;
        where.tool = descriptor.id;
        return error(ErrorCode::UnsupportedPixelFormat, "Tool has not declared support for this image format.", where);
    }
    return std::monostate{};
}

Result<ConfigValue> validate_parameter_value(const ParameterDescriptor& target, const std::optional<ConfigValue>& configured)
{ return configured_value(target, configured); }
Result<ConfigValue> validate_input_value(const InputDescriptor& target, const std::optional<ConfigValue>& configured)
{ return configured_value(target, configured); }
Result<ConfigValue> resolve_parameter_value(const ParameterDescriptor& target, const std::optional<Result<ConfigValue>>& source)
{ return resolved_value(target, source); }
Result<ConfigValue> resolve_input_value(const InputDescriptor& target, const std::optional<Result<ConfigValue>>& source)
{ return resolved_value(target, source); }
Result<std::monostate> validate_binding(const ParameterDescriptor& target, BindingSourceKind source, const DataType& source_type, ConversionKind conversion)
{ return binding_check(target, source, source_type, conversion); }
Result<std::monostate> validate_binding(const InputDescriptor& target, BindingSourceKind source, const DataType& source_type, ConversionKind conversion)
{ return binding_check(target, source, source_type, conversion); }

} // namespace visionpro::core
