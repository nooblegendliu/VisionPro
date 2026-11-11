#include <visionpro/core/value.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/text.hpp>

#include <any>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace visionpro::core;

template <class Value>
constexpr bool value_interface = !std::is_default_constructible_v<Value>
    && !std::is_constructible_v<Value, bool> && !std::is_constructible_v<Value, std::int64_t>
    && !std::is_constructible_v<Value, std::string> && !std::is_constructible_v<Value, std::any>
    && !std::is_constructible_v<Value, void*>
    && std::is_same_v<decltype(std::declval<Value&>().as_string()), const std::string*>
    && std::is_same_v<decltype(std::declval<Value&>().as_binary()), const std::vector<std::uint8_t>*>
    && std::is_same_v<decltype(std::declval<Value&>().elements()), const std::vector<Value>*>
    && std::is_same_v<decltype(std::declval<Value&>().optional_value()), const Value*>
    && std::is_same_v<decltype(std::declval<Value&>().type()), const DataType&>;
static_assert(value_interface<ConfigValue> && value_interface<DataValue>);
static_assert(!std::is_constructible_v<ConfigValue, DataValue> && !std::is_constructible_v<DataValue, ConfigValue>);
static_assert(!std::is_assignable_v<ConfigValue&, DataValue> && !std::is_assignable_v<DataValue&, ConfigValue>);
static_assert(!std::is_convertible_v<ConfigValue, DataValue> && !std::is_convertible_v<DataValue, ConfigValue>);
static_assert(std::is_same_v<decltype(std::declval<ConfigValue&>().fields()), const std::vector<ConfigFieldValue>*>);
static_assert(std::is_same_v<decltype(std::declval<DataValue&>().fields()), const std::vector<DataFieldValue>*>);
static_assert(!std::is_default_constructible_v<DataType> && !std::is_constructible_v<DataType, TypeKind>);
static_assert(std::is_same_v<decltype(std::declval<DataType&>().fields()), const std::vector<FieldSchema>&>);
static_assert(std::is_same_v<decltype(std::declval<DataType&>().enum_members()), const std::vector<EnumMember>&>);
static_assert(std::is_same_v<decltype(std::declval<DataType&>().element_type()), const DataType*>);

int checks = 0;
int failures = 0;

void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template <class T>
T value(Result<T> result)
{
    if (const auto* item = std::get_if<T>(&result)) { return *item; }
    throw std::runtime_error("Expected a valid fixture: " + std::get<Diagnostic>(result).message);
}

template <class T>
Diagnostic error(const Result<T>& result, ErrorCode expected)
{
    const auto* item = std::get_if<Diagnostic>(&result);
    check(item != nullptr, "expected failure returns a diagnostic instead of a value");
    if (item == nullptr) { throw std::runtime_error("Cannot inspect an unexpectedly successful result"); }
    check(item->code == expected, "diagnostic preserves the specific failure cause");
    check(item->severity == Severity::Error && !item->message.empty(), "diagnostic contains error severity and explanation");
    return *item;
}

template <SchemaIdentity Id>
Id key(std::string_view text) { return value(SchemaIdFactory::parse<Id>(text)); }

void field_location(const Diagnostic& diagnostic, const SchemaId& schema, const FieldId& field)
{
    check(diagnostic.location.schema == schema, "field diagnostic names its Schema");
    check(diagnostic.location.path.size() == 1, "field diagnostic has a specific path step");
    if (diagnostic.location.path.size() == 1) {
        const auto* location = std::get_if<FieldId>(&diagnostic.location.path.front());
        check(location != nullptr && *location == field, "field diagnostic names the offending field");
    }
}

