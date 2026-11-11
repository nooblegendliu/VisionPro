#include <visionpro/workflow/resolver.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
template<class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
template<UuidIdentity T> T uuid(unsigned value = 1)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - index - 1] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template<SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
template<RuntimeIdentity T> T number(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }
InputId input_id() { return key<InputId>("data"); }
OutputId output_id() { return key<OutputId>("value"); }
SnapshotIdentity snapshot_id(unsigned version = 1)
{ return {uuid<SolutionSessionId>(), number<ConfigSnapshotId>(version)}; }
Diagnostic cause(unsigned producer = 1)
{
    DiagnosticLocation where;
    where.solution = uuid<SolutionId>(); where.workflow = uuid<WorkflowId>();
    where.node = uuid<NodeId>(producer); where.output = output_id();
    where.path = {key<FieldId>("measurement"), DiagnosticElementIndex{2}};
    return {ErrorCode::InvalidValue, Severity::Error, where, "Artificial source failure"};
}

struct Fixture {
    Solution solution;
    std::vector<ToolDescriptor> tools;
    Workflow& flow() { return solution.workflows.front(); }
    Node& consumer() { return flow().nodes[1]; }
    InputDescriptor& input() { return tools[1].inputs.front(); }
    OutputReference output(unsigned node = 1, Selector selector = {})
    { return {flow().id, uuid<NodeId>(node), output_id(), std::move(selector)}; }
};
Fixture fixture(DataType output_type = DataType::integer(), DataType input_type = DataType::integer())
{
    ToolDescriptor producer{key<ToolTypeId>("resolver.producer"), "Producer", 1,
        {key<SchemaId>("resolver.producer.schema"), 1}, {}, {}, {{output_id(), "Value", output_type}}, {}};
    InputDescriptor input{input_id(), "Data", input_type};
    input.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput,
        BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead, BindingSourceKind::Resource};
    ToolDescriptor consumer{key<ToolTypeId>("resolver.consumer"), "Consumer", 1,
        {key<SchemaId>("resolver.consumer.schema"), 1}, {}, {input}, {}, {}};
    Workflow flow{uuid<WorkflowId>(), "Workflow", {key<SchemaId>("resolver.public"), 1},
        {{uuid<NodeId>(1), "First", ToolConfiguration{producer.id, 1}},
         {uuid<NodeId>(2), "Consumer", ToolConfiguration{consumer.id, 1}},
         {uuid<NodeId>(3), "Second", ToolConfiguration{producer.id, 1}}},
        {{uuid<ControlLinkId>(1), uuid<NodeId>(1), {}, uuid<NodeId>(2)},
         {uuid<ControlLinkId>(2), uuid<NodeId>(3), {}, uuid<NodeId>(2)}}, {}, {uuid<NodeId>(1), uuid<NodeId>(3)}};
    Fixture result{{uuid<SolutionId>(), "Resolver", 1, 1, {flow}}, {producer, consumer}};
    result.consumer().bindings = {{input_id(), {result.output()}}};
    return result;
}
struct Harness {
    ConfigSnapshot snapshot;
    BindingResolver resolver;
    ResultStore results;
    ResolutionContext context;
    explicit Harness(const Fixture& f, SnapshotIdentity identity = snapshot_id())
        : snapshot(take(ConfigSnapshot::capture(f.solution, f.tools, identity))),
          resolver(take(BindingResolver::create(snapshot))), results(identity.session),
          context{{identity, f.solution.workflows.front().id, number<WorkflowRunId>(1)},
              {number<WorkflowRunId>(1), uuid<NodeId>(2), number<ExecutionScopeId>(0), number<InvocationId>(2)},
              {{number<ExecutionScopeId>(0), {}, {}, {}}}, {},
              {{uuid<NodeId>(1), number<ExecutionScopeId>(0), number<InvocationId>(1)},
               {uuid<NodeId>(3), number<ExecutionScopeId>(0), number<InvocationId>(3)}}, {}} {}
    NodeResult record(OutputContent content, unsigned producer = 1,
        ExecutionStatus status = ExecutionStatus::Succeeded, unsigned scope = 0, unsigned invocation = 0,
        Judgement judgement = Judgement::OK) const
    {
        const auto logical_call = invocation == 0 ? producer : invocation;
        ResultIdentity identity{context.run,
            {context.run.run, uuid<NodeId>(producer), number<ExecutionScopeId>(scope), number<InvocationId>(logical_call)},
            number<AttemptId>(0)};
        std::vector<Diagnostic> diagnostics;
        if (status != ExecutionStatus::Succeeded) { diagnostics.push_back(cause(producer)); }
        const auto type = snapshot.find_tool(key<ToolTypeId>("resolver.producer"))->outputs.front().type;
        return take(NodeResult::create({identity, status,
            status == ExecutionStatus::Succeeded ? judgement : Judgement::NotEvaluated,
            {{output_id(), type, std::move(content)}}, diagnostics, std::chrono::nanoseconds{1234}}));
    }
    void put(OutputContent content, unsigned producer = 1, ExecutionStatus status = ExecutionStatus::Succeeded,
        unsigned scope = 0, unsigned invocation = 0, Judgement judgement = Judgement::OK)
    { take(results.insert(snapshot, record(std::move(content), producer, status, scope, invocation, judgement))); }
    BindingResolution resolve(BindingTarget target = input_id()) const
    { return resolver.resolve(context, target, results); }
};
const ResolvedBinding* resolved(const BindingResolution& value, std::string_view description)
{
    const auto* result = std::get_if<ResolvedBinding>(&value);
    check(result != nullptr, description);
    return result;
}
void integer(const BindingResolution& value, std::int64_t expected, std::string_view description)
{
    if (const auto* result = resolved(value, description)) {
        check(result->value.as_int64() && *result->value.as_int64() == expected, "resolved integer has the expected value");
    }
}
void failure(const BindingResolution& value, std::string_view description)
{
    const auto* result = std::get_if<BindingFailure>(&value);
    check(result != nullptr, description);
    if (result) {
        check(result->diagnostic.severity == Severity::Error && !result->diagnostic.message.empty(), "binding failure contains an explained diagnostic");
    }
}

