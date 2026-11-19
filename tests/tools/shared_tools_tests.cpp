#include "shared_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::tools;
using namespace std::chrono_literals;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity T> T key(std::string_view spelling) { return take(SchemaIdFactory::parse<T>(spelling)); }
template<UuidIdentity T> T id() { return take(IdFactory::generate<T>()); }
DataValue string(std::string_view spelling) { return take(DataValue::string(spelling)); }
RunInput in(std::string_view name, DataValue value) { return {key<InputId>(name), std::move(value)}; }
void check(bool passed, std::string_view message) { if (!passed) { throw std::runtime_error(std::string(message)); } }

Workflow flow(const ToolRegistry& catalog, std::string_view type, std::vector<SharedTargetId> targets)
{
    const auto& descriptor = *catalog.find_descriptor(key<ToolTypeId>(std::string("visionpro.shared.") + std::string(type)));
    const auto workflow = id<WorkflowId>();
    Node node{id<NodeId>(), descriptor.display_name, ToolConfiguration{descriptor.id, 1, std::move(targets)}};
    for (const auto& port : descriptor.inputs) {
        node.bindings.push_back({port.id, {ScopedInputReference{workflow, {}, port.id, {}}}});
    }
    if (descriptor.requires_correlation_join) {
        node.bindings.push_back({key<ParameterId>("join"), {take(ConfigValue::string("s64.tools.workpieces"))}});
    }
    return {workflow, descriptor.display_name, {key<SchemaId>(std::string("tools.") + std::string(type)), 1, descriptor.inputs},
        {node}, {}, {}, {node.id}};
}

Workflow queue_join(QueueId inputs, QueueId deliveries)
{
    const auto workflow = id<WorkflowId>();
    const Node receive{id<NodeId>(), "Receive fragment", ToolConfiguration{key<ToolTypeId>("visionpro.shared.queue_receive_fragment"), 1, {inputs}}};
    const auto message = [&](bool payload, std::string_view field) {
        Selector path{ElementSelection{0}};
        if (payload) { path.push_back(FieldSelection{key<FieldId>("payload")}); }
        path.push_back(FieldSelection{key<FieldId>(field)});
        return OutputReference{workflow, receive.id, key<OutputId>("messages"), std::move(path)};
    };
    const Node join{id<NodeId>(), "Pair", ToolConfiguration{key<ToolTypeId>("visionpro.shared.join_dimensions"), 1},
        {{key<ParameterId>("join"), {take(ConfigValue::string("s64.pipeline"))}},
         {key<InputId>("domain"), {message(false, "domain")}}, {key<InputId>("key"), {message(false, "key")}},
         {key<InputId>("batch"), {message(true, "batch")}}, {key<InputId>("source"), {message(true, "source")}},
         {key<InputId>("value"), {message(true, "value")}}}};
    const Node send{id<NodeId>(), "Send complete workpiece", ToolConfiguration{key<ToolTypeId>("visionpro.shared.queue_send_dimensions"), 1, {deliveries}},
        {{key<InputId>("domain"), {message(false, "domain")}}, {key<InputId>("key"), {message(false, "key")}},
         {key<InputId>("payload"), {OutputReference{workflow, join.id, key<OutputId>("record"), {}}}}}};
    return {workflow, "Queue to join", {key<SchemaId>("tools.fragment_pipeline"), 1}, {receive, join, send},
        {{id<ControlLinkId>(), receive.id, {}, join.id}, {id<ControlLinkId>(), join.id, key<ControlOutputId>("ready"), send.id}},
        {}, {receive.id}};
}

