#include "value_codec.hpp"

#include "geometry_codec.hpp"
#include "resource_codec.hpp"

#include <stdexcept>

namespace visionpro::infrastructure::detail {
using namespace core;

template<> struct EnumNames<TypeKind> {
    static constexpr auto values = std::to_array<std::string_view>({
        "bool", "int64", "float64", "string", "binary", "enum", "record", "list", "optional", "image",
        "point2d", "line2d", "circle2d", "pose2d", "transform2d", "region2d", "raster_mask", "resource_reference"});
};

namespace {

Json encode_member(const EnumMember& member)
{
    return {{"id", write_id(member.id)}, {"display_name", member.display_name}};
}

EnumMember decode_member(const Reader& reader)
{
    reader.object({"id", "display_name"});
    return {read_id<EnumMemberId>(reader.field("id")), std::string(reader.field("display_name").text())};
}

Json encode_field(const FieldSchema& field)
{
    return {{"id", write_id(field.id)}, {"type", encode_type(field.type)}};
}

FieldSchema decode_field(const Reader& reader)
{
    reader.object({"id", "type"});
    return {read_id<FieldId>(reader.field("id")), decode_type(reader.field("type"))};
}

Json encode_field_value(const ConfigFieldValue& field)
{
    return {{"id", write_id(field.id)}, {"value", encode_value(field.value)}};
}

ConfigFieldValue decode_field_value(const Reader& reader)
{
    reader.object({"id", "value"});
    return {read_id<FieldId>(reader.field("id")), decode_value(reader.field("value"))};
}

std::string encode_binary(const std::vector<std::uint8_t>& bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return hex;
}

std::uint8_t hex_digit(char character, const Reader& reader)
{
    if (character >= '0' && character <= '9') { return static_cast<std::uint8_t>(character - '0'); }
    if (character >= 'a' && character <= 'f') { return static_cast<std::uint8_t>(character - 'a' + 10); }
    if (character >= 'A' && character <= 'F') { return static_cast<std::uint8_t>(character - 'A' + 10); }
    reader.fail(ErrorCode::InvalidValue, "Binary requires hexadecimal digits.");
}

std::vector<std::uint8_t> decode_binary(const Reader& reader)
{
    const auto text = reader.text();
    if (text.size() % 2 != 0) { reader.fail(ErrorCode::InvalidValue, "Binary requires two hexadecimal digits per byte."); }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const auto high = hex_digit(text[index], reader);
        const auto low = hex_digit(text[index + 1], reader);
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

Json encode_payload(const ConfigValue& value)
{
    switch (value.type().kind()) {
    case TypeKind::Bool: return *value.as_bool();
    case TypeKind::Int64: return std::to_string(*value.as_int64());
    case TypeKind::Float64: return *value.as_float64();
    case TypeKind::String: return *value.as_string();
    case TypeKind::Binary: return encode_binary(*value.as_binary());
    case TypeKind::Enum: return write_id(*value.as_enum());
    case TypeKind::Record: return write_array(*value.fields(), encode_field_value);
    case TypeKind::List: return write_array(*value.elements(), encode_value);
    case TypeKind::Optional: {
        const auto* inner = value.optional_value();
        return inner ? encode_value(*inner) : Json(nullptr);
    }
    case TypeKind::Point2D: return encode_point(*value.as_point2d());
    case TypeKind::Line2D: return encode_line(*value.as_line2d());
    case TypeKind::Circle2D: return encode_circle(*value.as_circle2d());
    case TypeKind::Pose2D: return encode_pose(*value.as_pose2d());
    case TypeKind::Transform2D: return encode_transform(*value.as_transform2d());
    case TypeKind::Region2D: return encode_region(*value.as_region2d());
    case TypeKind::ResourceReference: return encode_resource_reference(*value.as_resource_reference());
    case TypeKind::Image:
    case TypeKind::RasterMask: break;
    }
    throw std::logic_error("ConfigValue has an unsupported payload type.");
}

ConfigValue decode_payload(const Reader& reader, const DataType& type)
{
    switch (type.kind()) {
    case TypeKind::Bool: return ConfigValue::boolean(reader.boolean());
    case TypeKind::Int64: return ConfigValue::integer(reader.i64());
    case TypeKind::Float64: return reader.checked(ConfigValue::real(reader.number()));
    case TypeKind::String: return reader.checked(ConfigValue::string(reader.text()));
    case TypeKind::Binary: return ConfigValue::binary(decode_binary(reader));
    case TypeKind::Enum: return reader.checked(ConfigValue::enumeration(type, read_id<EnumMemberId>(reader)));
    case TypeKind::Record:
        return reader.checked(ConfigValue::record(type, read_array<ConfigFieldValue>(reader, decode_field_value)));
    case TypeKind::List:
        return reader.checked(ConfigValue::list(*type.element_type(), read_array<ConfigValue>(reader, decode_value)));
    case TypeKind::Optional: {
        if (reader.is_null()) { return ConfigValue::none(*type.element_type()); }
        auto inner = decode_value(reader);
        // Optional::some derives its type from the value. Compare the full
        // encoded schema so rebuilding it also preserves enum display names.
        if (encode_type(inner.type()) != encode_type(*type.element_type())) {
            reader.fail(ErrorCode::TypeMismatch, "Optional value has a different type from its declaration.");
        }
        return ConfigValue::some(std::move(inner));
    }
    case TypeKind::Point2D: return reader.checked(ConfigValue::point2d(decode_point(reader)));
    case TypeKind::Line2D: return reader.checked(ConfigValue::line2d(decode_line(reader)));
    case TypeKind::Circle2D: return reader.checked(ConfigValue::circle2d(decode_circle(reader)));
    case TypeKind::Pose2D: return reader.checked(ConfigValue::pose2d(decode_pose(reader)));
    case TypeKind::Transform2D: return reader.checked(ConfigValue::transform2d(decode_transform(reader)));
    case TypeKind::Region2D: return reader.checked(ConfigValue::region2d(decode_region(reader)));
    case TypeKind::ResourceReference:
        return reader.checked(ConfigValue::resource_reference(decode_resource_reference(reader)));
    case TypeKind::Image:
    case TypeKind::RasterMask:
        reader.fail(ErrorCode::UnsupportedSolutionContent, "Image and raster mask payloads are runtime-only.");
    }
    reader.fail(ErrorCode::UnsupportedSolutionContent, "Unsupported configuration value type.");
}

} // namespace

Json encode_type(const DataType& type)
{
    Json result = {{"kind", write_enum(type.kind())}};
    switch (type.kind()) {
    case TypeKind::Enum:
        result["schema"] = write_id(*type.schema_id());
        result["version"] = type.schema_version();
        result["members"] = write_array(type.enum_members(), encode_member);
        break;
    case TypeKind::Record:
        result["schema"] = write_id(*type.schema_id());
        result["version"] = type.schema_version();
        result["fields"] = write_array(type.fields(), encode_field);
        break;
    case TypeKind::List:
    case TypeKind::Optional:
        result["element"] = encode_type(*type.element_type());
        break;
    default: break;
    }
    return result;
}

DataType decode_type(const Reader& reader)
{
    const auto kind = read_enum<TypeKind>(reader.field("kind"));
    switch (kind) {
    case TypeKind::Enum:
        reader.object({"kind", "schema", "version", "members"});
        return reader.checked(DataType::enumeration(read_id<SchemaId>(reader.field("schema")), reader.field("version").u32(),
            read_array<EnumMember>(reader.field("members"), decode_member)));
    case TypeKind::Record:
        reader.object({"kind", "schema", "version", "fields"});
        return reader.checked(DataType::record(read_id<SchemaId>(reader.field("schema")), reader.field("version").u32(),
            read_array<FieldSchema>(reader.field("fields"), decode_field)));
    case TypeKind::List:
        reader.object({"kind", "element"});
        return DataType::list(decode_type(reader.field("element")));
    case TypeKind::Optional:
        reader.object({"kind", "element"});
        return DataType::optional(decode_type(reader.field("element")));
    default:
        reader.object({"kind"});
        break;
    }

    switch (kind) {
    case TypeKind::Bool: return DataType::boolean();
    case TypeKind::Int64: return DataType::integer();
    case TypeKind::Float64: return DataType::real();
    case TypeKind::String: return DataType::string();
    case TypeKind::Binary: return DataType::binary();
    case TypeKind::Image: return DataType::image();
    case TypeKind::Point2D: return DataType::point2d();
    case TypeKind::Line2D: return DataType::line2d();
    case TypeKind::Circle2D: return DataType::circle2d();
    case TypeKind::Pose2D: return DataType::pose2d();
    case TypeKind::Transform2D: return DataType::transform2d();
    case TypeKind::Region2D: return DataType::region2d();
    case TypeKind::RasterMask: return DataType::raster_mask();
    case TypeKind::ResourceReference: return DataType::resource_reference();
    default: reader.fail(ErrorCode::UnsupportedSolutionContent, "Unsupported configuration schema type.");
    }
}

Json encode_value(const ConfigValue& value)
{
    return {{"type", encode_type(value.type())}, {"value", encode_payload(value)}};
}

ConfigValue decode_value(const Reader& reader)
{
    reader.object({"type", "value"});
    const auto type = decode_type(reader.field("type"));
    return decode_payload(reader.field("value"), type);
}

Json encode_semantics(const ValueSemantics& semantics)
{
    return {{"unit", write_enum(semantics.unit)},
        {"coordinate_system", write_optional(semantics.coordinate_system, write_id<CoordinateSystemId>)}};
}

ValueSemantics decode_semantics(const Reader& reader)
{
    reader.object({"unit", "coordinate_system"});
    return {read_enum<MeasurementUnit>(reader.field("unit")),
        read_optional<CoordinateSystemId>(reader.field("coordinate_system"), read_id<CoordinateSystemId>)};
}

} // namespace visionpro::infrastructure::detail
