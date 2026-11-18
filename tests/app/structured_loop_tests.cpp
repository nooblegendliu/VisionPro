#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

void check(bool passed, std::string_view message)
{ if (!passed) { throw std::runtime_error(std::string(message)); } }

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template<SchemaIdentity Id> Id key(std::string_view name) { return take(SchemaIdFactory::parse<Id>(name)); }
template<UuidIdentity Id> Id id(char digit = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = digit;
    return take(IdFactory::parse<Id>(text));
}

InputDescriptor integer_input(std::string_view name, std::int64_t value)
{
    InputDescriptor port{key<InputId>(name), std::string(name), DataType::integer(), false, ConfigValue::integer(value)};
    port.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput, BindingSourceKind::RunOutput};
    return port;
}
Binding bind(std::string_view name, BindingSource source)
{ return {key<InputId>(name), {std::move(source)}}; }
ScopedInputReference index_of(NodeId loop)
{ return {id<WorkflowId>(), loop, key<InputId>("index"), {}}; }
Node addition(NodeId node)
{
    return {node, "Add", ToolConfiguration{key<ToolTypeId>("visionpro.sim.add"), 1},
        {bind("left", ConfigValue::integer(4)), bind("right", ConfigValue::integer(7))}};
}
GroupConfiguration region(std::string_view schema, NodeId child, std::string_view source, DataType element,
    std::int64_t start, std::int64_t end, std::int64_t step)
{
    const auto values = key<OutputId>("values");
    return {{key<SchemaId>(schema), 1, {integer_input("start", start), integer_input("end", end), integer_input("index", 0)},
            {{values, "Values", DataType::list(element)}}, {}},
        {child}, {{values, {id<WorkflowId>(), child, key<OutputId>(source), {}}}}, {},
        LoopConfiguration{step, false, 0, {{values, LoopExportMode::Collect}}}};
}
Solution scenario(std::int64_t start, std::int64_t end, std::int64_t step = 1)
{
    const auto loop_id = id<NodeId>();
    const auto item_id = id<NodeId>('2');
    const auto values = take(ConfigValue::list(DataType::integer(),
        {ConfigValue::integer(6), ConfigValue::integer(9), ConfigValue::integer(11)}));
    Node item{item_id, "Select item", ToolConfiguration{key<ToolTypeId>("visionpro.list.int64_at"), 1},
        {bind("list", values), bind("index", index_of(loop_id))}};
    Workflow main{id<WorkflowId>(), "Main", {key<SchemaId>("loop.main"), 1, {}, {}, {}},
        {{loop_id, "Loop", region("loop.outer", item_id, "item", DataType::integer(), start, end, step)}, item},
        {}, {{loop_id, item_id}}, {loop_id}};
    const auto probe_id = id<NodeId>('4');
    Workflow probe{id<WorkflowId>('2'), "Probe", {key<SchemaId>("loop.probe"), 1, {}, {}, {}},
        {addition(probe_id)}, {}, {}, {probe_id}};
    return {id<SolutionId>(), "Loop tests", 1, 1, {main, probe}};
}
GroupConfiguration& outer(Solution& s) { return std::get<GroupConfiguration>(s.workflows[0].nodes[0].configuration); }

struct Runner {
    ToolRegistry registry{take(visionpro::app::make_tool_registry())};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    IdSequence<ConfigSnapshotId> snapshots;

