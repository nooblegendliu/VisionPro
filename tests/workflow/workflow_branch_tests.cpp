#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

int checks = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

struct AsyncCall {
    ToolInvocationIdentity identity;
    InvocationCompletion completion;
};

class Driver final {
public:
    void started(NodeId node)
    {
        std::lock_guard lock(mutex_);
        starts_.push_back(node);
    }

    bool record(const AsyncCall& call)
    {
        std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        pending_.push_back(call);
        changed_.notify_all();
        return true;
    }

    AsyncCall await(NodeId node)
    {
        std::unique_lock lock(mutex_);
        const auto find = [&] {
            return std::ranges::find_if(pending_, [&](const AsyncCall& call) { return call.identity.invocation.node == node; });
        };
        if (!changed_.wait_for(lock, 5s, [&] { return find() != pending_.end(); })) {
            throw std::runtime_error("The expected asynchronous branch did not start.");
        }
        return *find();
    }

    int calls(NodeId node)
    {
        std::lock_guard lock(mutex_);
        return static_cast<int>(std::ranges::count(starts_, node));
    }

    void cancelled()
    {
        std::lock_guard lock(mutex_);
        ++cancellations_;
    }

    int cancellations()
    {
        std::lock_guard lock(mutex_);
        return cancellations_;
    }

