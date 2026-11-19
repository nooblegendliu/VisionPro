#include "shared_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <nlohmann/json.hpp>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using nlohmann::json;

constexpr unsigned rounds = 20, slots = 2, waiters = 4;
constexpr unsigned messages = 5, history = 4, slow_notifications = 3;
constexpr std::size_t payload_bytes = 64 * 1024;

void require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

template<class T> T take(Result<T> result)
{
    if (auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<UuidIdentity T> T uuid(char suffix)
{
    std::string id = "00000000-0000-4000-8000-000000000000";
    id.back() = suffix;
    return take(IdFactory::parse<T>(id));
}

// The first two producers must actually enter their node before we release the batch.
// Admission, promotion, sending, receiving, history and observations use production code.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    unsigned inside = 0, peak = 0;
    std::map<WorkflowRunId, Clock::time_point> starts;
    std::weak_ptr<const SharedData> service;

    void enter(const InvocationContextData& context)
    {
        std::unique_lock lock(mutex);
        service = context.shared_data;
        require(starts.emplace(context.identity.invocation.workflow_run, Clock::now()).second, "Producer started twice.");
        peak = std::max(peak, ++inside);
        changed.notify_all();
        const bool released = changed.wait_for(lock, 10s, [&] { return open; });
        --inside;
        require(released, "Batch gate timed out.");
    }

    void release()
    {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
};

class Entry final : public ToolInstance {
public:
    Entry(const ToolExecutionConfiguration& config, std::shared_ptr<Gate> gate)
        : ToolInstance(config), gate_(std::move(gate)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        gate_->enter(context->data());
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated}};
    }
    std::shared_ptr<Gate> gate_;
};

class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Entry>(config, gate_)}; }
private:
    std::shared_ptr<Gate> gate_;
};

const auto producer = uuid<WorkflowId>('1'), drain = uuid<WorkflowId>('2');
const auto queue = uuid<QueueId>('3');
const auto send_type = key<ToolTypeId>("visionpro.shared.queue_send_text");
const auto gate_type = key<ToolTypeId>("test.load.gate");

ToolRegistry registry_for(const std::shared_ptr<Gate>& gate)
{
    auto tools = visionpro::tools::shared_registrations();
    auto send = std::ranges::find_if(tools, [](const auto& t) { return t.descriptor.id == send_type; });
    require(send != tools.end(), "Missing production send tool.");
    ToolDescriptor entry{gate_type, "Timestamped batch entry", 1, {key<SchemaId>("test.load.gate"), 1}};
    entry.shared_target = send->descriptor.shared_target;
    tools.push_back({entry, {entry.schema.id, 1, 1, {}}, std::make_shared<Factory>(gate)});
    return take(ToolRegistry::create(tools));
}

Solution load_solution(const ToolRegistry& registry)
{
    const auto& sending = *registry.find_descriptor(send_type);
    const auto gate = uuid<NodeId>('4'), send = uuid<NodeId>('5'), receive = uuid<NodeId>('6');
    Workflow source{producer, "Producers", {key<SchemaId>("test.load.producer"), 1}};
    for (const auto& input : sending.inputs) {
        if (input.id.text() != "domain") { source.interface.inputs.push_back(input); }
    }
    source.nodes = {{gate, "Measured entry", ToolConfiguration{gate_type, 1, {queue}}},
        {send, "Send workpiece", ToolConfiguration{send_type, 1, {queue}}}};
    for (const auto& input : source.interface.inputs) {
        source.nodes.back().bindings.push_back({input.id, {ScopedInputReference{producer, {}, input.id, {}}}});
    }
    source.nodes.back().bindings.push_back({key<InputId>("domain"), {take(ConfigValue::string("s612"))}});
    source.control_links = {{uuid<ControlLinkId>('7'), gate, {}, send}};
    source.entries = {gate};
    source.admission = {slots, waiters, AdmissionOverflow::Busy};
    Workflow consumer{drain, "Drain workpieces", {key<SchemaId>("test.load.drain"), 1}};
    consumer.nodes = {{receive, "Receive batch", ToolConfiguration{key<ToolTypeId>("visionpro.shared.queue_receive_text"), 1, {queue}},
        {{key<ParameterId>("count"), {ConfigValue::integer(messages)}}}}};
    consumer.entries = {receive};
    Solution solution{uuid<SolutionId>('8'), "Concurrent load", 1, 1, {std::move(source), std::move(consumer)}};
    solution.queues = {{queue, "Workpieces", sending.shared_target->type, messages, QueueOverflowPolicy::Reject,
        {EmptyQueuePolicy::Fail, messages, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::RequireExact}}};
    return solution;
}

