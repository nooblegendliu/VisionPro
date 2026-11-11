#pragma once

#include <compare>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>

namespace visionpro::core {

class SchemaIdFactory;

namespace detail {

template <class Tag>
class SchemaIdValue final {
public:
    SchemaIdValue(const SchemaIdValue&) = default;
    SchemaIdValue& operator=(const SchemaIdValue&) = default;
    // Preserve the source's valid identity as well as the destination on move.
    SchemaIdValue(SchemaIdValue&& other) : text_(other.text_) {}
    SchemaIdValue& operator=(SchemaIdValue&& other) { text_ = other.text_; return *this; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    auto operator<=>(const SchemaIdValue&) const = default;

private:
    friend class ::visionpro::core::SchemaIdFactory;
    explicit SchemaIdValue(std::string text) : text_(std::move(text)) {}
    std::string text_;
};

struct SchemaIdTag;
struct ToolTypeIdTag;
struct InputIdTag;
struct OutputIdTag;
struct ParameterIdTag;
struct FieldIdTag;
struct ControlOutputIdTag;
struct EnumMemberIdTag;
struct CapabilityIdTag;
struct CorrelationDomainIdTag;
struct CoordinateSystemIdTag;

} // namespace detail

// These case-sensitive IDs belong to their declared Schema/registry domain.
// They are stable keys, not editable display labels or globally unique UUIDs.
using SchemaId = detail::SchemaIdValue<detail::SchemaIdTag>;
using ToolTypeId = detail::SchemaIdValue<detail::ToolTypeIdTag>;
using InputId = detail::SchemaIdValue<detail::InputIdTag>;
using OutputId = detail::SchemaIdValue<detail::OutputIdTag>;
using ParameterId = detail::SchemaIdValue<detail::ParameterIdTag>;
using FieldId = detail::SchemaIdValue<detail::FieldIdTag>;
using ControlOutputId = detail::SchemaIdValue<detail::ControlOutputIdTag>;
using EnumMemberId = detail::SchemaIdValue<detail::EnumMemberIdTag>;
using CapabilityId = detail::SchemaIdValue<detail::CapabilityIdTag>;
using CorrelationDomainId = detail::SchemaIdValue<detail::CorrelationDomainIdTag>;
using CoordinateSystemId = detail::SchemaIdValue<detail::CoordinateSystemIdTag>;

template <class T>
concept SchemaIdentity = std::same_as<T, SchemaId> || std::same_as<T, ToolTypeId>
    || std::same_as<T, InputId> || std::same_as<T, OutputId>
    || std::same_as<T, ParameterId> || std::same_as<T, FieldId>
    || std::same_as<T, ControlOutputId> || std::same_as<T, EnumMemberId>
    || std::same_as<T, CapabilityId> || std::same_as<T, CorrelationDomainId>
    || std::same_as<T, CoordinateSystemId>;

} // namespace visionpro::core