    static void release(const AsyncCall& call)
    {
        static_cast<void>(call.completion.complete({call.identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(call.completion.mark_exited());
    }

    void drain()
    {
        std::vector<AsyncCall> pending;
        {
            std::lock_guard lock(mutex_);
            draining_ = true;
            pending.swap(pending_);
        }
        for (const auto& call : pending) { release(call); }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<NodeId> starts_;
    std::vector<AsyncCall> pending_;
    int cancellations_{0};
    bool draining_{false};
};

class BranchControl final : public InvocationControl {
public:
    explicit BranchControl(const std::shared_ptr<Driver>& driver) : driver_(driver) {}

    void request_cancel() override
    {
        if (const auto driver = driver_.lock()) { driver->cancelled(); }
    }

private:
    std::weak_ptr<Driver> driver_;
};

enum class Behavior { Choose, Measure, Merge, Send, Probe, Wait };

class BranchTool final : public ToolInstance {
public:
    BranchTool(const ToolExecutionConfiguration& configuration, Behavior behavior, const std::shared_ptr<Driver>& driver)
        : ToolInstance(configuration), behavior_(behavior), driver_(driver) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto driver = driver_.lock();
        const auto& data = context->data();
        if (!driver) { throw std::runtime_error("The fixture driver has expired."); }
        driver->started(data.identity.invocation.node);
        ToolCompletion result{data.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
        switch (behavior_) {
        case Behavior::Choose: {
            const auto mask = *data.inputs.front().value.as_int64();
            const auto outlets = std::array{"left", "nested", "right"};
            for (std::size_t i = 0; i < outlets.size(); ++i) {
                if ((mask & (1LL << i)) != 0) { result.selected_control_outputs.push_back(key<ControlOutputId>(outlets[i])); }
            }
            break;
        }
        case Behavior::Measure:
            result.judgement = ToolJudgement::NG;
            result.outputs.push_back({key<OutputId>("reading"), data.inputs.front().value});
            break;
        case Behavior::Merge:
        case Behavior::Send:
            result.outputs.push_back({key<OutputId>("readings"), data.inputs.front().value});
            break;
        case Behavior::Probe:
            break;
        case Behavior::Wait: {
            auto control = std::make_shared<BranchControl>(driver);
            take(completion.register_control(control));
            const AsyncCall call{data.identity, std::move(completion)};
            if (!driver->record(call)) { Driver::release(call); }
            return InvocationStart{AsynchronousInvocation{std::move(control)}};
        }
        }
        return InvocationStart{std::move(result)};
    }

    Behavior behavior_;
    std::weak_ptr<Driver> driver_;
};

class Factory final : public ToolInstanceFactory {
public:
    Factory(Behavior behavior, std::shared_ptr<Driver> driver) : behavior_(behavior), driver_(std::move(driver)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<BranchTool>(configuration, behavior_, driver_)}; }

private:
    Behavior behavior_;
    std::shared_ptr<Driver> driver_;
};

InputDescriptor input(std::string_view name, DataType type, BindingSourceKind kind)
{
    InputDescriptor descriptor{key<InputId>(name), std::string(name), std::move(type)};
    descriptor.binding.allowed_sources = {kind};
    return descriptor;
}

ToolRegistry registry_for(const std::shared_ptr<Driver>& driver)
{
    const auto integer = DataType::integer();
    const auto list = DataType::list(integer);
    const OutputDescriptor reading{key<OutputId>("reading"), "Reading", integer};
    const OutputDescriptor readings{key<OutputId>("readings"), "Readings", list};
    ToolDescriptor branch{key<ToolTypeId>("s34.choose"), "Choose", 1, {key<SchemaId>("s34.choose.schema"), 1},
        {}, {input("mask", integer, BindingSourceKind::PublicInput)}, {},
        {{key<ControlOutputId>("left"), "Left"}, {key<ControlOutputId>("nested"), "Nested"}, {key<ControlOutputId>("right"), "Right"}}};
    ToolDescriptor measure{key<ToolTypeId>("s34.measure"), "Measure", 1, {key<SchemaId>("s34.measure.schema"), 1},
        {}, {input("value", integer, BindingSourceKind::Constant)}, {reading}};
    auto sources = input("values", list, BindingSourceKind::RunOutput);
    sources.binding.cardinality = BindingCardinality::MultipleSources;
    ToolDescriptor merge{key<ToolTypeId>("s34.merge"), "Merge", 1, {key<SchemaId>("s34.merge.schema"), 1},
        {}, {sources}, {readings}};
    ToolDescriptor send{key<ToolTypeId>("s34.send"), "Send", 1, {key<SchemaId>("s34.send.schema"), 1},
        {}, {input("values", list, BindingSourceKind::RunOutput)}, {readings}};
    ToolDescriptor probe{key<ToolTypeId>("s34.probe"), "Probe", 1, {key<SchemaId>("s34.probe.schema"), 1}};
    ToolDescriptor waiting{key<ToolTypeId>("s34.wait"), "Asynchronous branch", 1, {key<SchemaId>("s34.wait.schema"), 1},
        {}, {}, {reading}};
    waiting.execution.mode = ToolExecutionMode::Asynchronous;
    waiting.execution.cancellation = ToolCancellation::Cooperative;
    const auto registration = [&](ToolDescriptor descriptor, Behavior behavior) {
        const auto schema = descriptor.schema.id;
        return ToolRegistration{std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<Factory>(behavior, driver)};
    };
    const std::array registrations{
        registration(std::move(branch), Behavior::Choose), registration(std::move(measure), Behavior::Measure),
        registration(std::move(merge), Behavior::Merge), registration(std::move(send), Behavior::Send),
        registration(std::move(probe), Behavior::Probe), registration(std::move(waiting), Behavior::Wait)};
    return take(ToolRegistry::create(registrations));
}

Node node(char id, std::string_view type, std::string name, std::vector<Binding> bindings = {})
{
    return {uuid<NodeId>(id), std::move(name), ToolConfiguration{key<ToolTypeId>(type), 1}, std::move(bindings)};
}

Solution solution(std::uint64_t revision, bool accept_absence, bool asynchronous)
{
    const auto flow = uuid<WorkflowId>();
    const auto outer_mask = key<InputId>("outer-mask");
    const auto inner_mask = key<InputId>("inner-mask");
    const auto choose = [&](char id, InputId mask) {
        return node(id, "s34.choose", "Choose", {{key<InputId>("mask"), {ScopedInputReference{flow, {}, mask, {}}}}});
    };
    const auto measure = [](char id, std::int64_t value) {
        return node(id, "s34.measure", "Measure", {{key<InputId>("value"), {ConfigValue::integer(value)}}});
    };
    const auto source = [&](char id) -> BindingSource {
        return OutputReference{flow, uuid<NodeId>(id), key<OutputId>("reading"), {}};
    };
    const auto link = [](char id, char from, char to, std::optional<ControlOutputId> outlet = {}) -> ControlLink {
        return {uuid<ControlLinkId>(id), uuid<NodeId>(from), std::move(outlet), uuid<NodeId>(to)};
    };
    const auto missing = accept_absence ? std::vector{MissingReason::NotSelected} : std::vector<MissingReason>{};
    Workflow graph{flow, "Nested branches", {key<SchemaId>("s34.graph"), 1,
        {input("outer-mask", DataType::integer(), BindingSourceKind::Constant), input("inner-mask", DataType::integer(), BindingSourceKind::Constant)}},
        {node('9', "s34.send", "Send", {{key<InputId>("values"), {OutputReference{flow, uuid<NodeId>('8'), key<OutputId>("readings"), {}}}}}),
         choose('3', inner_mask), choose('1', outer_mask),
         asynchronous ? node('2', "s34.wait", "Left async") : measure('2', 11),
         asynchronous ? node('4', "s34.wait", "Right async") : measure('4', 20),
         node('5', "s34.probe", "Always"), measure('6', 31), measure('7', 33),
         node('8', "s34.merge", "Merge", {{key<InputId>("values"), {source('7'), source('4'), source('2'), source('6')},
            BindingCardinality::MultipleSources, ConversionKind::Exact, {missing}}})},
        {link('1', '1', '2', key<ControlOutputId>("left")), link('2', '1', '3', key<ControlOutputId>("nested")),
         link('3', '1', '4', key<ControlOutputId>("right")), link('4', '1', '5'),
         link('5', '3', '6', key<ControlOutputId>("left")), link('6', '3', '7', key<ControlOutputId>("nested")),
         link('7', '2', '8'), link('8', '4', '8'), link('9', '6', '8'), link('a', '7', '8'), link('b', '8', '9')},
        {}, {uuid<NodeId>('5'), uuid<NodeId>()}};
    if (asynchronous) { graph.control_links.push_back(link('c', '2', '5')); }
    const auto right = key<ControlOutputId>("right");
    const auto right_value = key<OutputId>("right-reading");
    graph.interface.outputs = {{right_value, "Right reading", DataType::integer()}};
    graph.outputs = {{right_value, std::get<OutputReference>(source('4'))}};
    graph.interface.control_outputs = {{right, "Right"}};
    graph.control_exports = {{right, uuid<NodeId>('1'), right}};
    Workflow probe{uuid<WorkflowId>('2'), "Independent probe", {key<SchemaId>("s34.probe.flow"), 1},
        {node('a', "s34.probe", "Probe")}, {}, {}, {uuid<NodeId>('a')}};
    return {uuid<SolutionId>(), "Branch selection", 1, revision, {std::move(graph), std::move(probe)}};
}

RunProgress wait_for(const RunHandle& run, RunWaitTarget target = RunWaitTarget::Quiescence)
{
    auto waited = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied, "The workflow reached its expected wait target.");
    return std::move(waited.progress);
}

struct Fixture {
    std::shared_ptr<Driver> driver{std::make_shared<Driver>()};
    ToolRegistry registry{registry_for(driver)};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    IdSequence<ConfigSnapshotId> snapshots;

    ~Fixture() { driver->drain(); }

    RunHandle submit(std::int64_t outer, std::int64_t inner, bool accept_absence = true, bool asynchronous = false, char flow = '1')
    {
        const auto id = take(snapshots.next());
        const auto snapshot = take(ConfigSnapshot::capture(solution(id.value(), accept_absence, asynchronous), registry.descriptors(),
            {engine->session(), id}));
        std::vector<RunInput> inputs;
        if (flow == '1') {
            inputs = {{key<InputId>("outer-mask"), DataValue::integer(outer)}, {key<InputId>("inner-mask"), DataValue::integer(inner)}};
        }
        auto submission = engine->submit(take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>(flow)}, inputs,
            {TriggerKind::Manual, key<SchemaId>("s34.branch.test")})));
        if (const auto* rejected = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error("Run rejected: " + rejected->diagnostic.message);
        }
        check(std::holds_alternative<RunHandle>(submission), "The workflow request was accepted.");
        return std::get<RunHandle>(std::move(submission));
    }

    void probe()
    {
        const auto progress = wait_for(submit(0, 0, true, true, '2'));
        check(progress.status == ExecutionStatus::Succeeded, "The independent probe ran while branch operations waited.");
    }
};

