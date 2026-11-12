#include <visionpro/workflow/tool_execution.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;

void check(bool condition, std::string_view message)
{
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* invalid = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(invalid->message); }
    return std::get<T>(std::move(result));
}

template<class T> Diagnostic rejected(const Result<T>& result, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, error_code_name(code));
    return *failure;
}

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000001";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<RuntimeIdentity T> T number(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }

InputDescriptor input(std::string_view name, DataType type, BindingCardinality cardinality = BindingCardinality::SingleSource)
{
    InputDescriptor descriptor{key<InputId>(name), std::string(name), std::move(type)};
    descriptor.binding.allowed_sources = {BindingSourceKind::RunOutput};
    descriptor.binding.cardinality = cardinality;
    return descriptor;
}

ConfigSnapshot snapshot()
{
    const auto integer = DataType::integer();
    const auto optional = DataType::optional(integer);
    const auto list = DataType::list(integer);
    const ToolDescriptor producer{key<ToolTypeId>("completion.producer"), "Producer", 1,
        {key<SchemaId>("completion.producer.schema"), 1}, {}, {},
        {{key<OutputId>("zero"), "Zero", integer}, {key<OutputId>("match"), "Match", optional},
         {key<OutputId>("samples"), "Samples", list},
         {key<OutputId>("conditional"), "Conditional", integer, {}, false, {OutputAbsenceReason::ConditionNotMet}}},
        {}};
    const ToolDescriptor consumer{key<ToolTypeId>("completion.consumer"), "Consumer", 1,
        {key<SchemaId>("completion.consumer.schema"), 1}, {},
        {input("allow", optional), input("other_reason", optional), input("merge", list, BindingCardinality::MultipleSources),
         input("elapsed", integer)}, {}, {}};
    const OutputReference conditional{uuid<WorkflowId>(), uuid<NodeId>(), key<OutputId>("conditional"), {}};
    const OutputReference zero{uuid<WorkflowId>(), uuid<NodeId>(), key<OutputId>("zero"), {}};
    const OutputReference elapsed{uuid<WorkflowId>(), uuid<NodeId>(), SystemField::Elapsed, {}};
    Workflow flow{uuid<WorkflowId>(), "Completion contract", {key<SchemaId>("completion.public"), 1},
        {{uuid<NodeId>(), "Producer", ToolConfiguration{producer.id, 1}},
         {uuid<NodeId>('2'), "Consumer", ToolConfiguration{consumer.id, 1}}},
        {{uuid<ControlLinkId>(), uuid<NodeId>(), {}, uuid<NodeId>('2')}}, {}, {uuid<NodeId>()}};
    flow.nodes[1].bindings = {
        {key<InputId>("allow"), {conditional}, BindingCardinality::SingleSource, ConversionKind::WrapOptional,
            {{MissingReason::ConditionNotMet}}},
        {key<InputId>("other_reason"), {conditional}, BindingCardinality::SingleSource, ConversionKind::WrapOptional,
            {{MissingReason::ToolDidNotProduce}}},
        {key<InputId>("merge"), {zero, conditional}, BindingCardinality::MultipleSources, ConversionKind::Exact,
            {{MissingReason::ConditionNotMet}}},
        {key<InputId>("elapsed"), {elapsed}}};
    const Solution solution{uuid<SolutionId>(), "Completion contract", 1, 1, {flow}};
    const std::vector<ToolDescriptor> tools{producer, consumer};
    return take(ConfigSnapshot::capture(solution, tools, {uuid<SolutionSessionId>(), number<ConfigSnapshotId>(1)}));
}

ResolutionContext context(const ConfigSnapshot& configuration, bool consumer)
{
    const RunIdentity run{configuration.identity(), uuid<WorkflowId>(), number<WorkflowRunId>(1)};
    const auto scope = number<ExecutionScopeId>(0);
    ResolutionContext call{run, {run.run, uuid<NodeId>(consumer ? '2' : '1'), scope, number<InvocationId>(consumer ? 2 : 1)},
        {{scope, {}, {}, {}}}, {}, {}, {}};
    if (consumer) { call.sources.push_back({uuid<NodeId>(), scope, number<InvocationId>(1)}); }
    return call;
}

