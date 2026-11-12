#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/tool_execution.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {

using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
namespace fs = std::filesystem;

int checks = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
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

class PackageDirectories final {
public:
    PackageDirectories()
        : root_(fs::current_path() / ("s37-roundtrip-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        if (!fs::create_directory(root_)) { throw std::runtime_error("Fixture directory already exists."); }
    }
    PackageDirectories(const PackageDirectories&) = delete;
    PackageDirectories& operator=(const PackageDirectories&) = delete;
    ~PackageDirectories()
    {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    [[nodiscard]] fs::path package(std::u8string_view name) const
    {
        auto directory = root_ / fs::path(name);
        fs::create_directory(directory);
        return directory;
    }
private:
    const fs::path root_;
};

class UnusedFactory final : public ToolInstanceFactory {
public:
    unsigned calls{0};
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration&) override
    {
        ++calls;
        return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Configuration checks must not create an instance."};
    }
};

constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
constexpr auto highest = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t solution_revision = 9007199254740993ULL;

InputDescriptor integer_input(std::string_view id, std::int64_t fallback)
{
    InputDescriptor input{key<InputId>(id), std::string(id), DataType::integer(), false, ConfigValue::integer(fallback)};
    input.constraints.integer_range = IntegerRange{lowest, highest};
    input.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput,
        BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
    return input;
}

DataType measurement_type()
{
    return take(DataType::record(key<SchemaId>("roundtrip.measurement"), 2,
        {{key<FieldId>("items"), DataType::list(DataType::integer())}, {key<FieldId>("data"), DataType::binary()}}));
}

ToolDescriptor counter_tool()
{
    const auto per_run = key<EnumMemberId>("per-run");
    const auto session = key<EnumMemberId>("session");
    const auto policy = take(DataType::enumeration(key<SchemaId>("roundtrip.state-policy"), 2,
        {{per_run, "每轮初始化"}, {session, "会话累计"}}));
    ParameterDescriptor initial{key<ParameterId>("initial"), "初值", DataType::integer(), false, ConfigValue::integer(1)};
    initial.binding.time = BindingTime::SnapshotBuild;
    ParameterDescriptor mode{key<ParameterId>("mode"), "策略", policy, false, take(ConfigValue::enumeration(policy, session))};
    mode.binding.time = BindingTime::SnapshotBuild;
    InputDescriptor resource{key<InputId>("reference"), "外部引用", DataType::optional(DataType::resource_reference()),
        false, ConfigValue::none(DataType::resource_reference())};
    resource.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::Resource};
    ToolDescriptor tool{key<ToolTypeId>("roundtrip.counter"), "计数器", 3,
        {key<SchemaId>("roundtrip.counter.schema"), 4}, {initial, mode},
        {integer_input("offset", 3), integer_input("elapsed", 0), integer_input("global", 0), resource},
        {{key<OutputId>("measurement"), "测量结果", measurement_type()}},
        {{key<ControlOutputId>("done"), "完成"}}};
    tool.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0), StateScope::Invocation,
        StateResetPolicy::DomainStart, StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted,
        StateConcurrencyPolicy::Serialized};
    tool.state_parameters = {initial.id, StatePolicyParameter{mode.id,
        {{per_run, StateScope::WorkflowRun, StateResetPolicy::DomainStart},
         {session, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit}}}};
    tool.resources = {{key<SchemaId>("counter.data"), ResourceKind::Binary, true}};
    return tool;
}