const RunNodeProgress& progress_of(const RunProgress& progress, char id, ExecutionStatus expected)
{
    const auto found = std::ranges::find(progress.nodes, uuid<NodeId>(id), &RunNodeProgress::node);
    check(found != progress.nodes.end() && found->status == expected,
        std::string("Unexpected status for fixture node ") + id + '.');
    return *found;
}

void edge(const RunProgress& progress, char id, ControlLinkStatus status, std::optional<ControlCloseReason> reason = {})
{
    const auto found = std::ranges::find(progress.control_links, uuid<ControlLinkId>(id), &RunControlLinkProgress::link);
    check(found != progress.control_links.end() && found->status == status && found->close_reason == reason,
        std::string("Unexpected state or closure reason for fixture control link ") + id + '.');
}

void skipped(const RunProgress& progress, char id, NodeSkipReason expected)
{
    const auto& node = progress_of(progress, id, ExecutionStatus::Skipped);
    check(!node.invocation && node.skip_reason == expected, "A skipped node has a reason and no fabricated invocation.");
}

NodeResult result(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "An actual invocation has a key.");
    const auto found = take(run.lookup(*node.invocation));
    check(found && found->data().identity.invocation == *node.invocation && found->data().identity.run == run.identity(),
        "Lookup returns this run's exact committed invocation.");
    return *found;
}