PreparedInvocation prepared(const ConfigSnapshot& configuration, const ResultStore& store)
{
    const auto tool = take(PreparedTool::create(configuration, uuid<WorkflowId>(), uuid<NodeId>()));
    auto preparation = tool.prepare(context(configuration, false), number<AttemptId>(1), store);
    if (const auto* failure = std::get_if<InvocationFailure>(&preparation)) { throw std::runtime_error(failure->diagnostic.message); }
    return std::get<PreparedInvocation>(std::move(preparation));
}

struct Harness {
    ConfigSnapshot configuration{snapshot()};
    ResultStore store{configuration.identity().session};
    BindingResolver resolver{take(BindingResolver::create(configuration))};
    PreparedInvocation invocation{prepared(configuration, store)};
    ResolutionContext consumer{context(configuration, true)};

    ToolCompletion success() const
    {
        return {invocation.context.data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::NG,
            {{key<OutputId>("zero"), DataValue::integer(0)},
             {key<OutputId>("match"), DataValue::none(DataType::integer())},
             {key<OutputId>("samples"), take(DataValue::list(DataType::integer(), {}))},
             {key<OutputId>("conditional"), NotProduced{OutputAbsenceReason::ConditionNotMet}}}, {}, {}, {}};
    }

    NodeResult project(const ToolCompletion& completion) const
    { return take(checked_node_result(invocation, completion)); }

    BindingResolution read(std::string_view target) const
    { return resolver.resolve(consumer, key<InputId>(target), store); }
};

class ReportingTool final : public ToolInstance {
public:
    ReportingTool(ToolExecutionConfiguration configuration, ToolCompletion completion)
        : ToolInstance(std::move(configuration)), completion_(std::move(completion)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext>, InvocationCompletion) override
    { return InvocationStart{completion_}; }
    ToolCompletion completion_;
};

const OutputContent& output(const NodeResult& record, std::string_view name)
{
    const auto* slot = record.output(key<OutputId>(name));
    check(slot != nullptr, name);
    return slot->content;
}

void output_contract()
{
    Harness fixture;
    const auto completion = fixture.success();
    const auto result = fixture.project(completion);
    take(fixture.store.insert(fixture.configuration, result));
    check(result.data().status == ExecutionStatus::Succeeded && result.data().judgement == Judgement::NG,
        "No match can be a successful execution with NG.");
    check(std::get<DataValue>(output(result, "zero")) == DataValue::integer(0),
        "Zero remains an available integer.");
    check(!std::get<DataValue>(output(result, "match")).optional_value(),
        "Optional None remains available.");
    const auto* samples = std::get<DataValue>(output(result, "samples")).elements();
    check(samples && samples->empty(),
        "Empty List remains available.");
    check(std::get<NotProduced>(output(result, "conditional")).reason == OutputAbsenceReason::ConditionNotMet,
        "Legal omission keeps its output identity and declared reason.");
    const auto accepted = std::get<ResolvedBinding>(fixture.read("allow"));
    check(accepted.value == DataValue::none(DataType::integer()) && accepted.sources.size() == 1
        && accepted.sources.front().accepted_absence == MissingReason::ConditionNotMet,
        "Explicitly accepted tool omission yields None and keeps provenance.");
    const auto unaccepted = std::get<UnavailableBinding>(fixture.read("other_reason"));
    check(unaccepted.reason == OutputAbsenceReason::ConditionNotMet && unaccepted.diagnostic.code == ErrorCode::InputUnavailable,
        "A different declared consumer reason cannot accept the omission.");
    const auto merged = std::get<ResolvedBinding>(fixture.read("merge"));
    const auto* entries = merged.value.elements();
    check(entries && *entries == std::vector{DataValue::integer(0)} && merged.sources.size() == 2
        && merged.sources.back().accepted_absence == MissingReason::ConditionNotMet,
        "A collection keeps available zero and drops only the accepted omission.");

    auto bad = completion;
    bad.outputs.erase(bad.outputs.begin());
    const auto missing = rejected(checked_node_result(fixture.invocation, bad), ErrorCode::MissingRequiredValue);
    check(missing.location.output == key<OutputId>("zero"), "Missing required output is located.");
    bad = completion;
    bad.outputs.pop_back();
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::MissingRequiredValue);
    bad = completion;
    bad.outputs.back().content = NotProduced{OutputAbsenceReason::ToolDidNotProduce};
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
    bad = completion;
    bad.outputs.front().content = NotProduced{OutputAbsenceReason::ConditionNotMet};
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
    bad = completion;
    bad.outputs.front().content = DataValue::boolean(false);
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::TypeMismatch);

    auto descriptor = fixture.invocation.context.data().configuration.data().tool;
    descriptor.outputs.back().allowed_absence.clear();
    rejected(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor);
    descriptor.outputs.back().allowed_absence = {OutputAbsenceReason::NotSelected};
    rejected(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor);
    descriptor.outputs.back().allowed_absence = {OutputAbsenceReason::ConditionNotMet, OutputAbsenceReason::ConditionNotMet};
    rejected(validate_descriptor(descriptor), ErrorCode::InvalidDescriptor);
}