void resolver_values()
{
    auto f = fixture();
    f.consumer().bindings[0].sources = {ConfigValue::integer(0)};
    Harness constant(f);
    const auto constant_result = constant.resolve(); integer(constant_result, 0, "zero constant is a real value");
    if (const auto* value = std::get_if<ResolvedBinding>(&constant_result)) {
        check(value->sources.size() == 1 && std::holds_alternative<ConstantTrace>(value->sources[0].origin)
            && !std::get<ConstantTrace>(value->sources[0].origin).defaulted, "constant origin is traced separately from a default");
    }
    f.consumer().bindings.clear(); f.input().required = false; f.input().default_value = ConfigValue::integer(17);
    Harness defaulted(f); const auto default_result = defaulted.resolve(); integer(default_result, 17, "omitted binding resolves its declared default");
    if (const auto* value = std::get_if<ResolvedBinding>(&default_result)) {
        check(value->sources.size() == 1 && std::get<ConstantTrace>(value->sources[0].origin).defaulted, "default provenance remains explicit");
    }
    f.consumer().bindings = {{input_id(), {f.output()}}};
    Harness no_fallback(f);
    check(std::holds_alternative<PendingBinding>(no_fallback.resolve()), "pending configured source does not use target default");
    no_fallback.put(DataValue::integer(0), 1, ExecutionStatus::Succeeded, 0, 0, Judgement::NG);
    const auto output_result = no_fallback.resolve(); integer(output_result, 0, "NG does not invalidate a technically successful output");
    if (const auto* value = std::get_if<ResolvedBinding>(&output_result)) {
        const auto* trace = value->sources.empty() ? nullptr : std::get_if<ResultTrace>(&value->sources.front().origin);
        check(trace && trace->run == no_fallback.context.run && trace->scope == number<ExecutionScopeId>(0)
            && trace->invocation == number<InvocationId>(1) && trace->attempt == number<AttemptId>(0), "result trace records the exact run, scope, invocation and attempt");
    }
    failure(no_fallback.resolve(key<InputId>("unknown")), "unknown consumer input is rejected");

    const auto list = DataType::list(DataType::integer());
    const auto record_type = take(DataType::record(key<SchemaId>("resolver.record"), 1, {{key<FieldId>("items"), list}}));
    f = fixture(record_type);
    f.consumer().bindings[0].sources = {f.output(1, {FieldSelection{key<FieldId>("items")}, ElementSelection{1}})};
    Harness selected(f);
    selected.put(take(DataValue::record(record_type, {{key<FieldId>("items"), take(DataValue::list(DataType::integer(),
        {DataValue::integer(4), DataValue::integer(9)}))}})));
    integer(selected.resolve(), 9, "field then fixed index selects the current result value");
    Harness empty(f);
    empty.put(take(DataValue::record(record_type, {{key<FieldId>("items"), take(DataValue::list(DataType::integer(), {}))}})));
    const auto bounds = empty.resolve(); failure(bounds, "empty list index is an error, not absence");
    if (const auto* failed = std::get_if<BindingFailure>(&bounds)) {
        check(failed->diagnostic.location.node == uuid<NodeId>(2)
            && failed->diagnostic.location.input == input_id() && !failed->diagnostic.location.path.empty(), "selector failure locates consumer target and structured path");
    }

    f = fixture(DataType::integer(), DataType::optional(DataType::integer()));
    f.consumer().bindings[0].conversion = ConversionKind::WrapOptional;
    Harness wrapped(f); wrapped.put(DataValue::integer(8));
    const auto wrapped_value = wrapped.resolve();
    if (const auto* result = resolved(wrapped_value, "declared Optional wrapping is performed")) {
        check(result->value == DataValue::some(DataValue::integer(8)), "Optional wrapping retains the source integer");
    }
    f = fixture(DataType::integer(), DataType::real());
    f.input().binding.allowed_conversions.push_back(ConversionKind::Int64ToFloat64Explicit);
    f.consumer().bindings[0].conversion = ConversionKind::Int64ToFloat64Explicit;
    Harness converted(f); converted.put(DataValue::integer(12));
    const auto converted_value = converted.resolve();
    if (const auto* result = resolved(converted_value, "explicit numeric conversion is performed")) {
        check(result->value.as_float64() && *result->value.as_float64() == 12.0, "converted real retains the expected numeric value");
    }
    f = fixture(); f.input().constraints.integer_range = IntegerRange{1, 10};
    Harness constrained(f); constrained.put(DataValue::integer(11));
    failure(constrained.resolve(), "runtime range constraints are checked after source lookup");

    f = fixture(DataType::integer(), DataType::list(DataType::integer()));
    f.input().binding.cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].sources = {f.output(3), ConfigValue::integer(7), f.output(1)};
    Harness many(f); many.put(DataValue::integer(2)); many.put(DataValue::integer(5), 3);
    const auto many_value = many.resolve();
    if (const auto* result = resolved(many_value, "multiple sources aggregate in configured order")) {
        const auto* elements = result->value.elements();
        check(elements && *elements == std::vector{DataValue::integer(5), DataValue::integer(7), DataValue::integer(2)},
            "aggregation does not reorder by execution or node identity");
        check(result->sources.size() == 3 && result->sources[2].source_index == 2, "each aggregate source retains its configured index");
    }
    f = fixture(list, list); f.input().binding.cardinality = BindingCardinality::CollectionSource;
    f.consumer().bindings[0].cardinality = BindingCardinality::CollectionSource;
    Harness collection(f); collection.put(take(DataValue::list(DataType::integer(), {})));
    const auto empty_collection = collection.resolve();
    if (const auto* result = resolved(empty_collection, "empty collection is a valid collection source")) {
        check(result->value.elements() && result->value.elements()->empty(), "empty collection is preserved without manufactured elements");
    }
    f.input().constraints.max_length = 2;
    Harness collection_limit(f);
    collection_limit.put(take(DataValue::list(DataType::integer(),
        {DataValue::integer(1), DataValue::integer(2), DataValue::integer(3)})));
    failure(collection_limit.resolve(), "runtime collection length obeys the target limit");
    Harness collection_boundary(f);
    collection_boundary.put(take(DataValue::list(DataType::integer(), {DataValue::integer(1), DataValue::integer(2)})));
    check(std::holds_alternative<ResolvedBinding>(collection_boundary.resolve()), "runtime collection length accepts its exact boundary");

    f = fixture(DataType::optional(DataType::integer()), DataType::optional(DataType::integer()));
    Harness explicit_none(f); explicit_none.put(DataValue::none(DataType::integer()));
    const auto none_value = explicit_none.resolve();
    if (const auto* result = resolved(none_value, "published Optional None is an available value")) {
        check(!result->value.optional_value() && result->sources.size() == 1 && !result->sources[0].accepted_absence,
            "published None is distinguished from an accepted missing source");
    }

    f = fixture();
    auto parameter = ParameterDescriptor{key<ParameterId>("threshold"), "Threshold", DataType::integer()};
    parameter.subscribable = true; parameter.binding.allowed_sources = {BindingSourceKind::RunOutput};
    f.tools[1].parameters.push_back(parameter);
    f.consumer().bindings.push_back({parameter.id, {f.output()}});
    Harness subscribed(f); subscribed.put(DataValue::integer(6));
    integer(subscribed.resolve(parameter.id), 6, "subscribable parameter shares the input resolver");
}