std::string payload(const std::string& piece)
{
    std::string text = "measurement/" + piece + "|";
    text.resize(payload_bytes, '*');
    return text;
}

RunRequest request(const ConfigSnapshot& snapshot, WorkflowId flow, const std::string& piece = {})
{
    std::vector<RunInput> inputs;
    std::optional<CorrelationId> correlation;
    if (flow == producer) {
        inputs = {{key<InputId>("key"), take(DataValue::string(piece))},
            {key<InputId>("payload"), take(DataValue::string(payload(piece)))}};
        correlation = take(RuntimeIdFactory::correlation(key<CorrelationDomainId>("s612"), piece));
    }
    return take(RunRequest::create(snapshot, WorkflowTarget{flow}, inputs, {TriggerKind::Manual, key<SchemaId>("s612.load")}, correlation));
}

RunHandle accepted(RunSubmission submission)
{
    if (auto* refused = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(refused->diagnostic.message); }
    require(std::holds_alternative<RunHandle>(submission), "Unexpected Busy.");
    return std::get<RunHandle>(submission);
}

struct ArchiveKey { RunIdentity run; InvocationKey call; };
using Archive = std::vector<ArchiveKey>;

std::size_t indexed(WorkflowEngine& engine, const Archive& known)
{
    std::size_t count = 0;
    for (const auto& [run, call] : known) {
        auto found = engine.lookup_result(run, call);
        if (const auto* failure = std::get_if<Diagnostic>(&found)) {
            require(failure->code == ErrorCode::ResultRetired, "Unexpected history lookup error.");
        } else {
            require(std::get<std::optional<NodeResult>>(found).has_value(), "Missing archived invocation.");
            ++count;
        }
    }
    return count;
}

const DataValue& field(const DataValue& value, std::string_view name)
{
    auto* child = value.field(key<FieldId>(name));
    require(child != nullptr, "Missing message field.");
    return *child;
}

const std::string& string_value(const DataValue& value)
{
    require(value.as_string() != nullptr, "Expected a string field.");
    return *value.as_string();
}

json distribution(const std::vector<double>& raw)
{
    require(!raw.empty(), "Missing timing samples.");
    auto sorted = raw;
    std::ranges::sort(sorted);
    return {{"n", raw.size()}, {"p50", sorted[(sorted.size() + 1) / 2 - 1]},
        {"p95", sorted[(95 * sorted.size() + 99) / 100 - 1]}, {"max", sorted.back()}, {"samples", raw}};
}

json process_memory()
{
    PROCESS_MEMORY_COUNTERS_EX p{};
    p.cb = sizeof(p);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p), p.cb) != FALSE,
        "Could not sample Windows memory counters.");
    return {{"private_bytes", p.PrivateUsage}, {"working_set_bytes", p.WorkingSetSize},
        {"peak_working_set_bytes", p.PeakWorkingSetSize}, {"peak_commit_bytes", p.PeakPagefileUsage}};
}

struct Samples {
    Archive known;
    std::vector<double> queue_wait, run_time, ungated_time;
    unsigned busy{}, queued{}, queue_full{}, received{}, peak_messages{}, peak_waiters{};
    std::size_t history_peak{};
};