struct Fixture {
    const ToolRegistry catalog = take(ToolRegistry::create(shared_registrations()));
    const VariableId first = id<VariableId>(), second = id<VariableId>();
    const QueueId left = id<QueueId>(), right = id<QueueId>();
    const QueueId fragments = id<QueueId>(), deliveries = id<QueueId>();
    const Workflow read = flow(catalog, "global_read_int64", {first});
    const Workflow write = flow(catalog, "global_write_int64", {first});
    const Workflow many = flow(catalog, "global_read_many_int64", {second, first});
    const Workflow send_left = flow(catalog, "queue_send_dimensions", {left});
    const Workflow send_right = flow(catalog, "queue_send_dimensions", {right});
    const Workflow receive = flow(catalog, "queue_receive_dimensions", {left});
    const Workflow both = flow(catalog, "queue_receive_dimensions", {left, right});
    const Workflow join = flow(catalog, "join_dimensions", {});
    const Workflow send_fragment = flow(catalog, "queue_send_fragment", {fragments});
    const Workflow pipeline = queue_join(fragments, deliveries);
    const Solution solution{id<SolutionId>(), "Shared tools", 1, 1, {read, write, many, send_left, send_right, receive, both, join, send_fragment, pipeline}, {},
        {{first, "First", DataType::integer(), ConfigValue::integer(10)}, {second, "Second", DataType::integer(), ConfigValue::integer(20)}},
        {{left, "Left", dimensions_type(), 2, QueueOverflowPolicy::Wait,
            {EmptyQueuePolicy::Wait, 2, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::RequireExact}},
         {right, "Right", dimensions_type(), 2, QueueOverflowPolicy::Wait,
            {EmptyQueuePolicy::Wait, 2, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::RequireExact}},
         {fragments, "Fragments", measurement_fragment_type(), 4, QueueOverflowPolicy::Reject,
            {EmptyQueuePolicy::Fail, 1, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::RequireExact}},
         {deliveries, "Complete workpieces", dimensions_type(), 4, QueueOverflowPolicy::Reject,
            {EmptyQueuePolicy::Fail, 1, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::RequireExact}}}};
    const std::unique_ptr<WorkflowSession> session = take(WorkflowSession::create(catalog));
    const ConfigSnapshot snapshot = take(session->activate(solution));
    const std::shared_ptr<const SharedData> preview = take(session->shared_preview());

    RunHandle start(const Workflow& workflow, std::vector<RunInput> inputs = {}, std::optional<DebugStatePolicy> debug = {}) const
    {
        const RunTarget target = debug ? RunTarget{NodeTarget{workflow.id, workflow.nodes.front().id}} : RunTarget{WorkflowTarget{workflow.id}};
        const auto request = take(RunRequest::create(snapshot, target, inputs, {TriggerKind::Manual, key<SchemaId>("s64.tools")}, {}, debug));
        auto submission = session->submit(request);
        if (auto* rejected = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error(std::string(error_code_name(rejected->diagnostic.code)) + ": " + rejected->diagnostic.message);
        }
        check(std::holds_alternative<RunHandle>(submission), "The workflow was not admitted.");
        return std::get<RunHandle>(std::move(submission));
    }
};

RunProgress quiesce(const RunHandle& run)
{
    const auto done = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(done.outcome == RunWaitOutcome::Satisfied && done.progress.quiescent, "Shared operation did not quiesce.");
    return done.progress;
}

NodeResult result(const RunHandle& run)
{
    const auto progress = quiesce(run);
    if (progress.status != ExecutionStatus::Succeeded && !progress.diagnostics.empty()) {
        throw std::runtime_error(std::string(error_code_name(progress.diagnostics.front().code)) + ": " + progress.diagnostics.front().message);
    }
    check(progress.status == ExecutionStatus::Succeeded && progress.nodes.size() == 1 && progress.nodes.front().invocation,
        "Expected a successful shared-data tool invocation.");
    return *take(run.lookup(*progress.nodes.front().invocation));
}

const DataValue& output(const NodeResult& result, std::string_view port)
{ return std::get<DataValue>(result.output(key<OutputId>(port))->content); }

DataValue dimensions(std::int64_t width, std::int64_t height)
{
    return take(DataValue::record(dimensions_type(), {{key<FieldId>("width"), DataValue::integer(width)},
        {key<FieldId>("height"), DataValue::integer(height)}}));
}

