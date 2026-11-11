#include <visionpro/core/tool_descriptor.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace visionpro::core;
int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}

template <class T>
T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template <class Id>
Id id(std::string_view text) { return take(SchemaIdFactory::parse<Id>(text)); }

template <class T>
void expect_error(const Result<T>& result, ErrorCode code, std::string_view description)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, description);
}

template <class T>
bool success(const Result<T>& result) { return std::holds_alternative<T>(result); }

ParameterDescriptor parameter(DataType type = DataType::integer())
{ return {id<ParameterId>("threshold"), "Threshold", std::move(type)}; }
InputDescriptor input(DataType type = DataType::integer())
{ return {id<InputId>("source"), "Source", std::move(type)}; }
ToolDescriptor tool()
{
    return {id<ToolTypeId>("vision.measure"), "测量", 7,
        {id<SchemaId>("measure.schema"), 2, {}, true}, {parameter()}, {input()},
        {{id<OutputId>("result"), "Result", DataType::real()}},
        {{id<ControlOutputId>("result"), "Proceed"}}};
}

void metadata()
{
    auto descriptor = tool();
    check(success(validate_descriptor(descriptor)), "valid tool preserves separate data/control IDs with the same token");
    descriptor.parameters.front().display_name = "重命名";
    check(success(validate_descriptor(descriptor)), "display rename leaves stable Schema identity valid");
    descriptor.parameters.push_back(descriptor.parameters.front());
    const auto duplicate = validate_descriptor(descriptor);
    expect_error(duplicate, ErrorCode::InvalidDescriptor, "duplicate parameter ID is rejected");
    const auto* error = std::get_if<Diagnostic>(&duplicate);
    check(error && error->location.parameter == descriptor.parameters.front().id
        && error->location.tool == descriptor.id && error->location.schema == descriptor.schema.id,
        "descriptor diagnostic locates tool, Schema and parameter");
    descriptor = tool();
    descriptor.inputs.push_back(descriptor.inputs.front());
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "duplicate input ID is rejected");
    descriptor = tool();
    descriptor.outputs.push_back(descriptor.outputs.front());
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "duplicate output ID is rejected");
    descriptor = tool();
    descriptor.control_outputs.push_back(descriptor.control_outputs.front());
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "duplicate control output ID is rejected");
    descriptor = tool();
    descriptor.config_version = 0;
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "zero tool configuration version is rejected");
    descriptor = tool();
    descriptor.schema.version = 0;
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "zero Schema version is rejected independently");
    descriptor = tool();
    descriptor.display_name = std::string(1, static_cast<char>(0xff));
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "invalid UTF-8 display text is rejected");

    auto target = parameter();
    target.binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "non-subscribable parameter cannot declare result subscriptions");
    target.subscribable = true;
    check(success(validate_parameter_value(target, ConfigValue::integer(1))), "subscribable parameter can declare result sources");
    target.structural = true;
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "structural parameter cannot depend on runtime results");
    target.binding.allowed_sources = {BindingSourceKind::Constant};
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "structural parameter must be fixed before invocation");
    target.binding.time = BindingTime::SnapshotBuild;
    check(success(validate_parameter_value(target, ConfigValue::integer(1))), "constant structural parameter at snapshot time is valid");
    target.binding.allowed_sources.push_back(BindingSourceKind::Constant);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "duplicate allowed sources are rejected");
    target = parameter();
    target.binding.allowed_sources.clear();
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "empty sources are rejected");
    target = parameter();
    target.binding.allowed_sources = {static_cast<BindingSourceKind>(999)};
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "unknown source enum is rejected");
    target = parameter();
    target.binding.allowed_conversions.push_back(ConversionKind::Exact);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "duplicate conversions are rejected");
    target = parameter();
    target.binding.allowed_conversions = {static_cast<ConversionKind>(999)};
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "unknown conversion enum is rejected");
    target = parameter();
    target.binding.cardinality = static_cast<BindingCardinality>(999);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "unknown cardinality is rejected");
    target = parameter();
    target.binding.time = static_cast<BindingTime>(999);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "unknown binding time is rejected");
    target = parameter();
    target.semantics.unit = static_cast<MeasurementUnit>(999);
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "unknown unit is rejected");
    target.semantics.unit = MeasurementUnit::Pixel;
    target.semantics.coordinate_system = id<CoordinateSystemId>("sensor.raw");
    check(success(validate_parameter_value(target, ConfigValue::integer(1))), "typed unit and coordinate annotation can be represented");
}

