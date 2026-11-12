#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;

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

enum class SourceMode { Available, Failed, InvalidHint, EmptyList };
using Call = ToolCompletion (*)(const InvocationContextData&);

class TestInstance final : public ToolInstance {
public:
    TestInstance(ToolExecutionConfiguration configuration, Call invoke, std::shared_ptr<std::vector<NodeId>> starts)
        : ToolInstance(std::move(configuration)), invoke_(invoke), starts_(std::move(starts)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        starts_->push_back(context->data().identity.invocation.node);
        return InvocationStart{invoke_(context->data())};
    }

    Call invoke_;
    std::shared_ptr<std::vector<NodeId>> starts_;
};

class TestFactory final : public ToolInstanceFactory {
public:
    TestFactory(Call invoke, std::shared_ptr<std::vector<NodeId>> starts)
        : invoke_(invoke), starts_(std::move(starts)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<TestInstance>(configuration, invoke_, starts_)}; }
private:
    Call invoke_;
    std::shared_ptr<std::vector<NodeId>> starts_;
};

const DataValue& input(const InvocationContextData& call, std::string_view name)
{
    const auto found = std::ranges::find(call.inputs, key<InputId>(name), &FixedToolInput::id);
    if (found == call.inputs.end()) { throw std::runtime_error("Tool did not receive a required fixture input."); }
    return found->value;
}

ToolCompletion source(const InvocationContextData& call)
{
    const auto mode = static_cast<SourceMode>(*input(call, "mode").as_int64());
    DiagnosticLocation where;
    where.node = call.identity.invocation.node;
    if (mode == SourceMode::Failed) {
        return {call.identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
            {{ErrorCode::InvalidValue, Severity::Error, where, "The fixture source failed."}}};
    }
    const auto& type = call.configuration.data().tool.outputs.front().type;
    std::vector<DataValue> readings;
    if (mode != SourceMode::EmptyList) { readings = {DataValue::integer(7), DataValue::integer(11)}; }
    auto samples = take(DataValue::list(DataType::integer(), readings));
    auto record = take(DataValue::record(type, {{key<FieldId>("readings"), std::move(samples)}}));
    const auto hint = key<OutputId>("hint");
    OutputContent content = NotProduced{OutputAbsenceReason::ConditionNotMet};
    if (mode == SourceMode::InvalidHint) {
        where.output = hint;
        content = InvalidOutput{{ErrorCode::InvalidValue, Severity::Error, where, "The fixture hint is invalid."}};
    }
    return {call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NG,
        {{key<OutputId>("samples"), std::move(record)},
         {key<OutputId>("match"), DataValue::none(DataType::integer())},
         {hint, std::move(content)}}, {}};
}

ToolCompletion bridge(const InvocationContextData& call)
{ return {call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}}; }

ToolCompletion sink(const InvocationContextData& call)
{
    const auto reading = *input(call, "reading").as_float64();
    const auto offset = *call.parameters.at(0).value.as_int64();
    const auto none = DataValue::none(DataType::integer());
    const bool accepted = input(call, "match") == none && input(call, "hint") == none;
    return {call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
        {{key<OutputId>("score"), take(DataValue::real(reading + static_cast<double>(offset)))},
         {key<OutputId>("both-absent"), DataValue::boolean(accepted)}}, {}};
}

InputDescriptor input_descriptor(std::string_view name, DataType type, BindingSourceKind source_kind)
{
    InputDescriptor descriptor{key<InputId>(name), std::string(name), std::move(type)};
    descriptor.binding.allowed_sources = {source_kind};
    return descriptor;
}