void globals(Fixture& f)
{
    auto value = result(f.start(f.read));
    check(output(value, "revision") == string("0") && output(value, "value") == DataValue::integer(10), "Initial value and revision.");
    auto updated = result(f.start(f.write, {in("value", DataValue::integer(47))}));
    const auto entry = take(f.preview->read(f.first));
    check(output(updated, "revision") == string("1") && std::get<SessionInvocationKey>(entry.update_origin).key == updated.data().identity.invocation,
        "A write records the actual invocation and advances the shared revision.");
    auto items = result(f.start(f.many));
    const auto& rows = *output(items, "entries").elements();
    check(rows.size() == 2 && *rows.front().field(key<FieldId>("variable_id")) == string(f.second.to_string())
        && *rows[0].field(key<FieldId>("revision")) == string("0") && *rows[1].field(key<FieldId>("value")) == DataValue::integer(47)
        && *rows[1].field(key<FieldId>("revision")) == string("1"), "Multi-read returns one ordered snapshot.");
    auto private_value = result(f.start(f.read, {}, DebugStatePolicy::Isolated));
    check(output(private_value, "value") == DataValue::integer(10), "Isolated reads see the declared initial value.");
    result(f.start(f.write, {in("value", DataValue::integer(-1))}, DebugStatePolicy::Isolated));
    check(take(f.preview->read(f.first)).current_value == DataValue::integer(47), "Isolated writes leave active values unchanged.");
    result(f.start(f.write, {in("value", DataValue::integer(48))}, DebugStatePolicy::Active));
    check(take(f.preview->read(f.first)).revision == 2, "Active debug updates this session.");
}

void waiting(const RunHandle& run)
{
    const auto observed = take(run.wait(RunWaitTarget::Completion, std::chrono::steady_clock::now() + 50ms));
    check(observed.outcome == RunWaitOutcome::DeadlineExpired, "The queue operation should remain pending.");
}

void queues(Fixture& f)
{
    const auto send = [&](const Workflow& target, std::string_view part, std::int64_t width) {
        return f.start(target, {in("domain", string("parts")), in("key", string(part)), in("payload", dimensions(width, 1))});
    };
    const auto pending = f.start(f.receive);
    waiting(pending);
    const auto written = result(send(f.send_left, "A", 10));
    const auto received = result(pending);
    const auto& messages = *output(received, "messages").elements();
    check(messages.size() == 1 && *messages.front().field(key<FieldId>("key")) == string("A")
        && *messages.front().field(key<FieldId>("payload")) == dimensions(10, 1)
        && *messages.front().field(key<FieldId>("message_id")) == output(written, "message_id")
        && *messages.front().field(key<FieldId>("sequence")) == string("1"), "A sender wakes the other workflow and preserves message identity.");
    result(send(f.send_left, "B", 20));
    const auto combination = f.start(f.both);
    waiting(combination);
    check(take(f.preview->preview(f.left)).messages.size() == 1, "A waiting combination cannot partially consume its ready queue.");
    result(send(f.send_right, "C", 30));
    const auto combined = result(combination);
    const auto& rows = *output(combined, "messages").elements();
    check(output(combined, "count") == DataValue::integer(2) && *rows.at(0).field(key<FieldId>("queue_id")) == string(f.left.to_string())
        && *rows.at(1).field(key<FieldId>("key")) == string("C"), "Combined envelopes preserve queue order and correlation keys.");
    const auto empty = f.start(f.receive);
    waiting(empty);
    take(empty.cancel());
    check(quiesce(empty).status == ExecutionStatus::Cancelled && take(f.preview->preview(f.left)).received == 2,
        "Cancelling an empty wait releases the run without consuming.");
    result(send(f.send_left, "D", 40));
    result(send(f.send_left, "E", 50));
    const auto full = send(f.send_left, "F", 60);
    waiting(full);
    take(full.cancel());
    const auto end = quiesce(full);
    const auto preview = take(f.preview->preview(f.left));
    check(end.status == ExecutionStatus::Cancelled && preview.messages.size() == 2 && preview.enqueued == 4,
        "Cancelling a full queue wait does not enqueue the staged payload.");
}

