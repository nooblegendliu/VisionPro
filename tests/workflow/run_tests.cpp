#include <visionpro/workflow/run.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template <class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template <class T> Diagnostic expect_error(const Result<T>& result, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure != nullptr, "invalid input returns a diagnostic");
    if (!failure) { throw std::runtime_error("Expected diagnostic"); }
    check(failure->code == code, "diagnostic identifies the expected cause");
    return *failure;
}
template <UuidIdentity T> T uuid(int digit = 1)
{ return take(IdFactory::parse<T>(std::to_string(digit) + "0112233-4455-4677-8899-aabbccddeeff")); }
template <SchemaIdentity T> T token(std::string_view value)
{ return take(SchemaIdFactory::parse<T>(value)); }
template <RuntimeIdentity T> T counter(std::uint64_t value)
{ return take(RuntimeIdFactory::from_value<T>(value)); }
InputId count_id() { return token<InputId>("count"); }
InputId optional_id() { return token<InputId>("optional"); }
InputId default_id() { return token<InputId>("threshold"); }
InputId image_id() { return token<InputId>("image"); }
RunTrigger trigger() { return {TriggerKind::Manual, token<SchemaId>("test.manual")}; }
SnapshotIdentity snapshot_identity(std::uint64_t number = 1)
{ return {uuid<SolutionSessionId>(), counter<ConfigSnapshotId>(number)}; }

struct Fixture { Solution solution; std::vector<ToolDescriptor> tools; };
Fixture fixture()
{
    InputDescriptor count{count_id(), "Count", DataType::integer()};
    count.constraints.integer_range = IntegerRange{1, 10};
    count.constraints.allowed_values = {ConfigValue::integer(2), ConfigValue::integer(5), ConfigValue::integer(7)};
    InputDescriptor optional{optional_id(), "Optional", DataType::optional(DataType::integer())};
    InputDescriptor threshold{default_id(), "Threshold", DataType::real(), false, take(ConfigValue::real(2.5))};
    threshold.constraints.real_range = RealRange{0, 5};
    std::vector<InputDescriptor> inputs{count, optional, threshold};
    ToolDescriptor tool{token<ToolTypeId>("test.tool"), "Tool", 3,
        {token<SchemaId>("test.tool.schema"), 1}, {}, inputs, {}, {}};
    Node node{uuid<NodeId>(), "Node", ToolConfiguration{tool.id, 3}};
    Workflow flow{uuid<WorkflowId>(), "Flow", {token<SchemaId>("test.public"), 1, inputs},
        {node}, {}, {}, {node.id}};
    Solution solution{uuid<SolutionId>(), "Solution", 1, 4, {flow}};
    return {std::move(solution), {tool}};
}
ConfigSnapshot capture(const Fixture& value, std::uint64_t number = 1)
{ return take(ConfigSnapshot::capture(value.solution, value.tools, snapshot_identity(number))); }
std::vector<RunInput> inputs(std::int64_t count = 2)
{ return {{count_id(), DataValue::integer(count)}, {optional_id(), DataValue::none(DataType::integer())}}; }
RunRequest request(const ConfigSnapshot& snapshot, const std::vector<RunInput>& values)
{ return take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>()}, values, trigger())); }
ImageHandle image()
{
    const std::array pixels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    return take(ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, pixels,
        token<CoordinateSystemId>("camera")));
}

static_assert(!std::is_default_constructible_v<RunRequest>);
static_assert(!std::is_default_constructible_v<NodeResult>);
static_assert(!std::is_convertible_v<RunRequest, ConfigValue>);
static_assert(std::is_same_v<decltype(std::declval<const RunRequest&>().inputs()), std::span<const FixedRunInput>>);
static_assert(std::is_same_v<decltype(std::declval<const RunRequest&>().globals()), std::span<const GlobalVariableEntry>>);
static_assert(std::is_same_v<decltype(std::declval<const NodeResult&>().data()), const NodeResultData&>);
static_assert(!std::is_same_v<RunIdentity, ResultIdentity>);
static_assert(!std::is_same_v<OutputAddress, OutputReference>);