std::vector<ToolRegistration> registrations(const std::shared_ptr<std::vector<NodeId>>& starts)
{
    const auto integer = DataType::integer();
    const auto optional = DataType::optional(integer);
    const auto record = take(DataType::record(key<SchemaId>("s32.samples"), 1,
        {{key<FieldId>("readings"), DataType::list(integer)}}));
    ToolDescriptor first{key<ToolTypeId>("s32.source"), "Source", 1, {key<SchemaId>("s32.source.schema"), 1}, {},
        {input_descriptor("mode", integer, BindingSourceKind::PublicInput)},
        {{key<OutputId>("samples"), "Samples", record},
         {key<OutputId>("match"), "Match", optional},
         {key<OutputId>("hint"), "Hint", integer, {}, false, {OutputAbsenceReason::ConditionNotMet}}}};
    ToolDescriptor middle{key<ToolTypeId>("s32.bridge"), "Bridge", 1, {key<SchemaId>("s32.bridge.schema"), 1}};
    auto reading = input_descriptor("reading", DataType::real(), BindingSourceKind::RunOutput);
    reading.binding.allowed_conversions.push_back(ConversionKind::Int64ToFloat64Explicit);
    ParameterDescriptor offset{key<ParameterId>("offset"), "Offset", integer};
    offset.subscribable = true;
    offset.binding.allowed_sources = {BindingSourceKind::GlobalRead};
    ToolDescriptor last{key<ToolTypeId>("s32.sink"), "Sink", 1, {key<SchemaId>("s32.sink.schema"), 1}, {offset},
        {reading, input_descriptor("match", optional, BindingSourceKind::RunOutput),
         input_descriptor("hint", optional, BindingSourceKind::RunOutput)},
        {{key<OutputId>("score"), "Score", DataType::real()},
         {key<OutputId>("both-absent"), "Both inputs None", DataType::boolean()}}};
    return {{first, {first.schema.id, 1, 1, {}}, std::make_shared<TestFactory>(source, starts)},
        {middle, {middle.schema.id, 1, 1, {}}, std::make_shared<TestFactory>(bridge, starts)},
        {last, {last.schema.id, 1, 1, {}}, std::make_shared<TestFactory>(sink, starts)}};
}

Solution solution(bool allow_omission, std::uint64_t revision)
{
    const auto flow = uuid<WorkflowId>();
    const auto first = uuid<NodeId>('1');
    const auto middle = uuid<NodeId>('2');
    const auto last = uuid<NodeId>('3');
    const auto source_ref = [&](std::string_view name, Selector selector = {}) -> OutputReference {
        return {flow, first, key<OutputId>(name), std::move(selector)};
    };
    const AvailabilityPolicy omission{allow_omission ? std::vector{MissingReason::ConditionNotMet} : std::vector<MissingReason>{}};
    const auto mode = input_descriptor("mode", DataType::integer(), BindingSourceKind::Constant);
    Workflow pipeline{flow, "Pipeline", {key<SchemaId>("s32.pipeline"), 1, {mode}},
        {{last, "Sink", ToolConfiguration{key<ToolTypeId>("s32.sink"), 1}, {
            {key<InputId>("reading"), {source_ref("samples", {FieldSelection{key<FieldId>("readings")}, ElementSelection{1}})},
                BindingCardinality::SingleSource, ConversionKind::Int64ToFloat64Explicit},
            {key<InputId>("match"), {source_ref("match")}},
            {key<InputId>("hint"), {source_ref("hint")}, BindingCardinality::SingleSource, ConversionKind::WrapOptional, omission},
            {key<ParameterId>("offset"), {GlobalReference{uuid<VariableId>(), 7, {}}}}}},
         {middle, "Bridge", ToolConfiguration{key<ToolTypeId>("s32.bridge"), 1}},
         {first, "Source", ToolConfiguration{key<ToolTypeId>("s32.source"), 1},
            {{mode.id, {ScopedInputReference{flow, {}, mode.id, {}}}}}}},
        {{uuid<ControlLinkId>('2'), middle, {}, last}, {uuid<ControlLinkId>('1'), first, {}, middle}}, {}, {first}};
    return {uuid<SolutionId>(), "Execution test", 1, revision, {pipeline}, {},
        {{uuid<VariableId>(), "Offset", DataType::integer(), ConfigValue::integer(999)}}};
}

struct Fixture {
    std::shared_ptr<std::vector<NodeId>> starts{std::make_shared<std::vector<NodeId>>()};
    ToolRegistry registry{take(ToolRegistry::create(registrations(starts)))};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    IdSequence<ConfigSnapshotId> snapshots;