void sent(const RunHandle& run, const RunProgress& progress, const std::vector<DataValue>& expected)
{
    const auto record = result(run, progress_of(progress, '9', ExecutionStatus::Succeeded));
    const auto* output = record.output(key<OutputId>("readings"));
    const auto* values = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(values && *values == take(DataValue::list(DataType::integer(), expected)),
        "Send received the selected measurements in binding order.");
}

void branch_selection()
{
    Fixture fixture;
    const auto both = fixture.submit(3, 2);
    const auto mixed = wait_for(both);
    const auto exports = take(both.public_result());
    check(exports.outputs.size() == 1 && std::get<NotProduced>(exports.outputs.front().content).reason == OutputAbsenceReason::NotSelected
        && exports.exports.empty() && exports.selected_control_outputs.empty(),
        "An unselected root branch has typed absence, no fabricated producer and no selected public outlet.");
    check(mixed.status == ExecutionStatus::Succeeded, "A named multi-selection with a nested partial selection completed.");
    for (const auto id : std::string_view("12467ab")) { edge(mixed, id, ControlLinkStatus::Arrived); }
    for (const auto id : std::string_view("3589")) { edge(mixed, id, ControlLinkStatus::Closed, ControlCloseReason::NotSelected); }
    for (const auto id : std::string_view("46")) { skipped(mixed, id, NodeSkipReason::NotSelected); }
    check(result(both, progress_of(mixed, '2', ExecutionStatus::Succeeded)).data().judgement == Judgement::NG,
        "NG is a successful control arrival.");
    sent(both, mixed, {DataValue::integer(33), DataValue::integer(11)});
    const auto& join = progress_of(mixed, '8', ExecutionStatus::Succeeded);
    check(join.bindings.size() == 1 && join.bindings.front().sources.size() == 4, "Merge traces each declared binding source.");
    const auto& traces = join.bindings.front().sources;
    check(traces[1].accepted_absence == MissingReason::NotSelected && traces[3].accepted_absence == MissingReason::NotSelected
        && !std::get<ResultTrace>(traces[1].origin).invocation && !std::get<ResultTrace>(traces[3].origin).invocation,
        "Omissions are traced as known branch absences without invocation keys.");
    check(fixture.driver->calls(uuid<NodeId>('5')) == 1 && fixture.driver->calls(uuid<NodeId>('9')) == 1,
        "The unconditional branch and joined send each run once.");

    const auto nested_zero = fixture.submit(3, 0);
    const auto partial = wait_for(nested_zero);
    check(partial.status == ExecutionStatus::Succeeded, "Zero selected nested outlets close while an outer sibling still arrives.");
    for (const auto id : std::string_view("569a")) { edge(partial, id, ControlLinkStatus::Closed, ControlCloseReason::NotSelected); }
    sent(nested_zero, partial, {DataValue::integer(11)});

    const auto zero = wait_for(fixture.submit(0, 3));
    check(zero.status == ExecutionStatus::Succeeded, "Zero outer selections propagate closure to quiescence.");
    edge(zero, '4', ControlLinkStatus::Arrived);
    for (const auto id : std::string_view("12356789ab")) { edge(zero, id, ControlLinkStatus::Closed, ControlCloseReason::NotSelected); }
    for (const auto id : std::string_view("2346789")) { skipped(zero, id, NodeSkipReason::NotSelected); }
    check(fixture.driver->calls(uuid<NodeId>('5')) == 3 && fixture.driver->calls(uuid<NodeId>('8')) == 2
        && fixture.driver->calls(uuid<NodeId>('9')) == 2, "Zero arrivals skip merge and send while the unconditional tool still runs.");

    const auto required = wait_for(fixture.submit(3, 2, false));
    check(required.status == ExecutionStatus::Failed, "A mandatory omitted branch source fails preparation.");
    check(!progress_of(required, '8', ExecutionStatus::Failed).invocation, "Binding failure precedes the merge invocation.");
    const auto diagnostic = std::ranges::find(required.diagnostics, ErrorCode::InputUnavailable, &Diagnostic::code);
    check(diagnostic != required.diagnostics.end() && diagnostic->location.node == uuid<NodeId>('8')
        && diagnostic->location.input == key<InputId>("values"), "Unavailable data is attributed to its target input.");
    skipped(required, '9', NodeSkipReason::FailFast);
    edge(required, 'b', ControlLinkStatus::Closed, ControlCloseReason::Failed);
    check(fixture.driver->calls(uuid<NodeId>('9')) == 2, "An unavailable mandatory binding does not send.");
}