void resolver_availability()
{
    auto f = fixture(); Harness pending(f);
    check(std::holds_alternative<PendingBinding>(pending.resolve()), "selected call without a record remains pending");
    pending.context.sources[0].state = AwaitingSource{};
    check(std::holds_alternative<PendingBinding>(pending.resolve()), "source without a call identity remains pending");
    pending.context.sources[0].state = MissingReason::NotSelected;
    check(std::holds_alternative<UnavailableBinding>(pending.resolve()), "required unselected source is unavailable");

    f = fixture(DataType::integer(), DataType::optional(DataType::integer()));
    f.consumer().bindings[0].conversion = ConversionKind::WrapOptional;
    f.consumer().bindings[0].availability.accepted_absence = {MissingReason::NotSelected, MissingReason::Disabled};
    Harness absent(f); absent.context.sources[0].state = MissingReason::NotSelected;
    const auto absent_value = absent.resolve();
    if (const auto* result = resolved(absent_value, "accepted unselected source becomes Optional None")) {
        check(result->value.type() == f.input().type && !result->value.optional_value(), "absence is a correctly typed None");
        check(result->sources.size() == 1 && result->sources[0].accepted_absence == MissingReason::NotSelected, "accepted absence preserves its reason");
    }
    absent.context.sources[0].state = AwaitingSource{};
    check(std::holds_alternative<PendingBinding>(absent.resolve()), "Optional and accepted absence never swallow pending work");
    f.flow().nodes[0].enabled = false;
    Harness disabled(f); disabled.context.sources[0].state = MissingReason::Disabled;
    const auto disabled_value = disabled.resolve();
    if (const auto* result = resolved(disabled_value, "explicitly accepted disabled producer becomes None")) {
        check(!result->value.optional_value(), "disabled output is not replaced by history");
    }
    f.flow().nodes[0].enabled = true;
    for (const auto status : {ExecutionStatus::Failed, ExecutionStatus::Cancelled, ExecutionStatus::TimedOut}) {
        Harness failed(f);
        const auto reason = status == ExecutionStatus::Failed ? OutputAbsenceReason::Failed
            : status == ExecutionStatus::Cancelled ? OutputAbsenceReason::Cancelled : OutputAbsenceReason::TimedOut;
        failed.put(NotProduced{reason}, 1, status);
        const auto resolved_failure = failed.resolve();
        failure(resolved_failure, "Optional does not swallow failed, cancelled or timed-out source calls");
        if (const auto* result = std::get_if<BindingFailure>(&resolved_failure)) {
            const auto expected = status == ExecutionStatus::Failed ? ErrorCode::SourceFailed
                : status == ExecutionStatus::Cancelled ? ErrorCode::SourceCancelled : ErrorCode::SourceTimedOut;
            check(result->diagnostic.code == expected, "failed, cancelled and timed-out calls keep distinct resolver error codes");
            check(result->diagnostic.location.node == uuid<NodeId>(2) && result->diagnostic.location.input == input_id(),
                "failure summary locates the consumer binding");
            const auto original = cause();
            check(result->causes.size() == 1 && result->causes[0].code == original.code
                && result->causes[0].message == original.message && result->causes[0].location.node == original.location.node
                && result->causes[0].location.workflow == original.location.workflow
                && result->causes[0].location.output == original.location.output && result->causes[0].location.path == original.location.path,
                "source diagnostic preserves its original error code, message and producer path");
        }
    }
    Harness invalid(f); invalid.put(InvalidOutput{cause()}, 1, ExecutionStatus::Failed);
    failure(invalid.resolve(), "invalid output retains failure semantics despite Optional target");
    Harness not_produced(f);
    const auto incomplete = not_produced.record(NotProduced{OutputAbsenceReason::ToolDidNotProduce});
    const auto rejected = not_produced.results.insert(not_produced.snapshot, incomplete);
    const auto* invalid_record = std::get_if<Diagnostic>(&rejected);
    check(invalid_record && invalid_record->code == ErrorCode::InvalidNodeResult,
        "successful call without a required available output is rejected by the result store");
    check(not_produced.results.size() == 0, "rejected incomplete output never becomes a resolvable record");

    f = fixture(DataType::integer(), DataType::list(DataType::optional(DataType::integer())));
    f.input().binding.cardinality = BindingCardinality::MultipleSources;
    auto& binding = f.consumer().bindings[0]; binding.cardinality = BindingCardinality::MultipleSources;
    binding.conversion = ConversionKind::WrapOptional; binding.sources = {f.output(1), f.output(3)};
    binding.availability.accepted_absence = {MissingReason::NotSelected};
    Harness aggregate(f); aggregate.context.sources[0].state = MissingReason::NotSelected;
    aggregate.put(DataValue::integer(3), 3);
    const auto aggregate_value = aggregate.resolve();
    if (const auto* result = resolved(aggregate_value, "multiple-source absence omits that source even for Optional elements")) {
        const auto* elements = result->value.elements();
        check(elements && *elements == std::vector{DataValue::some(DataValue::integer(3))},
            "missing source is omitted, not replaced with an extra None element");
        check(result->sources.size() == 2 && result->sources[0].accepted_absence == MissingReason::NotSelected, "omitted source still has provenance");
    }

    f = fixture(DataType::integer(), DataType::list(DataType::integer()));
    f.input().binding.cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].sources = {f.output(1), f.output(3)};
    Harness priority(f); priority.context.sources[0].state = AwaitingSource{};
    priority.context.sources[1].state = MissingReason::NotSelected;
    check(std::holds_alternative<UnavailableBinding>(priority.resolve()), "unavailable source takes priority over pending source");
    priority.context.sources[1].state = number<InvocationId>(3);
    priority.put(NotProduced{OutputAbsenceReason::Failed}, 3, ExecutionStatus::Failed);
    const auto priority_value = priority.resolve();
    failure(priority_value, "later failure takes priority over earlier pending source");
    if (const auto* result = std::get_if<BindingFailure>(&priority_value)) {
        check(result->diagnostic.code == ErrorCode::SourceFailed && result->sources.size() == 2
            && result->causes.size() == 1 && result->causes[0].location.node == uuid<NodeId>(3),
            "aggregate checks the later error and keeps both source traces despite earlier pending work");
    }
    priority.context.sources[0].state = MissingReason::NotSelected;
    failure(priority.resolve(), "failure takes priority over earlier unavailable source");

    for (const auto field : {SystemField::ExecutionStatus, SystemField::Diagnostics, SystemField::Elapsed, SystemField::Judgement}) {
        f = fixture(DataType::integer(), take(system_field_type(field)));
        auto reference = f.output(); reference.output = field;
        f.consumer().bindings[0].sources = {reference};
        Harness system(f);
        check(std::holds_alternative<PendingBinding>(system.resolve()), "system field remains pending without a terminal record");
        system.put(NotProduced{OutputAbsenceReason::Failed}, 1, ExecutionStatus::Failed);
        const auto system_value = system.resolve();
        if (const auto* result = resolved(system_value, "system fields remain inspectable for failed terminal calls")) {
            check(result->value.type() == f.input().type, "system projection matches its public Schema");
            if (field == SystemField::ExecutionStatus) {
                check(result->value.as_enum() && *result->value.as_enum() == key<EnumMemberId>("Failed"), "system status reports Failed");
            }
            if (field == SystemField::Elapsed) { check(result->value == DataValue::integer(1234), "system elapsed uses nanoseconds"); }
            if (field == SystemField::Diagnostics) {
                check(result->value.elements() && result->value.elements()->size() == 1, "system diagnostic projection preserves source error record");
            }
        }
    }
}