Workflow configured_workflow(const ToolDescriptor& tool)
{
    const auto flow = uuid<WorkflowId>();
    const auto group_id = uuid<NodeId>('1');
    const auto child_id = uuid<NodeId>('2');
    const auto consumer_id = uuid<NodeId>('3');
    const auto done = key<ControlOutputId>("done");
    const auto total = key<OutputId>("total");
    const auto measurement = [&](NodeId id) {
        return OutputReference{flow, id, key<OutputId>("measurement"), {FieldSelection{key<FieldId>("items")}, ElementSelection{0}}};
    };
    GroupConfiguration group_ports{{key<SchemaId>("roundtrip.group"), 2, {integer_input("seed", 7)},
        {{total, "总数", DataType::integer()}}, {{done, "完成"}}}, {child_id},
        {{total, measurement(child_id)}}, {{done, child_id, done}}};
    Node group{group_id, "组合", group_ports,
        {{key<InputId>("seed"), {ScopedInputReference{flow, {}, key<InputId>("start"), {}}}}}};

    const auto& mode = tool.parameters[1];
    const auto state_policy = [&](std::string_view selected) {
        return take(ConfigValue::enumeration(mode.type, key<EnumMemberId>(selected)));
    };
    const auto slot = key<SchemaId>("counter.data");
    const ResourceReference external{uuid<ResourceId>('2'), ResourceKind::Binary, {}};
    Node child{child_id, "每轮", ToolConfiguration{tool.id, tool.config_version},
        {{key<InputId>("offset"), {ScopedInputReference{flow, group_id, key<InputId>("seed"), {}}}},
         {tool.parameters[0].id, {ConfigValue::integer(-5)}}, {mode.id, {state_policy("per-run")}}},
        {{slot, {uuid<ResourceId>('1'), ResourceKind::Binary, 7}}}};
    Node consumer{consumer_id, "跨轮", ToolConfiguration{tool.id, tool.config_version},
        {{key<InputId>("offset"), {OutputReference{flow, group_id, total, {}}}},
         {key<InputId>("elapsed"), {OutputReference{flow, group_id, SystemField::Elapsed, {}}}},
         {key<InputId>("global"), {GlobalReference{uuid<VariableId>('1'), 0, {}}}},
         {key<InputId>("reference"), {external}, BindingCardinality::SingleSource, ConversionKind::WrapOptional},
         {tool.parameters[0].id, {ConfigValue::integer(42)}}, {mode.id, {state_policy("session")}}},
        {{slot, external}}};
    const auto final = key<OutputId>("final");
    const auto finished = key<ControlOutputId>("finished");
    auto start = integer_input("start", 2);
    start.constraints.allowed_values = {ConfigValue::integer(2), ConfigValue::integer(5)};
    return {flow, "视觉流程", {key<SchemaId>("roundtrip.workflow"), 3, {start},
            {{final, "最终计数", DataType::integer()}}, {{finished, "完成"}}},
        {group, child, consumer}, {{uuid<ControlLinkId>(), group_id, done, consumer_id}},
        {{group_id, child_id}}, {group_id}, {{final, measurement(consumer_id)}}, true, {{finished, consumer_id, done}}};
}

ResourcePath relative(std::string path)
{ return {PathBase::SolutionDirectory, std::move(path), {}}; }

std::vector<ResourceDefinition> configured_resources(const ConfigValue& device)
{
    return {
        {uuid<ResourceId>('1'), "模板字节", ResourceKind::Binary,
            ManagedResource{relative("resources/模板.bin"), 7, std::string(64, 'a')}},
        {uuid<ResourceId>('2'), "外部字节", ResourceKind::Binary,
            ExternalFileResource{relative("../外部/采样.bin"), ResourceReadPolicy::PerInvocation, {}, {}}},
        {uuid<ResourceId>('3'), "掩膜", ResourceKind::Mask,
            ManagedResource{relative("resources/掩膜.bin"), 8, std::string(64, 'b')}},
        {uuid<ResourceId>('4'), "序列目录", ResourceKind::DirectorySequence,
            ImageSequenceResource{DirectorySelection{{PathBase::NamedRoot, "采集", key<SchemaId>("station")}, "*.png"},
                SequenceSort::NameDescending, SequenceAdvance::ExplicitCommand, SequenceRepeat::Loop, SequenceMissing::Skip,
                SequenceTrigger::External, 0}},
        {uuid<ResourceId>('5'), "明确顺序", ResourceKind::DirectorySequence,
            ImageSequenceResource{std::vector<ResourcePath>{relative("二.png"), relative("一.png")}, SequenceSort::ExplicitOrder,
                SequenceAdvance::PerInvocation, SequenceRepeat::Stop, SequenceMissing::Fail, SequenceTrigger::Manual, 1}},
        {uuid<ResourceId>('6'), "相机", ResourceKind::Device, DeviceResource{key<SchemaId>("roundtrip.adapter"), device}},
        {uuid<ResourceId>('7'), "结果目录", ResourceKind::OutputPath,
            OutputPathResource{relative("结果"), OutputNaming::CorrelationKey, "判定", "png", OutputOverwrite::UniqueSuffix, 8192}},
    };
}

GlobalVariableDefinition variable(char suffix, std::string name, const ConfigValue& value)
{ return {uuid<VariableId>(suffix), std::move(name), value.type(), value}; }