void constraints()
{
    auto target = parameter();
    expect_error(validate_parameter_value(target, std::nullopt), ErrorCode::MissingRequiredValue, "required binding rejects missing configuration");
    auto optional_target = parameter(DataType::optional(DataType::integer()));
    expect_error(validate_parameter_value(optional_target, std::nullopt), ErrorCode::MissingRequiredValue, "required Optional still requires a source");
    check(success(validate_parameter_value(optional_target, ConfigValue::none(DataType::integer()))), "explicit Optional None is a present value");
    target.required = false;
    expect_error(validate_parameter_value(target, std::nullopt), ErrorCode::InvalidDescriptor, "non-required target needs an explicit default");
    target.default_value = ConfigValue::integer(7);
    auto selected = take(validate_parameter_value(target, std::nullopt));
    check(selected.as_int64() && *selected.as_int64() == 7, "missing optional configuration selects declared default");
    selected = take(validate_parameter_value(target, ConfigValue::integer(0)));
    check(selected.as_int64() && *selected.as_int64() == 0, "configured zero does not select default");
    DiagnosticLocation upstream_location;
    upstream_location.input = id<InputId>("producer");
    const Diagnostic upstream{ErrorCode::IdGenerationFailed, Severity::Error, upstream_location, "upstream failed"};
    const auto resolved = resolve_parameter_value(target, Result<ConfigValue>{upstream});
    expect_error(resolved, ErrorCode::IdGenerationFailed, "source failure cannot be replaced by default");
    check(std::get<Diagnostic>(resolved).message == "upstream failed", "source diagnostic is preserved");
    check(std::get<Diagnostic>(resolved).location.input == upstream_location.input
        && !std::get<Diagnostic>(resolved).location.parameter,
        "source location is not overwritten with the consuming parameter");
    auto default_input = input();
    default_input.required = false;
    default_input.default_value = ConfigValue::integer(9);
    const auto input_default = take(resolve_input_value(default_input, std::nullopt));
    check(input_default.as_int64() && *input_default.as_int64() == 9, "input resolver applies default only to missing source");
    expect_error(resolve_input_value(default_input, Result<ConfigValue>{upstream}), ErrorCode::IdGenerationFailed,
        "input resolver also preserves source failure instead of applying default");
    expect_error(validate_parameter_value(target, ConfigValue::boolean(false)), ErrorCode::TypeMismatch, "type error cannot be replaced by default");
    target.required = true;
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "required plus default is rejected as ambiguous");

    target = parameter();
    target.constraints.integer_range = IntegerRange{-2, 2};
    check(success(validate_parameter_value(target, ConfigValue::integer(-2))), "integer lower boundary is inclusive");
    check(success(validate_parameter_value(target, ConfigValue::integer(2))), "integer upper boundary is inclusive");
    expect_error(validate_parameter_value(target, ConfigValue::integer(3)), ErrorCode::ConstraintViolation, "integer overflow of declared range fails");
    target.constraints.integer_range = IntegerRange{3, -3};
    expect_error(validate_parameter_value(target, ConfigValue::integer(0)), ErrorCode::InvalidDescriptor, "inverted range is rejected without needing a default");
    target.constraints.integer_range = IntegerRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()};
    check(success(validate_parameter_value(target, ConfigValue::integer(std::numeric_limits<std::int64_t>::max()))), "integer bounds do not lose precision through Float64");

    auto real_target = parameter(DataType::real());
    real_target.constraints.real_range = RealRange{-0.5, 0.5};
    check(success(validate_parameter_value(real_target, take(ConfigValue::real(0.5)))), "real boundary is inclusive");
    expect_error(validate_parameter_value(real_target, take(ConfigValue::real(0.6))), ErrorCode::ConstraintViolation, "real outside range is rejected");
    real_target.constraints.real_range = RealRange{0, std::numeric_limits<double>::infinity()};
    expect_error(validate_parameter_value(real_target, take(ConfigValue::real(0))), ErrorCode::InvalidDescriptor, "non-finite range is rejected");
    real_target.constraints.real_range = RealRange{std::numeric_limits<double>::quiet_NaN(), 1};
    expect_error(validate_parameter_value(real_target, take(ConfigValue::real(0))), ErrorCode::InvalidDescriptor, "NaN range is rejected");
    target = parameter();
    target.constraints.real_range = RealRange{0, 1};
    expect_error(validate_parameter_value(target, ConfigValue::integer(0)), ErrorCode::InvalidDescriptor, "real range cannot constrain an Int64");

    target = parameter();
    target.constraints.allowed_values = {ConfigValue::integer(1), ConfigValue::integer(3)};
    check(success(validate_parameter_value(target, ConfigValue::integer(3))), "declared allowed value is accepted");
    expect_error(validate_parameter_value(target, ConfigValue::integer(2)), ErrorCode::ConstraintViolation, "undeclared allowed value is rejected");
    target.constraints.allowed_values.push_back(ConfigValue::integer(3));
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "duplicate allowed value is invalid metadata");
    target.constraints.allowed_values = {ConfigValue::boolean(true)};
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "allowed value type must match target");
    target.constraints.allowed_values = {ConfigValue::integer(10)};
    target.constraints.integer_range = IntegerRange{0, 2};
    expect_error(validate_parameter_value(target, ConfigValue::integer(1)), ErrorCode::InvalidDescriptor, "contradictory range and allowed set are rejected");

    auto text_target = parameter(DataType::string());
    text_target.constraints.max_length = 3;
    check(success(validate_parameter_value(text_target, take(ConfigValue::string("中")))), "UTF-8 length limits count bytes");
    expect_error(validate_parameter_value(text_target, take(ConfigValue::string("中文"))), ErrorCode::ConstraintViolation, "over-capacity text is rejected without truncation");
    auto binary_target = input(DataType::binary());
    binary_target.constraints.max_length = 1;
    check(success(validate_input_value(binary_target, ConfigValue::binary({0}))), "binary zero byte is preserved within capacity");
    expect_error(validate_input_value(binary_target, ConfigValue::binary({0, 1})), ErrorCode::ConstraintViolation, "binary over-capacity is rejected");
    auto list_target = input(DataType::list(DataType::integer()));
    list_target.constraints.max_length = 0;
    check(success(validate_input_value(list_target, take(ConfigValue::list(DataType::integer(), {})))), "typed empty list satisfies zero capacity");
    expect_error(validate_input_value(list_target, take(ConfigValue::list(DataType::integer(), {ConfigValue::integer(0)}))), ErrorCode::ConstraintViolation, "nonempty list exceeds zero capacity");
    target = parameter();
    target.constraints.max_length = 1;
    expect_error(validate_parameter_value(target, ConfigValue::integer(0)), ErrorCode::InvalidDescriptor, "length constraint is not valid on integer scalar");
    optional_target.constraints.integer_range = IntegerRange{0, 2};
    check(success(validate_parameter_value(optional_target, ConfigValue::none(DataType::integer()))), "None has no numeric payload to constrain");
    expect_error(validate_parameter_value(optional_target, ConfigValue::some(ConfigValue::integer(3))), ErrorCode::ConstraintViolation, "Some payload is range checked");
    optional_target.constraints.allowed_values = {ConfigValue::some(ConfigValue::integer(1))};
    expect_error(validate_parameter_value(optional_target, ConfigValue::none(DataType::integer())), ErrorCode::ConstraintViolation, "allowed set may explicitly exclude None");

    const auto enum_type = take(DataType::enumeration(id<SchemaId>("mode"), 1,
        {{id<EnumMemberId>("fast"), "Fast"}, {id<EnumMemberId>("precise"), "Precise"}}));
    auto enum_target = parameter(enum_type);
    enum_target.constraints.allowed_values = {take(ConfigValue::enumeration(enum_type, id<EnumMemberId>("fast")))};
    check(success(validate_parameter_value(enum_target, enum_target.constraints.allowed_values.front())), "enum target accepts allowed member of its own Schema");
    expect_error(validate_parameter_value(enum_target, take(ConfigValue::enumeration(enum_type, id<EnumMemberId>("precise")))), ErrorCode::ConstraintViolation, "valid enum member may be excluded by target constraints");
}