template <class V>
void scalar_values()
{
    const auto yes = V::boolean(true);
    const auto no = V::boolean(false);
    check(yes.type() == DataType::boolean() && yes.as_bool() != nullptr && *yes.as_bool(), "Bool true retains type and value");
    check(no.as_bool() != nullptr && !*no.as_bool(), "Bool false is a valid value");
    const auto zero = V::integer(0);
    check(no != zero, "false and integer zero retain distinct types");
    check(zero.as_int64() != nullptr && *zero.as_int64() == 0, "integer zero remains valid");
    const auto minimum = V::integer(std::numeric_limits<std::int64_t>::min());
    const auto maximum = V::integer(std::numeric_limits<std::int64_t>::max());
    check(minimum.as_int64() && *minimum.as_int64() == std::numeric_limits<std::int64_t>::min(), "Int64 minimum is lossless");
    check(maximum.as_int64() && *maximum.as_int64() == std::numeric_limits<std::int64_t>::max(), "Int64 maximum is lossless");
    check(zero.as_bool() == nullptr && zero.as_float64() == nullptr && zero.as_string() == nullptr
        && zero.as_binary() == nullptr && zero.as_enum() == nullptr && zero.elements() == nullptr
        && zero.fields() == nullptr && zero.optional_value() == nullptr,
        "wrong-kind accessors do not reinterpret an integer payload");

    constexpr std::array finite{0.0, -0.0, 1.25, std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(), std::numeric_limits<double>::min(),
        std::numeric_limits<double>::denorm_min()};
    for (const auto input : finite) {
        const auto number = value(V::real(input));
        check(number.type() == DataType::real() && number.as_float64() != nullptr
            && *number.as_float64() == input, "finite Float64 values preserve type and value");
        check(number.as_int64() == nullptr, "Float64 does not silently convert to Int64");
    }
    const auto negative_zero = value(V::real(-0.0));
    check(negative_zero.as_float64() && std::signbit(*negative_zero.as_float64()), "Float64 preserves the sign of negative zero");
    error(V::real(std::numeric_limits<double>::quiet_NaN()), ErrorCode::InvalidValue);
    error(V::real(std::numeric_limits<double>::infinity()), ErrorCode::InvalidValue);
    error(V::real(-std::numeric_limits<double>::infinity()), ErrorCode::InvalidValue);

    auto input_bytes = std::vector<std::uint8_t>{0x00, 0x80, 0xff, 0x00, 0x01};
    const auto bytes = V::binary(input_bytes);
    input_bytes.assign({0x42});
    check(bytes.type() == DataType::binary() && bytes.as_binary() != nullptr
        && *bytes.as_binary() == std::vector<std::uint8_t>({0x00, 0x80, 0xff, 0x00, 0x01}),
        "Binary preserves zeros and arbitrary bytes independently of source storage");
    const auto empty_binary = V::binary({});
    check(empty_binary.as_binary() && empty_binary.as_binary()->empty(), "empty Binary is a typed valid value");
    check(bytes.as_string() == nullptr, "binary bytes are not implicitly UTF-8 strings");
}

