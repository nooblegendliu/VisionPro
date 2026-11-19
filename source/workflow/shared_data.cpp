#include <visionpro/workflow/shared_data.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include "runtime_values.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace visionpro::workflow {
namespace {
using namespace core;
using Consumer = std::variant<NodeId, SchemaId>;
struct Gate { std::mutex mutex; std::condition_variable_any changed; bool waits_stopped{}; };
struct VariableCell { std::shared_ptr<const GlobalVariableEntry> current; };
struct QueueCell {
    std::deque<QueueMessage> messages;
    std::shared_ptr<const Consumer> owner;
    std::uint64_t enqueued{}, received{}, dropped{}, rejected{};
};
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
Diagnostic located(Diagnostic failure, VariableId id)
{ failure.location.variable = id; return failure; }
Diagnostic located(Diagnostic failure, QueueId id)
{ failure.location.queue = id; return failure; }
bool compatible(const DataQueueDefinition& a, const DataQueueDefinition& b)
{
    return a.message_schema == b.message_schema && a.capacity == b.capacity && a.overflow == b.overflow
        && a.receive.empty == b.receive.empty && a.receive.maximum_batch_size == b.receive.maximum_batch_size
        && a.receive.consumers == b.receive.consumers && a.receive.batch == b.receive.batch;
}
Consumer consumer_key(const UpdateOrigin& origin)
{
    if (const auto* call = std::get_if<SessionInvocationKey>(&origin)) { return call->key.node; }
    return std::get<ExternalUpdateOrigin>(origin).source;
}
template<class T, class Id> const T* definition(const std::vector<T>& values, Id id)
{
    const auto it = std::ranges::find(values, id, &T::id);
    return it == values.end() ? nullptr : &*it;
}

struct RequestedQueue {
    const DataQueueDefinition& definition;
    QueueCell& cell;
    std::size_t count;
};
struct Readiness {
    // For insufficient combined inputs, Fail takes precedence over ReturnEmpty,
    // then Wait. Ownership mismatches are independent of message availability.
    enum State { Ready, Wait, Empty, Fail, WrongConsumer } state{Ready};
    std::optional<QueueId> queue;
};
Readiness readiness(const std::vector<RequestedQueue>& reads, const Consumer& consumer)
{
    Readiness result;
    for (const auto& read : reads) {
        const auto& [def, cell, count] = read;
        if (cell.owner && *cell.owner != consumer) { return {Readiness::WrongConsumer, def.id}; }
        const auto minimum = def.receive.batch == QueueBatchPolicy::RequireExact ? count : 1;
        if (cell.messages.size() >= minimum) { continue; }
        const auto state = def.receive.empty == EmptyQueuePolicy::Fail ? Readiness::Fail
            : def.receive.empty == EmptyQueuePolicy::ReturnEmpty ? Readiness::Empty : Readiness::Wait;
        if (state > result.state) { result = {state, def.id}; }
    }
    return result;
}
} // namespace

struct SharedDataService::Impl {
    ConfigSnapshot snapshot;
    std::shared_ptr<Gate> gate;
    std::map<VariableId, std::shared_ptr<VariableCell>> variables;
    std::map<QueueId, std::shared_ptr<QueueCell>> queues;
    bool owns(const UpdateOrigin& origin) const
    { return std::visit([&](const auto& value) { return value.session == snapshot.identity().session; }, origin); }
};

