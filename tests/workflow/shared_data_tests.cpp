#include <visionpro/workflow/shared_data.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
int checks{};
void check(bool value, const char* message)
{ ++checks; if (!value) { throw std::runtime_error(message); } }
template<class T> T take(Result<T> value)
{
    if (const auto* bad = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(bad->message); }
    return std::get<T>(std::move(value));
}
template<class T> Diagnostic fails(Result<T> value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code, "Operation must report the expected diagnostic.");
    return *failure;
}
template<class Id> Id uuid(char digit)
{
    std::string value = "00000000-0000-4000-8000-000000000000";
    value.back() = digit;
    return take(IdFactory::parse<Id>(value));
}
template<class Id> Id key(std::string_view value)
{ return take(SchemaIdFactory::parse<Id>(value)); }
struct Fixture {
    const SolutionSessionId session = uuid<SolutionSessionId>('1');
    const VariableId a = uuid<VariableId>('1'), b = uuid<VariableId>('2');
    const QueueId q = uuid<QueueId>('1'), r = uuid<QueueId>('2');
    const FieldId field = key<FieldId>("measure");
    const DataType schema = take(DataType::record(key<SchemaId>("test.measurement"), 1, {{field, DataType::integer()}}));
    const CorrelationId part = take(RuntimeIdFactory::correlation(key<CorrelationDomainId>("batch"), "A"));
    const UpdateOrigin first = ExternalUpdateOrigin{session, key<SchemaId>("test.first")};
    const UpdateOrigin second = ExternalUpdateOrigin{session, key<SchemaId>("test.second")};
    Solution config{uuid<SolutionId>('1'), "Shared data", 1, 1, {}, {},
        {{a, "A", DataType::integer(), ConfigValue::integer(0)}, {b, "B", DataType::integer(), ConfigValue::integer(0)}},
        {{q, "Q", schema, 2, QueueOverflowPolicy::Reject, {EmptyQueuePolicy::ReturnEmpty, 2, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::RequireExact}},
         {r, "R", schema, 2, QueueOverflowPolicy::DropOldest, {EmptyQueuePolicy::Fail, 2, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::UpToLimit}}}};
    ConfigSnapshot snapshot(unsigned sequence, SolutionSessionId owner) const
    { return take(ConfigSnapshot::capture(config, {}, {owner, take(RuntimeIdFactory::from_value<ConfigSnapshotId>(sequence))})); }
    std::shared_ptr<SharedDataService> service() const
    { return take(SharedDataService::create(snapshot(1, session))); }
    DataValue payload(std::int64_t n) const { return take(DataValue::record(schema, {{field, DataValue::integer(n)}})); }
    QueueMessage send(SharedDataService& service, QueueId queue, std::int64_t n) const
    { return take(service.send(queue, payload(n), part, first)); }
};

void global_transactions()
{
    Fixture f;
    auto service = f.service();
    const auto initial = take(service->read(f.a));
    check(initial.revision == 0 && *initial.current_value.as_int64() == 0, "A fresh service starts at the configured value and version zero.");
    const auto call = SessionInvocationKey{f.session,
        {take(RuntimeIdFactory::from_value<WorkflowRunId>(1)), uuid<NodeId>('f'),
         take(RuntimeIdFactory::from_value<ExecutionScopeId>(0)), take(RuntimeIdFactory::from_value<InvocationId>(1))}};
    const auto write = take(service->write(f.a, DataValue::integer(10), call));
    check(write.revision == 1 && std::get<SessionInvocationKey>(write.update_origin) == call,
        "A successful write advances the version and preserves its exact writer.");
    fails(service->write_many(std::array{GlobalWrite{f.a, DataValue::integer(99)}, GlobalWrite{f.b, DataValue::boolean(true)}},
        f.first), ErrorCode::TypeMismatch);
    check(take(service->read(f.a)).revision == 1, "A bad second write leaves the first variable untouched.");
    const std::array ids{f.b, f.a};
    const auto prior = take(service->read_many(ids));
    check(prior[0].variable == f.b && prior[0].revision == 0 && prior[1].current_value == write.current_value,
        "A read snapshot retains the requested order and values.");
    fails(service->read_many(std::array{f.a, f.a}), ErrorCode::InvalidGlobalVariable);
    const auto foreign = ExternalUpdateOrigin{uuid<SolutionSessionId>('2'), key<SchemaId>("test.foreign")};
    fails(service->write(f.a, DataValue::integer(80), foreign), ErrorCode::ResultIdentityMismatch);
    const auto pair = [&](int n) { return std::array{GlobalWrite{f.a, DataValue::integer(n)}, GlobalWrite{f.b, DataValue::integer(-n)}}; };
    take(service->write_many(pair(0), f.first));
    std::atomic<bool> begin{false};
    auto writer = std::async(std::launch::async, [&] {
        begin.wait(false);
        for (int n = 1; n <= 500; ++n) { take(service->write_many(pair(n), f.second)); }
    });
    begin.store(true); begin.notify_one();
    for (int i = 0; i < 500; ++i) {
        const auto now = take(service->read_many(ids));
        if (*now[0].current_value.as_int64() != -*now[1].current_value.as_int64()) {
            throw std::runtime_error("A batch reader observed a torn pair.");
        }
    }
    writer.get();
    check(*prior[1].current_value.as_int64() == 10 && *take(service->read(f.a)).current_value.as_int64() == 500,
        "Old read snapshots stay fixed across later transactions.");
}