void run_inputs()
{
    auto setup = fixture();
    auto snapshot = capture(setup);
    const auto target = WorkflowTarget{uuid<WorkflowId>()};
    const auto ready = request(snapshot, inputs());
    check(ready.snapshot().identity() == snapshot.identity(), "request is paired with an immutable snapshot identity");
    check(ready.inputs().size() == 3, "defaults are fixed into the request input set");
    check(ready.input(count_id())->origin == InputOrigin::Provided, "provided input preserves its provenance");
    check(ready.input(default_id())->origin == InputOrigin::Default && *ready.input(default_id())->value.as_float64() == 2.5,
        "missing non-required input uses the snapshot's configured default");
    check(!ready.input(optional_id())->value.optional_value(), "required Optional accepts an explicitly provided None");
    check(!ready.debug_state(), "normal workflow request has no independent-debug state policy");
    check(!ready.input(token<InputId>("absent")), "input lookup does not manufacture absent inputs");
    auto values = inputs();
    values.pop_back();
    const auto missing = expect_error(RunRequest::create(snapshot, target, values, trigger()), ErrorCode::MissingRequiredValue);
    check(missing.location.input == optional_id() && missing.location.workflow == uuid<WorkflowId>()
        && missing.location.solution == uuid<SolutionId>(), "missing-input diagnostic preserves public input and snapshot context");
    expect_error(RunRequest::create(snapshot, target, {}, trigger()), ErrorCode::MissingRequiredValue);
    values = inputs();
    values.push_back(values.front());
    expect_error(RunRequest::create(snapshot, target, values, trigger()), ErrorCode::InvalidRunRequest);
    values = inputs();
    values.push_back({token<InputId>("unknown"), DataValue::integer(2)});
    expect_error(RunRequest::create(snapshot, target, values, trigger()), ErrorCode::InvalidRunRequest);
    values = inputs();
    values.front().value = take(DataValue::real(2));
    expect_error(RunRequest::create(snapshot, target, values, trigger()), ErrorCode::TypeMismatch);
    expect_error(RunRequest::create(snapshot, target, inputs(11), trigger()), ErrorCode::ConstraintViolation);
    expect_error(RunRequest::create(snapshot, target, inputs(3), trigger()), ErrorCode::ConstraintViolation);
    values = inputs();
    values.push_back({default_id(), take(DataValue::real(6))});
    expect_error(RunRequest::create(snapshot, target, values, trigger()), ErrorCode::ConstraintViolation);
    values.back().value = take(DataValue::real(4));
    check(request(snapshot, values).input(default_id())->origin == InputOrigin::Provided, "explicit values override defaults without replacing configuration");
    auto bad_trigger = trigger(); bad_trigger.kind = static_cast<TriggerKind>(255);
    expect_error(RunRequest::create(snapshot, target, inputs(), bad_trigger), ErrorCode::InvalidRunRequest);
    expect_error(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>(2)}, inputs(), trigger()), ErrorCode::InvalidRunRequest);
    expect_error(RunRequest::create(snapshot, target, inputs(), trigger(), {}, DebugStatePolicy::Active), ErrorCode::InvalidRunRequest);
    expect_error(RunRequest::create(snapshot, NodeTarget{uuid<WorkflowId>(), uuid<NodeId>(2)}, inputs(), trigger()), ErrorCode::InvalidRunRequest);
    const auto independent = take(RunRequest::create(snapshot, NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger()));
    check(independent.debug_state() == DebugStatePolicy::Isolated, "independent node request defaults to isolated debug state");
    const auto active = take(RunRequest::create(snapshot, NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger(), {}, DebugStatePolicy::Active));
    check(active.debug_state() == DebugStatePolicy::Active, "active state use is explicit policy, not implicit access");
    expect_error(RunRequest::create(snapshot, NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger(), {}, static_cast<DebugStatePolicy>(7)), ErrorCode::InvalidRunRequest);

    setup.solution.workflows.front().enabled = false;
    expect_error(RunRequest::create(capture(setup), target, inputs(), trigger()), ErrorCode::InvalidRunRequest);
    setup.solution.workflows.front().enabled = true;
    setup.solution.workflows.front().nodes.front().enabled = false;
    expect_error(RunRequest::create(capture(setup), NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger()), ErrorCode::InvalidRunRequest);
    setup.solution.workflows.front().nodes.front().enabled = true;
    setup.tools.clear();
    expect_error(RunRequest::create(capture(setup), NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger()), ErrorCode::InvalidRunRequest);
    setup = fixture();
    std::get<ToolConfiguration>(setup.solution.workflows.front().nodes.front().configuration).config_version = 4;
    expect_error(RunRequest::create(capture(setup), NodeTarget{uuid<WorkflowId>(), uuid<NodeId>()}, inputs(), trigger()), ErrorCode::InvalidRunRequest);

    setup = fixture();
    auto& flow = setup.solution.workflows.front();
    Node group{uuid<NodeId>(2), "Group", GroupConfiguration{flow.interface, {uuid<NodeId>()}, {}}};
    flow.nodes.push_back(group);
    flow.containment.push_back({group.id, uuid<NodeId>()});
    flow.entries = {group.id};
    const auto group_request = take(RunRequest::create(capture(setup), NodeTarget{flow.id, group.id}, inputs(), trigger()));
    check(group_request.inputs().size() == 3 && group_request.debug_state() == DebugStatePolicy::Isolated,
        "Group targets use their own public schema and explicit independent scope");
    flow.nodes.back().enabled = false;
    expect_error(RunRequest::create(capture(setup), NodeTarget{flow.id, uuid<NodeId>()}, inputs(), trigger()), ErrorCode::InvalidRunRequest);

    // Defaults can contain any persistable domain value and nested schema-only image absence.
    setup = fixture();
    const auto record = take(DataType::record(token<SchemaId>("default.record"), 1,
        {{token<FieldId>("images"), DataType::list(DataType::image())},
         {token<FieldId>("mask"), DataType::optional(DataType::raster_mask())}}));
    const auto default_record = take(ConfigValue::record(record,
        {{token<FieldId>("images"), take(ConfigValue::list(DataType::image(), {}))},
         {token<FieldId>("mask"), ConfigValue::none(DataType::raster_mask())}}));
    auto& public_inputs = setup.solution.workflows.front().interface.inputs;
    public_inputs = {{token<InputId>("record"), "Record", record, false, default_record},
        {token<InputId>("label"), "Label", DataType::string(), false, take(ConfigValue::string("abc"))},
        {token<InputId>("bytes"), "Bytes", DataType::binary(), false, ConfigValue::binary({1, 2})}};
    public_inputs[1].constraints.max_length = 3;
    public_inputs[2].constraints.max_length = 2;
    const auto nested = request(capture(setup), {});
    check(nested.input(token<InputId>("record"))->value.type() == record, "configured Record materializes into the identical runtime Schema");
    check(nested.input(token<InputId>("record"))->value.field(token<FieldId>("images"))->elements()->empty(), "empty Image list default remains typed and contains no loaded buffers");
    check(!nested.input(token<InputId>("record"))->value.field(token<FieldId>("mask"))->optional_value(), "typed None mask default remains absent");
    check(*nested.input(token<InputId>("bytes"))->value.as_binary() == std::vector<std::uint8_t>{1, 2}, "binary default retains its exact bytes");
    expect_error(RunRequest::create(capture(setup), target,
        {{token<InputId>("label"), take(DataValue::string("abcd"))}}, trigger()), ErrorCode::ConstraintViolation);
    expect_error(RunRequest::create(capture(setup), target,
        {{token<InputId>("bytes"), DataValue::binary({1, 2, 3})}}, trigger()), ErrorCode::ConstraintViolation);
}

