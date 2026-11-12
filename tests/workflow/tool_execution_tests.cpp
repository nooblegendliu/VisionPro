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
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T take(Result<T> result)
{
    if (const auto* invalid = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(invalid->message); }
    return std::get<T>(std::move(result));
}
template<UuidIdentity T> T uuid(unsigned value = 1)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t i = 0; i < 8; ++i) { text[text.size() - 1 - i] = hex[(value >> (i * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<RuntimeIdentity T> T number(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }
SnapshotIdentity snapshot_id() { return {uuid<SolutionSessionId>(), number<ConfigSnapshotId>(1)}; }
InputDescriptor input(std::string_view name, DataType type = DataType::integer())
{
    InputDescriptor result{key<InputId>(name), std::string(name), type};
    result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput};
    return result;
}
OutputReference reference(unsigned node)
{ return {uuid<WorkflowId>(), uuid<NodeId>(node), key<OutputId>("value"), {}}; }
struct Fixture {
    Solution solution;
    std::vector<ToolDescriptor> tools;
    Node& worker() { return solution.workflows[0].nodes[1]; }
    ConfigSnapshot snapshot() const { return take(ConfigSnapshot::capture(solution, tools, snapshot_id())); }
};
Fixture fixture()
{
    ToolDescriptor producer{key<ToolTypeId>("bridge.producer"), "Producer", 1,
        {key<SchemaId>("bridge.producer.schema"), 1}, {}, {}, {{key<OutputId>("value"), "Value", DataType::integer()}}, {}};
    ToolDescriptor worker{key<ToolTypeId>("bridge.worker"), "Worker", 1,
        {key<SchemaId>("bridge.worker.schema"), 1}, {}, {input("first"), input("second")}, producer.outputs, {}};
    ParameterDescriptor factor{key<ParameterId>("factor"), "Factor", DataType::integer(), false, ConfigValue::integer(2)};
    factor.structural = true; factor.binding.time = BindingTime::SnapshotBuild;
    ParameterDescriptor labels{key<ParameterId>("labels"), "Labels", DataType::list(DataType::integer())};
    labels.structural = true; labels.binding.time = BindingTime::SnapshotBuild; labels.binding.cardinality = BindingCardinality::MultipleSources;
    ParameterDescriptor bias{key<ParameterId>("bias"), "Bias", DataType::integer(), false, ConfigValue::integer(1)};
    worker.parameters = {factor, labels, bias};
    worker.resources = {{key<SchemaId>("table"), ResourceKind::Binary, true}};
    ToolDescriptor sink{key<ToolTypeId>("bridge.sink"), "Sink", 1,
        {key<SchemaId>("bridge.sink.schema"), 1}, {}, {input("result")}, {}, {}};
    Workflow flow{uuid<WorkflowId>(), "Bridge", {key<SchemaId>("bridge.public"), 1},
        {{uuid<NodeId>(1), "Producer one", ToolConfiguration{producer.id, 1}},
         {uuid<NodeId>(2), "Worker", ToolConfiguration{worker.id, 1}},
         {uuid<NodeId>(3), "Producer two", ToolConfiguration{producer.id, 1}},
         {uuid<NodeId>(4), "Sink", ToolConfiguration{sink.id, 1}}},
        {{uuid<ControlLinkId>(1), uuid<NodeId>(1), {}, uuid<NodeId>(2)},
         {uuid<ControlLinkId>(2), uuid<NodeId>(3), {}, uuid<NodeId>(2)},
         {uuid<ControlLinkId>(3), uuid<NodeId>(2), {}, uuid<NodeId>(4)}}, {}, {uuid<NodeId>(1), uuid<NodeId>(3)}};
    flow.nodes[1].bindings = {{key<InputId>("first"), {reference(1)}}, {key<InputId>("second"), {reference(3)}},
        {labels.id, {ConfigValue::integer(7), ConfigValue::integer(8)}, BindingCardinality::MultipleSources}};
    flow.nodes[1].resources = {{key<SchemaId>("table"), {uuid<ResourceId>(), ResourceKind::Binary, {}}}};
    flow.nodes[3].bindings = {{key<InputId>("result"), {reference(2)}}};
    Solution solution{uuid<SolutionId>(), "Bridge", 1, 1, {flow}};
    solution.resources = {{uuid<ResourceId>(), "Table", ResourceKind::Binary,
        ManagedResource{{PathBase::SolutionDirectory, "never-opened.bin", {}}, 1, std::string(64, 'a')}}};
    return {solution, {producer, worker, sink}};
}
ResolutionContext context(unsigned consumer = 2)
{
    const RunIdentity run{snapshot_id(), uuid<WorkflowId>(), number<WorkflowRunId>(1)};
    ResolutionContext result{run, {run.run, uuid<NodeId>(consumer), number<ExecutionScopeId>(0), number<InvocationId>(consumer)},
        {{number<ExecutionScopeId>(0), {}, {}, {}}}, {}, {}, {}};
    if (consumer == 2) {
        result.sources = {{uuid<NodeId>(1), number<ExecutionScopeId>(0), number<InvocationId>(1)},
            {uuid<NodeId>(3), number<ExecutionScopeId>(0), number<InvocationId>(3)}};
    } else if (consumer == 4) { result.sources = {{uuid<NodeId>(2), number<ExecutionScopeId>(0), number<InvocationId>(2)}}; }
    return result;
}
NodeResult record(const ConfigSnapshot& snapshot, unsigned node, DataValue value, unsigned attempt = 0)
{
    const auto run = context().run;
    const ResultIdentity identity{run, {run.run, uuid<NodeId>(node), number<ExecutionScopeId>(0), number<InvocationId>(node)}, number<AttemptId>(attempt)};
    const auto* configured = find_node(snapshot.solution().workflows[0], uuid<NodeId>(node));
    const auto* tool = snapshot.find_tool(std::get<ToolConfiguration>(configured->configuration).type);
    return take(NodeResult::create({identity, ExecutionStatus::Succeeded, Judgement::OK,
        {{key<OutputId>("value"), tool->outputs[0].type, std::move(value)}}, {}}));
}
class Lease final : public ResourceLease {
public:
    explicit Lease(ResourceReference reference) : ResourceLease(std::move(reference)) {}
};
std::vector<InvocationResourceLease> leases()
{ return {{key<SchemaId>("table"), std::make_shared<const Lease>(ResourceReference{uuid<ResourceId>(), ResourceKind::Binary, 1})}}; }

class RecordingResourceProvider final : public InvocationResourceProvider {
public:
    enum class Behavior { Acquire, StaleRevision, RejectSecond, ThrowOnSecond };

    explicit RecordingResourceProvider(Behavior behavior = Behavior::Acquire) : behavior_(behavior) {}
    std::span<const ResourceKind> kinds() const noexcept override
    {
        static constexpr ResourceKind supported[]{ResourceKind::Binary};
        return supported;
    }
    Result<std::shared_ptr<const ResourceLease>> acquire(const ToolExecutionConfiguration&,
        const ToolResourceBinding& binding) override
    {
        requests.push_back(binding);
        if (requests.size() == 2) {
            if (behavior_ == Behavior::RejectSecond) {
                return Diagnostic{ErrorCode::ResourceNotFound, Severity::Error, {}, "Resource is unavailable."};
            }
            if (behavior_ == Behavior::ThrowOnSecond) { throw std::runtime_error("Provider failure"); }
        }
        auto reference = binding.resource;
        if (behavior_ == Behavior::StaleRevision) { ++reference.revision.value(); }
        auto lease = std::make_shared<const Lease>(std::move(reference));
        issued.emplace_back(lease);
        return std::shared_ptr<const ResourceLease>{std::move(lease)};
    }

    std::vector<ToolResourceBinding> requests;
    std::vector<std::weak_ptr<const ResourceLease>> issued;
private:
    Behavior behavior_;
};

class AddInstance final : public ToolInstance {
public:
    AddInstance(ToolExecutionConfiguration configuration, int& starts) : ToolInstance(std::move(configuration)), starts_(starts) {}
protected:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> invocation, InvocationCompletion) override
    {
        ++starts_;
        std::int64_t sum = 0;
        for (const auto& input : invocation->data().inputs) { sum += *input.value.as_int64(); }
        for (const auto& parameter : invocation->data().parameters) {
            if (const auto* value = parameter.value.as_int64()) { sum += *value; }
        }
        return InvocationStart{ToolCompletion{invocation->data().identity, ToolCompletionStatus::Succeeded,
            ToolJudgement::NG, {{key<OutputId>("value"), DataValue::integer(sum)}}, {}, std::chrono::nanoseconds{42}, {}}};
    }
private:
    int& starts_;
};
class AddFactory final : public ToolInstanceFactory {
public:
    int creates{0};
    int starts{0};
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { ++creates; return std::shared_ptr<ToolInstance>{std::make_shared<AddInstance>(configuration, starts)}; }
};

void projection_and_operation()
{
    auto editable = fixture();
    const auto snapshot = editable.snapshot();
    const auto prepared = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>(2)));
    const auto& fixed = prepared.configuration().data();
    check(fixed.session == snapshot_id().session && fixed.snapshot == snapshot_id().snapshot
        && fixed.workflow == uuid<WorkflowId>() && fixed.node == uuid<NodeId>(2), "fixed tool configuration preserves exact snapshot and node identity");
    check(fixed.parameters.size() == 2 && fixed.parameters[0].id == key<ParameterId>("factor")
        && fixed.parameters[0].value == ConfigValue::integer(2), "only SnapshotBuild parameters are fixed, including their defaults");
    check(fixed.parameters.size() == 2 && fixed.parameters[1].value == take(ConfigValue::list(DataType::integer(),
        {ConfigValue::integer(7), ConfigValue::integer(8)})), "multiple fixed constant sources are aggregated without dropping values");
    check(fixed.resources.size() == 1 && fixed.resources[0].slot == key<SchemaId>("table")
        && fixed.resources[0].resource.revision == 1, "unversioned slot reference is pinned to the snapshot catalog's managed revision");
    editable.tools[1].parameters[0].default_value = ConfigValue::integer(99);
    editable.worker().bindings[2].sources = {ConfigValue::integer(100)};
    check(prepared.configuration().data().parameters[0].value == ConfigValue::integer(2)
        && prepared.snapshot().solution().revision == 1, "editing configuration does not change prepared execution metadata");

    ResultStore store(snapshot_id().session);
    const auto owned_leases = leases();
    AddFactory factory;
    auto current = context();
    auto pending = prepared.prepare(current, number<AttemptId>(2), store);
    const auto* pending_inputs = std::get_if<PendingInvocation>(&pending);
    check(pending_inputs && pending_inputs->bindings.size() == 5, "pending invocation preserves every input and parameter resolution");
    check(std::holds_alternative<PendingInvocation>(pending), "pending sources do not require acquiring declared resource leases early");
    check(factory.creates == 0 && factory.starts == 0, "preparing pending inputs does not create or start a tool");
    current.sources[1].state = MissingReason::NotSelected;
    const auto unavailable = prepared.prepare(current, number<AttemptId>(2), store);
    check(std::holds_alternative<UnavailableInvocation>(unavailable),
        "unavailable input takes priority over pending input without constructing an invocation");
    if (const auto* unavailable_inputs = std::get_if<UnavailableInvocation>(&unavailable)) {
        const auto second = std::ranges::find(unavailable_inputs->bindings, BindingTarget{key<InputId>("second")}, &TargetResolution::target);
        const auto* absence = second == unavailable_inputs->bindings.end() ? nullptr : std::get_if<UnavailableBinding>(&second->resolution);
        check(absence && absence->reason == OutputAbsenceReason::NotSelected,
            "unavailable invocation preserves the exact target and unaccepted absence reason");
    }
    check(factory.creates == 0 && factory.starts == 0, "unavailable inputs cannot implicitly create or start a tool");
    current.sources[1].state = number<InvocationId>(3);
    auto failed_data = record(snapshot, 3, DataValue::integer(1)).data();
    failed_data.status = ExecutionStatus::Failed; failed_data.judgement = Judgement::NotEvaluated;
    failed_data.outputs[0].content = NotProduced{OutputAbsenceReason::Failed};
    failed_data.diagnostics = {{ErrorCode::InvalidValue, Severity::Error, {}, "Artificial upstream failure"}};
    ResultStore failed_store(snapshot_id().session); take(failed_store.insert(snapshot, take(NodeResult::create(failed_data))));
    const auto failure = prepared.prepare(current, number<AttemptId>(2), failed_store, owned_leases);
    const auto* invalid = std::get_if<InvocationFailure>(&failure);
    check(invalid && invalid->diagnostic.code == ErrorCode::SourceFailed && invalid->bindings.size() == 5,
        "later target failure is preserved even when an earlier input is pending");
    take(store.insert(snapshot, record(snapshot, 1, DataValue::integer(10))));
    take(store.insert(snapshot, record(snapshot, 3, DataValue::integer(20))));
    check(std::holds_alternative<InvocationFailure>(prepared.prepare(current, number<AttemptId>(2), store)),
        "ready data cannot silently omit required resource leases");
    auto bad_leases = owned_leases; bad_leases[0].lease.reset();
    check(std::holds_alternative<InvocationFailure>(prepared.prepare(current, number<AttemptId>(2), store, bad_leases)),
        "ready invocation rejects a null resource lease");
    bad_leases[0].lease = std::make_shared<const Lease>(ResourceReference{uuid<ResourceId>(), ResourceKind::Binary, 2});
    const auto wrong_revision = prepared.prepare(current, number<AttemptId>(2), store, bad_leases);
    check(std::holds_alternative<InvocationFailure>(wrong_revision)
        && std::get<InvocationFailure>(wrong_revision).diagnostic.code == ErrorCode::ResourceVersionMismatch,
        "ready invocation rejects a lease for another pinned resource revision");
    bad_leases[0].lease = std::make_shared<const Lease>(ResourceReference{uuid<ResourceId>(), ResourceKind::Binary, {}});
    check(std::holds_alternative<InvocationFailure>(prepared.prepare(current, number<AttemptId>(2), store, bad_leases)),
        "a managed resource lease must retain the actual fixed revision instead of null");
    auto ready = prepared.prepare(current, number<AttemptId>(2), store, owned_leases);
    const auto* invocation = std::get_if<PreparedInvocation>(&ready);
    check(invocation != nullptr, "all fixed values and leases produce a complete core invocation");
    if (!invocation) { return; }
    check(invocation->context.data().inputs.size() == 2 && invocation->context.data().parameters.size() == 3
        && invocation->context.data().resources.size() == 1, "core context receives complete inputs, all parameters and owning leases");
    check(invocation->snapshot.identity() == snapshot.identity()
        && invocation->context.data().identity.attempt == number<AttemptId>(2), "prepared invocation retains its snapshot and exact attempt");
    const auto instance = take(create_tool_instance(factory, prepared.configuration()));
    const auto operation = take(InvocationOperation::start(instance, invocation->context));
    const auto progress = operation.poll();
    check(factory.creates == 1 && factory.starts == 1 && progress.completion && progress.physically_exited,
        "explicit factory and operation calls run one synchronous test double after preparation");
    if (!progress.completion) { return; }
    auto completion = *progress.completion;
    const auto mapped = take(checked_node_result(*invocation, completion));
    check(mapped.data().identity.run == current.run && mapped.data().identity.invocation == current.consumer
        && mapped.data().identity.attempt == number<AttemptId>(2) && mapped.data().judgement == Judgement::NG,
        "completion projection preserves run, invocation, attempt and NG as successful execution");
    check(mapped.output(key<OutputId>("value"))
        && std::get<DataValue>(mapped.output(key<OutputId>("value"))->content) == DataValue::integer(33),
        "test-double output uses both resolved inputs and fixed/default parameters");
    take(store.insert(snapshot, mapped));
    const auto resolver = take(BindingResolver::create(snapshot));
    const auto downstream = resolver.resolve(context(4), key<InputId>("result"), store);
    const auto* value = std::get_if<ResolvedBinding>(&downstream);
    check(value && value->value == DataValue::integer(33), "mapped result flows through the existing store and downstream resolver");
    if (value && value->sources.size() == 1) {
        const auto* trace = std::get_if<ResultTrace>(&value->sources[0].origin);
        check(trace && trace->attempt == number<AttemptId>(2), "downstream trace retains the accepted tool attempt");
    }
    completion.identity.attempt = number<AttemptId>(3);
    const auto wrong_attempt = checked_node_result(*invocation, completion);
    check(std::holds_alternative<Diagnostic>(wrong_attempt)
        && std::get<Diagnostic>(wrong_attempt).code == ErrorCode::ResultIdentityMismatch, "completion from another attempt cannot be projected");
    completion = *progress.completion; completion.outputs.clear();
    check(std::holds_alternative<Diagnostic>(checked_node_result(*invocation, completion)),
        "successful completion with missing declared output is rejected before store publication");
    completion = *progress.completion; completion.status = ToolCompletionStatus::Cancelled;
    completion.judgement = ToolJudgement::NotEvaluated; completion.outputs.clear();
    const auto cancelled = take(checked_node_result(*invocation, completion));
    check(cancelled.data().status == ExecutionStatus::Cancelled
        && std::get<NotProduced>(cancelled.data().outputs[0].content).reason == OutputAbsenceReason::Cancelled,
        "cancelled tool completion creates typed unavailable output slots, never stale values");

    const auto zero = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>(1)));
    auto empty_context = context(1);
    ResultStore empty_store(snapshot_id().session);
    check(std::holds_alternative<PreparedInvocation>(zero.prepare(empty_context, number<AttemptId>(0), empty_store)),
        "zero-target tool can form an explicitly identified empty invocation");
    empty_context.consumer.scope = number<ExecutionScopeId>(9);
    check(std::holds_alternative<InvocationFailure>(zero.prepare(empty_context, number<AttemptId>(0), empty_store)),
        "zero-target tool still rejects an undeclared runtime scope");
    empty_context = context(1); take(empty_store.release_run(empty_context.run));
    const auto retired = zero.prepare(empty_context, number<AttemptId>(0), empty_store);
    check(std::holds_alternative<InvocationFailure>(retired)
        && std::get<InvocationFailure>(retired).diagnostic.code == ErrorCode::ResultRetired,
        "zero-target tool cannot bypass retirement validation");
    current.consumer.node = uuid<NodeId>(4);
    check(std::holds_alternative<InvocationFailure>(prepared.prepare(current, number<AttemptId>(2), store, owned_leases)),
        "fixed tool configuration cannot be reused for another consumer node");

    auto external = fixture();
    external.solution.resources[0].location = ExternalFileResource{
        {PathBase::SolutionDirectory, "dynamic.bin", {}}, ResourceReadPolicy::PerInvocation, {}, {}};
    const auto dynamic = take(PreparedTool::create(external.snapshot(), uuid<WorkflowId>(), uuid<NodeId>(2)));
    check(!dynamic.configuration().data().resources[0].resource.revision,
        "dynamic external resource without a catalog version remains explicitly unversioned");
    auto& fixed_external = std::get<ExternalFileResource>(external.solution.resources[0].location);
    fixed_external.read_policy = ResourceReadPolicy::SnapshotLocked;
    fixed_external.expected_sha256 = std::string(64, 'b');
    fixed_external.revision = 9;
    const auto versioned = take(PreparedTool::create(external.snapshot(), uuid<WorkflowId>(), uuid<NodeId>(2)));
    check(versioned.configuration().data().resources[0].resource.revision == 9,
        "versioned external resource pins the directory's actual revision");
    auto device = fixture();
    device.solution.resources[0].kind = ResourceKind::Device;
    const auto device_type = take(DataType::record(key<SchemaId>("test.device.config"), 1,
        {{key<FieldId>("channel"), DataType::integer()}}));
    device.solution.resources[0].location = DeviceResource{key<SchemaId>("test.device"),
        take(ConfigValue::record(device_type, {{key<FieldId>("channel"), ConfigValue::integer(0)}}))};
    device.tools[1].resources[0].kind = ResourceKind::Device;
    device.worker().resources[0].resource.kind = ResourceKind::Device;
    const auto connected = take(PreparedTool::create(device.snapshot(), uuid<WorkflowId>(), uuid<NodeId>(2)));
    check(!connected.configuration().data().resources[0].resource.revision,
        "device lease identity does not invent a resource revision");
}