void measure_batch(unsigned batch, WorkflowEngine& engine, const ConfigSnapshot& snapshot, Gate& gate, Samples& samples)
{
    struct Pending { RunHandle handle; std::string piece; Clock::time_point submitted; };
    std::vector<Pending> pending;
    auto events = take(engine.observe(32));
    {
        std::lock_guard lock(gate.mutex);
        gate.open = false;
        gate.starts.clear();
    }
    struct ReleaseOnExit { Gate& gate; ~ReleaseOnExit() { gate.release(); } } guard{gate};
    unsigned waiting = 0;
    for (unsigned i = 0; i <= slots + waiters; ++i) {
        const auto piece = "piece-" + std::to_string(batch) + "-" + std::to_string(i);
        const auto prepared = request(snapshot, producer, piece);
        const auto submitted = Clock::now();
        auto submission = engine.submit(prepared);
        if (i == slots + waiters) {
            require(std::holds_alternative<RunBusy>(submission), "A saturated admission FIFO did not report Busy.");
            ++samples.busy;
        } else {
            auto run = accepted(std::move(submission));
            require((run.admission() == RunAdmission::Queued) == (i >= slots), "Unexpected admission order.");
            if (run.admission() == RunAdmission::Queued) { ++waiting; }
            pending.push_back({std::move(run), piece, submitted});
        }
    }
    samples.queued += waiting;
    samples.peak_waiters = std::max(samples.peak_waiters, waiting);
    {
        std::unique_lock lock(gate.mutex);
        require(gate.changed.wait_for(lock, 5s, [&] { return gate.inside == slots; }), "Did not reach concurrent in-flight capacity.");
    }
    const auto released_at = Clock::now();
    gate.release();
    std::map<WorkflowRunId, Clock::time_point> finished;
    const auto deadline = Clock::now() + 10s;
    while (finished.size() < pending.size()) {
        const auto event = events.wait(deadline);
        require(event.event && !event.dropped, "Lost a measured run event or timed out.");
        if (event.event->kind == RunObservationKind::RunQuiescent) {
            require(finished.emplace(event.event->run.run, Clock::now()).second, "Duplicate quiescent event.");
        }
    }
    events.unsubscribe();

    // Every accepted producer either owns exactly one delivered message or reports QueueFull.
    std::map<WorkflowRunId, std::pair<std::string, std::string>> sent;
    for (const auto& [handle, piece, submitted] : pending) {
        const auto id = handle.identity().run;
        const auto started = gate.starts.at(id);
        if (handle.admission() == RunAdmission::Queued) {
            samples.queue_wait.push_back(std::chrono::duration<double, std::milli>(started - submitted).count());
        }
        samples.run_time.push_back(std::chrono::duration<double, std::milli>(finished.at(id) - started).count());
        samples.ungated_time.push_back(std::chrono::duration<double, std::milli>(finished.at(id) - std::max(started, released_at)).count());
        const auto progress = take(handle.inspect());
        require(progress.quiescent && progress.nodes.size() == 2 && progress.nodes.back().invocation, "Incomplete producer trace.");
        const auto call = *progress.nodes.back().invocation;
        samples.known.push_back({handle.identity(), call});
        auto result = take(handle.lookup(call)).value();
        if (progress.status == ExecutionStatus::Failed) {
            require(std::ranges::any_of(result.data().diagnostics, [](const Diagnostic& diagnostic) {
                return diagnostic.code == ErrorCode::QueueFull;
            }), "Unexpected producer failure.");
            ++samples.queue_full;
        } else {
            require(progress.status == ExecutionStatus::Succeeded, "Unexpected producer status.");
            auto* output = result.output(key<OutputId>("message_id"));
            require(output != nullptr, "Missing sent message identity.");
            sent.emplace(id, std::pair{piece, string_value(std::get<DataValue>(output->content))});
        }
    }
    require(sent.size() == messages, "Expected exactly five successful sends.");
    auto service = gate.service.lock();
    require(service != nullptr, "Queue service unavailable.");
    const auto full = take(service->preview(queue));
    require(full.messages.size() == messages && full.enqueued == (batch + 1) * messages && full.rejected == batch + 1
        && full.dropped_oldest == 0, "Queue counters did not account for every producer.");
    samples.peak_messages = std::max(samples.peak_messages, static_cast<unsigned>(full.messages.size()));
    auto consumer = accepted(engine.submit(request(snapshot, drain)));
    const auto done = take(consumer.wait(RunWaitTarget::Quiescence, Clock::now() + 5s));
    require(done.outcome == RunWaitOutcome::Satisfied && done.progress.status == ExecutionStatus::Succeeded
        && done.progress.nodes.front().invocation, "Consumer did not drain the queue.");
    const auto call = *done.progress.nodes.front().invocation;
    samples.known.push_back({consumer.identity(), call});
    const auto drained = take(consumer.lookup(call)).value();
    const auto* output = drained.output(key<OutputId>("messages"));
    require(output != nullptr, "Missing consumer message list.");
    const auto* items = std::get<DataValue>(output->content).elements();
    require(items && items->size() == messages, "Incomplete consumer batch.");
    for (std::size_t i = 0; i < items->size(); ++i) {
        const auto& preview = full.messages.at(i);
        const auto& origin = std::get<SessionInvocationKey>(preview.producer_origin);
        const auto expected = sent.find(origin.key.workflow_run);
        require(expected != sent.end(), "Unknown or repeated producer in the FIFO.");
        const auto& [piece, message_id] = expected->second;
        const auto& actual = items->at(i);
        require(origin.session == snapshot.identity().session && preview.id.to_string() == message_id
            && preview.correlation.key() == piece && preview.sequence == batch * messages + i + 1
            && string_value(field(actual, "message_id")) == message_id
            && string_value(field(actual, "domain")) == "s612" && string_value(field(actual, "key")) == piece
            && string_value(field(actual, "sequence")) == std::to_string(preview.sequence)
            && string_value(field(field(actual, "payload"), "text")) == payload(piece), "Message identity, order or payload crossed workpieces.");
        sent.erase(expected);
    }
    samples.received += static_cast<unsigned>(items->size());
    const auto empty = take(service->preview(queue));
    require(sent.empty() && empty.messages.empty() && empty.received == samples.received, "Drain left unaccounted messages.");
    const auto retained = indexed(engine, samples.known);
    require(retained == history, "Completed-run archive exceeded or fell below its configured bound.");
    samples.history_peak = std::max(samples.history_peak, retained);
}

