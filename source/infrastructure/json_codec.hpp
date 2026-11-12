#pragma once

#include <visionpro/core/coordinate.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/resource_reference.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <initializer_list>
#include <string_view>

namespace visionpro::infrastructure::detail {

using Json = nlohmann::json;

struct CodecError { core::Diagnostic diagnostic; };

[[nodiscard]] Json parse_manifest(std::string_view text);
[[nodiscard]] std::string serialize_manifest(const Json& value);

// Carries a JSON Pointer through typed decoding. Unknown object members are
// reported separately from a value that cannot be interpreted at all.
class Reader final {
public:
    Reader(const Json& value, std::vector<core::Diagnostic>& unknown) : value_(value), unknown_(unknown) {}
    void object(std::initializer_list<std::string_view> keys) const;
    [[nodiscard]] Reader field(std::string_view key) const;
    [[nodiscard]] Reader at(std::size_t index) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::string_view text() const;
    [[nodiscard]] bool boolean() const;
    [[nodiscard]] std::uint32_t u32() const;
    [[nodiscard]] std::uint64_t u64() const;
    [[nodiscard]] std::int64_t i64() const;
    [[nodiscard]] std::size_t size_value() const;
    [[nodiscard]] double number() const;
    [[nodiscard]] bool is_null() const noexcept { return value_.is_null(); }
    [[noreturn]] void fail(core::ErrorCode code, std::string message) const;

    template<class T> [[nodiscard]] T checked(core::Result<T> value) const
    {
        if (auto* diagnostic = std::get_if<core::Diagnostic>(&value)) {
            diagnostic->message = describe(std::move(diagnostic->message));
            throw CodecError{std::move(*diagnostic)};
        }
        return std::get<T>(std::move(value));
    }

private:
    Reader(const Json& value, std::vector<core::Diagnostic>& unknown, std::string pointer)
        : value_(value), unknown_(unknown), pointer_(std::move(pointer)) {}
    [[nodiscard]] std::string child(std::string_view key) const;
    [[nodiscard]] std::string describe(std::string message) const;

    const Json& value_;
    std::vector<core::Diagnostic>& unknown_;
    std::string pointer_;
};

template<class E> struct EnumNames;
template<> struct EnumNames<core::ResourceKind> {
    static constexpr auto values = std::to_array<std::string_view>({
        "image", "mask", "template", "calibration", "device", "directory_sequence", "output_path", "binary"});
};
template<> struct EnumNames<core::MeasurementUnit> {
    static constexpr auto values = std::to_array<std::string_view>({
        "unspecified", "dimensionless", "pixel", "millimetre", "second", "radian", "degree"});
};

template<class E> [[nodiscard]] E read_enum(const Reader& reader)
{
    const auto& names = EnumNames<E>::values;
    const auto token = reader.text();
    const auto found = std::ranges::find(names, token);
    if (found == names.end()) {
        reader.fail(core::ErrorCode::UnsupportedSolutionContent, "Unrecognized value " + Json(token).dump() + ".");
    }
    return static_cast<E>(found - names.begin());
}

template<class E> [[nodiscard]] Json write_enum(E value)
{
    const auto& names = EnumNames<E>::values;
    const auto index = static_cast<std::size_t>(value);
    if (index >= names.size()) {
        throw CodecError{{core::ErrorCode::UnsupportedSolutionContent, core::Severity::Error, {},
            "The model contains an unrecognized value."}};
    }
    return Json(names[index]);
}

template<class T> [[nodiscard]] T read_id(const Reader& reader)
{
    if constexpr (core::UuidIdentity<T>) { return reader.checked(core::IdFactory::parse<T>(reader.text())); }
    else { return reader.checked(core::SchemaIdFactory::parse<T>(reader.text())); }
}

template<class T> [[nodiscard]] Json write_id(const T& id)
{
    if constexpr (core::UuidIdentity<T>) { return Json(id.to_string()); }
    else { return Json(id.text()); }
}

template<class T, class Decode> [[nodiscard]] std::vector<T> read_array(const Reader& reader, Decode decode)
{
    std::vector<T> values;
    const auto size = reader.size();
    values.reserve(size);
    for (std::size_t index = 0; index < size; ++index) { values.emplace_back(std::invoke(decode, reader.at(index))); }
    return values;
}

template<class Range, class Encode> [[nodiscard]] Json write_array(const Range& values, Encode encode)
{
    auto result = Json::array();
    for (const auto& value : values) { result.push_back(std::invoke(encode, value)); }
    return result;
}

template<class T, class Decode> [[nodiscard]] std::optional<T> read_optional(const Reader& reader, Decode decode)
{
    if (reader.is_null()) { return std::nullopt; }
    return std::optional<T>{std::in_place, std::invoke(decode, reader)};
}

template<class T, class Encode> [[nodiscard]] Json write_optional(const std::optional<T>& value, Encode encode)
{
    return value ? Json(std::invoke(encode, *value)) : Json(nullptr);
}

} // namespace visionpro::infrastructure::detail