template <class V>
void container_values()
{
    const auto empty_ints = value(V::list(DataType::integer(), {}));
    const auto empty_strings = value(V::list(DataType::string(), {}));
    check(empty_ints.type() == DataType::list(DataType::integer()) && empty_ints.elements()
        && empty_ints.elements()->empty(), "an empty List retains its element type");
    check(empty_ints != empty_strings, "empty Lists with different element types are distinct");
    const auto none = V::none(DataType::integer());
    const auto some_zero = V::some(V::integer(0));
    check(none.type() == DataType::optional(DataType::integer()) && !none.optional_value(), "None retains Optional element type");
    check(none.type() == some_zero.type() && none != some_zero, "None differs from Some(0) within the same Optional type");
    check(some_zero.optional_value() && some_zero.optional_value()->as_int64()
        && *some_zero.optional_value()->as_int64() == 0, "Some exposes its actual contained zero value");
    check(none != V::none(DataType::string()), "None of different Optional types is distinct");
    check(none != empty_ints && none != V::integer(0) && none != V::boolean(false), "None does not alias empty List, zero, or false");
    const std::optional<V> absent;
    const std::optional<V> explicit_none = none;
    check(!absent && explicit_none.has_value(), "an unconfigured slot differs from an explicitly supplied None value");
    const Result<V> failed = Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "unavailable source fixture"};
    check(std::get_if<V>(&failed) == nullptr && std::get_if<Diagnostic>(&failed) != nullptr,
        "failure is not representable as a successful None value");
    const auto nested_none = V::some(none);
    check(nested_none.type() == DataType::optional(DataType::optional(DataType::integer()))
        && nested_none.optional_value() && !nested_none.optional_value()->optional_value(),
        "nested Optional distinguishes outer Some from inner None");
    const auto some_empty = V::some(empty_ints);
    const auto no_list = V::none(DataType::list(DataType::integer()));
    check(some_empty.type() == no_list.type() && some_empty != no_list && some_empty.optional_value()->elements()->empty(),
        "Optional<List> distinguishes an empty present List from None");

    auto source = std::vector<V>{V::integer(1), V::integer(2)};
    const auto list = value(V::list(DataType::integer(), source));
    source[0] = V::integer(99);
    source.clear();
    check(list.elements() && list.elements()->size() == 2 && *list.elements()->front().as_int64() == 1,
        "List owns immutable values independently of the caller's vector");
    auto copy = list;
    copy = empty_ints;
    check(list.elements()->size() == 2 && copy.elements()->empty(), "assigning a value copy does not mutate another owner");
    auto move_source = list;
    auto moved = std::move(move_source);
    check(moved == list && move_source == list, "moving values preserves source and destination invariants");
    moved = std::move(move_source);
    check(moved == list && move_source == list, "move assignment preserves both valid values");
    const auto mismatch = error(V::list(DataType::integer(), {V::integer(1), V::boolean(true)}), ErrorCode::TypeMismatch);
    check(mismatch.location.path.size() == 1, "heterogeneous List error has a location");
    if (mismatch.location.path.size() == 1) {
        const auto* index = std::get_if<DiagnosticElementIndex>(&mismatch.location.path.front());
        check(index && index->value == 1, "heterogeneous List error locates the offending element index");
    }
    error(V::list(DataType::optional(DataType::integer()), {V::integer(1)}), ErrorCode::TypeMismatch);
    const auto optionals = value(V::list(DataType::optional(DataType::integer()), {none, some_zero}));
    check(optionals.elements()->size() == 2, "List of Optional preserves explicit None and Some elements");
}