void conversions()
{
    auto target = input();
    check(success(validate_binding(target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Exact)), "exact match is allowed");
    expect_error(validate_binding(target, BindingSourceKind::RunOutput, DataType::integer(), ConversionKind::Exact), ErrorCode::DisallowedBindingSource, "source must be explicitly permitted");
    target.binding.allowed_sources.push_back(BindingSourceKind::RunOutput);
    check(success(validate_binding(target, BindingSourceKind::RunOutput, DataType::integer(), ConversionKind::Exact)), "declared runtime output is allowed");
    expect_error(validate_binding(target, static_cast<BindingSourceKind>(999), DataType::integer(), ConversionKind::Exact), ErrorCode::DisallowedBindingSource, "unknown source does not slip through policy");
    expect_error(validate_binding(target, BindingSourceKind::Constant, DataType::real(), ConversionKind::Exact), ErrorCode::UnsupportedConversion, "exact conversion does not silently coerce numeric types");
    auto optional_target = input(DataType::optional(DataType::integer()));
    check(success(validate_binding(optional_target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::WrapOptional)), "T to Optional T wrapping is allowed when declared");
    expect_error(validate_binding(optional_target, BindingSourceKind::Constant, DataType::real(), ConversionKind::WrapOptional), ErrorCode::UnsupportedConversion, "Optional wrapping does not change element type");
    expect_error(validate_binding(target, BindingSourceKind::Constant, optional_target.type, ConversionKind::Exact), ErrorCode::UnsupportedConversion, "Optional cannot implicitly unwrap to required scalar");
    optional_target.binding.allowed_conversions = {ConversionKind::Exact};
    expect_error(validate_binding(optional_target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::WrapOptional), ErrorCode::UnsupportedConversion, "target may forbid wrapping");
    auto real_target = input(DataType::real());
    expect_error(validate_binding(real_target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Exact), ErrorCode::UnsupportedConversion, "Int64 to Float64 is not generally lossless");
    expect_error(validate_binding(real_target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Int64ToFloat64Explicit), ErrorCode::UnsupportedConversion, "lossy conversion requires target permission");
    real_target.binding.allowed_conversions.push_back(ConversionKind::Int64ToFloat64Explicit);
    check(success(validate_binding(real_target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Int64ToFloat64Explicit)), "explicit declared Int64 to Float64 policy can be checked");
    expect_error(validate_binding(real_target, BindingSourceKind::Constant, DataType::string(), ConversionKind::Int64ToFloat64Explicit), ErrorCode::UnsupportedConversion, "explicit numeric policy does not permit strings");
    expect_error(validate_binding(real_target, BindingSourceKind::Constant, DataType::integer(), static_cast<ConversionKind>(999)), ErrorCode::UnsupportedConversion, "unknown selected conversion is rejected");

    auto multiple = input(DataType::list(DataType::integer()));
    multiple.binding.cardinality = BindingCardinality::MultipleSources;
    check(success(validate_binding(multiple, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Exact)), "multiple-source metadata checks each scalar against list element type");
    expect_error(validate_binding(multiple, BindingSourceKind::Constant, multiple.type, ConversionKind::Exact), ErrorCode::UnsupportedConversion, "collection input is not the same as several scalar sources");
    multiple.binding.cardinality = BindingCardinality::CollectionSource;
    check(success(validate_binding(multiple, BindingSourceKind::Constant, multiple.type, ConversionKind::Exact)), "collection source checks the complete List type");
    expect_error(validate_binding(multiple, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Exact), ErrorCode::UnsupportedConversion, "collection source rejects scalar");
    target.binding.cardinality = BindingCardinality::MultipleSources;
    expect_error(validate_binding(target, BindingSourceKind::Constant, DataType::integer(), ConversionKind::Exact), ErrorCode::InvalidDescriptor, "multiple-source target must declare collection type");
}

