#pragma once

#include <visionpro/core/id.hpp>
#include <visionpro/core/schema_id.hpp>

#include <compare>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace visionpro::core {

class RuntimeIdFactory;

namespace detail {

template <class Tag>
class RuntimeIdValue final {
public:
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    auto operator<=>(const RuntimeIdValue&) const = default;

private:
    friend class ::visionpro::core::RuntimeIdFactory;
    explicit RuntimeIdValue(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_;
};

struct ConfigSnapshotIdTag;
struct WorkflowRunIdTag;
struct ExecutionScopeIdTag;
struct InvocationIdTag;
struct AttemptIdTag;

} // namespace detail

using ConfigSnapshotId = detail::RuntimeIdValue<detail::ConfigSnapshotIdTag>;
using WorkflowRunId = detail::RuntimeIdValue<detail::WorkflowRunIdTag>;
using ExecutionScopeId = detail::RuntimeIdValue<detail::ExecutionScopeIdTag>;
using InvocationId = detail::RuntimeIdValue<detail::InvocationIdTag>;
using AttemptId = detail::RuntimeIdValue<detail::AttemptIdTag>;

template <class T>
concept RuntimeIdentity = std::same_as<T, ConfigSnapshotId> || std::same_as<T, WorkflowRunId>
    || std::same_as<T, ExecutionScopeId> || std::same_as<T, InvocationId> || std::same_as<T, AttemptId>;

template <RuntimeIdentity Id>
inline constexpr std::uint64_t minimum_runtime_id =
    std::same_as<Id, ExecutionScopeId> || std::same_as<Id, AttemptId> ? 0 : 1;

// InvocationKey is addressed within one solution session; a retry retains it.
struct InvocationKey {
    WorkflowRunId workflow_run;
    NodeId node;
    ExecutionScopeId scope;
    InvocationId invocation;
    auto operator<=>(const InvocationKey&) const = default;
};

struct SessionInvocationKey {
    SolutionSessionId session;
    InvocationKey key;
    auto operator<=>(const SessionInvocationKey&) const = default;
};

// Frame sequences are unique within their source's declared lifetime.
// A raw device frame counter is insufficient without its source identity.
struct FrameId {
    FrameId(ResourceId source_id, std::uint64_t sequence_number) noexcept
        : source(source_id), sequence(sequence_number) {}
    ResourceId source;
    std::uint64_t sequence;
    auto operator<=>(const FrameId&) const = default;
};

class CorrelationId final {
public:
    CorrelationId(const CorrelationId&) = default;
    CorrelationId& operator=(const CorrelationId&) = default;
    // Copy on move keeps a previously validated source usable as an identity.
    CorrelationId(CorrelationId&& other) : domain_(other.domain_), key_(other.key_) {}
    CorrelationId& operator=(CorrelationId&& other)
    {
        domain_ = other.domain_;
        key_ = other.key_;
        return *this;
    }
    [[nodiscard]] const CorrelationDomainId& domain() const noexcept { return domain_; }
    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    auto operator<=>(const CorrelationId&) const = default;

private:
    friend class RuntimeIdFactory;
    CorrelationId(CorrelationDomainId domain, std::string key)
        : domain_(std::move(domain)), key_(std::move(key)) {}

    CorrelationDomainId domain_;
    std::string key_;
};

} // namespace visionpro::core