void invalid_output()
{
    Harness fixture;
    const auto provider = key<SchemaId>("vendor.fixture");
    const Diagnostic invalid{ErrorCode::InvalidValue, Severity::Error, {}, "External output status failed.",
        ExternalDiagnostic{provider, "readOutput", ExternalStatusDomain::Unknown, -37,
            ExternalCodeMapping::Unmapped, key<SchemaId>("vendor.unmapped"), "Unknown status", "1.0"}};
    auto bad = fixture.success();
    bad.outputs.back().content = InvalidOutput{invalid};
    const auto diagnostic = rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
    check(diagnostic.location.output == key<OutputId>("conditional") && diagnostic.external && diagnostic.external->raw_code == -37,
        "Invalid non-required output rejects success with the located original error.");

    auto instance = std::make_shared<ReportingTool>(fixture.invocation.context.data().configuration, bad);
    const auto operation = take(InvocationOperation::start(instance, fixture.invocation.context));
    const auto failed = operation.poll();
    check(failed.physically_exited && failed.completion && failed.completion->status == ToolCompletionStatus::Failed
        && failed.completion->outputs.empty() && failed.completion->diagnostics.size() == 1
        && failed.completion->diagnostics.front().external && failed.completion->diagnostics.front().external->provider == provider,
        "A contradictory synchronous completion fails the invocation and exposes no available outputs.");

    auto terminal = bad;
    terminal.status = ToolCompletionStatus::Failed;
    terminal.judgement = ToolJudgement::NotEvaluated;
    terminal.outputs.erase(terminal.outputs.begin(), terminal.outputs.end() - 1);
    const auto result = fixture.project(terminal);
    const auto& reported = std::get<InvalidOutput>(output(result, "conditional")).diagnostic;
    check(result.data().status == ExecutionStatus::Failed && reported.external
        && reported.external->provider_domain == key<SchemaId>("vendor.unmapped")
        && reported.location.output == key<OutputId>("conditional"), "A failed record retains output diagnostics and their domain.");
    check(std::ranges::none_of(result.data().outputs, [](const OutputSlot& slot) {
        return std::holds_alternative<DataValue>(slot.content);
    }), "Failed result cannot publish algorithm values.");
    take(fixture.store.insert(fixture.configuration, result));
    const auto failure = std::get<BindingFailure>(fixture.read("allow"));
    check(failure.diagnostic.code == ErrorCode::SourceFailed && failure.causes.size() == 1
        && failure.causes.front().external && failure.causes.front().external->raw_code == -37,
        "Optional consumers cannot turn Invalid into a missing value.");
}

