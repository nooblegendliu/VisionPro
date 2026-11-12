#include "json_codec.hpp"

#include <visionpro/core/text.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <set>

namespace visionpro::infrastructure::detail {
namespace {
using namespace core;
constexpr int manifest_depth = 128;

template<class T> T decimal(const Reader& reader)
{
    const auto token = reader.text();
    T value{};
    const auto end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        reader.fail(ErrorCode::InvalidSolutionManifest, "Expected a decimal integer string in range.");
    }
    return value;
}

[[noreturn]] void invalid_manifest(std::string message)
{
    throw CodecError{{ErrorCode::InvalidSolutionManifest, Severity::Error, {}, std::move(message)}};
}

void check_tree(const Json& value, int depth)
{
    if (depth > manifest_depth) { invalid_manifest("Solution manifest nesting exceeds 128 levels."); }
    if (value.is_number_float() && !std::isfinite(value.get<double>())) {
        invalid_manifest("A manifest cannot encode a nonfinite number.");
    }
    if (value.is_structured()) {
        for (const auto& child : value) { check_tree(child, depth + 1); }
    }
}
} // namespace

Json parse_manifest(std::string_view text)
{
    if (text.find('\0') != std::string_view::npos || !is_valid_utf8(text)) {
        invalid_manifest("Expected a UTF-8 manifest with escaped control characters.");
    }
    std::vector<std::set<std::string>> object_keys;
    const auto validate = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > manifest_depth) { invalid_manifest("Solution manifest nesting exceeds 128 levels."); }
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key) {
            const auto& key = value.get_ref<const std::string&>();
            if (!object_keys.back().insert(key).second) {
                invalid_manifest("Solution manifest contains a duplicate object key " + value.dump() + ".");
            }
        } else if (event == Json::parse_event_t::object_end) {
            object_keys.pop_back();
        }
        return true;
    };
    return Json::parse(text.begin(), text.end(), validate);
}

std::string serialize_manifest(const Json& value)
{
    check_tree(value, 0);
    return value.dump(2) + '\n';
}

std::string Reader::child(std::string_view key) const
{
    std::string path = pointer_ + '/';
    for (const char character : key) {
        if (character == '~') { path += "~0"; }
        else if (character == '/') { path += "~1"; }
        else { path += character; }
    }
    return path;
}

std::string Reader::describe(std::string message) const
{
    return (pointer_.empty() ? std::string("/") : pointer_) + ": " + std::move(message);
}

void Reader::object(std::initializer_list<std::string_view> keys) const
{
    if (!value_.is_object()) { fail(ErrorCode::InvalidSolutionManifest, "Expected an object."); }
    for (auto current = value_.begin(); current != value_.end(); ++current) {
        if (std::ranges::find(keys, current.key()) == keys.end()) {
            unknown_.push_back({ErrorCode::UnsupportedSolutionContent, Severity::Warning, {},
                child(current.key()) + ": Unknown manifest member."});
        }
    }
}

Reader Reader::field(std::string_view key) const
{
    if (!value_.is_object()) { fail(ErrorCode::InvalidSolutionManifest, "Expected an object."); }
    const auto found = value_.find(key);
    if (found == value_.end()) {
        invalid_manifest(child(key) + ": Required manifest member is missing.");
    }
    return Reader{*found, unknown_, child(key)};
}

Reader Reader::at(std::size_t index) const
{
    if (index >= size()) { fail(ErrorCode::InvalidSolutionManifest, "Array index is out of range."); }
    return Reader{value_[index], unknown_, child(std::to_string(index))};
}

std::size_t Reader::size() const
{
    if (!value_.is_array()) { fail(ErrorCode::InvalidSolutionManifest, "Expected an array."); }
    return value_.size();
}

std::string_view Reader::text() const
{
    if (!value_.is_string()) { fail(ErrorCode::InvalidSolutionManifest, "Expected a string."); }
    return value_.get_ref<const std::string&>();
}

bool Reader::boolean() const
{
    if (!value_.is_boolean()) { fail(ErrorCode::InvalidSolutionManifest, "Expected a boolean."); }
    return value_.get<bool>();
}

std::uint32_t Reader::u32() const
{
    const auto unsigned_value = value_.is_number_unsigned();
    if (!value_.is_number_integer() || (!unsigned_value && value_.get<std::int64_t>() < 0)
        || value_.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) {
        fail(ErrorCode::InvalidSolutionManifest, "Expected a JSON integer in the UInt32 range.");
    }
    return value_.get<std::uint32_t>();
}

std::uint64_t Reader::u64() const { return decimal<std::uint64_t>(*this); }
std::int64_t Reader::i64() const { return decimal<std::int64_t>(*this); }
std::size_t Reader::size_value() const { return decimal<std::size_t>(*this); }

double Reader::number() const
{
    if (!value_.is_number()) { fail(ErrorCode::InvalidSolutionManifest, "Expected a finite JSON number."); }
    const double converted = value_.get<double>();
    if (!std::isfinite(converted)) { fail(ErrorCode::InvalidSolutionManifest, "Expected a finite JSON number."); }
    return converted;
}

void Reader::fail(ErrorCode code, std::string message) const
{
    throw CodecError{{code, Severity::Error, {}, describe(std::move(message))}};
}

} // namespace visionpro::infrastructure::detail