void type_schemas()
{
    const auto schema = key<SchemaId>("measurement.result");
    const auto other_schema = key<SchemaId>("measurement.other");
    const auto radius = key<FieldId>("radius");
    const auto label = key<FieldId>("label");
    const auto record = value(DataType::record(schema, 1, {{radius, DataType::real()}, {label, DataType::string()}}));
    const auto reordered = value(DataType::record(schema, 1, {{label, DataType::string()}, {radius, DataType::real()}}));
    check(record == reordered, "Record field declaration order does not change its type");
    check(record.schema_id() == schema && record.schema_version() == 1 && record.kind() == TypeKind::Record,
        "Record type carries Schema identity and version");
    check(record != value(DataType::record(other_schema, 1, {{radius, DataType::real()}, {label, DataType::string()}})), "Record Schema identity affects type equality");
    check(record != value(DataType::record(schema, 2, {{radius, DataType::real()}, {label, DataType::string()}})), "Record Schema version affects type equality");
    check(record != value(DataType::record(schema, 1, {{radius, DataType::integer()}, {label, DataType::string()}})), "Record field types affect type equality");
    check(record != value(DataType::record(schema, 1, {{radius, DataType::real()}})), "Record field membership affects type equality");
    error(DataType::record(schema, 0, {}), ErrorCode::InvalidSchema);
    const auto duplicate = error(DataType::record(schema, 1, {{radius, DataType::real()}, {radius, DataType::integer()}}), ErrorCode::DuplicateSchemaMember);
    field_location(duplicate, schema, radius);
    check(value(DataType::record(schema, 1, {})).fields().empty(), "empty Record Schema is valid");
    auto source_fields = std::vector<FieldSchema>{{radius, DataType::real()}};
    const auto independent = value(DataType::record(schema, 1, source_fields));
    source_fields.front().type = DataType::boolean();
    source_fields.clear();
    check(independent.fields().size() == 1 && independent.fields().front().type == DataType::real(), "Record Schema owns immutable field definitions");
    auto type_source = record;
    auto moved_type = std::move(type_source);
    check(moved_type == record && type_source == record, "moving DataType preserves valid source and destination");
    moved_type = std::move(type_source);
    check(moved_type == record && type_source == record, "DataType move assignment preserves invariants");
    check(DataType::integer() == DataType::integer() && DataType::integer() != DataType::real(), "scalar type equality is semantic");
    check(DataType::list(record) == DataType::list(reordered), "nested List type comparison uses Record structure");
    check(DataType::optional(record) != DataType::list(record), "Optional and List remain different constructors");
    check(DataType::integer().element_type() == nullptr && !DataType::integer().schema_id()
        && DataType::integer().schema_version() == 0 && DataType::integer().fields().empty()
        && DataType::integer().enum_members().empty(), "scalar types do not claim composite Schema metadata");

    const auto enum_schema = key<SchemaId>("inspection.mode");
    const auto fast = key<EnumMemberId>("fast");
    const auto precise = key<EnumMemberId>("precise");
    const auto enumeration = value(DataType::enumeration(enum_schema, 1, {{fast, "快速"}, {precise, "精确"}}));
    const auto renamed = value(DataType::enumeration(enum_schema, 1, {{precise, "Accurate"}, {fast, "Quick"}}));
    check(enumeration == renamed, "Enum member order and display labels do not change identity or type");
    check(enumeration != value(DataType::enumeration(other_schema, 1, {{fast, "Quick"}, {precise, "Accurate"}})), "Enum Schema identity affects type equality");
    check(enumeration != value(DataType::enumeration(enum_schema, 2, {{fast, "Quick"}, {precise, "Accurate"}})), "Enum Schema version affects type equality");
    check(enumeration != value(DataType::enumeration(enum_schema, 1, {{fast, "Quick"}})), "Enum member set affects type equality");
    error(DataType::enumeration(enum_schema, 0, {{fast, "Fast"}}), ErrorCode::InvalidSchema);
    error(DataType::enumeration(enum_schema, 1, {}), ErrorCode::InvalidSchema);
    const auto duplicate_enum = error(DataType::enumeration(enum_schema, 1, {{fast, "Fast"}, {fast, "Quick"}}), ErrorCode::DuplicateSchemaMember);
    check(duplicate_enum.location.schema == enum_schema, "duplicate Enum member identifies its Schema");
    error(DataType::enumeration(enum_schema, 1, {{fast, std::string("\xff")}}), ErrorCode::InvalidUtf8);
    auto source_members = std::vector<EnumMember>{{fast, "Fast"}};
    const auto owned_enum = value(DataType::enumeration(enum_schema, 1, source_members));
    source_members.front().display_name = "Changed";
    source_members.clear();
    check(owned_enum.enum_members().front().display_name == "Fast", "Enum Schema owns member display text");
}