void diagnostic_and_timing_contract()
{
    Harness fixture;
    auto completion = fixture.success();
    const auto provider = key<SchemaId>("vendor.fixture");
    completion.diagnostics = {
        {ErrorCode::InvalidValue, Severity::Info, {}, "Call status.",
            ExternalDiagnostic{provider, "execute", ExternalStatusDomain::Call, 0, ExternalCodeMapping::Mapped}},
        {ErrorCode::InvalidValue, Severity::Info, {}, "Output status.",
            ExternalDiagnostic{provider, "readOutput", ExternalStatusDomain::Output, 1, ExternalCodeMapping::Mapped}},
        {ErrorCode::InvalidValue, Severity::Info, {}, "Judgement.",
            ExternalDiagnostic{provider, "readJudgement", ExternalStatusDomain::Judgement, 0, ExternalCodeMapping::Mapped}}};
    completion.timings = {{TimingScope::ToolModule, TimingSource::ToolReported, TimingUnit::Unknown, 4.5, provider},
        {TimingScope::ToolAlgorithm, TimingSource::ToolReported, TimingUnit::Milliseconds, 0.0, provider},
        {TimingScope::QueueWait, TimingSource::RuntimeClock, TimingUnit::Nanoseconds}};
    const auto unmeasured = fixture.project(completion);
    const auto& data = unmeasured.data();
    check(data.timings.size() == 3 && !data.elapsed && !data.timings[2].value && data.timings[1].value == 0.0,
        "Unmeasured values are distinct from measured zero.");
    check(data.timings[0].unit == TimingUnit::Unknown && data.timings[0].value == 4.5
        && data.timings[0].scope == TimingScope::ToolModule && data.timings[1].scope == TimingScope::ToolAlgorithm,
        "Reported module and algorithm time retain separate scope and raw units.");
    check(data.diagnostics.size() == 3
        && std::ranges::all_of(data.diagnostics, [](const Diagnostic& item) { return item.external.has_value(); })
        && data.diagnostics[0].external->domain == ExternalStatusDomain::Call && data.diagnostics[0].external->raw_code == 0
        && data.diagnostics[1].external->domain == ExternalStatusDomain::Output && data.diagnostics[1].external->raw_code == 1
        && data.diagnostics[2].external->domain == ExternalStatusDomain::Judgement && data.judgement == Judgement::NG,
        "Status domains and original codes are preserved independently of the business judgement.");
    take(fixture.store.insert(fixture.configuration, unmeasured));
    const auto unavailable = std::get<BindingFailure>(fixture.read("elapsed"));
    check(unavailable.diagnostic.code == ErrorCode::InputUnavailable, "Unknown native duration cannot bind as zero.");

    completion.elapsed = std::chrono::nanoseconds{0};
    ResultStore zero_store(fixture.configuration.identity().session);
    take(zero_store.insert(fixture.configuration, fixture.project(completion)));
    const auto measured = fixture.resolver.resolve(fixture.consumer, key<InputId>("elapsed"), zero_store);
    check(std::get<ResolvedBinding>(measured).value == DataValue::integer(0), "Measured native zero remains bindable.");
    auto bad = completion;
    bad.timings.push_back(bad.timings.front());
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
    bad = completion;
    bad.timings.front().value = -1.0;
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
    bad = completion;
    bad.diagnostics.front().external->domain = ExternalStatusDomain::Unknown;
    rejected(checked_node_result(fixture.invocation, bad), ErrorCode::InvalidValue);
}

} // namespace

int main()
{
    try {
        output_contract();
        invalid_output();
        diagnostic_and_timing_contract();
    } catch (const std::exception& error) {
        std::cerr << "Completion contract failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