void provider_acquisition()
{
    auto editable = fixture();
    const auto model_slot = key<SchemaId>("model");
    const auto model_id = uuid<ResourceId>(2);
    editable.tools[1].resources.push_back({model_slot, ResourceKind::Binary, true});
    editable.worker().resources.push_back({model_slot, {model_id, ResourceKind::Binary, {}}});
    editable.solution.resources.push_back({model_id, "Model", ResourceKind::Binary,
        ManagedResource{{PathBase::SolutionDirectory, "model.bin", {}}, 7, std::string(64, 'b')}});
    const auto snapshot = editable.snapshot();
    const auto tool = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>(2)));
    const auto attempt = number<AttemptId>(0);
    ResultStore store(snapshot_id().session);
    RecordingResourceProvider provider;

    auto current = context();
    check(std::holds_alternative<PendingInvocation>(tool.prepare(current, attempt, store, provider))
        && provider.requests.empty(), "the provider is untouched while bindings are pending");
    current.sources[1].state = MissingReason::NotSelected;
    check(std::holds_alternative<UnavailableInvocation>(tool.prepare(current, attempt, store, provider))
        && provider.requests.empty(), "unavailable inputs do not acquire any leases");

    current = context();
    take(store.insert(snapshot, record(snapshot, 1, DataValue::integer(10))));
    take(store.insert(snapshot, record(snapshot, 3, DataValue::integer(20))));
    {
        const auto prepared = tool.prepare(current, attempt, store, provider);
        const auto* invocation = std::get_if<PreparedInvocation>(&prepared);
        check(provider.requests.size() == 2 && provider.requests[0].slot == key<SchemaId>("table")
            && provider.requests[0].resource.revision == 1 && provider.requests[1].slot == model_slot
            && provider.requests[1].resource.revision == 7, "the provider receives each slot with its pinned reference once");
        check(invocation && invocation->context.data().resources.size() == 2 && provider.issued.size() == 2
            && !provider.issued[0].expired() && !provider.issued[1].expired(),
            "prepared context owns both provider leases");
    }
    check(std::ranges::all_of(provider.issued, [](const auto& lease) { return lease.expired(); }),
        "destroying the prepared invocation releases its provider leases");

    const auto reject = [&](RecordingResourceProvider::Behavior behavior, ErrorCode code, std::size_t attempts, ResourceId resource) {
        RecordingResourceProvider failing(behavior);
        const auto prepared = tool.prepare(current, attempt, store, failing);
        const auto* failure = std::get_if<InvocationFailure>(&prepared);
        check(failure && failure->diagnostic.code == code && failure->diagnostic.location.resource == resource
            && failure->diagnostic.location.solution == snapshot.solution().id
            && failure->diagnostic.location.workflow == current.run.workflow
            && failure->diagnostic.location.node == current.consumer.node && failure->bindings.size() == 5,
            "resource failure preserves the resource, invocation location and binding resolutions");
        check(failing.requests.size() == attempts && failing.issued.size() == 1 && failing.issued[0].expired(),
            "resource acquisition stops at the first failure and releases previously acquired leases");
    };
    reject(RecordingResourceProvider::Behavior::RejectSecond, ErrorCode::ResourceNotFound, 2, model_id);
    reject(RecordingResourceProvider::Behavior::ThrowOnSecond, ErrorCode::ExecutionBoundaryFailure, 2, model_id);
    reject(RecordingResourceProvider::Behavior::StaleRevision, ErrorCode::ResourceVersionMismatch, 1, uuid<ResourceId>());
}

