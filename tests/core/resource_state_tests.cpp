#include <visionpro/core/resource.hpp>
#include <visionpro/core/state.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {
using namespace visionpro::core;
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view description)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; }
}
template <class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template <class T> bool success(const Result<T>& result) { return std::holds_alternative<T>(result); }
template <class T> void expect_error(const Result<T>& result, ErrorCode code, std::string_view description)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, description);
}
template <UuidIdentity Id> Id uuid(bool second = false)
{ return take(IdFactory::parse<Id>(second ? "10112233-4455-4677-8899-aabbccddeeff" : "00112233-4455-4677-8899-aabbccddeeff")); }
template <SchemaIdentity Id> Id key(std::string_view text) { return take(SchemaIdFactory::parse<Id>(text)); }
template <RuntimeIdentity Id> Id count(std::uint64_t value) { return take(RuntimeIdFactory::from_value<Id>(value)); }
ResourcePath relative(std::string path = "resources/input.png") { return {PathBase::SolutionDirectory, std::move(path), {}}; }
ResourceDefinition managed()
{ return {uuid<ResourceId>(), "输入图像", ResourceKind::Image, ManagedResource{relative(), 3, std::string(64, 'a')}}; }
DataType record_type()
{ return take(DataType::record(key<SchemaId>("measurement"), 1, {{key<FieldId>("width"), DataType::integer()}})); }
ConfigValue config_record(std::int64_t value)
{ return take(ConfigValue::record(record_type(), {{key<FieldId>("width"), ConfigValue::integer(value)}})); }
DataValue data_record(std::int64_t value)
{ return take(DataValue::record(record_type(), {{key<FieldId>("width"), DataValue::integer(value)}})); }
UpdateOrigin origin()
{ return ExternalUpdateOrigin{uuid<SolutionSessionId>(), key<SchemaId>("test.source")}; }
SessionInvocationKey invocation(bool other_node = false)
{
    return {uuid<SolutionSessionId>(), {count<WorkflowRunId>(1), uuid<NodeId>(other_node),
        count<ExecutionScopeId>(0), count<InvocationId>(1)}};
}

static_assert(!std::is_same_v<GlobalVariableDefinition, GlobalVariableEntry>);
static_assert(!std::is_same_v<DataQueueDefinition, QueueMessage>);
static_assert(!std::is_same_v<NodeStateDefinition, NodeState>);
static_assert(std::is_same_v<decltype(GlobalVariableDefinition::initial_value), ConfigValue>);
static_assert(std::is_same_v<decltype(NodeStateDefinition::initial_value), ConfigValue>);
static_assert(std::is_same_v<decltype(DeviceResource::configuration), ConfigValue>);
static_assert(std::is_same_v<decltype(GlobalVariableEntry::current_value), DataValue>);
static_assert(std::is_same_v<decltype(QueueMessage::payload), DataValue>);
static_assert(!std::is_constructible_v<ConfigValue, DataValue>);
static_assert(UuidIdentity<MessageId> && !PersistentId<MessageId>);
static_assert(!std::is_constructible_v<MessageId, QueueId>);