Solution configured_solution(const ToolDescriptor& tool)
{
    const auto frame = key<CoordinateSystemId>("roundtrip.image");
    const auto unit = MeasurementUnit::Pixel;
    const Transform2D transform{frame, frame, unit, unit, TransformModel::Affine, {1, 0, 1.25, 0, 1, -0.5, 0, 0, 1},
        {}, TransformDomain{0, 0, 64, 32}};
    const MaskConfig mask{{uuid<ResourceId>('3'), ResourceKind::Mask, 8}, frame, 64, 32,
        {PixelDepth::UInt8, ChannelOrder::Mono}, {frame, 64, 32, {}, MaskSampling::None}, {MaskValueMode::NonZero}};
    const auto region = take(ConfigValue::region2d({uuid<RegionId>(), frame, unit,
        RegionPrimitive{frame, unit, RotatedRect{8, 5, 4, 3, 0.25}, 0.125, transform},
        {RegionPrimitive{frame, unit, mask}}, RegionBoundaryPolicy::Fill, RegionFill{RegionFillMode::Constant, 0.5},
        EmptyRegionPolicy::ValidEmptyOutput}));
    const auto device_field = key<FieldId>("address");
    const auto device_schema = take(DataType::record(key<SchemaId>("roundtrip.device"), 2, {{device_field, DataType::string()}}));
    const auto device = take(ConfigValue::record(device_schema, {{device_field, take(ConfigValue::string("相机 A"))}}));
    const auto flow = configured_workflow(tool);
    return {uuid<SolutionId>(), "检测方案", 1, solution_revision, {flow}, configured_resources(device),
        {variable('1', "最小", ConfigValue::integer(lowest)), variable('2', "最大", ConfigValue::integer(highest)),
         variable('3', "空列表", take(ConfigValue::list(DataType::integer(), {}))),
         variable('4', "无值", ConfigValue::none(DataType::image())),
         variable('5', "嵌套可选", ConfigValue::some(ConfigValue::none(DataType::binary()))),
         variable('6', "二进制", ConfigValue::binary({0, 1, 0x80, 0xff})), variable('7', "测量区域", region)},
        {{uuid<QueueId>(), "测量队列", measurement_type(), 9, QueueOverflowPolicy::DropOldest,
            {EmptyQueuePolicy::ReturnEmpty, 3, QueueConsumerPolicy::CompetingConsumers, QueueBatchPolicy::RequireExact}}},
        {{frame, CoordinateKind::Image, unit, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}},
        {{flow.id, {{flow.nodes[0].id, -2.5, 1.125}, {flow.nodes[1].id, 11, 21}, {flow.nodes[2].id, 24, 16}}}}};
}