    RunSubmission submit(const Solution& s, char workflow = '1')
    {
        auto snap = take(ConfigSnapshot::capture(s, registry.descriptors(), {engine->session(), take(snapshots.next())}));
        return engine->submit(take(RunRequest::create(snap, WorkflowTarget{id<WorkflowId>(workflow)}, {},
            {TriggerKind::Manual, key<SchemaId>("loop.headless.test")})));
    }
    RunHandle start(const Solution& s, char workflow = '1')
    {
        auto submitted = submit(s, workflow);
        if (auto* rejection = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(rejection->diagnostic.message); }
        check(std::holds_alternative<RunHandle>(submitted), "Workflow was busy.");
        return std::get<RunHandle>(std::move(submitted));
    }
};
RunProgress done(const RunHandle& handle, ExecutionStatus expected)
{
    auto result = take(handle.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 4s));
    check(result.outcome == RunWaitOutcome::Satisfied, "The workflow did not reach quiescence before the deadline.");
    if (result.progress.status != expected && !result.progress.diagnostics.empty()) {
        throw std::runtime_error(result.progress.diagnostics.front().message);
    }
    check(result.progress.status == expected, "Unexpected terminal execution status.");
    return result.progress;
}
const RunNodeProgress& first_call(const RunProgress& progress, NodeId node)
{
    const auto found = std::ranges::find(progress.nodes, node, &RunNodeProgress::node);
    check(found != progress.nodes.end() && found->invocation.has_value(), "Missing invocation identity.");
    return *found;
}
DataValue exported(const RunHandle& handle, const RunProgress& progress, std::string_view name = "values")
{
    const auto record = take(handle.lookup(*first_call(progress, id<NodeId>()).invocation));
    const auto* slot = record ? record->output(key<OutputId>(name)) : nullptr;
    check(slot && std::holds_alternative<DataValue>(slot->content), "Public loop export has no value.");
    return std::get<DataValue>(slot->content);
}
std::vector<std::int64_t> integers(const DataValue& value)
{
    check(value.type() == DataType::list(DataType::integer()), "The list element type changed.");
    std::vector<std::int64_t> result;
    for (const auto& item : *value.elements()) { result.push_back(*item.as_int64()); }
    return result;
}
void no_complete_export(const RunHandle& handle, const RunProgress& progress)
{
    const auto record = take(handle.lookup(*first_call(progress, id<NodeId>()).invocation));
    if (!record) { return; }
    check(record->data().status != ExecutionStatus::Succeeded, "An interrupted loop cannot report success.");
    for (const auto& slot : record->data().outputs) {
        check(!std::holds_alternative<DataValue>(slot.content), "A partial aggregate was published after interruption.");
    }
}
bool reports(const RunProgress& progress, ErrorCode code)
{ return std::ranges::any_of(progress.diagnostics, [&](const auto& diagnostic) { return diagnostic.code == code; }); }

void descending_and_empty(Runner& runner)
{
    auto draft = scenario(2, 0, -1);
    outer(draft).loop->inclusive_end = true;
    const auto descending = runner.start(draft);
    auto trace = done(descending, ExecutionStatus::Succeeded);
    check(integers(exported(descending, trace)) == std::vector<std::int64_t>{11, 9, 6}, "Descending inclusive order changed.");
    std::set<ExecutionScopeId> scopes;
    std::vector<std::int64_t> indices;
    for (const auto& iteration : trace.iterations) {
        scopes.insert(iteration.scope);
        indices.push_back(iteration.index);
        check(iteration.exports.size() == 1 && iteration.exports.front().producer.run == descending.identity(),
            "A collected item must identify one producer in its own run.");
        const auto& producer = iteration.exports.front().producer.invocation;
        check(producer.node == id<NodeId>('2') && producer.scope == iteration.scope,
            "A collected item cannot reuse a producer from an earlier iteration.");
        const auto record = take(descending.lookup(producer));
        check(record && record->data().identity == iteration.exports.front().producer,
            "The collected item's producer must be queryable by its exact invocation.");
    }
    check(scopes.size() == 3 && indices == std::vector<std::int64_t>{2, 1, 0}, "Iterations must have distinct scopes and ordered indices.");

    draft = scenario(2, 0);
    auto& group = outer(draft);
    const auto add_export = [&](std::string_view name, DataType type, LoopExportMode mode, std::optional<ConfigValue> initial = {}) {
        const auto port = key<OutputId>(name);
        group.interface.outputs.push_back({port, std::string(name), type});
        group.outputs.push_back({port, group.outputs.front().source});
        group.loop->exports.push_back({port, mode, std::move(initial)});
    };
    add_export("first", DataType::optional(DataType::integer()), LoopExportMode::First);
    add_export("last", DataType::optional(DataType::integer()), LoopExportMode::Last);
    add_export("sum", DataType::integer(), LoopExportMode::Sum, ConfigValue::integer(23));
    const auto empty = runner.start(draft);
    trace = done(empty, ExecutionStatus::Succeeded);
    check(trace.iterations.empty() && integers(exported(empty, trace)).empty(), "Empty loop must retain an empty typed list.");
    for (const auto port : {"first", "last"}) {
        const auto optional = exported(empty, trace, port);
        check(optional.type() == DataType::optional(DataType::integer()) && !optional.optional_value(), "Empty first/last must be typed None.");
    }
    check(*exported(empty, trace, "sum").as_int64() == 23, "Empty Sum did not preserve its seed.");
    group.interface.outputs[1].type = DataType::integer();
    const auto required = runner.start(draft);
    trace = done(required, ExecutionStatus::Failed);
    check(reports(trace, ErrorCode::MissingRequiredValue), "An empty required First must diagnose the missing output.");
    no_complete_export(required, trace);
}

