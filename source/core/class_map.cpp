#include "class_map_validation.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <limits>

namespace visionpro::core {
namespace {
template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

struct ClassMapFields {
    FieldId pixels = key<FieldId>("pixels");
    FieldId classes = key<FieldId>("classes");
    FieldId id = key<FieldId>("id");
    FieldId name = key<FieldId>("name");
    FieldId count = key<FieldId>("count");
};

const ClassMapFields& fields()
{
    static const ClassMapFields value;
    return value;
}

const DataType& entry_type()
{
    static const DataType type = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.image.class_map.entry"), 1,
        {{fields().id, DataType::integer()}, {fields().name, DataType::string()}, {fields().count, DataType::integer()}}));
    return type;
}

Diagnostic invalid(ErrorCode code, std::string message)
{
    DiagnosticLocation location;
    location.schema = class_map_type().schema_id();
    return {code, Severity::Error, std::move(location), std::move(message)};
}
} // namespace

const DataType& class_map_type()
{
    static const DataType type = std::get<DataType>(DataType::record(key<SchemaId>(class_map_schema), 1,
        {{fields().pixels, DataType::image()}, {fields().classes, DataType::list(entry_type())}}));
    return type;
}

Result<std::monostate> detail::validate_class_map_record(const DataValue& value)
{
    if (value.type() != class_map_type()) {
        return invalid(ErrorCode::TypeMismatch, "Class map fields must match the version 1 raster and class table schema.");
    }
    const auto& pixels = value.field(fields().pixels)->as_image()->image;
    if (pixels.format() != PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) {
        return invalid(ErrorCode::UnsupportedPixelFormat, "Class map pixels must be single-channel UInt8 class IDs.");
    }
    const auto& entries = *value.field(fields().classes)->elements();
    if (entries.empty() || entries.size() > 256) {
        return invalid(ErrorCode::ConstraintViolation, "A UInt8 class map requires between 1 and 256 class entries.");
    }
    std::array<bool, 256> declared{};
    std::array<std::int64_t, 256> counts{};
    for (const auto& entry : entries) {
        const auto id = *entry.field(fields().id)->as_int64();
        if (id < 0 || id > 255 || declared[static_cast<std::size_t>(id)]) {
            return invalid(ErrorCode::ConstraintViolation, "Class table IDs must be unique UInt8 values.");
        }
        if (entry.field(fields().name)->as_string()->empty() || *entry.field(fields().count)->as_int64() < 0) {
            return invalid(ErrorCode::ConstraintViolation, "A class table entry requires a nonempty name and nonnegative pixel count.");
        }
        declared[static_cast<std::size_t>(id)] = true;
    }
    if (pixels.height() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / pixels.width()) {
        return invalid(ErrorCode::SizeOverflow, "Pixel counts exceed the class table's Int64 representation.");
    }
    for (std::size_t y = 0; y < pixels.height(); ++y) {
        const auto row = std::get<std::span<const std::byte>>(pixels.row(y));
        for (auto byte : row) {
            const auto id = std::to_integer<std::uint8_t>(byte);
            if (!declared[id]) { return invalid(ErrorCode::MissingField, "Class table is missing pixel ID " + std::to_string(id) + "."); }
            ++counts[id];
        }
    }
    for (const auto& entry : entries) {
        const auto id = *entry.field(fields().id)->as_int64();
        if (*entry.field(fields().count)->as_int64() != counts[static_cast<std::size_t>(id)]) {
            return invalid(ErrorCode::ConstraintViolation, "Class table count differs from the raster for ID " + std::to_string(id) + ".");
        }
    }
    return std::monostate{};
}

Result<DataValue> make_class_map(const ClassMap& map)
{
    std::vector<DataValue> entries;
    for (const auto& entry : map.classes) {
        auto name = DataValue::string(entry.name);
        if (const auto* failure = std::get_if<Diagnostic>(&name)) { return *failure; }
        auto item = DataValue::record(entry_type(), {{fields().id, DataValue::integer(entry.id)},
            {fields().name, std::get<DataValue>(std::move(name))}, {fields().count, DataValue::integer(entry.count)}});
        if (const auto* failure = std::get_if<Diagnostic>(&item)) { return *failure; }
        entries.push_back(std::get<DataValue>(std::move(item)));
    }
    auto image = DataValue::image(map.pixels);
    if (const auto* failure = std::get_if<Diagnostic>(&image)) { return *failure; }
    return DataValue::record(class_map_type(), {{fields().pixels, std::get<DataValue>(std::move(image))},
        {fields().classes, std::get<DataValue>(DataValue::list(entry_type(), entries))}});
}

Result<ClassMap> read_class_map(const DataValue& value)
{
    if (value.type() != class_map_type()) { return invalid(ErrorCode::TypeMismatch, "Expected a version 1 class map record."); }
    ClassMap map{*value.field(fields().pixels)->as_image(), {}};
    for (const auto& item : *value.field(fields().classes)->elements()) {
        map.classes.push_back({static_cast<std::uint8_t>(*item.field(fields().id)->as_int64()),
            *item.field(fields().name)->as_string(), *item.field(fields().count)->as_int64()});
    }
    return map;
}
} // namespace visionpro::core
