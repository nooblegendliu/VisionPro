#pragma once

#include <visionpro/core/runtime_id.hpp>
#include <visionpro/core/value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace visionpro::core {

// Each field of the nonempty Record is one required source. Its FieldId and
// declared DataType identify the contribution independently of its arrival order.
struct CorrelationJoinConfig {
    SchemaId join;
    DataType record;
    std::size_t capacity{256}; // Maximum simultaneously incomplete workpieces.
    std::chrono::milliseconds timeout{30000}; // From first accepted contribution.
    std::size_t batch_key_limit{65536}; // Includes incomplete, delivered and expired keys.
    bool operator==(const CorrelationJoinConfig&) const = default;
};

enum class CorrelationJoinStatus { Pending, Complete, Duplicate, Late, Capacity };
struct CorrelationJoinSnapshot {
    std::uint64_t batch{};
    std::size_t pending{};
    std::size_t retained_keys{};
    // Totals cover all batches of the named join within this session.
    std::uint64_t completed{};
    std::uint64_t duplicates{};
    std::uint64_t late{};
    std::uint64_t expired{};
    std::uint64_t abandoned_at_rollover{};
    std::uint64_t pending_capacity_rejections{};
    std::uint64_t batch_capacity_rejections{};
    std::uint64_t join_capacity_rejections{}; // Session-wide rejected new joins.
};

struct NamedCorrelationJoinSnapshot { SchemaId join; CorrelationJoinSnapshot stats; };
enum class CorrelationItemState { Pending, Complete, Expired };
struct CorrelationItemSnapshot {
    CorrelationItemState state;
    std::vector<FieldId> missing;
};

struct CorrelationJoinResult {
    CorrelationJoinStatus status;
    std::optional<DataValue> record{}; // Present exactly once per completed workpiece.
    CorrelationJoinSnapshot stats{}; // Captured atomically with this offer.
};

class CorrelationJoinAccess {
public:
    virtual ~CorrelationJoinAccess() = default;
    // Positive batches increase per join. A newer batch discards the previous
    // batch's incomplete work and closes it against all later contributions.
    [[nodiscard]] virtual Result<CorrelationJoinResult> offer(const CorrelationJoinConfig& config,
        std::uint64_t batch, const CorrelationId& workpiece, const FieldId& source, const DataValue& value) = 0;
    [[nodiscard]] virtual std::optional<CorrelationJoinSnapshot> inspect(const SchemaId& join) const = 0;
    [[nodiscard]] virtual std::vector<NamedCorrelationJoinSnapshot> inspect_all() const = 0;
    [[nodiscard]] virtual std::optional<CorrelationItemSnapshot> inspect_item(const SchemaId& join,
        std::uint64_t batch, const CorrelationId& workpiece) const = 0;
    [[nodiscard]] virtual std::uint64_t join_capacity_rejections() const = 0;
};

} // namespace visionpro::core