void resources()
{
    auto definition = managed();
    check(success(validate_resource_definition(definition)), "managed resource validates without accessing its file");
    ResourceReference reference{definition.id, definition.kind, 3};
    std::vector<ResourceDefinition> catalog{definition};
    check(success(validate_resource_reference(reference, catalog)), "resource reference validates identity, kind and pinned revision");
    reference.revision = 4;
    expect_error(validate_resource_reference(reference, catalog), ErrorCode::ResourceVersionMismatch, "wrong revision is not silently replaced");
    reference.revision.reset();
    reference.kind = ResourceKind::Calibration;
    expect_error(validate_resource_reference(reference, catalog), ErrorCode::ResourceKindMismatch, "wrong resource category is explicit");
    reference.kind = ResourceKind::Image;
    reference.id = uuid<ResourceId>(true);
    expect_error(validate_resource_reference(reference, catalog), ErrorCode::ResourceNotFound, "missing identity cannot bind by display name");
    reference = {definition.id, ResourceKind::Image, 0};
    expect_error(validate_resource_reference(reference), ErrorCode::InvalidResource, "zero pinned revision is invalid");
    reference.kind = static_cast<ResourceKind>(255);
    reference.revision.reset();
    expect_error(validate_resource_reference(reference), ErrorCode::InvalidResource, "unknown resource kind is rejected");
    catalog.push_back(definition);
    expect_error(validate_resource_catalog(catalog), ErrorCode::InvalidResource, "duplicate resource identity is rejected");
    const auto duplicate = validate_resource_catalog(catalog);
    check(std::get<Diagnostic>(duplicate).location.resource == definition.id, "resource errors carry stable resource identity");
    ResourceRequirement requirement{key<SchemaId>("camera"), ResourceKind::Device, true};
    check(success(validate_resource_requirement(requirement)), "tool resource requirements declare slots without instance identities");
    requirement.kind = static_cast<ResourceKind>(-1);
    expect_error(validate_resource_requirement(requirement), ErrorCode::InvalidResource, "resource requirements reject unsupported kinds");

    check(success(validate_resource_path(relative("资源/图像.png"), true)), "UTF-8 managed filename is supported");
    check(success(validate_resource_path({PathBase::Absolute, "C:/images/test.png", {}})), "absolute drive path is explicit");
    check(success(validate_resource_path({PathBase::Absolute, "//server/share/images", {}})), "UNC base is explicit");
    check(success(validate_resource_path({PathBase::NamedRoot, "batch/input.png", key<SchemaId>("images")})), "named root is structurally bound");
    expect_error(validate_resource_path({PathBase::NamedRoot, "input.png", {}}), ErrorCode::InvalidPath, "named root cannot be omitted");
    expect_error(validate_resource_path({PathBase::SolutionDirectory, "input.png", key<SchemaId>("images")}), ErrorCode::InvalidPath, "unrelated named root is not ignored");
    expect_error(validate_resource_path(relative("C:/images/input.png")), ErrorCode::InvalidPath, "absolute path cannot claim a relative base");
    expect_error(validate_resource_path({PathBase::Absolute, "input.png", {}}), ErrorCode::InvalidPath, "relative path cannot claim absolute base");
    expect_error(validate_resource_path({PathBase::Absolute, "/images/input.png", {}}), ErrorCode::InvalidPath, "slash-rooted path cannot depend on the current drive");
    expect_error(validate_resource_path({PathBase::Absolute, "\\images\\input.png", {}}), ErrorCode::InvalidPath, "backslash-rooted path cannot depend on the current drive");
    expect_error(validate_resource_path({PathBase::Absolute, "C:input.png", {}}), ErrorCode::InvalidPath, "drive-relative path cannot claim absolute base");
    expect_error(validate_resource_path(relative("/images/input.png")), ErrorCode::InvalidPath, "root-relative path cannot claim a solution-relative base");
    expect_error(validate_resource_path(relative("../input.png"), true), ErrorCode::InvalidPath, "managed resources cannot escape through parents");
    expect_error(validate_resource_path(relative("images/../../input.png"), true), ErrorCode::InvalidPath, "nested traversal is rejected");
    expect_error(validate_resource_path(relative("images/.. /input.png"), true), ErrorCode::InvalidPath, "Windows normalized parent aliases are rejected");
    expect_error(validate_resource_path(relative("C:input.png")), ErrorCode::InvalidPath, "drive-relative current-directory semantics are forbidden");
    expect_error(validate_resource_path(relative("images//input.png")), ErrorCode::InvalidPath, "empty path components are rejected");
    expect_error(validate_resource_path({PathBase::Absolute, "//server", {}}), ErrorCode::InvalidPath, "UNC path requires share");
    expect_error(validate_resource_path({PathBase::Absolute, "//./pipe/camera", {}}), ErrorCode::InvalidPath, "Windows device namespace cannot masquerade as a UNC resource");
    expect_error(validate_resource_path({PathBase::Absolute, "//server/../camera", {}}), ErrorCode::InvalidPath, "UNC share must be an explicit name");
    expect_error(validate_resource_path(relative(std::string("a\0b", 3))), ErrorCode::InvalidPath, "embedded NUL does not truncate path validation");
    expect_error(validate_resource_path(relative(std::string(1, static_cast<char>(0xff)))), ErrorCode::InvalidPath, "invalid UTF-8 path is rejected");
    expect_error(validate_resource_path(relative("*.png")), ErrorCode::InvalidPath, "file path is not a directory filter");
    std::get<ManagedResource>(definition.location).sha256 = "short";
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "invalid checksum representation is rejected");
    definition = managed();
    std::get<ManagedResource>(definition.location).revision = 0;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "managed revisions start at one");

    ExternalFileResource external{{PathBase::Absolute, "D:/images/input.png", {}}, ResourceReadPolicy::PerInvocation, {}, {}};
    definition = {uuid<ResourceId>(), "外部", ResourceKind::Image, external};
    check(success(validate_resource_definition(definition)), "dynamic external images remain explicit external dependencies");
    definition.kind = ResourceKind::Template;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "templates cannot silently track per-invocation file changes");
    external.read_policy = ResourceReadPolicy::SnapshotLocked;
    definition.location = external;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "snapshot lock requires content identity");
    external.expected_sha256 = std::string(64, 'b');
    external.revision = 8;
    definition.location = external;
    check(success(validate_resource_definition(definition)), "fixed external template has explicit content identity");
    catalog = {definition};
    check(success(validate_resource_reference({definition.id, ResourceKind::Template, 8}, catalog)), "external resource revision can be pinned");

    ImageSequenceResource sequence{DirectorySelection{relative("images"), "*.png"}, SequenceSort::NameAscending,
        SequenceAdvance::PerInvocation, SequenceRepeat::Loop, SequenceMissing::Fail, SequenceTrigger::Manual, 0};
    definition = {uuid<ResourceId>(), "序列", ResourceKind::DirectorySequence, sequence};
    check(success(validate_resource_definition(definition)), "directory sequence declares deterministic order and all policies");
    sequence.sort = SequenceSort::ExplicitOrder;
    definition.location = sequence;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "directory enumeration cannot pretend to be an explicit ordered file list");
    sequence.selection = std::vector<ResourcePath>{relative("one.png"), relative("two.png")};
    definition.location = sequence;
    check(success(validate_resource_definition(definition)), "explicit sequence retains specified order");
    sequence.initial_index = 2;
    definition.location = sequence;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "explicit sequence checks initial cursor against list size");
    sequence.initial_index = 0;
    sequence.selection = std::vector<ResourcePath>{};
    definition.location = sequence;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "empty image sequence is not a valid definition");

    definition = {uuid<ResourceId>(), "相机", ResourceKind::Device, DeviceResource{key<SchemaId>("camera.adapter"), config_record(10)}};
    check(success(validate_resource_definition(definition)), "device persists typed configuration instead of handles");
    std::get<DeviceResource>(definition.location).configuration = ConfigValue::integer(5);
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "device parameters require Record Schema");
    OutputPathResource output{relative("results"), OutputNaming::CorrelationKey, "inspection", "png", OutputOverwrite::Fail, 4096};
    definition = {uuid<ResourceId>(), "输出", ResourceKind::OutputPath, output};
    check(success(validate_resource_definition(definition)), "output path declares naming, overwrite and capacity policy");
    output.capacity_bytes = 0;
    definition.location = output;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "output capacity cannot be zero");
    output.capacity_bytes = 4096;
    output.stem = "../escape";
    definition.location = output;
    expect_error(validate_resource_definition(definition), ErrorCode::InvalidResource, "output filename stem cannot contain a path");
}