void nested_break(Runner& runner)
{
    auto draft = scenario(0, 2);
    auto& flow = draft.workflows[0];
    const auto outer_id = id<NodeId>();
    const auto item_id = id<NodeId>('2');
    const auto inner_id = id<NodeId>('3');
    const auto selector_id = id<NodeId>('5');
    const auto break_id = id<NodeId>('6');
    outer(draft) = region("loop.outer", inner_id, "values", DataType::list(DataType::integer()), 0, 2, 1);
    auto inner = region("loop.inner", item_id, "item", DataType::integer(), 0, 3, 1);
    inner.entries.push_back(selector_id);
    inner.loop->break_source = break_id;
    flow.nodes[1].bindings[1] = bind("index", index_of(inner_id));
    flow.nodes.push_back({inner_id, "Inner loop", inner, {bind("start", index_of(outer_id))}});
    flow.nodes.push_back({selector_id, "Break when left is selected", ToolConfiguration{key<ToolTypeId>("visionpro.sim.select"), 1},
        {bind("mask", index_of(inner_id))}});
    flow.nodes.push_back(addition(break_id));
    flow.containment = {{outer_id, inner_id}, {inner_id, item_id}, {inner_id, selector_id}, {inner_id, break_id}};
    flow.control_links = {{id<ControlLinkId>(), selector_id, key<ControlOutputId>("left"), break_id}};
    const auto handle = runner.start(draft);
    const auto trace = done(handle, ExecutionStatus::Succeeded);
    const auto value = exported(handle, trace);
    check(value.elements() && value.elements()->size() == 2, "Outer loop did not export both inner lists.");
    check(integers(value.elements()->at(0)) == std::vector<std::int64_t>{6, 9}
        && integers(value.elements()->at(1)) == std::vector<std::int64_t>{9}, "Nested loop lost the outer index or ignored its break branch.");
    check(trace.iterations.size() == 5 && std::ranges::count(trace.call_order, break_id) == 2,
        "An unselected break must continue and a successful break must end only its owning loop.");
}

void failure_cases(Runner& runner)
{
    auto draft = scenario(0, 1, 0);
    auto rejected = runner.submit(draft);
    check(std::holds_alternative<RunRejected>(rejected)
        && std::get<RunRejected>(rejected).diagnostic.code == ErrorCode::ConstraintViolation, "A zero step must fail before admission.");

    const auto max = std::numeric_limits<std::int64_t>::max();
    draft = scenario(max - 1, max, 2);
    draft.workflows[0].nodes[1].bindings[1] = bind("index", ConfigValue::integer(0));
    const auto overflow = runner.start(draft);
    auto trace = done(overflow, ExecutionStatus::Failed);
    check(trace.iterations.size() == 1 && reports(trace, ErrorCode::ConstraintViolation), "Index overflow must stop before a wrapped iteration.");
    no_complete_export(overflow, trace);

    draft = scenario(0, 5);
    const auto invalid = runner.start(draft);
    trace = done(invalid, ExecutionStatus::Failed);
    check(trace.iterations.size() == 4 && reports(trace, ErrorCode::IndexOutOfRange), "An out-of-range fourth item must stop the loop.");
    no_complete_export(invalid, trace);
}

void cancel_between_iterations(Runner& runner)
{
    auto draft = scenario(0, 3);
    outer(draft).loop->interval_ms = 60000;
    const auto observer = take(runner.engine->observe());
    const auto handle = runner.start(draft);
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    for (;;) {
        const auto update = observer.wait(deadline);
        check(update.event.has_value(), "The first body call did not publish before the deadline.");
        if (update.event->run == handle.identity() && update.event->result
            && update.event->result->invocation.node == id<NodeId>('2')) { break; }
    }
    const auto probe = runner.start(draft, '2');
    const auto independent = done(probe, ExecutionStatus::Succeeded);
    check(independent.call_order == std::vector<NodeId>{id<NodeId>('4')}, "An interval wait blocked the independent workflow.");
    check(take(handle.cancel()) == RunCancelDisposition::Requested, "Interval cancellation was not accepted.");
    const auto trace = done(handle, ExecutionStatus::Cancelled);
    check(trace.iterations.size() == 1 && std::ranges::count(trace.call_order, id<NodeId>('2')) == 1,
        "Cancellation during an interval must never open the next iteration.");
    no_complete_export(handle, trace);
}
} // namespace

int main()
{
    try {
        Runner runner;
        descending_and_empty(runner);
        nested_break(runner);
        failure_cases(runner);
        cancel_between_iterations(runner);
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