template <ValueDomain Domain>
void structured_values()
{
    using V = BasicValue<Domain>;
    using F = BasicFieldValue<Domain>;
    const auto schema = key<SchemaId>("measurement.result");
    const auto count = key<FieldId>("count");
    const auto comment = key<FieldId>("comment");
    const auto unknown = key<FieldId>("unknown");
    const auto type = value(DataType::record(schema, 1, {{count, DataType::integer()}, {comment, DataType::optional(DataType::string())}}));
    const auto no_comment = V::none(DataType::string());
    const auto record = value(V::record(type, {{count, V::integer(3)}, {comment, no_comment}}));
    check(record == value(V::record(type, {{comment, no_comment}, {count, V::integer(3)}})), "Record field value order does not change its value");
    check(record.field(count) && record.field(count)->as_int64() && *record.field(count)->as_int64() == 3, "Record fields resolve by stable FieldId");
    check(record.field(comment) && record.field(comment)->type().kind() == TypeKind::Optional
        && !record.field(comment)->optional_value(), "Record explicitly stores a None field");
    check(record.field(unknown) == nullptr && V::integer(1).field(count) == nullptr, "field access does not invent an undeclared value");
    field_location(error(V::record(type, {{count, V::integer(3)}}), ErrorCode::MissingField), schema, comment);
    field_location(error(V::record(type, {{comment, no_comment}}), ErrorCode::MissingField), schema, count);
    field_location(error(V::record(type, {{count, V::integer(3)}, {comment, no_comment}, {unknown, V::integer(9)}}), ErrorCode::UnknownField), schema, unknown);
    field_location(error(V::record(type, {{count, V::boolean(true)}, {comment, no_comment}}), ErrorCode::TypeMismatch), schema, count);
    field_location(error(V::record(type, {{count, V::integer(3)}, {comment, V::none(DataType::integer())}}), ErrorCode::TypeMismatch), schema, comment);
    field_location(error(V::record(type, {{count, V::integer(3)}, {count, V::integer(4)}, {comment, no_comment}}), ErrorCode::DuplicateSchemaMember), schema, count);
    error(V::record(DataType::integer(), {}), ErrorCode::TypeMismatch);
    const auto empty_record = value(V::record(value(DataType::record(schema, 1, {})), {}));
    check(empty_record.fields() && empty_record.fields()->empty(), "empty Record value is valid under an empty Record Schema");
    auto caller_fields = std::vector<F>{{count, V::integer(3)}, {comment, no_comment}};
    const auto owned = value(V::record(type, caller_fields));
    caller_fields.front().value = V::integer(99);
    caller_fields.clear();
    check(owned == record, "Record owns values independently of the caller's mutable field vector");
    const auto list = value(V::list(type, {record, owned}));
    check(list.type() == DataType::list(type) && list.elements()->size() == 2 && list.elements()->front() == record,
        "List<Record> preserves a shared fixed Schema and field values");
    const auto changed_type = value(DataType::record(schema, 2, {{count, DataType::integer()}, {comment, DataType::optional(DataType::string())}}));
    const auto changed_record = value(V::record(changed_type, {{count, V::integer(3)}, {comment, no_comment}}));
    check(record != changed_record, "equal field payloads with different Schema versions remain distinct values");
    error(V::list(type, {record, changed_record}), ErrorCode::TypeMismatch);

    const auto enum_schema = key<SchemaId>("inspection.mode");
    const auto fast = key<EnumMemberId>("fast");
    const auto precise = key<EnumMemberId>("precise");
    const auto absent = key<EnumMemberId>("absent");
    const auto enum_type = value(DataType::enumeration(enum_schema, 1, {{fast, "Fast"}, {precise, "Precise"}}));
    const auto selected = value(V::enumeration(enum_type, fast));
    check(selected.as_enum() && *selected.as_enum() == fast && selected.type() == enum_type, "Enum value retains declared Schema and member identity");
    check(selected != value(V::enumeration(enum_type, precise)), "different enum members remain distinct values");
    error(V::enumeration(DataType::integer(), fast), ErrorCode::TypeMismatch);
    const auto invalid_member = error(V::enumeration(enum_type, absent), ErrorCode::InvalidEnumValue);
    check(invalid_member.location.schema == enum_schema, "undeclared Enum value identifies its Schema");
    const auto renamed = value(DataType::enumeration(enum_schema, 1, {{precise, "精确"}, {fast, "快速"}}));
    check(selected == value(V::enumeration(renamed, fast)), "renaming Enum display labels does not change the selected value");
}

