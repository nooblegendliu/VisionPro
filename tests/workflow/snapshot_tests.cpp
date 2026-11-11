#include <visionpro/workflow/snapshot.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

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
template <class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
template <class T> void invalid(const Result<T>& result, std::string_view description)
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, description);
    check(error && !error->message.empty() && error->severity == Severity::Error, "snapshot failure retains an explained diagnostic");
}
template <UuidIdentity T> T uuid(unsigned value)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template <SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
SnapshotIdentity identity(unsigned session = 1, std::uint64_t revision = 1)
{ return {uuid<SolutionSessionId>(session), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(revision))}; }
PublicInterface ports()
{
    return {key<SchemaId>("group.public"), 1,
        {{key<InputId>("number"), "Number", DataType::integer()}},
        {{key<OutputId>("result"), "Result", DataType::integer()}},
        {{key<ControlOutputId>("complete"), "Complete"}}};
}
Solution fixture()
{
    const auto flow_id = uuid<WorkflowId>(1);
    const auto group_id = uuid<NodeId>(1), leaf_id = uuid<NodeId>(2);
    const auto input = key<InputId>("number");
    const auto output = key<OutputId>("result");
    Node group{group_id, "Group name long enough for owned string storage",
        GroupConfiguration{ports(), {leaf_id}, {{output, {flow_id, leaf_id, output, {}}}},
            {{key<ControlOutputId>("complete"), leaf_id, {}}}},
        {{input, {ScopedInputReference{flow_id, {}, input, {}}}}}};
    Node leaf{leaf_id, "Leaf", ToolConfiguration{key<ToolTypeId>("test.inspect"), 7},
        {{input, {ScopedInputReference{flow_id, group_id, input, {}}}},
            {key<ParameterId>("threshold"), {ConfigValue::integer(5)}}},
        {{key<SchemaId>("source"), {uuid<ResourceId>(1), ResourceKind::Image, 3}}}};
    Workflow flow{flow_id, "Workflow", ports(), {group, leaf}, {}, {{group_id, leaf_id}}, {group_id},
        {{output, {flow_id, group_id, output, {}}}}};
    flow.control_exports = {{key<ControlOutputId>("complete"), group_id, key<ControlOutputId>("complete")}};
    const auto record = take(DataType::record(key<SchemaId>("queue.payload"), 1, {{key<FieldId>("count"), DataType::integer()}}));
    Solution result{uuid<SolutionId>(1), "Solution", 2, 12, {flow},
        {{uuid<ResourceId>(1), "Image", ResourceKind::Image,
            ManagedResource{{PathBase::SolutionDirectory, "resources/source.png", {}}, 3, std::string(64, 'a')}}},
        {{uuid<VariableId>(1), "Count", DataType::integer(), ConfigValue::integer(0)}},
        {{uuid<QueueId>(1), "Measurements", record, 4, QueueOverflowPolicy::Reject,
            {EmptyQueuePolicy::ReturnEmpty, 2, QueueConsumerPolicy::SingleConsumer, QueueBatchPolicy::UpToLimit}}},
        {{key<CoordinateSystemId>("sensor"), CoordinateKind::Image, MeasurementUnit::Pixel,
            {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}},
        {{flow_id, {{group_id, 1, 2}, {leaf_id, 3, 4}}}}};
    return result;
}
ToolDescriptor descriptor()
{
    ToolDescriptor result{key<ToolTypeId>("test.inspect"), "Inspect", 7,
        {key<SchemaId>("test.inspect.schema"), 2, {}, true},
        {{key<ParameterId>("threshold"), "Threshold label longer than short storage", DataType::integer(), false, ConfigValue::integer(5)}},
        {{key<InputId>("number"), "Number", DataType::integer()}},
        {{key<OutputId>("result"), "Result", DataType::integer()}}, {}};
    result.parameters.front().constraints.integer_range = IntegerRange{1, 20};
    result.parameters.front().constraints.allowed_values = {ConfigValue::integer(5), ConfigValue::integer(6)};
    result.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0), StateScope::SolutionSession,
        StateResetPolicy::DomainStartAndExplicit, StateCompatibilityPolicy::PreserveCompatible,
        StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
    result.resources = {{key<SchemaId>("source"), ResourceKind::Image, true}};
    result.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono}};
    return result;
}