void run_isolation()
{
    auto setup = fixture();
    auto original = capture(setup);
    auto provided = inputs(2);
    auto* retained_alias = &provided.front();
    const auto first = request(original, std::move(provided));
    check(provided.data() == retained_alias && provided.size() == 2, "rvalue request input preserves caller storage");
    if (provided.data() == retained_alias && !provided.empty()) { retained_alias->value = DataValue::integer(7); }
    check(*first.input(count_id())->value.as_int64() == 2, "old writable input aliases cannot change a published request");
    const auto second = request(original, inputs(5));
    check(*second.input(count_id())->value.as_int64() == 5 && *first.input(count_id())->value.as_int64() == 2,
        "two prepared requests hold distinct input sets for the same snapshot");
    expect_error(RunRequest::create(original, WorkflowTarget{uuid<WorkflowId>()}, {}, trigger()), ErrorCode::MissingRequiredValue);
    check(*first.input(count_id())->value.as_int64() == 2, "rejected request leaves earlier input sets intact");
    setup.solution.name = "Edited";
    setup.solution.revision = 5;
    setup.solution.workflows.front().interface.inputs[2].default_value = take(ConfigValue::real(4));
    const auto later = capture(setup, 2);
    const auto third = request(later, inputs(7));
    check(first.snapshot().solution().revision == 4 && third.snapshot().solution().revision == 5, "each request retains its own configuration revision");
    check(*first.input(default_id())->value.as_float64() == 2.5 && *third.input(default_id())->value.as_float64() == 4,
        "editing a default changes only requests from later snapshots");
    check(first.snapshot().identity() != third.snapshot().identity(), "new snapshots keep separate identities");
    const auto correlation = take(RuntimeIdFactory::correlation(token<CorrelationDomainId>("batch"), "workpiece-42"));
    const auto tagged = take(RunRequest::create(later, WorkflowTarget{uuid<WorkflowId>()}, inputs(),
        {TriggerKind::External, token<SchemaId>("plc")}, correlation));
    check(tagged.correlation() == correlation && tagged.trigger().kind == TriggerKind::External
        && tagged.trigger().source == token<SchemaId>("plc"), "request preserves trigger and business identity separately from runs");

    std::optional<RunRequest> image_request;
    std::optional<ImageHandle> branch;
    {
        auto image_setup = fixture();
        image_setup.solution.workflows.front().interface.inputs = {{image_id(), "Image", DataType::image()}};
        auto local_snapshot = capture(image_setup);
        auto pixels = image();
        branch = pixels;
        std::vector<RunInput> image_inputs{{image_id(), take(DataValue::image({pixels, std::nullopt, std::nullopt}))}};
        auto* alias = &image_inputs.front();
        image_request = request(local_snapshot, std::move(image_inputs));
        check(image_inputs.size() == 1 && image_inputs.data() == alias, "image input vector is not stolen from retained aliases");
        if (!image_inputs.empty() && image_inputs.data() == alias) { alias->value = DataValue::integer(99); }
        check(image_request->input(image_id())->value.as_image()->image.shares_storage_with(pixels), "request holds the original immutable image buffer");
    }
    check(image_request->input(image_id())->value.as_image()->image.copy_pixels()
        == std::vector{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}, "request retains pixels after source image, input vector and local snapshot are destroyed");
    check(image_request->input(image_id())->value.as_image()->image.shares_storage_with(*branch), "independent reader and request safely share the read-only image");
    auto moved = std::move(*image_request);
    check(image_request->input(image_id()) && moved.input(image_id()), "moving a request preserves validated source invariants");
}