    RunHandle submit(SourceMode mode, bool allow_omission = true, std::optional<std::uint64_t> global_revision = 7)
    {
        const auto id = take(snapshots.next());
        const auto snapshot = take(ConfigSnapshot::capture(solution(allow_omission, id.value()), registry.descriptors(),
            {engine->session(), id}));
        std::vector<GlobalVariableEntry> globals;
        if (global_revision) {
            globals.push_back({uuid<VariableId>(), DataValue::integer(5), *global_revision,
                ExternalUpdateOrigin{engine->session(), key<SchemaId>("s32.sensor")}});
        }
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>()},
            {{key<InputId>("mode"), DataValue::integer(static_cast<std::int64_t>(mode))}},
            {TriggerKind::Manual, key<SchemaId>("s32.test")}, {}, {}, globals));
        globals.clear();
        auto submission = engine->submit(request);
        if (const auto* rejected = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error("Run rejected: " + rejected->diagnostic.message);
        }
        check(std::holds_alternative<RunHandle>(submission), "A quiescent workflow accepts another request.");
        return std::get<RunHandle>(std::move(submission));
    }
};

RunProgress finished(const RunHandle& run)
{
    const auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent, "The in-memory run must reach quiescence.");
    check(waited.progress.nodes.size() == 3, "The run reports all three planned nodes.");
    return waited.progress;
}

NodeResult result(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "A started node has an invocation key.");
    auto found = take(run.lookup(*node.invocation));
    check(found.has_value() && found->data().identity.run == run.identity()
        && found->data().identity.invocation == *node.invocation, "Lookup returns the exact committed invocation.");
    return *found;
}

const OutputContent& output(const NodeResult& result, std::string_view name)
{
    const auto* found = result.output(key<OutputId>(name));
    check(found != nullptr, name);
    return found->content;
}

const SourceTrace& trace(const RunNodeProgress& node, const BindingTarget& target)
{
    const auto found = std::ranges::find(node.bindings, target, &RunBindingTrace::target);
    check(found != node.bindings.end() && found->sources.size() == 1, "Binding keeps its one actual source.");
    return found->sources.front();
}

const Diagnostic& diagnostic(const RunProgress& progress, ErrorCode code)
{
    const auto found = std::ranges::find(progress.diagnostics, code, &Diagnostic::code);
    check(found != progress.diagnostics.end(), error_code_name(code));
    return *found;
}

void execution_chain()
{
    Fixture fixture;
    const auto run = fixture.submit(SourceMode::Available);
    const auto progress = finished(run);
    check(progress.status == ExecutionStatus::Succeeded && progress.diagnostics.empty(), "NG does not cause a technical failure.");
    const std::vector order{uuid<NodeId>('1'), uuid<NodeId>('2'), uuid<NodeId>('3')};
    check(*fixture.starts == order, "Actual tools start in control order independent of the editable node order.");
    std::set<InvocationId> calls;
    for (std::size_t index = 0; index < order.size(); ++index) {
        const auto& node = progress.nodes[index];
        const auto record = result(run, node);
        check(node.node == order[index] && node.status == ExecutionStatus::Succeeded
            && node.invocation->scope.value() == 0 && record.data().identity.attempt.value() == 0
            && calls.insert(node.invocation->invocation).second, "Each actual call has a distinct identity in the root scope and one attempt.");
    }
    const auto first = result(run, progress.nodes.front());
    check(first.data().judgement == Judgement::NG
        && std::get<DataValue>(output(first, "match")) == DataValue::none(DataType::integer())
        && std::get<NotProduced>(output(first, "hint")).reason == OutputAbsenceReason::ConditionNotMet,
        "The source publishes NG with an available None and an independent legal omission.");
    const auto& request_input = std::get<PublicInputTrace>(trace(progress.nodes.front(), key<InputId>("mode")).origin);
    check(request_input.run == run.identity() && request_input.origin == InputOrigin::Provided
        && request_input.reference.input == key<InputId>("mode"), "The source traces the current request input.");
    const auto last = result(run, progress.nodes.back());
    check(std::get<DataValue>(output(last, "score")) == take(DataValue::real(16.0))
        && std::get<DataValue>(output(last, "both-absent")) == DataValue::boolean(true),
        "The sink consumes the converted List element, frozen global and two optional values.");
    const auto& reading = std::get<ResultTrace>(trace(progress.nodes.back(), key<InputId>("reading")).origin);
    check(reading.run == run.identity() && reading.invocation == first.data().identity.invocation.invocation
        && reading.attempt == first.data().identity.attempt
        && reading.reference.selector == Selector{FieldSelection{key<FieldId>("readings")}, ElementSelection{1}},
        "The nonadjacent source trace identifies this run, producer call and selector.");
    const auto accepted = trace(progress.nodes.back(), key<InputId>("hint")).accepted_absence;
    check(accepted == MissingReason::ConditionNotMet
        && !trace(progress.nodes.back(), key<InputId>("match")).accepted_absence,
        "Only the explicitly accepted omission is recorded as absence.");
    const auto& global = std::get<GlobalReadTrace>(trace(progress.nodes.back(), key<ParameterId>("offset")).origin);
    check(global.variable == uuid<VariableId>() && global.revision == 7
        && std::get<ExternalUpdateOrigin>(global.update_origin).session == run.identity().snapshot.session,
        "The global trace retains the exact revision and update origin.");
}