std::string read_manifest(const fs::path& directory)
{
    std::ifstream stream(directory / "solution.json", std::ios::binary);
    if (!stream) { throw std::runtime_error("Fixture manifest was not written."); }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void check_relations(const Solution& solution)
{
    const auto& flow = solution.workflows.at(0);
    check(solution.id == uuid<SolutionId>() && solution.revision == solution_revision && solution.format_version == 1
        && solution.name == "检测方案" && flow.id == uuid<WorkflowId>() && flow.name == "视觉流程",
        "Solution identity, revision, format version and UTF-8 names survive.");
    const auto& group = std::get<GroupConfiguration>(flow.nodes.at(0).configuration);
    const auto& link = flow.control_links.at(0);
    const auto& ownership = flow.containment.at(0);
    check(link.id == uuid<ControlLinkId>() && link.source == flow.nodes.at(0).id && link.target == flow.nodes.at(2).id
        && link.output == key<ControlOutputId>("done") && ownership.parent == link.source && ownership.child == flow.nodes.at(1).id,
        "Named control links and Group containment preserve their independent stable references.");
    const auto& child_scope = std::get<ScopedInputReference>(flow.nodes.at(1).bindings.at(0).sources.at(0));
    const auto& output = group.outputs.at(0).source;
    const auto& system = std::get<OutputReference>(flow.nodes.at(2).bindings.at(1).sources.at(0));
    check(child_scope.group == ownership.parent && child_scope.input == key<InputId>("seed")
        && output.node == ownership.child && output.selector == Selector{FieldSelection{key<FieldId>("items")}, ElementSelection{0}}
        && std::get<SystemField>(system.output) == SystemField::Elapsed && system.node == ownership.parent,
        "Scoped input, field/element selection and system-field data references round-trip.");
    const auto& root_input = flow.interface.inputs.at(0);
    const auto& group_control = group.control_exports.at(0);
    check(flow.interface.version == 3 && root_input.default_value == ConfigValue::integer(2)
        && root_input.constraints.integer_range->minimum == lowest && root_input.constraints.integer_range->maximum == highest
        && root_input.constraints.allowed_values == std::vector{ConfigValue::integer(2), ConfigValue::integer(5)}
        && group.interface.version == 2 && group_control.source == ownership.child && group_control.source_output == link.output,
        "Public schema versions, constraints, defaults and control exports round-trip.");
    const auto& layout = solution.layouts.at(0);
    check(layout.workflow == flow.id && layout.nodes.size() == 3 && layout.nodes.at(0).node == ownership.parent
        && layout.nodes.at(0).x == -2.5 && layout.nodes.at(0).y == 1.125,
        "Layout coordinates are preserved separately from execution relationships.");
}

void check_values(const Solution& actual, const Solution& original)
{
    check(actual.variables.size() == original.variables.size(), "All configured variable definitions survive.");
    for (std::size_t index = 0; index < original.variables.size(); ++index) {
        const auto& variable = actual.variables.at(index);
        const auto& expected = original.variables.at(index);
        check(variable.id == expected.id && variable.name == expected.name && variable.type == expected.type
            && variable.initial_value == expected.initial_value, "Variable ID, schema and explicit typed initial value round-trip.");
    }
    const auto& queue = actual.queues.at(0);
    check(queue.id == uuid<QueueId>() && queue.message_schema == measurement_type() && queue.capacity == 9
        && queue.overflow == QueueOverflowPolicy::DropOldest && queue.receive.empty == EmptyQueuePolicy::ReturnEmpty
        && queue.receive.maximum_batch_size == 3 && queue.receive.consumers == QueueConsumerPolicy::CompetingConsumers
        && queue.receive.batch == QueueBatchPolicy::RequireExact, "Queue record schema, capacity and receive policies survive.");
    check(actual.coordinates == original.coordinates, "Coordinate frame convention is retained.");
    const auto& managed = std::get<ManagedResource>(actual.resources.at(0).location);
    const auto& external = std::get<ExternalFileResource>(actual.resources.at(1).location);
    const auto& sequence = std::get<ImageSequenceResource>(actual.resources.at(3).location);
    check(actual.resources.size() == 7 && managed.path.path == "resources/模板.bin" && managed.revision == 7
        && managed.sha256 == std::string(64, 'a') && external.path.path == "../外部/采样.bin"
        && external.read_policy == ResourceReadPolicy::PerInvocation
        && std::get<DirectorySelection>(sequence.selection).directory.named_root == key<SchemaId>("station"),
        "Managed revisions and digests, external UTF-8 paths and named roots remain configured.");
}

void check_state_projection(const Solution& solution, const ToolRegistry& registry)
{
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(),
        {uuid<SolutionSessionId>(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}));
    const auto first = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>('2')));
    const auto second = take(PreparedTool::create(snapshot, uuid<WorkflowId>(), uuid<NodeId>('3')));
    const auto& local = first.configuration().effective_state();
    const auto& shared = second.configuration().effective_state();
    check(local && local->initial_value == ConfigValue::integer(-5) && local->scope == StateScope::WorkflowRun
        && local->reset == StateResetPolicy::DomainStart && shared && shared->initial_value == ConfigValue::integer(42)
        && shared->scope == StateScope::SolutionSession && shared->reset == StateResetPolicy::DomainStartAndExplicit,
        "The same tool type projects two independent initial values and reset policies after reload.");
    const auto& node = solution.workflows.at(0).nodes.at(1);
    check(std::get<ToolConfiguration>(node.configuration).config_version == 3
        && node.resources.at(0).resource == ResourceReference{uuid<ResourceId>('1'), ResourceKind::Binary, 7},
        "Tool configuration version and node slot reference stay independent of the file version.");
}

void roundtrip()
{
    const auto tool = counter_tool();
    const auto factory = std::make_shared<UnusedFactory>();
    const std::array registrations{ToolRegistration{tool, {tool.schema.id, 4, 4, {}}, factory}};
    const auto registry = take(ToolRegistry::create(registrations));
    const auto original = configured_solution(tool);
    take(validate_solution(original, registry.descriptors()));

    const PackageDirectories directories;
    const auto first = directories.package(u8"第一份方案");
    const auto second = directories.package(u8"第二份方案");
    take(write_new_solution(first, original));
    const auto manifest = read_manifest(first);
    const auto loaded = take(load_solution(first, registry));
    check(loaded.can_save() && fs::equivalent(first, loaded.directory()),
        "A recognized manifest under a Unicode package directory remains saveable.");
    check_relations(loaded.solution());
    check_values(loaded.solution(), original);
    check_state_projection(loaded.solution(), registry);
    take(write_new_solution(second, loaded, loaded.solution()));
    check(read_manifest(second) == manifest,
        "Writing the decoded package to a new destination produces identical manifest bytes.");
    const auto reopened = take(load_solution(second, registry));
    check(reopened.solution().variables.at(0).initial_value == ConfigValue::integer(lowest)
        && reopened.solution().variables.at(1).initial_value == ConfigValue::integer(highest) && factory->calls == 0,
        "Both Int64 boundaries reopen exactly and configuration handling leaves the tool factory unused.");
}

} // namespace

int main()
{
    try {
        roundtrip();
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