void shared_definitions()
{
    GlobalVariableDefinition variable{uuid<VariableId>(), "计数", DataType::integer(), ConfigValue::integer(7)};
    GlobalVariableEntry entry{variable.id, DataValue::integer(11), 0, origin()};
    check(success(validate_global_variable_entry(variable, entry)), "global current value and definition validate independently");
    entry.current_value = DataValue::integer(99);
    check(*variable.initial_value.as_int64() == 7, "changing runtime shared value does not modify configured initial value");
    entry.variable = uuid<VariableId>(true);
    expect_error(validate_global_variable_entry(variable, entry), ErrorCode::InvalidGlobalVariable, "shared value cannot bind to another variable identity");
    entry.variable = variable.id;
    entry.current_value = DataValue::boolean(false);
    expect_error(validate_global_variable_entry(variable, entry), ErrorCode::InvalidGlobalVariable, "global current value must retain declared type");
    variable.initial_value = ConfigValue::boolean(false);
    expect_error(validate_global_variable_definition(variable), ErrorCode::InvalidGlobalVariable, "global initial value type is checked");
    variable.initial_value = ConfigValue::integer(7);
    std::array variables{variable, variable};
    expect_error(validate_global_variable_definitions(variables), ErrorCode::InvalidGlobalVariable, "duplicate global variable identities are rejected");
    variables[1].id = uuid<VariableId>(true);
    check(success(validate_global_variable_definitions(variables)), "same display name does not merge separate global identities");

    DataQueueDefinition queue{uuid<QueueId>(), "测量队列", record_type(), 8, QueueOverflowPolicy::Reject,
        {EmptyQueuePolicy::ReturnEmpty, 2, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::UpToLimit}};
    const auto correlation = take(RuntimeIdFactory::correlation(key<CorrelationDomainId>("batch"), "工件-102"));
    QueueMessage message{uuid<MessageId>(), queue.id, 1, correlation, origin(), {}, data_record(10)};
    check(success(validate_queue_definition(queue)), "queue declares complete messages and explicit receive/overflow semantics");
    check(success(validate_queue_message(queue, message)), "complete correlated queue message validates without enqueue or consumption");
    const auto original_payload = message.payload;
    check(success(validate_queue_message(queue, message)) && message.payload == original_payload && message.sequence == 1,
        "ordinary validation/preview does not consume or mutate a message");
    message.sequence = 0;
    expect_error(validate_queue_message(queue, message), ErrorCode::InvalidQueue, "message sequence starts at successful enqueue order one");
    message.sequence = 1;
    message.queue = uuid<QueueId>(true);
    expect_error(validate_queue_message(queue, message), ErrorCode::InvalidQueue, "queue identity is distinct from message identity");
    message.queue = queue.id;
    message.payload = DataValue::integer(10);
    expect_error(validate_queue_message(queue, message), ErrorCode::InvalidQueue, "partial scalar cannot replace complete record payload");
    auto invalid = queue;
    invalid.capacity = 0;
    expect_error(validate_queue_definition(invalid), ErrorCode::InvalidQueue, "queue capacity must be positive");
    invalid = queue;
    invalid.receive.maximum_batch_size = 9;
    expect_error(validate_queue_definition(invalid), ErrorCode::InvalidQueue, "batch cannot require more than queue capacity");
    invalid = queue;
    invalid.receive.maximum_batch_size = 0;
    expect_error(validate_queue_definition(invalid), ErrorCode::InvalidQueue, "zero batch is rejected");
    invalid = queue;
    invalid.overflow = static_cast<QueueOverflowPolicy>(99);
    expect_error(validate_queue_definition(invalid), ErrorCode::InvalidQueue, "unknown overflow policy is not silently defaulted");
    invalid = queue;
    invalid.message_schema = DataType::integer();
    expect_error(validate_queue_definition(invalid), ErrorCode::InvalidQueue, "queue requires a typed complete record");
    std::array queues{queue, queue};
    expect_error(validate_queue_definitions(queues), ErrorCode::InvalidQueue, "queue identifiers must be unique");
    queues[1].id = uuid<QueueId>(true);
    check(success(validate_queue_definitions(queues)), "queue definitions remain separate despite equal schemas");
}