void region_projection()
{
    auto f = fixture();
    const auto frame = key<CoordinateSystemId>("camera");
    f.solution.coordinates = {{frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
    auto producer = f.tools[0]; producer.id = key<ToolTypeId>("bridge.region"); producer.schema.id = key<SchemaId>("bridge.region.schema");
    producer.outputs[0].type = DataType::region2d(); producer.outputs[0].semantics = {MeasurementUnit::Pixel, frame};
    f.tools.push_back(producer); f.solution.workflows[0].nodes[0].configuration = ToolConfiguration{producer.id, 1};
    const InheritedRegion inherited{uuid<WorkflowId>(), uuid<NodeId>(1), key<OutputId>("value"), frame, MeasurementUnit::Pixel};
    const RegionDefinition symbolic{uuid<RegionId>(), frame, MeasurementUnit::Pixel, inherited, {}};
    const RegionDefinition local{uuid<RegionId>(2), frame, MeasurementUnit::Pixel,
        RegionPrimitive{frame, MeasurementUnit::Pixel, CircleRegion{4, 5, 2}}, {}};
    const auto record_type = take(DataType::record(key<SchemaId>("bridge.nested_region"), 1,
        {{key<FieldId>("payload"), DataType::list(DataType::optional(DataType::region2d()))}}));
    const auto nested = [&](const RegionDefinition& region) {
        return take(ConfigValue::record(record_type, {{key<FieldId>("payload"),
            take(ConfigValue::list(DataType::optional(DataType::region2d()), {ConfigValue::some(take(ConfigValue::region2d(region)))}))}}));
    };
    f.tools[1].inputs[0].type = DataType::list(record_type);
    f.tools[1].inputs[0].binding.cardinality = BindingCardinality::MultipleSources;
    f.worker().bindings[0].cardinality = BindingCardinality::MultipleSources;
    f.worker().bindings[0].sources = {nested(local), nested(symbolic)};
    const auto snapshot = f.snapshot();
    const auto prepared = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>(2)));
    ResultStore results(snapshot_id().session);
    take(results.insert(snapshot, record(snapshot, 1, take(DataValue::region2d(local)), 5)));
    take(results.insert(snapshot, record(snapshot, 3, DataValue::integer(20))));
    const auto invocation = prepared.prepare(context(), number<AttemptId>(7), results, leases());
    const auto* value = std::get_if<PreparedInvocation>(&invocation);
    check(value && value->context.data().regions.size() == 1, "resolved ROI sidecar is not dropped while projecting to core");
    if (value && value->context.data().regions.size() == 1) {
        const auto& region = value->context.data().regions[0];
        const std::vector<DiagnosticPathStep> path{key<FieldId>("payload"), DiagnosticElementIndex{0}, key<FieldId>("source")};
        check(region.target == ToolValueTarget{key<InputId>("first")} && region.reference == inherited && region.value == local,
            "ROI projection retains target and complete symbolic/reference region definitions");
        check(region.source_index == 1 && region.path == path, "ROI projection preserves non-first source and nested path");
        check(region.producer.session == snapshot_id().session && region.producer.snapshot == snapshot_id().snapshot
            && region.producer.invocation.node == uuid<NodeId>(1) && region.producer.attempt == number<AttemptId>(5)
            && value->context.data().identity.attempt == number<AttemptId>(7), "producer attempt stays independent from the consumer attempt");
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "tool_execution") { return 2; }
    try { projection_and_operation(); provider_acquisition(); region_projection(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << "tool_execution: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