void group_exports_and_debug()
{
    Fixture fixture;
    auto draft = solution(1, true, false);
    auto& flow = draft.workflows.front();
    const auto outer = uuid<NodeId>('b'), inner = uuid<NodeId>('c'), choose = uuid<NodeId>('1'), measure = uuid<NodeId>('2');
    const auto mask = key<InputId>("mask");
    const auto out = key<OutputId>("value");
    const auto yes = key<ControlOutputId>("yes"), done = key<ControlOutputId>("done");
    auto port = input("mask", DataType::integer(), BindingSourceKind::PublicInput);
    port.binding.allowed_sources.push_back(BindingSourceKind::GlobalRead);
    const auto region = [&](std::string_view name, std::vector<NodeId> entries, OutputReference source, ControlOutputMapping control) {
        const ControlOutputMapping completed{done, control.source, {}};
        return GroupConfiguration{{key<SchemaId>(name), 1, {port}, {{out, "Value", DataType::integer()}}, {{yes, "Yes"}, {done, "Done"}}},
            std::move(entries), {{out, std::move(source)}}, {std::move(control), completed}};
    };
    flow.interface = {key<SchemaId>("s62.flow"), 1, {port}};
    flow.outputs.clear();
    flow.control_exports.clear();
    const auto branch = key<ControlOutputId>("left");
    flow.nodes = {
        {outer, "Outer", region("s62.outer", {inner}, {flow.id, inner, out, {}}, {yes, inner, yes}),
            {{mask, {ScopedInputReference{flow.id, {}, mask, {}}}}}},
        {inner, "Inner", region("s62.inner", {choose, measure}, {flow.id, measure, key<OutputId>("reading"), {}}, {yes, choose, branch}),
            {{mask, {ScopedInputReference{flow.id, outer, mask, {}}}}}},
        node('1', "s34.choose", "Choose", {{mask, {ScopedInputReference{flow.id, inner, mask, {}}}}}),
        node('2', "s34.measure", "Measure", {{key<InputId>("value"), {ConfigValue::integer(7)}}}),
        node('3', "s34.probe", "Internal branch"), node('4', "s34.probe", "Outer successor"),
        node('5', "s34.probe", "Unconditional successor"), node('6', "s34.probe", "Completed child successor")};
    flow.containment = {{outer, inner}, {inner, choose}, {inner, measure}, {inner, uuid<NodeId>('3')}};
    flow.entries = {outer};
    flow.control_links = {{uuid<ControlLinkId>('1'), choose, branch, uuid<NodeId>('3')},
        {uuid<ControlLinkId>('2'), outer, yes, uuid<NodeId>('4')},
        {uuid<ControlLinkId>('3'), outer, {}, uuid<NodeId>('5')}, {uuid<ControlLinkId>('4'), outer, done, uuid<NodeId>('6')}};
    auto capture = [&] { return take(ConfigSnapshot::capture(draft, fixture.registry.descriptors(),
        {fixture.engine->session(), take(fixture.snapshots.next())})); };
    const RunTrigger manual{TriggerKind::Manual, key<SchemaId>("s62.group.test")};
    auto start = [&](RunRequest request) {
        auto submission = fixture.engine->submit(request);
        if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
        return std::get<RunHandle>(std::move(submission));
    };
    const auto snapshot = capture();
    for (std::int64_t bit : {1, 0}) {
        const auto handle = start(take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, {{mask, DataValue::integer(bit)}}, manual)));
        const auto trace = wait_for(handle);
        check(trace.status == ExecutionStatus::Succeeded && trace.groups.size() == 2 && trace.iterations.empty(),
            "Two nested Group calls execute exactly once and retain child scopes.");
        const auto parent = result(handle, progress_of(trace, 'b', ExecutionStatus::Succeeded));
        const auto child = result(handle, progress_of(trace, 'c', ExecutionStatus::Succeeded));
        check(*std::get<DataValue>(parent.output(out)->content).as_int64() == 7
            && trace.groups[0].exports.front().producer.invocation == child.data().identity.invocation
            && trace.groups[1].exports.front().producer.invocation.node == measure,
            "Marshalled public outputs retain the exact child invocation at each boundary.");
        check(parent.data().selected_control_outputs == (bit ? std::vector{yes, done} : std::vector{done}),
            "A named child outlet follows selection while a successful child always opens its completion mapping.");
        progress_of(trace, '5', ExecutionStatus::Succeeded);
        progress_of(trace, '6', ExecutionStatus::Succeeded);
        progress_of(trace, '3', bit ? ExecutionStatus::Succeeded : ExecutionStatus::Skipped);
        progress_of(trace, '4', bit ? ExecutionStatus::Succeeded : ExecutionStatus::Skipped);
    }

    auto& inner_group = std::get<GroupConfiguration>(flow.nodes[1].configuration);
    inner_group.entries = {choose};
    flow.control_links.push_back({uuid<ControlLinkId>('5'), choose, branch, measure});
    const auto decision = key<OutputId>("decision_status");
    inner_group.interface.outputs.insert(inner_group.interface.outputs.begin(), OutputDescriptor{
        decision, "Decision status", take(system_field_type(SystemField::ExecutionStatus))});
    inner_group.outputs.insert(inner_group.outputs.begin(), OutputMapping{decision, {flow.id, choose, SystemField::ExecutionStatus, {}}});
    const auto closed = start(take(RunRequest::create(capture(), WorkflowTarget{flow.id}, {{mask, DataValue::integer(0)}}, manual)));
    const auto incomplete = wait_for(closed);
    const auto failed = result(closed, progress_of(incomplete, 'c', ExecutionStatus::Failed));
    const auto failed_group = std::ranges::find(incomplete.groups, inner, [](const RunGroupCall& call) { return call.group.node; });
    check(incomplete.status == ExecutionStatus::Failed && !failed.output(out) && !failed.output(decision)
        && failed_group != incomplete.groups.end() && failed_group->exports.empty()
        && failed.data().diagnostics.front().code == ErrorCode::InputUnavailable
        && failed.data().diagnostics.front().location.output == out,
        "One missing required export prevents publication of all Group outputs and their producer traces.");
    inner_group.interface.outputs.erase(inner_group.interface.outputs.begin());
    inner_group.outputs.erase(inner_group.outputs.begin());
    auto& public_output = inner_group.interface.outputs.front();
    const auto required = public_output;
    public_output.required = false;
    for (const auto reason : {OutputAbsenceReason::NotSelected, OutputAbsenceReason::Disabled}) {
        public_output.allowed_absence = {reason};
        const auto declared = validate_public_interface(inner_group.interface);
        const auto* invalid = std::get_if<Diagnostic>(&declared);
        check(invalid && invalid->code == ErrorCode::InvalidDescriptor && invalid->location.output == out,
            "Control closure cannot be declared as a successful public output omission.");
    }
    public_output.allowed_absence = {OutputAbsenceReason::ConditionNotMet};
    auto& producer = *std::ranges::find(flow.nodes, measure, &Node::id);
    for (const bool disabled : {false, true}) {
        producer.enabled = !disabled;
        const auto skipped = start(take(RunRequest::create(capture(), WorkflowTarget{flow.id},
            {{mask, DataValue::integer(disabled ? 1 : 0)}}, manual)));
        const auto progress = wait_for(skipped);
        const auto& skipped_node = progress_of(progress, '2', ExecutionStatus::Skipped);
        const auto record = result(skipped, progress_of(progress, 'c', ExecutionStatus::Failed));
        const auto frame = std::ranges::find(progress.groups, inner, [](const RunGroupCall& call) { return call.group.node; });
        check(!skipped_node.invocation && skipped_node.skip_reason == (disabled ? NodeSkipReason::Disabled : NodeSkipReason::NotSelected)
            && frame != progress.groups.end() && frame->exports.empty() && !record.output(out)
            && record.data().diagnostics.front().code == ErrorCode::InputUnavailable,
            "A skipped producer preserves its control reason and creates neither an output slot nor an export identity.");
    }
    producer.enabled = true;
    public_output = required;
    flow.control_links.pop_back();
    inner_group.entries = {choose, measure};

    // The detached target is nested and its surrounding persistent input is now invalid.
    const auto external = uuid<VariableId>();
    draft.variables.push_back({external, "Host mask", DataType::integer(), ConfigValue::integer(0)});
    flow.nodes[1].bindings.front().sources = {GlobalReference{external, {}, {}}};
    const auto detached = capture();
    const auto debug = RunTrigger{TriggerKind::Debug, key<SchemaId>("s62.debug")};
    const auto missing = RunRequest::create(detached, NodeTarget{flow.id, inner}, {}, debug);
    const auto* error = std::get_if<Diagnostic>(&missing);
    check(error && error->code == ErrorCode::MissingRequiredValue && error->location.input == mask,
        "Missing host input is diagnosed on the Group public port.");
    const auto group = start(take(RunRequest::create(detached, NodeTarget{flow.id, inner}, {{mask, DataValue::integer(1)}}, debug)));
    const auto trace = wait_for(group);
    check(trace.groups.size() == 1 && trace.groups.front().group.node == inner && trace.groups.front().group.scope.value() == 0
        && std::ranges::count(trace.call_order, outer) == 0 && std::ranges::count(trace.call_order, uuid<NodeId>('4')) == 0,
        "Debugging a nested Group ignores outer bindings, enclosing Groups and exterior successors.");
    const auto& input_trace = progress_of(trace, 'c', ExecutionStatus::Succeeded).bindings.front().sources.front();
    check(std::holds_alternative<IndependentInputTrace>(input_trace.origin), "Injected Group input has independent-call provenance.");

    const auto unavailable = fixture.engine->submit(take(RunRequest::create(detached, NodeTarget{flow.id, outer}, {{mask, DataValue::integer(1)}}, debug)));
    const auto* refusal = std::get_if<RunRejected>(&unavailable);
    check(refusal && refusal->diagnostic.code == ErrorCode::GlobalUnavailable && refusal->diagnostic.location.variable == external,
        "Detached host must explicitly supply a global used inside its Group.");
    const std::array globals{GlobalVariableEntry{external, DataValue::integer(0), 3, ExternalUpdateOrigin{fixture.engine->session(), key<SchemaId>("s62.host")}}};
    const auto supplied = start(take(RunRequest::create(detached, NodeTarget{flow.id, outer}, {{mask, DataValue::integer(1)}}, debug, {}, {}, globals)));
    check(wait_for(supplied).status == ExecutionStatus::Succeeded, "The explicitly injected host service satisfies the exact dependency.");

    flow.nodes[0].configuration = region("s62.outer", {inner}, {flow.id, measure, key<OutputId>("reading"), {}}, {yes, inner, yes});
    const auto illegal = fixture.engine->submit(take(RunRequest::create(capture(), WorkflowTarget{flow.id}, {{mask, DataValue::integer(1)}}, manual)));
    refusal = std::get_if<RunRejected>(&illegal);
    check(refusal && refusal->diagnostic.code == ErrorCode::ScopeViolation, "A parent cannot export through its child's public boundary.");
}

