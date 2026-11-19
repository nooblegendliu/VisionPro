#include <visionpro/workflow/correlation_join.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace visionpro::workflow {
namespace {
using namespace core;
using Clock = CorrelationJoinService::Clock;

Diagnostic invalid(const CorrelationJoinConfig& config, ErrorCode code, std::string message, const FieldId* field = nullptr)
{
    DiagnosticLocation where;
    where.schema = config.join;
    if (field) { where.path.emplace_back(*field); }
    return {code, Severity::Error, std::move(where), std::move(message)};
}

Result<std::monostate> validate_offer(const CorrelationJoinConfig& config, std::uint64_t batch,
    const FieldId& source, const DataValue& value)
{
    if (!batch || !config.capacity || config.timeout <= std::chrono::milliseconds::zero() || !config.batch_key_limit) {
        return invalid(config, ErrorCode::ConstraintViolation, "Join batch, capacity, timeout and key limit must be positive.");
    }
    if (config.record.kind() != TypeKind::Record || config.record.fields().empty()) {
        return invalid(config, ErrorCode::InvalidSchema, "A correlation join requires a nonempty Record schema.");
    }
    const auto& fields = config.record.fields();
    const auto declared = std::ranges::find(fields, source, &FieldSchema::id);
    if (declared == fields.end()) { return invalid(config, ErrorCode::UnknownField, "Unknown join contribution source.", &source); }
    if (declared->type != value.type()) { return invalid(config, ErrorCode::TypeMismatch, "Contribution type differs from its declared field.", &source); }
    return std::monostate{};
}

struct Item {
    Clock::time_point first;
    CorrelationItemState state{CorrelationItemState::Pending};
    std::vector<DataFieldValue> values;
    std::vector<FieldId> missing_at_timeout;
};

struct Channel {
    CorrelationJoinConfig config;
    CorrelationJoinSnapshot stats;
    std::map<CorrelationId, Item> items;
    // Stable map nodes keep this queue valid through completion and expiry.
    // It is bounded by the number of distinct admitted keys in this batch.
    std::deque<Item*> expiry_order;

    CorrelationJoinSnapshot snapshot(std::uint64_t rejected_joins) const
    {
        auto result = stats;
        result.retained_keys = items.size();
        result.join_capacity_rejections = rejected_joins;
        return result;
    }

    std::vector<FieldId> missing(const Item& item) const
    {
        std::vector<FieldId> fields;
        for (const auto& declared : config.record.fields()) {
            if (std::ranges::find(item.values, declared.id, &DataFieldValue::id) == item.values.end()) {
                fields.push_back(declared.id);
            }
        }
        return fields;
    }

    void expire(Clock::time_point now)
    {
        while (!expiry_order.empty()) {
            auto& item = *expiry_order.front();
            if (item.state == CorrelationItemState::Pending) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - item.first) < config.timeout) { break; }
                item.missing_at_timeout = missing(item);
                item.state = CorrelationItemState::Expired;
                std::vector<DataFieldValue>{}.swap(item.values);
                --stats.pending;
                ++stats.expired;
            }
            expiry_order.pop_front();
        }
    }

    Result<CorrelationJoinResult> accept(const CorrelationId& key, const FieldId& source, const DataValue& value, Clock::time_point now)
    {
        auto found = items.find(key);
        if (found == items.end()) {
            if (items.size() == config.batch_key_limit) {
                ++stats.batch_capacity_rejections;
                return CorrelationJoinResult{CorrelationJoinStatus::Capacity};
            }
            if (stats.pending == config.capacity) {
                ++stats.pending_capacity_rejections;
                return CorrelationJoinResult{CorrelationJoinStatus::Capacity};
            }
            if (config.record.fields().size() == 1) {
                auto record = DataValue::record(config.record, {{source, value}});
                if (const auto* failure = std::get_if<Diagnostic>(&record)) { return *failure; }
                items.emplace(key, Item{now, CorrelationItemState::Complete});
                ++stats.completed;
                return CorrelationJoinResult{CorrelationJoinStatus::Complete, std::get<DataValue>(std::move(record))};
            }
            auto added = items.emplace(key, Item{now, CorrelationItemState::Pending, {{source, value}}}).first;
            try { expiry_order.push_back(&added->second); }
            catch (...) { items.erase(added); throw; }
            ++stats.pending;
            return CorrelationJoinResult{CorrelationJoinStatus::Pending};
        }
        auto& item = found->second;
        if (item.state == CorrelationItemState::Expired) {
            ++stats.late;
            return CorrelationJoinResult{CorrelationJoinStatus::Late};
        }
        if (item.state == CorrelationItemState::Complete ||
            std::ranges::find(item.values, source, &DataFieldValue::id) != item.values.end()) {
            ++stats.duplicates;
            return CorrelationJoinResult{CorrelationJoinStatus::Duplicate};
        }
        if (item.values.size() + 1 < config.record.fields().size()) {
            item.values.push_back({source, value});
            return CorrelationJoinResult{CorrelationJoinStatus::Pending};
        }
        auto fields = item.values;
        fields.push_back({source, value});
        auto record = DataValue::record(config.record, fields);
        if (const auto* failure = std::get_if<Diagnostic>(&record)) { return *failure; }
        item.state = CorrelationItemState::Complete;
        std::vector<DataFieldValue>{}.swap(item.values);
        --stats.pending;
        ++stats.completed;
        return CorrelationJoinResult{CorrelationJoinStatus::Complete, std::get<DataValue>(std::move(record))};
    }
};
} // namespace

