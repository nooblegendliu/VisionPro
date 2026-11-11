#pragma once

#include <visionpro/core/runtime_id.hpp>
#include <visionpro/core/result.hpp>

#include <cstdint>
#include <limits>
#include <string_view>

namespace visionpro::core {

class RuntimeIdFactory final {
public:
    template <RuntimeIdentity Id>
    [[nodiscard]] static Result<Id> from_value(std::uint64_t value)
    {
        if (value < minimum_runtime_id<Id>) {
            return Diagnostic{ErrorCode::InvalidRuntimeId, Severity::Error, {},
                "This runtime identity starts at 1; zero is reserved for root scopes and first attempts."};
        }
        return Id{value};
    }

    template <RuntimeIdentity Id>
    [[nodiscard]] static Result<Id> next(Id current)
    {
        if (current.value() == std::numeric_limits<std::uint64_t>::max()) {
            return Diagnostic{ErrorCode::IdentityExhausted, Severity::Error, {},
                "Runtime identity exhausted; counter must not wrap or reuse an identity."};
        }
        return Id{current.value() + 1};
    }

    [[nodiscard]] static Result<CorrelationId> correlation(CorrelationDomainId domain, std::string_view key);
};

// One sequence per identity domain, owned by the caller. Not thread-safe.
// Session/run/call ownership and concurrency are enforced by the S3 runtime.
// No copy, move, or reset may accidentally fork/reuse the sequence state.
template <RuntimeIdentity Id>
class IdSequence final {
public:
    IdSequence() noexcept = default;
    explicit IdSequence(Id next_id) noexcept : next_(next_id.value()) {}
    IdSequence(const IdSequence&) = delete;
    IdSequence& operator=(const IdSequence&) = delete;
    IdSequence(IdSequence&&) = delete;
    IdSequence& operator=(IdSequence&&) = delete;

    [[nodiscard]] Result<Id> next()
    {
        if (exhausted_) {
            return Diagnostic{ErrorCode::IdentityExhausted, Severity::Error, {},
                "Runtime identity sequence exhausted; create a new owning domain before issuing more IDs."};
        }
        auto result = RuntimeIdFactory::from_value<Id>(next_);
        if (next_ == std::numeric_limits<std::uint64_t>::max()) {
            exhausted_ = true;
        } else {
            ++next_;
        }
        return result;
    }

private:
    std::uint64_t next_ = minimum_runtime_id<Id>;
    bool exhausted_ = false;
};

} // namespace visionpro::core