void branch_fail_fast()
{
    Fixture fixture;
    const auto run = fixture.submit(5, 3, true, true);
    const auto left = fixture.driver->await(uuid<NodeId>('2'));
    const auto right = fixture.driver->await(uuid<NodeId>('4'));
    fixture.probe();
    const auto waiting = take(run.inspect());
    check(!progress_of(waiting, '5', ExecutionStatus::Pending).invocation && fixture.driver->calls(uuid<NodeId>('5')) == 0,
        "An explicit entry still waits for its real predecessor.");
    check(!progress_of(waiting, '8', ExecutionStatus::Pending).invocation,
        "The join remains pending while both selected branches are running.");
    edge(waiting, '4', ControlLinkStatus::Arrived);
    edge(waiting, 'c', ControlLinkStatus::Pending);

    take(right.completion.complete({right.identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
        {{ErrorCode::InvalidValue, Severity::Error, {}, "Selected branch failed."}}}));
    take(right.completion.mark_exited());
    const auto failed = wait_for(run, RunWaitTarget::Completion);
    check(failed.status == ExecutionStatus::Failed && !failed.quiescent, "A failed selected path ends the run while the other invocation drains.");
    check(result(run, progress_of(failed, '4', ExecutionStatus::Failed)).data().status == ExecutionStatus::Failed,
        "The selected failing branch keeps its failed terminal record.");
    const auto diagnostic = std::ranges::find(failed.diagnostics, ErrorCode::InvalidValue, &Diagnostic::code);
    check(diagnostic != failed.diagnostics.end() && diagnostic->location.node == uuid<NodeId>('4'),
        "The technical diagnostic identifies the selected failing branch.");
    skipped(failed, '8', NodeSkipReason::FailFast);
    skipped(failed, '9', NodeSkipReason::FailFast);
    edge(failed, '2', ControlLinkStatus::Closed, ControlCloseReason::NotSelected);
    for (const auto id : std::string_view("78bc")) { edge(failed, id, ControlLinkStatus::Closed, ControlCloseReason::Failed); }
    fixture.probe();
    check(fixture.driver->cancellations() == 1, "FailFast signals the outstanding cooperative branch once.");

    take(left.completion.complete({left.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
        {{key<OutputId>("reading"), DataValue::integer(77)}}, {}}));
    take(left.completion.mark_exited());
    const auto quiescent = wait_for(run);
    check(quiescent.status == ExecutionStatus::Failed && quiescent.quiescent, "Failure remains terminal after late backend cleanup.");
    const auto cancelled = result(run, progress_of(quiescent, '2', ExecutionStatus::Cancelled));
    const auto* reading = cancelled.output(key<OutputId>("reading"));
    const auto* missing = reading ? std::get_if<NotProduced>(&reading->content) : nullptr;
    check(missing && missing->reason == OutputAbsenceReason::Cancelled && fixture.driver->calls(uuid<NodeId>('9')) == 0,
        "The outstanding branch's late value cannot replace cancellation or trigger send.");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "branch_selection") { branch_selection(); }
        else if (test == "group_exports_and_debug") { group_exports_and_debug(); }
        else if (test == "branch_fail_fast") { branch_fail_fast(); }
        else { throw std::runtime_error("Unknown branch integration test."); }
        std::cout << test << ": " << checks << " checks, 0 failures\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