void fixed_globals()
{
    auto setup = fixture();
    const auto variable = uuid<VariableId>();
    const auto session = snapshot_identity().session;
    const auto source = token<SchemaId>("test.global.source");
    setup.solution.variables.push_back({variable, "Threshold", DataType::integer(), ConfigValue::integer(9)});
    const auto snapshot = capture(setup);
    const auto target = WorkflowTarget{uuid<WorkflowId>()};
    const GlobalVariableEntry entry{variable, DataValue::integer(42), 6, ExternalUpdateOrigin{session, source}};
    std::vector globals{entry};
    const auto create = [&](std::span<const GlobalVariableEntry> values) {
        return RunRequest::create(snapshot, target, inputs(), trigger(), {}, {}, values);
    };

    const auto frozen = take(create(globals));
    globals.front() = {variable, DataValue::integer(7), 8, ExternalUpdateOrigin{session, token<SchemaId>("test.updated")}};
    const auto earlier = frozen.globals();
    const auto later = take(create(globals));
    check(earlier.size() == 1 && earlier.front().variable == variable
        && earlier.front().current_value == entry.current_value && earlier.front().revision == 6,
        "editing caller-owned globals leaves an earlier request's value and revision intact");
    const auto* origin = earlier.empty() ? nullptr : std::get_if<ExternalUpdateOrigin>(&earlier.front().update_origin);
    check(origin && origin->session == session && origin->source == source,
        "request preserves the supplied update origin");
    check(later.globals().size() == 1 && later.globals().front().current_value == globals.front().current_value
        && later.globals().front().revision == 8, "a later request can use a new current value");
    check(request(snapshot, inputs()).globals().empty(), "configuration initial values do not fill an absent read snapshot");

    const std::array duplicates{entry, entry};
    const auto duplicate = expect_error(create(duplicates), ErrorCode::InvalidResolutionContext);
    check(duplicate.location.variable == variable, "duplicate read values locate the repeated variable");
    globals.front() = entry;
    globals.front().variable = uuid<VariableId>(2);
    expect_error(create(globals), ErrorCode::InvalidResolutionContext);
    globals.front() = entry;
    globals.front().current_value = DataValue::boolean(true);
    expect_error(create(globals), ErrorCode::InvalidGlobalVariable);
    globals.front() = entry;
    globals.front().update_origin = ExternalUpdateOrigin{uuid<SolutionSessionId>(2), source};
    expect_error(create(globals), ErrorCode::ResultIdentityMismatch);
}