void compatibility()
{
    auto descriptor = tool();
    const auto required = id<CapabilityId>("typed-records");
    descriptor.schema.required_capabilities = {required};
    SchemaSupport support{descriptor.schema.id, 1, 2, {required}};
    check(success(validate_schema_support(descriptor, support)), "supported Schema version and capability are accepted");
    check(descriptor.config_version == 7 && descriptor.schema.version == 2, "tool configuration and Schema versions remain separate");
    support.maximum_version = 1;
    expect_error(validate_schema_support(descriptor, support), ErrorCode::IncompatibleSchemaVersion, "newer unknown Schema version fails explicitly");
    support.minimum_version = 3;
    support.maximum_version = 4;
    expect_error(validate_schema_support(descriptor, support), ErrorCode::IncompatibleSchemaVersion, "older unsupported Schema version fails explicitly");
    support.minimum_version = 0;
    expect_error(validate_schema_support(descriptor, support), ErrorCode::InvalidDescriptor, "zero supported version is invalid");
    support.minimum_version = 5;
    expect_error(validate_schema_support(descriptor, support), ErrorCode::InvalidDescriptor, "inverted supported version range is invalid");
    support.minimum_version = 1;
    support.id = id<SchemaId>("different.schema");
    expect_error(validate_schema_support(descriptor, support), ErrorCode::IncompatibleSchemaVersion, "matching number does not replace Schema identity check");
    support.id = descriptor.schema.id;
    support.capabilities.clear();
    expect_error(validate_schema_support(descriptor, support), ErrorCode::MissingCapability, "missing required capability is explicit");
    check(descriptor.outputs.size() == 1 && descriptor.outputs.front().id == id<OutputId>("result"), "failed support check does not delete declared output fields");
    support.capabilities = {required, required};
    expect_error(validate_schema_support(descriptor, support), ErrorCode::InvalidDescriptor, "duplicate available capabilities are rejected");
    support.capabilities = {required};
    descriptor.schema.required_capabilities.push_back(required);
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor, "duplicate required capabilities are rejected");
    descriptor.schema.required_capabilities = {required};
    descriptor.schema.complete = false;
    expect_error(validate_schema_support(descriptor, support), ErrorCode::IncompleteSchema, "known enumeration omissions cannot be accepted as a complete Schema");
    check(descriptor.outputs.size() == 1, "incomplete Schema rejection preserves declarations");

    const auto record_id = id<SchemaId>("shared.record");
    const auto field_id = id<FieldId>("value");
    const auto integer_record = take(DataType::record(record_id, 1, {{field_id, DataType::integer()}}));
    const auto conflicting_record = take(DataType::record(record_id, 1, {{field_id, DataType::real()}}));
    descriptor = tool();
    descriptor.parameters.front().type = integer_record;
    descriptor.inputs.front().type = DataType::list(DataType::optional(conflicting_record));
    const auto inconsistent = validate_descriptor(descriptor);
    expect_error(inconsistent, ErrorCode::InvalidSchema, "same Schema identity/version cannot change nested field types across targets");
    const auto* inconsistent_error = std::get_if<Diagnostic>(&inconsistent);
    check(inconsistent_error && inconsistent_error->location.tool == descriptor.id
        && inconsistent_error->location.schema == record_id
        && inconsistent_error->location.input == descriptor.inputs.front().id,
        "inconsistent Schema identifies the tool, conflicting Schema and consuming input");
    descriptor.inputs.front().type = DataType::list(DataType::optional(integer_record));
    descriptor.outputs.front().type = DataType::optional(conflicting_record);
    expect_error(validate_descriptor(descriptor), ErrorCode::InvalidSchema, "Schema consistency also spans output declarations");
    const auto enclosing = take(DataType::record(id<SchemaId>("enclosing.record"), 1,
        {{id<FieldId>("child"), conflicting_record}}));
    descriptor.outputs.front().type = enclosing;
    const auto nested = validate_descriptor(descriptor);
    expect_error(nested, ErrorCode::InvalidSchema, "consistency recursively inspects nested Record fields");
    const auto* nested_error = std::get_if<Diagnostic>(&nested);
    check(nested_error && nested_error->location.path.size() == 1
        && std::get<FieldId>(nested_error->location.path.front()) == id<FieldId>("child"),
        "nested Schema conflict retains its field path");
    descriptor.outputs.front().type = take(DataType::record(record_id, 2, {{field_id, DataType::real()}}));
    check(success(validate_descriptor(descriptor)), "different Schema versions may coexist with different structures");

    const auto enum_id = id<SchemaId>("shared.enum");
    const auto member_id = id<EnumMemberId>("enabled");
    const auto enum_original = take(DataType::enumeration(enum_id, 1, {{member_id, "Enabled"}}));
    const auto enum_renamed = take(DataType::enumeration(enum_id, 1, {{member_id, "启用"}}));
    descriptor = tool();
    descriptor.parameters.front().type = enum_original;
    descriptor.inputs.front().type = DataType::optional(enum_renamed);
    descriptor.outputs.front().type = DataType::list(enum_original);
    check(success(validate_descriptor(descriptor)), "display-only enum label changes preserve one structural Schema definition");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "metadata") { metadata(); }
        else if (group == "constraints") { constraints(); }
        else if (group == "conversions") { conversions(); }
        else if (group == "compatibility") { compatibility(); }
        else { return 2; }
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 1;
    } catch (...) { return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