static_assert(!std::is_default_constructible_v<ConfigSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<ConfigSnapshot&>().solution()), const Solution&>);
static_assert(std::is_same_v<decltype(std::declval<ConfigSnapshot&>().tools()), std::span<const ToolDescriptor>>);
static_assert(std::is_same_v<decltype(std::declval<ConfigSnapshot&>().find_tool(std::declval<const ToolTypeId&>())), const ToolDescriptor*>);
static_assert(std::is_const_v<std::remove_reference_t<decltype(std::declval<ConfigSnapshot&>().solution().workflows.front())>>);
static_assert(!std::is_constructible_v<ConfigSnapshot, Solution>);

void snapshots()
{
    auto source = fixture();
    std::vector<ToolDescriptor> tools{descriptor()};
    auto* node_alias = &source.workflows.front().nodes.front();
    auto* name_alias = node_alias->name.data();
    auto* binding_alias = &source.workflows.front().nodes.back().bindings.back();
    auto* resource_alias = &source.resources.front();
    auto* group_control_alias = &std::get<GroupConfiguration>(source.workflows.front().nodes.front().configuration).control_exports.front();
    auto* workflow_control_alias = &source.workflows.front().control_exports.front();
    auto* parameter_alias = &tools.front().parameters.front();
    auto* parameter_label_alias = parameter_alias->display_name.data();
    auto* allowed_alias = &parameter_alias->constraints.allowed_values.front();
    const auto captured = take(ConfigSnapshot::capture(std::move(source), tools, identity()));
    check(captured.identity() == identity() && captured.solution().revision == 12 && captured.solution().format_version == 2,
        "snapshot identity, editable revision and file format version remain distinct");
    check(source.workflows.front().nodes.data() == node_alias && source.resources.data() == resource_alias,
        "capture does not steal rvalue source containers that still have writable aliases");
    node_alias->name[0] = 'X';
    *name_alias = 'Y';
    binding_alias->sources.front() = ConfigValue::integer(6);
    resource_alias->name = "Changed image";
    std::get<ManagedResource>(resource_alias->location).revision = 4;
    parameter_alias->default_value = ConfigValue::integer(6);
    *parameter_label_alias = 'X';
    *allowed_alias = ConfigValue::integer(99);
    tools.front().state->initial_value = ConfigValue::integer(42);
    tools.front().resources.front().kind = ResourceKind::Mask;
    tools.front().image_formats.front().order = ChannelOrder::RGB;
    source.variables.front().initial_value = ConfigValue::integer(99);
    source.queues.front().capacity = 999;
    source.coordinates.front().origin.x = 42;
    source.layouts.front().nodes.front().x = 999;
    std::get<GroupConfiguration>(source.workflows.front().nodes.front().configuration).interface.inputs.front().display_name = "Changed public input";
    std::get<GroupConfiguration>(source.workflows.front().nodes.front().configuration).outputs.front().source.node = uuid<NodeId>(999);
    group_control_alias->source = uuid<NodeId>(999);
    group_control_alias->source_output = key<ControlOutputId>("changed");
    workflow_control_alias->output = key<ControlOutputId>("changed");
    source.revision = 13;
    source.workflows.front().enabled = false;
    const auto& frozen = captured.solution();
    check(frozen.workflows.front().nodes.front().name == "Group name long enough for owned string storage",
        "retained editable string pointers cannot change a captured node");
    check(*std::get<ConfigValue>(frozen.workflows.front().nodes.back().bindings.back().sources.front()).as_int64() == 5,
        "replacing an editable binding source cannot change the snapshot's value");
    check(frozen.resources.front().name == "Image" && std::get<ManagedResource>(frozen.resources.front().location).revision == 3,
        "resource metadata and locked revision are copied without loading resource bytes");
    check(*frozen.variables.front().initial_value.as_int64() == 0 && frozen.queues.front().capacity == 4,
        "shared-variable and queue definitions do not acquire mutable runtime state");
    check(frozen.coordinates.front().origin.x == 0 && frozen.layouts.front().nodes.front().x == 1,
        "coordinates and layout remain part of the captured configuration revision");
    const auto& group = std::get<GroupConfiguration>(frozen.workflows.front().nodes.front().configuration);
    check(group.interface.inputs.front().display_name == "Number" && group.outputs.front().source.node == uuid<NodeId>(2),
        "nested group interface and export vectors are detached from editing");
    check(group.control_exports.front().source == uuid<NodeId>(2) && !group.control_exports.front().source_output
        && frozen.workflows.front().control_exports.front().output == key<ControlOutputId>("complete"),
        "retained aliases to group and workflow control mapping vectors cannot change captured control exports");
    check(frozen.revision == 12 && frozen.workflows.front().enabled, "an edit or disable affects subsequent snapshots only");
    const auto* frozen_tool = captured.find_tool(key<ToolTypeId>("test.inspect"));
    if (!frozen_tool) { throw std::runtime_error("Captured tool missing"); }
    check(frozen_tool->parameters.front().display_name == "Threshold label longer than short storage"
        && *frozen_tool->parameters.front().default_value->as_int64() == 5,
        "tool names and default values are independent from retained metadata aliases");
    check(*frozen_tool->parameters.front().constraints.allowed_values.front().as_int64() == 5,
        "allowed-values vectors cannot be modified after snapshot publication");
    check(*frozen_tool->state->initial_value.as_int64() == 0
        && frozen_tool->resources.front().kind == ResourceKind::Image && frozen_tool->image_formats.front().order == ChannelOrder::Mono,
        "state/resource/format capabilities remain consistent within one snapshot");
    check(captured.find_tool(key<ToolTypeId>("absent")) == nullptr, "tool lookup does not substitute a name or version for a missing identity");
    auto copy = captured;
    auto moved = std::move(copy);
    check(moved.identity() == captured.identity() && copy.solution().revision == 12,
        "snapshot copies and moves preserve valid ownership without borrowing the editor");
    const auto other_session = take(ConfigSnapshot::capture(fixture(), std::vector{descriptor()}, identity(2, 1)));
    const auto next_snapshot = take(ConfigSnapshot::capture(fixture(), std::vector{descriptor()}, identity(1, 2)));
    check(other_session.identity() != captured.identity() && next_snapshot.identity() != captured.identity(),
        "both session and counter participate in snapshot identity");

    std::optional<ConfigSnapshot> retained;
    {
        auto temporary_solution = fixture();
        std::vector<ToolDescriptor> temporary_tools{descriptor()};
        retained = take(ConfigSnapshot::capture(temporary_solution, temporary_tools, identity(3, 1)));
        temporary_solution.workflows.clear();
        temporary_tools.clear();
    }
    check(retained->solution().workflows.front().nodes.size() == 2
        && retained->tools().front().parameters.front().display_name == "Threshold label longer than short storage",
        "snapshot remains complete after the editor and metadata containers are destroyed");
    auto draft = fixture();
    draft.workflows.front().entries.clear();
    std::get<GroupConfiguration>(draft.workflows.front().nodes.front().configuration).entries.clear();
    const auto without_tools = take(ConfigSnapshot::capture(draft, {}, identity(4, 1)));
    check(without_tools.tools().empty() && without_tools.solution().workflows.front().entries.empty(),
        "configuration capture preserves incomplete drafts rather than pretending to generate an executable plan");

    auto invalid_model = fixture(); invalid_model.workflows.front().nodes.push_back(invalid_model.workflows.front().nodes.back());
    invalid(ConfigSnapshot::capture(invalid_model, {}, identity()), "snapshot capture rejects locally invalid duplicate node identity");
    std::vector<ToolDescriptor> repeated{descriptor(), descriptor()};
    invalid(ConfigSnapshot::capture(fixture(), repeated, identity()), "tool metadata identities cannot conflict within a snapshot");
    repeated = {descriptor()}; repeated.front().config_version = 0;
    invalid(ConfigSnapshot::capture(fixture(), repeated, identity()), "snapshot capture applies tool descriptor validation");
    check(captured.solution().revision == 12 && captured.tools().front().config_version == 7,
        "a failed capture leaves earlier snapshots unchanged");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "snapshots") { return 2; }
    try { snapshots(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << "snapshots: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