ResultIdentity identity()
{
    return {{snapshot_identity(), uuid<WorkflowId>(), counter<WorkflowRunId>(3)},
        {counter<WorkflowRunId>(3), uuid<NodeId>(), counter<ExecutionScopeId>(0), counter<InvocationId>(2)},
        counter<AttemptId>(0)};
}
Diagnostic failure_diagnostic()
{ return {ErrorCode::InvalidValue, Severity::Error, {}, "The tool failed."}; }
NodeResultData successful()
{
    return {identity(), ExecutionStatus::Succeeded, Judgement::NG,
        {{token<OutputId>("count"), DataType::integer(), DataValue::integer(0)},
         {token<OutputId>("optional"), DataType::optional(DataType::integer()), DataValue::none(DataType::integer())},
         {token<OutputId>("list"), DataType::list(DataType::integer()), take(DataValue::list(DataType::integer(), {}))},
         {token<OutputId>("omitted"), DataType::integer(), NotProduced{OutputAbsenceReason::ToolDidNotProduce}}},
        {}, std::chrono::nanoseconds{10}, {token<ControlOutputId>("ng")}};
}

void result_models()
{
    auto data = successful();
    const auto result = take(NodeResult::create(data));
    check(result.data().status == ExecutionStatus::Succeeded && result.data().judgement == Judgement::NG,
        "technical success and NG are distinct and compatible");
    check(std::holds_alternative<DataValue>(result.output(token<OutputId>("optional"))->content)
        && !std::get<DataValue>(result.output(token<OutputId>("optional"))->content).optional_value(), "Available None is not NotProduced");
    check(std::get<DataValue>(result.output(token<OutputId>("list"))->content).elements()->empty(), "Available empty list remains a value");
    check(std::holds_alternative<NotProduced>(result.output(token<OutputId>("omitted"))->content), "not-produced slot has an explicit absence reason");
    check(!result.output(token<OutputId>("unknown")), "result lookup never invents an undeclared slot");
    check(result.data().elapsed == std::chrono::nanoseconds{10} && result.data().selected_control_outputs.size() == 1,
        "terminal result retains duration and named control selection");
    const auto base = identity();
    auto changed = base; changed.run.snapshot.session = uuid<SolutionSessionId>(2);
    check(changed != base, "result identity includes the session");
    check(output_address(changed, token<OutputId>("count")) != output_address(base, token<OutputId>("count")), "external output address is session-qualified");
    changed = base; changed.run.snapshot.snapshot = counter<ConfigSnapshotId>(2); check(changed != base, "result identity includes the snapshot");
    changed = base; changed.run.workflow = uuid<WorkflowId>(2); check(changed != base, "result identity includes the workflow");
    changed = base; changed.run.run = counter<WorkflowRunId>(4); changed.invocation.workflow_run = changed.run.run; check(changed != base, "result identity includes the run");
    changed = base; changed.invocation.node = uuid<NodeId>(2); check(changed != base, "result identity includes the node");
    changed = base; changed.invocation.scope = counter<ExecutionScopeId>(1); check(changed != base, "result identity includes the execution scope");
    changed = base; changed.invocation.invocation = counter<InvocationId>(3); check(changed != base, "result identity includes the logical invocation");
    changed = base; changed.attempt = counter<AttemptId>(1); check(changed != base, "result identity includes the attempt");
    check(output_address(changed, token<OutputId>("count")) == output_address(base, token<OutputId>("count")), "retry keeps the logical output address while attempt identity changes");
    check(output_address(base, token<OutputId>("a")) != output_address(base, token<OutputId>("b")), "output identity is distinct from call identity");

    data.identity.invocation.workflow_run = counter<WorkflowRunId>(4);
    expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    for (auto status : {ExecutionStatus::Pending, ExecutionStatus::Running, ExecutionStatus::Skipped, static_cast<ExecutionStatus>(255)}) {
        data = successful(); data.status = status; expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    }
    data = successful(); data.judgement = static_cast<Judgement>(255); expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.elapsed = std::chrono::nanoseconds{-1}; expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.outputs.push_back(data.outputs.front()); expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.outputs.front().type = DataType::boolean(); expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.outputs.front().content = InvalidOutput{failure_diagnostic()}; expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.diagnostics.push_back(failure_diagnostic()); expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.outputs.back().content = NotProduced{OutputAbsenceReason::Failed}; expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data = successful(); data.selected_control_outputs.push_back(data.selected_control_outputs.front()); expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);

    for (const auto status : {ExecutionStatus::Failed, ExecutionStatus::Cancelled, ExecutionStatus::TimedOut}) {
        data = successful(); data.status = status; data.judgement = Judgement::NotEvaluated; data.selected_control_outputs.clear();
        data.diagnostics = {failure_diagnostic()};
        expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
        const auto reason = status == ExecutionStatus::Failed ? OutputAbsenceReason::Failed
            : (status == ExecutionStatus::Cancelled ? OutputAbsenceReason::Cancelled : OutputAbsenceReason::TimedOut);
        for (auto& slot : data.outputs) { slot.content = NotProduced{reason}; }
        check(std::holds_alternative<NodeResult>(NodeResult::create(data)), "unsuccessful result retains diagnostics without available algorithm output");
        data.judgement = Judgement::NG; expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    }
    data = successful(); data.status = ExecutionStatus::Failed; data.judgement = Judgement::NotEvaluated;
    data.outputs.clear(); data.selected_control_outputs.clear();
    expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);
    data.outputs = {{token<OutputId>("invalid"), DataType::integer(), InvalidOutput{failure_diagnostic()}}};
    check(std::holds_alternative<NodeResult>(NodeResult::create(data)), "invalid output carries an error cause for failed result");
    std::get<InvalidOutput>(data.outputs.front().content).diagnostic.severity = Severity::Warning;
    expect_error(NodeResult::create(data), ErrorCode::InvalidNodeResult);

    data = successful();
    auto* alias = &data.outputs.front();
    const auto frozen = take(NodeResult::create(std::move(data)));
    check(data.outputs.data() == alias && data.outputs.size() == 4, "result publication does not steal editable output storage");
    if (data.outputs.data() == alias && !data.outputs.empty()) { alias->content = DataValue::integer(99); }
    check(*std::get<DataValue>(frozen.output(token<OutputId>("count"))->content).as_int64() == 0,
        "retained output aliases cannot modify a result");
    std::optional<NodeResult> pixels_result;
    {
        auto local = successful();
        auto pixels = image();
        local.outputs = {{token<OutputId>("image"), DataType::image(), take(DataValue::image({pixels, {}, {}}))}};
        pixels_result = take(NodeResult::create(local));
        check(std::get<DataValue>(pixels_result->data().outputs.front().content).as_image()->image.shares_storage_with(pixels), "published image result shares immutable pixels");
    }
    check(std::get<DataValue>(pixels_result->data().outputs.front().content).as_image()->image.copy_pixels().size() == 4,
        "result owns pixels after temporary result builder is destroyed");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "run_inputs") { run_inputs(); }
        else if (group == "run_isolation") { run_isolation(); fixed_globals(); }
        else if (group == "result_models") { result_models(); }
        else { return 2; }
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n'; return 1;
    } catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