struct CorrelationJoinService::Impl {
    Now now;
    std::size_t max_joins;
    std::mutex mutex;
    std::map<SchemaId, std::unique_ptr<Channel>> channels;
    std::uint64_t rejected_joins{};
};

CorrelationJoinService::CorrelationJoinService(Now now, std::size_t max_joins) : impl_(std::make_unique<Impl>())
{
    if (!now) { throw std::invalid_argument("Correlation joins require a monotonic clock."); }
    impl_->now = std::move(now);
    impl_->max_joins = max_joins;
}

CorrelationJoinService::~CorrelationJoinService() = default;

Result<CorrelationJoinResult> CorrelationJoinService::offer(const CorrelationJoinConfig& config, std::uint64_t batch,
    const CorrelationId& workpiece, const FieldId& source, const DataValue& value)
{
    auto valid = validate_offer(config, batch, source, value);
    if (const auto* failure = std::get_if<Diagnostic>(&valid)) { return *failure; }
    const std::lock_guard lock(impl_->mutex);
    auto found = impl_->channels.find(config.join);
    const auto now = impl_->now();
    const auto capture = [&](const Channel& channel, Result<CorrelationJoinResult> result) {
        if (auto* offered = std::get_if<CorrelationJoinResult>(&result)) { offered->stats = channel.snapshot(impl_->rejected_joins); }
        return result;
    };
    CorrelationJoinSnapshot stats;
    if (found != impl_->channels.end()) {
        auto& current = *found->second;
        current.expire(now);
        if (batch < current.stats.batch) {
            ++current.stats.late;
            return capture(current, CorrelationJoinResult{CorrelationJoinStatus::Late});
        }
        if (batch == current.stats.batch) {
            if (config != current.config) { return invalid(config, ErrorCode::InvalidState, "Join configuration changed within an active batch."); }
            return capture(current, current.accept(workpiece, source, value, now));
        }
        stats = current.stats;
        stats.abandoned_at_rollover += stats.pending;
        stats.pending = 0;
    } else if (impl_->channels.size() == impl_->max_joins) {
        stats.batch = batch;
        stats.join_capacity_rejections = ++impl_->rejected_joins;
        return CorrelationJoinResult{CorrelationJoinStatus::Capacity, {}, stats};
    }
    stats.batch = batch;
    auto next = std::make_unique<Channel>(Channel{config, stats});
    auto result = capture(*next, next->accept(workpiece, source, value, now));
    if (std::holds_alternative<Diagnostic>(result)) { return result; }
    impl_->channels.insert_or_assign(config.join, std::move(next));
    return result;
}

std::optional<CorrelationJoinSnapshot> CorrelationJoinService::inspect(const SchemaId& join) const
{
    const std::lock_guard lock(impl_->mutex);
    const auto found = impl_->channels.find(join);
    if (found == impl_->channels.end()) { return {}; }
    auto& channel = *found->second;
    channel.expire(impl_->now());
    return channel.snapshot(impl_->rejected_joins);
}

std::vector<NamedCorrelationJoinSnapshot> CorrelationJoinService::inspect_all() const
{
    const std::lock_guard lock(impl_->mutex);
    const auto now = impl_->now();
    std::vector<NamedCorrelationJoinSnapshot> result;
    result.reserve(impl_->channels.size());
    for (const auto& [join, channel] : impl_->channels) {
        channel->expire(now);
        result.push_back({join, channel->snapshot(impl_->rejected_joins)});
    }
    return result;
}

std::optional<CorrelationItemSnapshot> CorrelationJoinService::inspect_item(const SchemaId& join, std::uint64_t batch,
    const CorrelationId& workpiece) const
{
    const std::lock_guard lock(impl_->mutex);
    const auto found = impl_->channels.find(join);
    if (found == impl_->channels.end() || found->second->stats.batch != batch) { return {}; }
    auto& channel = *found->second;
    channel.expire(impl_->now());
    const auto item = channel.items.find(workpiece);
    if (item == channel.items.end()) { return {}; }
    const auto& value = item->second;
    return CorrelationItemSnapshot{value.state, value.state == CorrelationItemState::Pending
        ? channel.missing(value) : value.missing_at_timeout};
}

std::uint64_t CorrelationJoinService::join_capacity_rejections() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->rejected_joins;
}
} // namespace visionpro::workflow