void queue_transactions()
{
    Fixture f;
    auto service = f.service();
    const auto q1 = f.send(*service, f.q, 1);
    const auto q2 = f.send(*service, f.q, 2);
    fails(service->send(f.q, f.payload(3), f.part, f.first), ErrorCode::QueueFull);
    check(q1.sequence == 1 && q2.sequence == 2 && q1.id != q2.id && q2.correlation == f.part
        && std::get<ExternalUpdateOrigin>(q2.producer_origin).source == std::get<ExternalUpdateOrigin>(f.first).source,
        "FIFO assigns its own sequence and message ID and retains the business correlation and origin.");
    const std::array both{QueueRead{f.q, 2, f.schema}, QueueRead{f.r, 1, f.schema}};
    check(fails(service->receive_many(both, f.first), ErrorCode::QueueEmpty).location.queue == f.r,
        "A missing second queue identifies which batch was unavailable.");
    fails(service->receive_many(std::array{both[0], QueueRead{f.r, 1, DataType::integer()}}, f.first), ErrorCode::TypeMismatch);
    fails(service->receive_many(std::array{both[0], both[0]}, f.first), ErrorCode::InvalidQueue);
    const auto first = take(service->preview(f.q)), again = take(service->preview(f.q));
    check(first.messages.size() == 2 && again.messages[0].id == first.messages[0].id && again.messages[1].sequence == 2
        && again.enqueued == 2 && again.received == 0 && again.rejected == 1, "Failed combined receive and repeated previews do not consume.");
    fails(service->receive({f.q, 1, DataType::integer()}, f.first), ErrorCode::TypeMismatch);
    fails(service->send(f.r, DataValue::integer(4), f.part, f.first), ErrorCode::TypeMismatch);
    for (int i = 1; i <= 3; ++i) { f.send(*service, f.r, i); }
    const auto dropped = take(service->preview(f.r));
    check(dropped.enqueued == 3 && dropped.dropped_oldest == 1 && dropped.messages.front().sequence == 2,
        "DropOldest keeps the two newest complete FIFO records.");
    const auto received = take(service->receive_many(both, f.second));
    check(received.size() == 3 && received[0].id == q1.id && received[1].id == q2.id && received[2].queue == f.r
        && received[2].sequence == 2, "Combined consumption returns every requested prefix in request order.");
    fails(service->receive(both[0], f.first), ErrorCode::QueueConsumerConflict);
    f.send(*service, f.q, 7);
    check(take(service->receive(both[0], f.second)).empty() && take(service->preview(f.q)).received == 2,
        "Exact batch + ReturnEmpty consumes nothing when one more record is needed.");
    check(take(service->receive({f.r, 2, f.schema}, f.first)).size() == 1,
        "UpToLimit returns the available nonempty prefix to a competing consumer.");
    check(take(service->preview(f.q)).messages.front().sequence == 3, "A rejected send never consumes a sequence number.");
}