SharedDataService::SharedDataService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SharedDataService::~SharedDataService() = default;
void SharedDataService::stop_waits() noexcept
{
    auto& gate = *impl_->gate;
    { const std::lock_guard lock(gate.mutex); gate.waits_stopped = true; }
    gate.changed.notify_all();
}
Result<std::shared_ptr<SharedDataService>> SharedDataService::create(const ConfigSnapshot& snapshot,
    std::shared_ptr<const SharedDataService> previous)
{
    const auto& solution = snapshot.solution();
    if (previous && (previous->impl_->snapshot.identity().session != snapshot.identity().session
        || previous->impl_->snapshot.solution().id != solution.id)) {
        auto failure = error(ErrorCode::ResultIdentityMismatch, "Shared-data publications must belong to the same solution and session.");
        failure.location.solution = solution.id;
        return failure;
    }
    auto impl = std::make_unique<Impl>(Impl{snapshot, previous ? previous->impl_->gate : std::make_shared<Gate>()});
    static const auto initial = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>("visionpro.shared.initial"));
    for (const auto& def : solution.variables) {
        const auto* old = previous ? previous->variable_definition(def.id) : nullptr;
        if (old && old->type == def.type && old->initial_value == def.initial_value) {
            impl->variables.emplace(def.id, previous->impl_->variables.at(def.id));
            continue;
        }
        auto value = detail::materialize_config_value(def.initial_value);
        if (const auto* bad = std::get_if<Diagnostic>(&value)) { return located(*bad, def.id); }
        auto& data = std::get<DataValue>(value);
        auto checked = detail::validate_runtime_domain(data, solution, {});
        if (const auto* bad = std::get_if<Diagnostic>(&checked)) { return located(*bad, def.id); }
        auto entry = std::make_shared<GlobalVariableEntry>(GlobalVariableEntry{def.id, data, 0,
            ExternalUpdateOrigin{snapshot.identity().session, initial}});
        impl->variables.emplace(def.id, std::make_shared<VariableCell>(VariableCell{std::move(entry)}));
    }
    for (const auto& def : solution.queues) {
        const auto* old = previous ? previous->queue_definition(def.id) : nullptr;
        impl->queues.emplace(def.id, old && compatible(*old, def) ? previous->impl_->queues.at(def.id) : std::make_shared<QueueCell>());
    }
    return std::shared_ptr<SharedDataService>{new SharedDataService(std::move(impl))};
}

const GlobalVariableDefinition* SharedDataService::variable_definition(VariableId id) const noexcept
{ return definition(impl_->snapshot.solution().variables, id); }
const DataQueueDefinition* SharedDataService::queue_definition(QueueId id) const noexcept
{ return definition(impl_->snapshot.solution().queues, id); }

Result<std::vector<GlobalVariableEntry>> SharedDataService::read_many(std::span<const VariableId> ids) const
{
    std::vector<GlobalVariableEntry> result;
    std::set<VariableId> seen;
    result.reserve(ids.size());
    const std::lock_guard lock(impl_->gate->mutex);
    for (const auto id : ids) {
        const auto it = impl_->variables.find(id);
        if (it == impl_->variables.end() || !seen.insert(id).second) {
            return located(error(ErrorCode::InvalidGlobalVariable, "Global snapshot requires unique, declared variable IDs."), id);
        }
        const auto& entry = *it->second->current;
        auto checked = detail::validate_runtime_domain(entry.current_value, impl_->snapshot.solution(), {});
        if (const auto* bad = std::get_if<Diagnostic>(&checked)) { return located(*bad, id); }
        result.push_back(entry);
    }
    return result;
}
Result<std::vector<GlobalVariableEntry>> SharedDataService::read_all() const
{
    std::vector<VariableId> ids;
    for (const auto& definition : impl_->snapshot.solution().variables) { ids.push_back(definition.id); }
    return read_many(ids);
}
Result<GlobalVariableEntry> SharedDataService::read(VariableId id) const
{
    auto entries = read_many(std::span{&id, 1});
    if (const auto* bad = std::get_if<Diagnostic>(&entries)) { return *bad; }
    return std::get<std::vector<GlobalVariableEntry>>(entries).front();
}

