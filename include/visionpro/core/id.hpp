#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstdint>
#include <string>

namespace visionpro::core {

class IdFactory;

namespace detail {

using UuidBytes = std::array<std::uint8_t, 16>;

// Bytes follow UUID text order, independent of native GUID layout/endianness.
[[nodiscard]] std::string format_uuid(const UuidBytes& bytes);

template <class Tag>
class IdValue final {
public:
    [[nodiscard]] std::string to_string() const { return format_uuid(bytes_); }
    auto operator<=>(const IdValue&) const = default;

private:
    friend class ::visionpro::core::IdFactory;
    explicit IdValue(UuidBytes bytes) noexcept : bytes_(bytes) {}

    UuidBytes bytes_;
};

struct SolutionIdTag;
struct WorkflowIdTag;
struct NodeIdTag;
struct ResourceIdTag;
struct VariableIdTag;
struct QueueIdTag;
struct ControlLinkIdTag;
struct RegionIdTag;
struct SolutionSessionIdTag;
struct MessageIdTag;

} // namespace detail

// Only validated parsing or generation may create an ID. Absence uses optional.
using SolutionId = detail::IdValue<detail::SolutionIdTag>;
using WorkflowId = detail::IdValue<detail::WorkflowIdTag>;
using NodeId = detail::IdValue<detail::NodeIdTag>;
using ResourceId = detail::IdValue<detail::ResourceIdTag>;
using VariableId = detail::IdValue<detail::VariableIdTag>;
using QueueId = detail::IdValue<detail::QueueIdTag>;
using ControlLinkId = detail::IdValue<detail::ControlLinkIdTag>;
using RegionId = detail::IdValue<detail::RegionIdTag>;
using SolutionSessionId = detail::IdValue<detail::SolutionSessionIdTag>;
using MessageId = detail::IdValue<detail::MessageIdTag>;

template <class T>
concept PersistentId = std::same_as<T, SolutionId> || std::same_as<T, WorkflowId>
    || std::same_as<T, NodeId> || std::same_as<T, ResourceId>
    || std::same_as<T, VariableId> || std::same_as<T, QueueId>
    || std::same_as<T, ControlLinkId> || std::same_as<T, RegionId>;

template <class T>
concept UuidIdentity = PersistentId<T> || std::same_as<T, SolutionSessionId> || std::same_as<T, MessageId>;

} // namespace visionpro::core