void resolver_isolation()
{
    auto f = fixture(); Harness h(f); h.put(DataValue::integer(41));
    integer(h.resolve(), 41, "current selected invocation reads its own value");
    h.context.sources[0].state = number<InvocationId>(9);
    check(std::holds_alternative<PendingBinding>(h.resolve()), "new invocation never falls back to the node's old invocation");
    h.context.sources[0].state = number<InvocationId>(1);
    h.context.run.run = number<WorkflowRunId>(2); h.context.consumer.workflow_run = h.context.run.run;
    check(std::holds_alternative<PendingBinding>(h.resolve()), "new run never reads the prior run's completed output");
    h.context.consumer.workflow_run = number<WorkflowRunId>(1);
    failure(h.resolve(), "consumer and run identities must agree");
    h.context.consumer.workflow_run = h.context.run.run;
    h.context.run.snapshot.snapshot = number<ConfigSnapshotId>(2);
    failure(h.resolve(), "resolver rejects a different configuration snapshot");
    h.context.run.snapshot = snapshot_id(); h.context.run.snapshot.session = uuid<SolutionSessionId>(2);
    failure(h.resolve(), "resolver rejects a different solution session");
    Harness session(f); ResultStore foreign(uuid<SolutionSessionId>(2));
    failure(session.resolver.resolve(session.context, input_id(), foreign), "result store cannot cross session ownership");

    Harness scoped(f); scoped.put(DataValue::integer(18));
    scoped.context.scopes.push_back({number<ExecutionScopeId>(1), number<ExecutionScopeId>(0), {}, {}});
    scoped.context.scopes.push_back({number<ExecutionScopeId>(2), number<ExecutionScopeId>(0), {}, {}});
    scoped.context.consumer.scope = number<ExecutionScopeId>(1);
    failure(scoped.resolve(), "ancestor result requires explicitly declared outer visibility");
    scoped.context.visible_outer_scopes = {number<ExecutionScopeId>(0)};
    integer(scoped.resolve(), 18, "explicitly visible ancestor in the same static scope can be read");
    scoped.context.sources[0].scope = number<ExecutionScopeId>(2);
    scoped.context.visible_outer_scopes.push_back(number<ExecutionScopeId>(2));
    failure(scoped.resolve(), "listing a sibling as visible does not grant ancestor access");
    scoped.context.visible_outer_scopes = {number<ExecutionScopeId>(0)};
    scoped.context.sources[0].scope = number<ExecutionScopeId>(1);
    check(std::holds_alternative<PendingBinding>(scoped.resolve()), "same node in a new scope does not read the ancestor without selecting it");
    scoped.put(DataValue::integer(27), 1, ExecutionStatus::Succeeded, 1);
    integer(scoped.resolve(), 27, "current scope resolves its distinct invocation record");
    scoped.context.scopes.push_back(scoped.context.scopes.back());
    failure(scoped.resolve(), "duplicate runtime scope identities are rejected");

    Harness malformed(f); malformed.context.sources.push_back(malformed.context.sources.front());
    failure(malformed.resolve(), "duplicate source selections cannot arbitrarily choose a record");
    malformed.context.sources.pop_back(); malformed.context.sources.erase(malformed.context.sources.begin());
    failure(malformed.resolve(), "missing explicit source selection is not inferred from stored history");
    Harness snapshot(f); f.consumer().bindings[0].sources = {ConfigValue::integer(88)};
    snapshot.put(DataValue::integer(5));
    integer(snapshot.resolve(), 5, "editing the source model cannot alter a resolver's captured binding");

    f = fixture(); Harness retired_pending(f);
    retired_pending.context.sources[0].state = AwaitingSource{};
    take(retired_pending.results.release_run(retired_pending.context.run));
    const auto retired_wait = retired_pending.resolve();
    failure(retired_wait, "retired run is rejected before an Awaiting source can return Pending");
    if (const auto* result = std::get_if<BindingFailure>(&retired_wait)) {
        check(result->diagnostic.code == ErrorCode::ResultRetired, "retired awaiting context has explicit retirement diagnostic");
    }
    f.consumer().bindings[0].sources = {ConfigValue::integer(73)};
    Harness retired_constant(f);
    take(retired_constant.results.release_run(retired_constant.context.run));
    const auto retired_value = retired_constant.resolve();
    failure(retired_value, "constant binding cannot bypass retirement of its run");
    if (const auto* result = std::get_if<BindingFailure>(&retired_value)) {
        check(result->diagnostic.code == ErrorCode::ResultRetired, "retired constant context has explicit retirement diagnostic");
    }
}