Result<std::vector<GlobalVariableEntry>> SharedDataService::write_many(std::span<const GlobalWrite> updates, const UpdateOrigin& origin)
{
    if (!impl_->owns(origin)) {
        auto failure = error(ErrorCode::ResultIdentityMismatch, "The global writer belongs to another session.");
        if (!updates.empty()) { failure.location.variable = updates.front().variable; }
        return failure;
    }
    std::set<VariableId> seen;
    std::vector<GlobalVariableEntry> result;
    std::vector<std::pair<VariableCell*, std::shared_ptr<const GlobalVariableEntry>>> staged;
    const std::lock_guard lock(impl_->gate->mutex);
    for (const auto& [id, value] : updates) {
        const auto* def = variable_definition(id);
        if (!def || !seen.insert(id).second) {
            return located(error(ErrorCode::InvalidGlobalVariable, "Global writes require unique, declared variable IDs."), id);
        }
        if (value.type() != def->type) { return located(error(ErrorCode::TypeMismatch, "Global write requires the declared value type."), id); }
        auto checked = detail::validate_runtime_domain(value, impl_->snapshot.solution(), {});
        if (const auto* bad = std::get_if<Diagnostic>(&checked)) { return located(*bad, id); }
        auto& cell = *impl_->variables.at(id);
        if (cell.current->revision == std::numeric_limits<std::uint64_t>::max()) {
            return located(error(ErrorCode::IdentityExhausted, "Global revision cannot wrap."), id);
        }
        result.push_back({id, value, cell.current->revision + 1, origin});
        staged.emplace_back(&cell, std::make_shared<const GlobalVariableEntry>(result.back()));
    }
    for (auto& [cell, entry] : staged) { cell->current = std::move(entry); }
    return result;
}
Result<GlobalVariableEntry> SharedDataService::write(VariableId id, const DataValue& value, const UpdateOrigin& origin)
{
    const GlobalWrite update{id, value};
    auto entries = write_many(std::span{&update, 1}, origin);
    if (const auto* bad = std::get_if<Diagnostic>(&entries)) { return *bad; }
    return std::get<std::vector<GlobalVariableEntry>>(entries).front();
}

Result<QueueMessage> SharedDataService::send(QueueId id, const DataValue& payload, const CorrelationId& correlation,
    const UpdateOrigin& producer, std::stop_token stop)
{
    const auto* def = queue_definition(id);
    if (!def) { return located(error(ErrorCode::InvalidQueue, "Unknown queue."), id); }
    if (!impl_->owns(producer)) { return located(error(ErrorCode::ResultIdentityMismatch, "The sender belongs to another session."), id); }
    if (payload.type() != def->message_schema) { return located(error(ErrorCode::TypeMismatch, "Queue payload requires the exact declared record."), id); }
    auto valid = detail::validate_runtime_domain(payload, impl_->snapshot.solution(), {});
    if (const auto* bad = std::get_if<Diagnostic>(&valid)) { return located(*bad, id); }
    auto message_id = IdFactory::generate<MessageId>();
    if (const auto* bad = std::get_if<Diagnostic>(&message_id)) { return located(*bad, id); }
    auto& cell = *impl_->queues.at(id);
    auto& gate = *impl_->gate;
    std::unique_lock lock(gate.mutex);
    if (def->overflow == QueueOverflowPolicy::Wait && cell.messages.size() == def->capacity) {
        gate.changed.wait(lock, stop, [&] { return gate.waits_stopped || cell.messages.size() < def->capacity; });
        if (gate.waits_stopped) { return located(error(ErrorCode::SourceCancelled, "The session closed while waiting to send."), id); }
    }
    if (stop.stop_requested()) { return located(error(ErrorCode::SourceCancelled, "Queue send was cancelled before enqueue."), id); }
    const bool full = cell.messages.size() == def->capacity;
    if (full && def->overflow == QueueOverflowPolicy::Reject) {
        if (cell.rejected != std::numeric_limits<std::uint64_t>::max()) { ++cell.rejected; }
        return located(error(ErrorCode::QueueFull, "Queue is full; send was rejected."), id);
    }
    if (cell.enqueued == std::numeric_limits<std::uint64_t>::max()) {
        return located(error(ErrorCode::IdentityExhausted, "Queue sequence cannot wrap."), id);
    }
    QueueMessage message{std::get<MessageId>(message_id), id, cell.enqueued + 1, correlation, producer,
        std::chrono::time_point_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now()), payload};
    cell.messages.push_back(message);
    ++cell.enqueued;
    if (full) { cell.messages.pop_front(); ++cell.dropped; }
    lock.unlock();
    gate.changed.notify_all();
    return message;
}