void execution_fail_fast()
{
    Fixture fixture;
    const auto good = fixture.submit(SourceMode::Available);
    const auto good_progress = finished(good);
    const auto good_result = result(good, good_progress.nodes.back());
    check(good_progress.status == ExecutionStatus::Succeeded, "The first run publishes a usable result.");

    for (const auto mode : {SourceMode::Failed, SourceMode::InvalidHint}) {
        const auto before = fixture.starts->size();
        const auto bad = fixture.submit(mode);
        const auto progress = finished(bad);
        check(bad.identity().run != good.identity().run && progress.status == ExecutionStatus::Failed
            && fixture.starts->size() == before + 1 && fixture.starts->back() == uuid<NodeId>(),
            "The failed source stops both successors after an earlier successful run.");
        const auto& failure = diagnostic(progress, ErrorCode::InvalidValue);
        check(failure.location.node == uuid<NodeId>() && (mode != SourceMode::InvalidHint
            || failure.location.output == key<OutputId>("hint")), "Failure preserves the source and invalid output location.");
        const auto failed = result(bad, progress.nodes.front());
        check(failed.data().status == ExecutionStatus::Failed && std::ranges::none_of(failed.data().outputs,
            [](const OutputSlot& slot) { return std::holds_alternative<DataValue>(slot.content); }),
            "Failed calls retain a record without available algorithm output.");
        for (const auto& skipped : std::span{progress.nodes}.subspan(1)) {
            check(skipped.status == ExecutionStatus::Skipped && !skipped.invocation
                && skipped.skip_reason == NodeSkipReason::FailFast, "FailFast skips uncalled nodes without creating fake invocation keys.");
        }
        const auto previous = bad.lookup(good_result.data().identity.invocation);
        const auto* mismatch = std::get_if<Diagnostic>(&previous);
        check(mismatch && mismatch->code == ErrorCode::ResultIdentityMismatch, "A failed run cannot query the older result as its own.");
    }
    check(std::get<DataValue>(output(result(good, good_progress.nodes.back()), "score")) == take(DataValue::real(16.0)),
        "The older handle still owns its original result.");

    const auto blocked_sink = [&](SourceMode mode, bool accept, std::optional<std::uint64_t> revision, ErrorCode code) {
        const auto before = fixture.starts->size();
        const auto run = fixture.submit(mode, accept, revision);
        const auto progress = finished(run);
        check(progress.status == ExecutionStatus::Failed && progress.nodes.back().status == ExecutionStatus::Failed
            && !progress.nodes.back().invocation && fixture.starts->size() == before + 2,
            "Binding failure stops the sink before its invocation.");
        return diagnostic(progress, code);
    };
    const auto index = blocked_sink(SourceMode::EmptyList, true, 7, ErrorCode::IndexOutOfRange);
    check(index.location.input == key<InputId>("reading") && index.location.node == uuid<NodeId>('3'),
        "An available empty List reports the actual out-of-range input.");
    const auto omission = blocked_sink(SourceMode::Available, false, 7, ErrorCode::InputUnavailable);
    check(omission.location.input == key<InputId>("hint"), "Unaccepted omission does not silently become None.");
    const auto absent = blocked_sink(SourceMode::Available, true, {}, ErrorCode::GlobalUnavailable);
    check(absent.location.variable == uuid<VariableId>(), "Absent global does not use the definition's initial value.");
    const auto stale = blocked_sink(SourceMode::Available, true, 6, ErrorCode::GlobalRevisionMismatch);
    check(stale.location.variable == uuid<VariableId>(), "A different global revision is reported before the sink starts.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "execution_chain") { execution_chain(); }
        else if (test == "execution_fail_fast") { execution_fail_fast(); }
        else { throw std::runtime_error("Unknown workflow execution test group."); }
        std::cout << test << ": " << checks << " checks, 0 failures\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: " << failure.what() << '\n';
        return 1;
    }
}