template <class V>
void utf8_values()
{
    const std::array valid{
        std::string{}, std::string("plain ASCII"), std::string("中文路径/工件"),
        std::string("\xf0\x9f\x98\x80"), std::string("a\0b", 3),
        std::string("\x7f"), std::string("\xc2\x80"), std::string("\xdf\xbf"),
        std::string("\xe0\xa0\x80"), std::string("\xed\x9f\xbf"),
        std::string("\xee\x80\x80"), std::string("\xef\xbf\xbf"),
        std::string("\xf0\x90\x80\x80"), std::string("\xf4\x8f\xbf\xbf")};
    for (const auto& input : valid) {
        check(is_valid_utf8(input), "valid Unicode scalar sequence is accepted by UTF-8 validation");
        const auto text = value(V::string(input));
        check(text.type() == DataType::string() && text.as_string() && *text.as_string() == input,
            "String preserves UTF-8 bytes and length, including embedded U+0000");
    }
    const std::array invalid{
        std::string("\x80"), std::string("\xbf"), std::string("\xc0\x80"), std::string("\xc1\xbf"),
        std::string("\xc2"), std::string("\xc2\x7f"), std::string("\xc2\xc0"),
        std::string("\xe0\x80\x80"), std::string("\xe0\x9f\xbf"),
        std::string("\xe2"), std::string("\xe2\x82"), std::string("\xe2\x82\x7f"),
        std::string("\xed\xa0\x80"), std::string("\xed\xbf\xbf"),
        std::string("\xf0\x80\x80\x80"), std::string("\xf0\x8f\xbf\xbf"),
        std::string("\xf0"), std::string("\xf0\x90"), std::string("\xf0\x90\x80"),
        std::string("\xf4\x90\x80\x80"), std::string("\xf5\x80\x80\x80"),
        std::string("\xf8\x88\x80\x80\x80"), std::string("\xfc\x84\x80\x80\x80\x80"),
        std::string("\xfe"), std::string("\xff"), std::string("valid\x80")};
    for (const auto& input : invalid) {
        check(!is_valid_utf8(input), "malformed, overlong, surrogate, out-of-range, or truncated UTF-8 is rejected");
        error(V::string(input), ErrorCode::InvalidUtf8);
    }
    const std::string bounded = "prefix" + std::string("中文") + "suffix";
    const auto view = std::string_view(bounded).substr(6, std::string_view("中文").size());
    const auto bounded_value = value(V::string(view));
    check(bounded_value.as_string() && *bounded_value.as_string() == "中文", "String factory respects a non-NUL-terminated view");
    auto caller = std::string("original");
    const auto owned = value(V::string(caller));
    caller.assign("changed");
    check(owned.as_string() && *owned.as_string() == "original", "String owns bytes independently of the caller's storage");
    const auto nul = value(V::string(std::string("\0", 1)));
    check(nul.as_string()->size() == 1 && nul != value(V::string("")), "embedded U+0000 is not an empty string sentinel");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Expected values, schema, or utf8 test group\n";
        return 2;
    }
    const std::string_view group = argv[1];
    try {
        if (group == "values") {
            scalar_values<ConfigValue>();
            scalar_values<DataValue>();
            container_values<ConfigValue>();
            container_values<DataValue>();
        } else if (group == "schema") {
            type_schemas();
            structured_values<ValueDomain::Config>();
            structured_values<ValueDomain::Data>();
        } else if (group == "utf8") {
            utf8_values<ConfigValue>();
            utf8_values<DataValue>();
        } else {
            std::cerr << "Unknown test group: " << group << '\n';
            return 2;
        }
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception\n";
        return 1;
    }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