Result<std::vector<QueueMessage>> SharedDataService::receive_many(std::span<const QueueRead> requests,
    const UpdateOrigin& consumer, std::stop_token stop)
{
    if (requests.empty()) { return error(ErrorCode::InvalidQueue, "Receive requires at least one queue."); }
    if (!impl_->owns(consumer)) {
        return located(error(ErrorCode::ResultIdentityMismatch, "The receiver belongs to another session."), requests.front().queue);
    }
    std::set<QueueId> seen;
    std::vector<RequestedQueue> reads;
    for (const auto& request : requests) {
        const auto* def = queue_definition(request.queue);
        if (!def || !seen.insert(request.queue).second || request.count == 0 || request.count > def->receive.maximum_batch_size) {
            return located(error(ErrorCode::InvalidQueue, "Receive requires unique defined queues and a batch size within each queue's limit."), request.queue);
        }
        if (def->message_schema != request.expected_schema) {
            return located(error(ErrorCode::TypeMismatch, "The receiver's expected record differs from the queue schema."), request.queue);
        }
        reads.push_back({*def, *impl_->queues.at(request.queue), request.count});
    }
    const auto owner = std::make_shared<const Consumer>(consumer_key(consumer));
    auto& gate = *impl_->gate;
    std::unique_lock lock(gate.mutex);
    auto decision = readiness(reads, *owner);
    if (decision.state == Readiness::Wait) {
        gate.changed.wait(lock, stop, [&] {
            decision = readiness(reads, *owner);
            return decision.state != Readiness::Wait || gate.waits_stopped;
        });
        if (gate.waits_stopped) {
            return located(error(ErrorCode::SourceCancelled, "The session closed while waiting to receive."), requests.front().queue);
        }
    }
    if (stop.stop_requested()) { return located(error(ErrorCode::SourceCancelled, "Queue receive was cancelled before consumption."), requests.front().queue); }
    if (decision.state == Readiness::WrongConsumer) { return located(error(ErrorCode::QueueConsumerConflict, "This queue already has a different consumer."), *decision.queue); }
    if (decision.state == Readiness::Fail) { return located(error(ErrorCode::QueueEmpty, "Required queue batch is not available."), *decision.queue); }
    std::vector<QueueMessage> result;
    if (decision.state == Readiness::Empty) { return result; }

    // Prepare the complete reply before removing any prefix or claiming a consumer.
    for (const auto& [def, cell, count] : reads) {
        const auto amount = std::min(count, cell.messages.size());
        for (std::size_t i = 0; i < amount; ++i) {
            const auto& message = cell.messages[i];
            auto valid = detail::validate_runtime_domain(message.payload, impl_->snapshot.solution(), {});
            if (const auto* bad = std::get_if<Diagnostic>(&valid)) { return located(*bad, def.id); }
            result.push_back(message);
        }
    }
    if (stop.stop_requested()) { return located(error(ErrorCode::SourceCancelled, "Queue receive was cancelled before consumption."), requests.front().queue); }
    for (auto& [def, cell, count] : reads) {
        const auto amount = std::min(count, cell.messages.size());
        for (std::size_t i = 0; i < amount; ++i) { cell.messages.pop_front(); }
        cell.received += amount;
        if (def.receive.consumers == QueueConsumerPolicy::SingleConsumer) { cell.owner = owner; }
    }
    lock.unlock();
    gate.changed.notify_all();
    return result;
}

Result<QueuePreview> SharedDataService::preview(QueueId id) const
{
    const auto it = impl_->queues.find(id);
    if (it == impl_->queues.end()) { return located(error(ErrorCode::InvalidQueue, "Unknown queue."), id); }
    const std::lock_guard lock(impl_->gate->mutex);
    const auto& cell = *it->second;
    return QueuePreview{id, {cell.messages.begin(), cell.messages.end()}, cell.enqueued, cell.received, cell.dropped, cell.rejected};
}

} // namespace visionpro::workflow