void publication_and_waits()
{
    Fixture f;
    f.config.queues[0].overflow = QueueOverflowPolicy::Wait;
    f.config.queues[0].receive.empty = EmptyQueuePolicy::Wait;
    f.config.queues[1].receive.empty = EmptyQueuePolicy::Wait;
    auto old = f.service();
    take(old->write(f.a, DataValue::integer(8), f.first));
    f.send(*old, f.q, 1);
    f.config.variables.front().name = "Renamed";
    f.config.queues.front().name = "Renamed";
    auto live = take(SharedDataService::create(f.snapshot(2, f.session), old));
    check(take(live->read(f.a)).revision == 1 && take(live->preview(f.q)).messages.size() == 1,
        "Compatible definitions retain data and counters.");
    const std::array reads{QueueRead{f.q, 1, f.schema}, QueueRead{f.r, 1, f.schema}};
    std::stop_source stop;
    auto waiting = std::async(std::launch::async, [&] { return live->receive_many(reads, f.first, stop.get_token()); });
    check(waiting.wait_for(20ms) == std::future_status::timeout, "A waiting combined receiver does not take its ready prefix.");
    stop.request_stop();
    fails(waiting.get(), ErrorCode::SourceCancelled);
    check(take(old->preview(f.q)).received == 0, "Cancelling a combined wait consumes no queues.");
    auto receiver = std::async(std::launch::async, [&] { return live->receive_many(reads, f.second); });
    f.send(*old, f.r, 2);
    check(take(receiver.get()).size() == 2, "A compatible older view wakes a receiver for an atomic combined read.");
    f.send(*live, f.q, 3); f.send(*live, f.q, 4);
    std::stop_source send_stop;
    auto sender = std::async(std::launch::async, [&] { return live->send(f.q, f.payload(5), f.part, f.first, send_stop.get_token()); });
    check(sender.wait_for(20ms) == std::future_status::timeout, "Wait overflow blocks until capacity becomes available.");
    send_stop.request_stop();
    fails(sender.get(), ErrorCode::SourceCancelled);
    check(take(old->preview(f.q)).enqueued == 3, "A cancelled blocked send does not allocate a queue sequence.");
    auto resumed = std::async(std::launch::async, [&] { return live->send(f.q, f.payload(5), f.part, f.first); });
    take(old->receive(reads.front(), f.second));
    check(take(resumed.get()).sequence == 4, "Releasing capacity wakes a waiting writer.");

    f.config.queues.front().capacity = 3;
    f.config.variables.front().initial_value = ConfigValue::integer(17);
    auto revised = take(SharedDataService::create(f.snapshot(3, f.session), live));
    take(live->write(f.a, DataValue::integer(99), f.first));
    check(*take(revised->read(f.a)).current_value.as_int64() == 17 && take(revised->read(f.a)).revision == 0
        && take(revised->preview(f.q)).enqueued == 0 && take(live->preview(f.q)).messages.size() == 2,
        "Incompatible definitions fork fresh cells and allow in-flight old requests to finish against their own view.");
    auto closing = std::async(std::launch::async, [&] { return live->send(f.q, f.payload(9), f.part, f.first); });
    revised->stop_waits();
    fails(closing.get(), ErrorCode::SourceCancelled);
    fails(revised->receive(reads.front(), f.first), ErrorCode::SourceCancelled);
    f.send(*revised, f.q, 4);
    check(take(revised->receive(reads.front(), f.first)).size() == 1, "Closing wakes waits and still permits already-ready work to drain.");
    const auto next_session = uuid<SolutionSessionId>('2');
    auto reopened = take(SharedDataService::create(f.snapshot(1, next_session)));
    check(take(reopened->preview(f.q)).messages.empty() && take(reopened->read(f.a)).revision == 0
        && *take(reopened->read(f.a)).current_value.as_int64() == 17, "Reopening starts from the saved definitions and no messages.");
    fails(SharedDataService::create(f.snapshot(1, next_session), live), ErrorCode::ResultIdentityMismatch);
}
} // namespace

int main()
{
    try { global_transactions(); queue_transactions(); publication_and_waits(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "shared_data: " << checks << " checks passed\n";
}