void resolver_domains()
{
    auto f = fixture();
    InputDescriptor public_input{key<InputId>("public"), "Public", DataType::integer(), false, ConfigValue::integer(7)};
    f.flow().interface.inputs = {public_input};
    f.consumer().bindings[0].sources = {ScopedInputReference{f.flow().id, {}, public_input.id, {}}};
    Harness public_values(f); const auto default_value = public_values.resolve();
    integer(default_value, 7, "missing public input uses its declared default");
    if (const auto* value = std::get_if<ResolvedBinding>(&default_value)) {
        const auto* trace = value->sources.empty() ? nullptr : std::get_if<PublicInputTrace>(&value->sources[0].origin);
        check(trace && trace->origin == InputOrigin::Default && trace->scope == number<ExecutionScopeId>(0), "public input default traces its owning scope");
    }
    public_values.context.scopes[0].inputs = {{public_input.id, DataValue::integer(9)}};
    integer(public_values.resolve(), 9, "current public input overrides the default for this context only");
    public_values.context.scopes[0].inputs[0].value = DataValue::boolean(false);
    failure(public_values.resolve(), "wrong public input type does not fall back to default");
    public_values.context.scopes[0].inputs = {{public_input.id, DataValue::integer(9)}, {public_input.id, DataValue::integer(10)}};
    failure(public_values.resolve(), "duplicate public inputs are rejected");

    f = fixture();
    const auto group_id = uuid<NodeId>(4);
    const auto other_group_id = uuid<NodeId>(5);
    auto group_input = public_input;
    group_input.required = true; group_input.default_value.reset();
    f.flow().interface.inputs = {public_input};
    f.consumer().bindings[0].sources = {ScopedInputReference{f.flow().id, group_id, public_input.id, {}}};
    f.flow().nodes.push_back({group_id, "Group", GroupConfiguration{
        {key<SchemaId>("resolver.group"), 1, {group_input}}, {uuid<NodeId>(1), uuid<NodeId>(3)}, {}, {}},
        {{public_input.id, {ConfigValue::integer(1)}}}});
    f.flow().nodes.push_back({other_group_id, "Other group", GroupConfiguration{
        {key<SchemaId>("resolver.other_group"), 1}, {}, {}, {}}});
    f.flow().containment = {{group_id, uuid<NodeId>(1)}, {group_id, uuid<NodeId>(2)}, {group_id, uuid<NodeId>(3)}};
    f.flow().entries = {group_id};
    Harness grouped(f);
    grouped.context.scopes = {
        {number<ExecutionScopeId>(0), {}, {}, {{public_input.id, DataValue::integer(99)}}},
        {number<ExecutionScopeId>(1), number<ExecutionScopeId>(0), group_id, {{public_input.id, DataValue::integer(11)}}},
        {number<ExecutionScopeId>(2), number<ExecutionScopeId>(0), group_id, {{public_input.id, DataValue::integer(22)}}},
        {number<ExecutionScopeId>(3), number<ExecutionScopeId>(0), other_group_id, {}}};
    grouped.context.consumer.scope = number<ExecutionScopeId>(1);
    for (auto& selected : grouped.context.sources) { selected.scope = number<ExecutionScopeId>(1); }
    integer(grouped.resolve(), 11, "Group public input resolves from the consumer's first runtime scope");
    grouped.context.consumer.scope = number<ExecutionScopeId>(2);
    for (auto& selected : grouped.context.sources) { selected.scope = number<ExecutionScopeId>(2); }
    const auto group_second = grouped.resolve();
    integer(group_second, 22, "same Group identity has independent inputs in a second runtime scope");
    if (const auto* value = std::get_if<ResolvedBinding>(&group_second)) {
        const auto* trace = value->sources.empty() ? nullptr : std::get_if<PublicInputTrace>(&value->sources[0].origin);
        check(trace && trace->scope == number<ExecutionScopeId>(2) && trace->reference.group == group_id,
            "Group public-input provenance records both static Group and current runtime scope");
    }
    grouped.context.scopes[2].inputs.clear();
    failure(grouped.resolve(), "missing Group input cannot fall back to an ancestor or another invocation of that Group");
    grouped.context.scopes[2].inputs = {{public_input.id, DataValue::integer(22)}};
    grouped.context.visible_outer_scopes = {number<ExecutionScopeId>(0)};
    failure(grouped.resolve(), "outer visibility does not permit crossing the Group public-input boundary");
    grouped.context.visible_outer_scopes.clear();
    grouped.context.sources[0].scope = number<ExecutionScopeId>(3);
    failure(grouped.resolve(), "source selection cannot use a runtime scope owned by another Group");
    auto cross_boundary = f;
    cross_boundary.consumer().bindings[0].sources = {ScopedInputReference{f.flow().id, {}, public_input.id, {}}};
    const auto cross_snapshot = take(ConfigSnapshot::capture(cross_boundary.solution, cross_boundary.tools, snapshot_id()));
    const auto cross_resolver = BindingResolver::create(cross_snapshot);
    const auto* cross_error = std::get_if<Diagnostic>(&cross_resolver);
    check(cross_error && cross_error->code == ErrorCode::ScopeViolation,
        "resolver construction rejects a Group node directly bound to its ancestor's public input");

    f = fixture(); const auto variable = uuid<VariableId>();
    f.solution.variables.push_back({variable, "Current", DataType::integer(), ConfigValue::integer(99)});
    f.consumer().bindings[0].sources = {GlobalReference{variable, 5, {}}};
    Harness global(f);
    failure(global.resolve(), "missing global read snapshot never falls back to the configured initial value");
    const auto origin = ExternalUpdateOrigin{snapshot_id().session, key<SchemaId>("test.update")};
    global.context.globals.push_back({variable, DataValue::integer(42), 5, origin});
    const auto global_value = global.resolve(); integer(global_value, 42, "global source reads supplied immutable value snapshot");
    if (const auto* value = std::get_if<ResolvedBinding>(&global_value)) {
        const auto* trace = value->sources.empty() ? nullptr : std::get_if<GlobalReadTrace>(&value->sources[0].origin);
        check(trace && trace->variable == variable && trace->revision == 5, "global revision and source identity are traced");
    }
    check(global.context.globals[0].revision == 5 && *global.context.globals[0].current_value.as_int64() == 42,
        "resolution does not mutate the supplied global snapshot");
    global.context.globals[0].revision = 6;
    failure(global.resolve(), "pinned global revision does not silently read another revision");

    f = fixture(DataType::integer(), DataType::resource_reference());
    const ResourceReference reference{uuid<ResourceId>(), ResourceKind::Binary, 7};
    f.solution.resources.push_back({reference.id, "Binary", reference.kind,
        ManagedResource{{PathBase::SolutionDirectory, "never-opened.bin", {}}, 7, std::string(64, 'a')}});
    f.consumer().bindings[0].sources = {reference};
    Harness resource(f); const auto resource_value = resource.resolve();
    if (const auto* value = resolved(resource_value, "resource binding returns its validated reference without I/O")) {
        check(value->value.as_resource_reference() && *value->value.as_resource_reference() == reference,
            "resource identity, kind and pinned revision remain intact");
        check(value->sources.size() == 1 && std::holds_alternative<ResourceReference>(value->sources[0].origin),
            "resource binding records resource provenance");
    }

    const auto frame = key<CoordinateSystemId>("camera");
    f = fixture(DataType::region2d(), DataType::region2d());
    f.solution.coordinates.push_back({frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    f.tools[0].outputs[0].semantics = {MeasurementUnit::Pixel, frame};
    f.input().semantics = {MeasurementUnit::Pixel, frame};
    const InheritedRegion inherited{f.flow().id, uuid<NodeId>(1), output_id(), frame, MeasurementUnit::Pixel};
    const RegionDefinition symbolic{uuid<RegionId>(1), frame, MeasurementUnit::Pixel, inherited, {}};
    f.consumer().bindings[0].sources = {take(ConfigValue::region2d(symbolic))};
    Harness roi(f);
    check(std::holds_alternative<PendingBinding>(roi.resolve()), "symbolic ROI remains pending until its selected source result is available");
    RegionDefinition published{uuid<RegionId>(2), frame, MeasurementUnit::Pixel,
        RegionPrimitive{frame, MeasurementUnit::Pixel, RotatedRect{8, 9, 4, 3, 0.3}},
        {RegionPrimitive{frame, MeasurementUnit::Pixel, CircleRegion{8, 9, 0.5}}}};
    roi.put(take(DataValue::region2d(published)));
    const auto roi_value = roi.resolve();
    if (const auto* value = resolved(roi_value, "ROI inheritance freezes the complete selected region definition")) {
        check(value->value.as_region2d() && *value->value.as_region2d() == symbolic,
            "resolved binding retains the caller's symbolic ROI expression");
        check(value->regions.size() == 1 && value->regions[0].reference == inherited
            && value->regions[0].value == published && value->regions[0].identity.run == roi.context.run,
            "frozen inherited region retains shape, exclusions and exact result identity");
    }
    published.exclusions.clear();
    if (const auto* value = std::get_if<ResolvedBinding>(&roi_value)) {
        check(value->regions.size() == 1 && value->regions[0].value.exclusions.size() == 1,
            "editing an original region cannot mutate the frozen resolved region");
    }

    f = fixture(DataType::region2d(), DataType::region2d());
    f.solution.coordinates.push_back({frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    f.tools[0].outputs[0].semantics = {MeasurementUnit::Pixel, frame};
    f.solution.resources.push_back({uuid<ResourceId>(10), "Mask", ResourceKind::Mask,
        ManagedResource{{PathBase::SolutionDirectory, "mask-reference.bin", {}}, 1, std::string(64, 'a')}});
    const MaskConfig mask{{uuid<ResourceId>(10), ResourceKind::Mask, 1}, frame, 2, 2,
        {PixelDepth::UInt8, ChannelOrder::Mono}, {frame, 2, 2, {}, MaskSampling::None}, {}};
    Transform2D correction{frame, frame, MeasurementUnit::Pixel, MeasurementUnit::Pixel};
    correction.matrix = {1, 0, 2, 0, 1, 3, 0, 0, 1};
    const RegionDefinition masked{uuid<RegionId>(3), frame, MeasurementUnit::Pixel,
        RegionPrimitive{frame, MeasurementUnit::Pixel, mask, 0.2, correction},
        {RegionPrimitive{frame, MeasurementUnit::Pixel, mask, 0.4, correction}}};
    auto symbolic_mask = symbolic;
    symbolic_mask.exclusions = {RegionPrimitive{frame, MeasurementUnit::Pixel, CircleRegion{1, 1, 0.25}}};
    const auto nested_type = take(DataType::record(key<SchemaId>("resolver.nested_roi"), 1,
        {{key<FieldId>("payload"), DataType::list(DataType::optional(DataType::region2d()))}}));
    const auto nested = [&](const RegionDefinition& region) {
        return take(ConfigValue::record(nested_type, {{key<FieldId>("payload"),
            take(ConfigValue::list(DataType::optional(DataType::region2d()),
                {ConfigValue::some(take(ConfigValue::region2d(region)))}))}}));
    };
    const RegionDefinition geometric{uuid<RegionId>(4), frame, MeasurementUnit::Pixel,
        RegionPrimitive{frame, MeasurementUnit::Pixel, CircleRegion{1, 1, 0.5}}, {}};
    f.input().type = DataType::list(nested_type);
    f.input().binding.cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].cardinality = BindingCardinality::MultipleSources;
    f.consumer().bindings[0].sources = {nested(geometric), nested(symbolic_mask)};
    f.tools[1].region_capabilities = RegionCapabilities{{RegionShapeKind::Circle, RegionShapeKind::RasterMask},
        {RegionBoundaryPolicy::Reject}, GeometryMaskPolicy::Exclusive, true, true, false, {PixelDepth::UInt8}};
    Harness exclusive(f); exclusive.put(take(DataValue::region2d(masked)));
    const auto exclusive_value = exclusive.resolve();
    failure(exclusive_value, "runtime inherited mask plus outer geometric exclusion violates Exclusive composition");
    if (const auto* failed = std::get_if<BindingFailure>(&exclusive_value)) {
        check(failed->diagnostic.code == ErrorCode::UnsupportedRegion,
            "composition rejection reports the unsupported geometry/mask combination");
    }
    f.tools[1].region_capabilities->geometry_mask_policy = GeometryMaskPolicy::Combine;
    Harness combined(f); combined.put(take(DataValue::region2d(masked)));
    const auto combined_value = combined.resolve();
    if (const auto* value = resolved(combined_value, "declared Combine supports inherited mask and outer geometry together")) {
        const std::vector<DiagnosticPathStep> path{key<FieldId>("payload"), DiagnosticElementIndex{0}, key<FieldId>("source")};
        check(value->regions.size() == 1 && value->regions[0].source_index == 1 && value->regions[0].path == path,
            "frozen region locates its non-first binding source and nested record/list path");
        check(value->regions.size() == 1 && value->regions[0].value == masked,
            "mask-region freeze retains complete source, exclusions, local directions and transforms");
    }

    f = fixture(DataType::image(), DataType::image());
    f.solution.coordinates.push_back({frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    const PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
    f.tools[0].image_formats = {mono8}; f.tools[1].image_formats = {mono8};
    Harness images(f);
    const std::array pixels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    {
        const auto owned = take(ImageHandle::copy({2, 2, 2, mono8}, pixels, frame));
        images.put(take(DataValue::image({owned, {}, {}})));
    }
    const auto retained_image = images.resolve();
    take(images.results.release_run(images.context.run));
    if (const auto* value = resolved(retained_image, "resolved image survives retirement of its source run")) {
        const auto* image = value->value.as_image();
        check(image && image->image.copy_pixels() == std::vector<std::byte>(pixels.begin(), pixels.end()),
            "returned binding value keeps readable image ownership after the result store releases its record");
    }
    f.tools[1].image_formats.clear();
    Harness unsupported_image(f);
    const auto owned = take(ImageHandle::copy({2, 2, 2, mono8}, pixels, frame));
    unsupported_image.put(take(DataValue::image({owned, {}, {}})));
    const auto unsupported_value = unsupported_image.resolve();
    failure(unsupported_value, "consumer with no declared image format cannot accept runtime pixels");
    if (const auto* failed = std::get_if<BindingFailure>(&unsupported_value)) {
        check(failed->diagnostic.code == ErrorCode::UnsupportedPixelFormat, "missing image support has a concrete format diagnostic");
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "resolver_values") { resolver_values(); }
        else if (group == "resolver_availability") { resolver_availability(); }
        else if (group == "resolver_isolation") { resolver_isolation(); }
        else if (group == "resolver_domains") { resolver_domains(); }
        else { return 2; }
    } catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