void joins(Fixture& f)
{
    const auto offer = [&](std::string_view workpiece, std::string_view source, std::int64_t value) {
        return result(f.start(f.join, {in("batch", DataValue::integer(1)), in("domain", string("parts")), in("key", string(workpiece)),
            in("source", string(source)), in("value", DataValue::integer(value))}));
    };
    const auto a = offer("A", "width", 10);
    check(a.data().selected_control_outputs == std::vector{key<ControlOutputId>("waiting")}
        && std::get<NotProduced>(a.output(key<OutputId>("record"))->content).reason == OutputAbsenceReason::ConditionNotMet,
        "An incomplete workpiece cannot publish a partial record.");
    offer("B", "width", 20);
    const auto b = offer("B", "height", 2);
    check(b.data().selected_control_outputs == std::vector{key<ControlOutputId>("ready")} && output(b, "record") == dimensions(20, 2),
        "The ready outlet delivers the completed B record.");
    const auto completed = offer("A", "height", 1);
    check(output(completed, "record") == dimensions(10, 1), "A's width survives the interleaving.");
    auto repeat = offer("A", "width", 77);
    check(repeat.data().selected_control_outputs == std::vector{key<ControlOutputId>("ignored")}
        && *output(repeat, "status").as_enum() == key<EnumMemberId>("Duplicate"), "Already delivered work cannot publish another record.");
    const auto& stats = output(repeat, "statistics");
    check(*stats.field(key<FieldId>("pending")) == string("0") && *stats.field(key<FieldId>("completed")) == string("2")
        && *stats.field(key<FieldId>("duplicates")) == string("1") && *stats.field(key<FieldId>("expired")) == string("0"),
        "A tool result records its own join counters at the instant the offered contribution was classified.");
    const auto& first_stats = output(a, "statistics");
    check(*first_stats.field(key<FieldId>("pending")) == string("1") && *first_stats.field(key<FieldId>("completed")) == string("0"),
        "Later contributions cannot change the counts recorded on an older invocation.");
}

void pipeline(Fixture& f)
{
    const auto ingest = [&](std::string_view part, std::string_view source, std::int64_t value) {
        const auto payload = take(DataValue::record(measurement_fragment_type(), {{key<FieldId>("batch"), DataValue::integer(1)},
            {key<FieldId>("source"), string(source)}, {key<FieldId>("value"), DataValue::integer(value)}}));
        result(f.start(f.send_fragment, {in("domain", string("parts")), in("key", string(part)), in("payload", payload)}));
        const auto run = quiesce(f.start(f.pipeline));
        if (!run.diagnostics.empty()) { throw std::runtime_error(run.diagnostics.front().message); }
        check(run.status == ExecutionStatus::Succeeded, "Fragment receive/join/send workflow failed.");
    };
    ingest("A", "width", 10);
    ingest("B", "width", 20);
    check(take(f.preview->preview(f.deliveries)).messages.empty(), "A waiting join does not enqueue a partial record.");
    ingest("B", "height", 2);
    ingest("A", "height", 1);
    ingest("B", "width", 20);
    const auto sent = take(f.preview->preview(f.deliveries));
    check(sent.messages.size() == 2 && sent.messages[0].correlation.key() == "B" && sent.messages[0].payload == dimensions(20, 2)
        && sent.messages[1].correlation.key() == "A" && sent.messages[1].payload == dimensions(10, 1),
        "Typed queue envelopes feed the join, and only its ready outlet sends each completed workpiece once.");
}
} // namespace

int main()
{
    try {
        Fixture fixture;
        globals(fixture);
        queues(fixture);
        joins(fixture);
        pipeline(fixture);
        std::cout << "Shared workflow tools passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