NodeStateDefinition state_definition(StateScope scope = StateScope::SolutionSession)
{
    return {DataType::integer(), ConfigValue::integer(0), scope, StateResetPolicy::DomainStartAndExplicit,
        StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
}

void state_models()
{
    auto definition = state_definition();
    NodeState first{{uuid<NodeId>(), 1, SessionStateDomain{uuid<SolutionSessionId>()}}, StateInitialization::Ready, DataValue::integer(10), {}};
    auto second = first;
    second.key.node = uuid<NodeId>(true);
    second.current_value = DataValue::integer(20);
    check(success(validate_node_state(definition, first)) && success(validate_node_state(definition, second)), "same tool state Schema supports separate nodes");
    check(first.key != second.key && *first.current_value->as_int64() == 10 && *definition.initial_value.as_int64() == 0,
        "editing another node runtime value does not change the first node or configuration");
    auto old_generation = first;
    first.key.generation = take(next_state_generation(first.key.generation));
    first.current_value = DataValue::integer(0);
    check(first.key != old_generation.key && *old_generation.current_value->as_int64() == 10,
        "new generation can coexist with still-held old state without rewriting it");
    first.key.generation = 0;
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "state generation cannot be zero");
    expect_error(next_state_generation(0), ErrorCode::InvalidState, "generation advance rejects zero");
    expect_error(next_state_generation(std::numeric_limits<std::uint64_t>::max()), ErrorCode::InvalidState, "generation advance never wraps");
    first = old_generation;
    first.key.domain = SessionStateDomain{uuid<SolutionSessionId>(true)};
    check(first.key != old_generation.key, "session domain is part of persistent runtime state identity");
    first.key.domain = WorkflowRunStateDomain{uuid<SolutionSessionId>(), count<WorkflowRunId>(1)};
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "state lifetime cannot silently change domain");
    definition.scope = StateScope::WorkflowRun;
    check(success(validate_node_state(definition, first)), "workflow-scoped state explicitly contains its session and run");
    auto next_run = first;
    next_run.key.domain = WorkflowRunStateDomain{uuid<SolutionSessionId>(), count<WorkflowRunId>(2)};
    check(first.key != next_run.key, "run-scoped keys do not reuse another run state");
    definition.scope = StateScope::Invocation;
    definition.concurrency = StateConcurrencyPolicy::IsolatedPerInvocation;
    first.key.domain = InvocationStateDomain{invocation()};
    check(success(validate_node_state(definition, first)), "invocation state explicitly owns complete logical call identity");
    first.key.domain = InvocationStateDomain{invocation(true)};
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "invocation state cannot be assigned to another node");
    definition.scope = StateScope::SolutionSession;
    expect_error(validate_node_state_definition(definition), ErrorCode::InvalidState, "per-call isolated storage cannot pretend to be one shared session state");
    definition = state_definition();
    first = old_generation;
    first.initialization = StateInitialization::NotInitialized;
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "uninitialized state cannot expose runtime data");
    first.current_value.reset();
    check(success(validate_node_state(definition, first)), "uninitialized state is distinct from a valid zero value");
    first.initialization = StateInitialization::Initializing;
    check(success(validate_node_state(definition, first)), "initialization in progress has no usable current state");
    first.initialization = StateInitialization::Failed;
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "failed initialization requires a diagnostic");
    first.initialization_error = Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "initialization failed"};
    check(success(validate_node_state(definition, first)), "initialization failure keeps its error separate from the payload");
    first.initialization_error->severity = Severity::Warning;
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "failed state cannot masquerade as a warning");
    first = old_generation;
    first.current_value = DataValue::boolean(false);
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "ready current state retains declared Schema");
    first = old_generation;
    first.current_value.reset();
    expect_error(validate_node_state(definition, first), ErrorCode::InvalidState, "ready state must have actual data");
    definition.initial_value = ConfigValue::boolean(false);
    expect_error(validate_node_state_definition(definition), ErrorCode::InvalidState, "state initial value is checked independently of runtime state");
    definition = state_definition();
    definition.reset = static_cast<StateResetPolicy>(99);
    expect_error(validate_node_state_definition(definition), ErrorCode::InvalidState, "unknown reset policy is explicit");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) { return 2; }
    const std::string_view group = argv[1];
    try {
        if (group == "resources") { resources(); }
        else if (group == "shared_definitions") { shared_definitions(); }
        else if (group == "state_models") { state_models(); }
        else { return 2; }
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected exception: " << exception.what() << '\n';
        return 1;
    } catch (...) { return 1; }
    std::cout << group << ": " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