json measure()
{
    const auto baseline = process_memory();
    const auto gate = std::make_shared<Gate>();
    const auto registry = registry_for(gate);
    auto engine = take(WorkflowEngine::create(registry, {}, {history}));
    const auto snapshot = take(ConfigSnapshot::capture(load_solution(registry), registry.descriptors(),
        {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}));
    auto slow = take(engine->observe(slow_notifications));
    Samples samples;
    for (unsigned batch = 0; batch < rounds; ++batch) { measure_batch(batch, *engine, snapshot, *gate, samples); }
    unsigned notifications = 0;
    auto read = slow.wait(Clock::now());
    const auto dropped = read.dropped;
    while (read.event) { ++notifications; read = slow.wait(Clock::now()); }
    // Each producer publishes two results and two terminal events, each consumer one result and two terminal events.
    require(notifications == slow_notifications && dropped + notifications == rounds * ((slots + waiters) * 4 + 3),
        "Observation overflow was not explicitly accounted for.");
    const auto before_close = process_memory();
    const auto empty = take(gate->service.lock()->preview(queue));
    take(engine->close());
    const auto remaining_history = indexed(*engine, samples.known);
    require(remaining_history == 0 && !slow.wait(Clock::now()).event, "Closed engine retained queryable results or notifications.");
    engine.reset();
    require(gate->service.expired(), "Destroyed engine retained the shared queue service.");
    require(samples.queue_full == rounds && samples.busy == rounds && samples.received == rounds * messages, "Load totals do not reconcile.");
    return {{"rounds", rounds}, {"producer_attempts", rounds * (slots + waiters + 1)}, {"accepted_producers", samples.run_time.size()},
        {"accepted_consumers", rounds}, {"admission", {{"slots", slots}, {"peak_concurrent_entries", gate->peak}, {"waiting_capacity", waiters},
            {"peak_waiting", samples.peak_waiters}, {"queued", samples.queued}, {"busy", samples.busy}}},
        {"data_queue", {{"capacity", messages}, {"peak_messages", samples.peak_messages}, {"payload_bytes_each", payload_bytes},
            {"peak_payload_text_bytes", samples.peak_messages * payload_bytes}, {"received", samples.received}, {"rejected_full", samples.queue_full},
            {"dropped_oldest", empty.dropped_oldest}, {"messages_after_drain", empty.messages.size()}, {"service_released", gate->service.expired()}}},
        {"history", {{"capacity", history}, {"peak_completed_indexed", samples.history_peak}, {"completed_runs", samples.known.size()},
            {"indexed_after_close", remaining_history}}},
        {"slow_observer", {{"capacity", slow_notifications}, {"retained_before_drain", notifications}, {"overflow_dropped", dropped},
            {"retained_after_drain", static_cast<unsigned>(read.event.has_value())}}},
        {"queued_submit_to_entry_ms", distribution(samples.queue_wait)}, {"entry_to_observed_quiescence_ms", distribution(samples.run_time)},
        {"ungated_execution_to_observed_quiescence_ms", distribution(samples.ungated_time)},
        {"process_bytes", {{"baseline", baseline}, {"before_close", before_close}, {"after_release", process_memory()}}}};
}
} // namespace

int main()
{
    try { std::cout << measure().dump(2) << '\n'; return std::cout ? 0 : 1; }
    catch (const std::exception& error) { std::cerr << "S6.12 concurrent load: " << error.what() << '\n'; return 1; }
}
