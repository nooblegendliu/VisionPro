#pragma once

#include <visionpro/core/correlation_join.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace visionpro::workflow {

// A session owns one service. Each join has a monotonic batch watermark and
// retains its current batch's keys, including tombstones, until rollover.
class CorrelationJoinService final : public core::CorrelationJoinAccess {
public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    explicit CorrelationJoinService(Now now = Clock::now, std::size_t max_joins = 64);
    ~CorrelationJoinService() override;
    [[nodiscard]] core::Result<core::CorrelationJoinResult> offer(const core::CorrelationJoinConfig& config,
        std::uint64_t batch, const core::CorrelationId& workpiece, const core::FieldId& source, const core::DataValue& value) override;

    // Reads report expiry as of now, without consuming records or refreshing timeouts.
    [[nodiscard]] std::optional<core::CorrelationJoinSnapshot> inspect(const core::SchemaId& join) const override;
    [[nodiscard]] std::vector<core::NamedCorrelationJoinSnapshot> inspect_all() const override;
    [[nodiscard]] std::optional<core::CorrelationItemSnapshot> inspect_item(const core::SchemaId& join,
        std::uint64_t batch, const core::CorrelationId& workpiece) const override;
    [[nodiscard]] std::uint64_t join_capacity_rejections() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
