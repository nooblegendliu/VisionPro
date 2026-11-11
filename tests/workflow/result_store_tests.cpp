#include <visionpro/workflow/result_store.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
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
template <class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
template <class T> Diagnostic rejected(const Result<T>& result, ErrorCode code, std::string_view description)
{
    const auto* error = std::get_if<Diagnostic>(&result);
    check(error != nullptr, description);
    if (!error) { throw std::runtime_error("Expected rejection"); }
    check(error->code == code && error->severity == Severity::Error && !error->message.empty(),
        "rejection identifies and explains its cause");
    return *error;
}
template <UuidIdentity T> T uuid(unsigned value = 1)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template <SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
template <RuntimeIdentity T> T counter(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }
OutputId value_id() { return key<OutputId>("value"); }
OutputId optional_id() { return key<OutputId>("optional"); }
OutputId list_id() { return key<OutputId>("list"); }
struct Fixture { Solution solution; std::vector<ToolDescriptor> tools; };
Fixture fixture()
{
    ToolDescriptor tool{key<ToolTypeId>("test.output"), "Output", 3,
        {key<SchemaId>("test.output.schema"), 1}, {}, {},
        {{value_id(), "Value", DataType::integer()},
            {optional_id(), "Optional", DataType::optional(DataType::integer())},
            {list_id(), "List", DataType::list(DataType::integer())}},
        {{key<ControlOutputId>("complete"), "Complete"}}};
    const auto node = [&](unsigned number) {
        return Node{uuid<NodeId>(number), "Node " + std::to_string(number), ToolConfiguration{tool.id, 3}};
    };
    Workflow flow{uuid<WorkflowId>(), "Workflow", {key<SchemaId>("workflow.public"), 1, {}, {}, {}},
        {node(1), node(2)}, {}, {}, {uuid<NodeId>(1)}};
    auto other = flow; other.id = uuid<WorkflowId>(2); other.name = "Other";
    other.nodes = {node(3)}; other.entries = {uuid<NodeId>(3)};
    return {{uuid<SolutionId>(), "Solution", 1, 1, {flow, other}}, {tool}};
}
ConfigSnapshot capture(const Fixture& setup, std::uint64_t version = 1, unsigned session = 1)
{
    return take(ConfigSnapshot::capture(setup.solution, setup.tools,
        {uuid<SolutionSessionId>(session), counter<ConfigSnapshotId>(version)}));
}
ResultIdentity identity(const ConfigSnapshot& snapshot, std::uint64_t run = 1, unsigned node = 1,
    std::uint64_t scope = 0, std::uint64_t invocation = 1, unsigned workflow = 1)
{
    return {{snapshot.identity(), uuid<WorkflowId>(workflow), counter<WorkflowRunId>(run)},
        {counter<WorkflowRunId>(run), uuid<NodeId>(node), counter<ExecutionScopeId>(scope), counter<InvocationId>(invocation)},
        counter<AttemptId>(1)};
}
NodeResultData success(const ResultIdentity& id, std::int64_t value = 12)
{
    return {id, ExecutionStatus::Succeeded, Judgement::NG,
        {{value_id(), DataType::integer(), DataValue::integer(value)},
            {optional_id(), DataType::optional(DataType::integer()), DataValue::none(DataType::integer())},
            {list_id(), DataType::list(DataType::integer()), take(DataValue::list(DataType::integer(), {}))}},
        {}, std::chrono::nanoseconds{20}, {key<ControlOutputId>("complete")}};
}
Diagnostic explanation()
{ return {ErrorCode::InvalidValue, Severity::Error, {}, "Artificial tool failure"}; }

void result_store()
{
    const auto setup = fixture();
    const auto snapshot = capture(setup);
    const auto id = identity(snapshot);
    ResultStore store{snapshot.identity().session};
    check(store.session() == snapshot.identity().session && store.size() == 0, "store starts empty in its declared session");
    check(!take(store.lookup(id.run, id.invocation)), "unrecorded current invocation is absent, not an invented result");
    const auto first = take(NodeResult::create(success(id)));
    take(store.insert(snapshot, first));
    check(store.size() == 1, "valid complete success is inserted once");
    const auto found = take(store.lookup(id.run, id.invocation));
    check(found && *std::get<DataValue>(found->output(value_id())->content).as_int64() == 12,
        "lookup reads the exact recorded invocation value");
    check(found && found->data().judgement == Judgement::NG, "NG remains a successful available result");
    check(found && !std::get<DataValue>(found->output(optional_id())->content).optional_value(),
        "explicit Optional None is an available value");
    check(found && std::get<DataValue>(found->output(list_id())->content).elements()->empty(),
        "empty collection is an available value");
    rejected(store.insert(snapshot, first), ErrorCode::DuplicateResult, "duplicate logical invocation is not overwritten");
    auto retry = success(id, 99); retry.identity.attempt = counter<AttemptId>(2);
    rejected(store.insert(snapshot, take(NodeResult::create(retry))), ErrorCode::DuplicateResult,
        "different attempt cannot overwrite an accepted logical invocation");
    check(*std::get<DataValue>(take(store.lookup(id.run, id.invocation))->output(value_id())->content).as_int64() == 12,
        "duplicate rejection preserves the original result");

    const auto another_scope = identity(snapshot, 1, 1, 2);
    check(!take(store.lookup(another_scope.run, another_scope.invocation)), "same node in a new scope has no old-scope fallback");
    take(store.insert(snapshot, take(NodeResult::create(success(another_scope, 21)))));
    const auto another_invocation = identity(snapshot, 1, 1, 0, 2);
    check(!take(store.lookup(another_invocation.run, another_invocation.invocation)), "new logical invocation does not reuse the prior call");
    take(store.insert(snapshot, take(NodeResult::create(success(another_invocation, 22)))));
    const auto another_node = identity(snapshot, 1, 2);
    check(!take(store.lookup(another_node.run, another_node.invocation)), "same-type node retains a separate result identity");
    take(store.insert(snapshot, take(NodeResult::create(success(another_node, 23)))));
    const auto another_run = identity(snapshot, 2);
    check(!take(store.lookup(another_run.run, another_run.invocation)), "new run does not read a historical result");
    take(store.insert(snapshot, take(NodeResult::create(success(another_run, 24)))));
    check(store.size() == 5, "run, scope, invocation and node dimensions remain distinct");

    auto wrong_lookup = id.run; wrong_lookup.workflow = uuid<WorkflowId>(2);
    rejected(store.lookup(wrong_lookup, id.invocation), ErrorCode::ResultIdentityMismatch,
        "lookup with another workflow cannot reinterpret a registered run");
    wrong_lookup = id.run; wrong_lookup.snapshot.snapshot = counter<ConfigSnapshotId>(2);
    rejected(store.lookup(wrong_lookup, id.invocation), ErrorCode::ResultIdentityMismatch,
        "lookup with another snapshot cannot reinterpret a registered run");
    wrong_lookup = id.run; wrong_lookup.snapshot.session = uuid<SolutionSessionId>(2);
    rejected(store.lookup(wrong_lookup, id.invocation), ErrorCode::ResultIdentityMismatch, "lookup cannot cross session boundary");
    rejected(store.lookup(id.run, another_run.invocation), ErrorCode::ResultIdentityMismatch,
        "lookup requires invocation and requested run IDs to agree");
    const auto later_snapshot = capture(setup, 2);
    rejected(store.insert(later_snapshot, first), ErrorCode::ResultIdentityMismatch,
        "insert requires the record's original snapshot identity");
    rejected(store.insert(later_snapshot, take(NodeResult::create(success(identity(later_snapshot))))),
        ErrorCode::ResultIdentityMismatch, "same run ID cannot be registered against a newer snapshot");
    rejected(store.insert(snapshot, take(NodeResult::create(success(identity(snapshot, 1, 3, 0, 1, 2))))),
        ErrorCode::ResultIdentityMismatch, "same run ID cannot be reused by another workflow");
    const auto other_session = capture(setup, 1, 2);
    rejected(store.insert(other_session, take(NodeResult::create(success(identity(other_session))))),
        ErrorCode::ResultIdentityMismatch, "insert cannot cross session boundary");
    ResultStore independent{other_session.identity().session};
    take(independent.insert(other_session, take(NodeResult::create(success(identity(other_session), 88)))));
    check(independent.size() == 1 && store.size() == 5, "separate session stores may use the same numeric run IDs independently");

    const auto candidate = identity(snapshot, 3);
    auto invalid = success(candidate); invalid.outputs.erase(invalid.outputs.begin() + 1);
    auto diagnostic = rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::InvalidNodeResult,
        "successful result cannot omit a declared Optional output");
    check(diagnostic.location.solution == snapshot.solution().id && diagnostic.location.workflow == candidate.run.workflow
        && diagnostic.location.node == candidate.invocation.node && diagnostic.location.output == optional_id(),
        "output validation failure retains solution, workflow, node and output identity");
    invalid = success(candidate); invalid.outputs[0].content = NotProduced{OutputAbsenceReason::ToolDidNotProduce};
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::InvalidNodeResult,
        "success cannot use unavailable output when its Schema has no missing-value permission");
    invalid = success(candidate); invalid.outputs[0].id = key<OutputId>("unknown");
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::UnknownTarget,
        "result cannot introduce an undeclared output");
    invalid = success(candidate); invalid.outputs[0].type = DataType::boolean(); invalid.outputs[0].content = DataValue::boolean(false);
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::TypeMismatch,
        "valid self-consistent result value must still match the tool output Schema");
    invalid = success(candidate); invalid.selected_control_outputs = {key<ControlOutputId>("unknown")};
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::InvalidNodeResult,
        "result cannot select an undeclared control outlet");
    invalid = success(identity(snapshot, 3, 99));
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::ResultIdentityMismatch,
        "result node must exist inside the stated workflow");
    invalid = success(identity(snapshot, 3, 3));
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::ResultIdentityMismatch,
        "existing node from another workflow cannot be accepted");
    invalid = success(identity(snapshot, 3, 1, 0, 1, 99));
    rejected(store.insert(snapshot, take(NodeResult::create(invalid))), ErrorCode::ResultIdentityMismatch,
        "result workflow must exist in the snapshot");
    const auto other_workflow = identity(snapshot, 3, 3, 0, 1, 2);
    take(store.insert(snapshot, take(NodeResult::create(success(other_workflow, 33)))));
    check(take(store.lookup(other_workflow.run, other_workflow.invocation)).has_value(),
        "failed inserts do not reserve the rejected run identity or publish partial records");

    auto missing_metadata = setup; missing_metadata.tools.clear();
    const auto missing_snapshot = capture(missing_metadata, 3);
    rejected(store.insert(missing_snapshot, take(NodeResult::create(success(identity(missing_snapshot, 4))))),
        ErrorCode::UnknownTool, "snapshot draft with missing tool cannot accept typed outputs");
    auto version_mismatch = setup;
    std::get<ToolConfiguration>(version_mismatch.solution.workflows[0].nodes[0].configuration).config_version = 4;
    const auto wrong_version_snapshot = capture(version_mismatch, 4);
    rejected(store.insert(wrong_version_snapshot, take(NodeResult::create(success(identity(wrong_version_snapshot, 4))))),
        ErrorCode::IncompatibleSchemaVersion, "result requires exact tool configuration version");

    auto failed_data = success(identity(snapshot, 10));
    failed_data.status = ExecutionStatus::Failed; failed_data.judgement = Judgement::NotEvaluated;
    failed_data.outputs.clear(); failed_data.selected_control_outputs.clear(); failed_data.diagnostics = {explanation()};
    take(store.insert(snapshot, take(NodeResult::create(failed_data))));
    check(take(store.lookup(failed_data.identity.run, failed_data.identity.invocation))->data().outputs.empty(),
        "failed result may be recorded without publishing outputs");
    failed_data.identity = identity(snapshot, 11);
    failed_data.outputs = {{value_id(), DataType::integer(), InvalidOutput{explanation()}},
        {optional_id(), DataType::optional(DataType::integer()), NotProduced{OutputAbsenceReason::Failed}}};
    take(store.insert(snapshot, take(NodeResult::create(failed_data))));
    check(take(store.lookup(failed_data.identity.run, failed_data.identity.invocation))->output(value_id()) != nullptr,
        "failed result retains typed invalid and not-produced slots for diagnostics");
    failed_data.identity = identity(snapshot, 12); failed_data.outputs[0].type = DataType::boolean();
    rejected(store.insert(snapshot, take(NodeResult::create(failed_data))), ErrorCode::TypeMismatch,
        "failed output slots cannot bypass declared Schema validation");
    failed_data.outputs[0].type = DataType::integer(); failed_data.outputs[0].id = key<OutputId>("unknown");
    rejected(store.insert(snapshot, take(NodeResult::create(failed_data))), ErrorCode::UnknownTarget,
        "failed records cannot introduce unknown output slots");

    auto group_setup = setup;
    const auto& tool = group_setup.tools[0];
    group_setup.solution.workflows[0].nodes[1].configuration = GroupConfiguration{
        {key<SchemaId>("group.public"), 1, {}, tool.outputs, tool.control_outputs}, {}, {}, {}};
    const auto group_snapshot = capture(group_setup, 5);
    take(store.insert(group_snapshot, take(NodeResult::create(success(identity(group_snapshot, 20, 2))))));
    check(take(store.lookup(identity(group_snapshot, 20, 2).run, identity(group_snapshot, 20, 2).invocation)).has_value(),
        "Group results validate against their public outputs and control outlets");

    auto roi_setup = fixture();
    const auto roi_frame = key<CoordinateSystemId>("roi.frame");
    roi_setup.solution.coordinates = {{roi_frame, CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
    roi_setup.tools[0].outputs = {{value_id(), "Region", DataType::region2d(), {MeasurementUnit::Pixel, roi_frame}}};
    const auto roi_snapshot = capture(roi_setup, 6);
    const RegionDefinition unresolved{uuid<RegionId>(), roi_frame, MeasurementUnit::Pixel,
        InheritedRegion{uuid<WorkflowId>(), uuid<NodeId>(2), value_id(), roi_frame, MeasurementUnit::Pixel}, {}};
    const auto roi_identity = identity(roi_snapshot, 21);
    const auto unresolved_result = take(NodeResult::create({roi_identity, ExecutionStatus::Succeeded, Judgement::OK,
        {{value_id(), DataType::region2d(), take(DataValue::region2d(unresolved))}}, {}}));
    rejected(store.insert(roi_snapshot, unresolved_result), ErrorCode::InvalidValue,
        "published runtime ROI cannot retain an unresolved output reference");

    const auto size_before_release = store.size();
    rejected(store.release_run(wrong_lookup), ErrorCode::ResultIdentityMismatch, "release cannot cross session boundary");
    wrong_lookup = id.run; wrong_lookup.workflow = uuid<WorkflowId>(2);
    rejected(store.release_run(wrong_lookup), ErrorCode::ResultIdentityMismatch, "release cannot reinterpret a registered run identity");
    take(store.release_run(id.run));
    check(store.size() == size_before_release - 4, "release removes exactly the selected run's records");
    check(take(store.lookup(another_run.run, another_run.invocation)).has_value(), "release preserves other runs");
    rejected(store.lookup(id.run, id.invocation), ErrorCode::ResultRetired, "released run is not reported as a pending missing result");
    rejected(store.insert(snapshot, first), ErrorCode::ResultRetired, "late result cannot resurrect a retired run");
    take(store.release_run(id.run));
    const auto retired_before_insert = identity(snapshot, 90);
    take(store.release_run(retired_before_insert.run));
    rejected(store.insert(snapshot, take(NodeResult::create(success(retired_before_insert)))), ErrorCode::ResultRetired,
        "retiring an empty run also rejects its late first result");
    check(found && *std::get<DataValue>(found->output(value_id())->content).as_int64() == 12,
        "previously returned records retain ownership after store release");

    std::optional<NodeResult> image_record;
    {
        auto image_setup = fixture();
        const auto frame = key<CoordinateSystemId>("camera");
        image_setup.solution.coordinates = {{frame, CoordinateKind::Image, MeasurementUnit::Pixel,
            {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
        image_setup.tools[0].outputs = {{value_id(), "Image", DataType::image(), {MeasurementUnit::Pixel, frame}}};
        image_setup.tools[0].image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono}};
        const auto image_snapshot = capture(image_setup);
        const std::array pixels{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
        const auto handle = take(ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, pixels, frame));
        const auto value = take(DataValue::image({handle, std::nullopt, std::nullopt}));
        const auto image_identity = identity(image_snapshot);
        const auto image_result = take(NodeResult::create({image_identity, ExecutionStatus::Succeeded, Judgement::OK,
            {{value_id(), DataType::image(), value}}, {}}));
        ResultStore image_store{image_snapshot.identity().session};
        take(image_store.insert(image_snapshot, image_result));
        image_record = take(image_store.lookup(image_identity.run, image_identity.invocation));
        check(std::get<DataValue>(image_record->output(value_id())->content).as_image()->image.shares_storage_with(handle),
            "store shares published immutable image ownership without copying pixels");
        take(image_store.release_run(image_identity.run));
        check(image_store.size() == 0, "image run releases its store-held record");
    }
    check(image_record && std::get<DataValue>(image_record->output(value_id())->content).as_image()->image.copy_pixels()
        == std::vector{std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}},
        "retained image result remains readable after store, snapshot and original handles are destroyed");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "result_store") { return 2; }
    try { result_store(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << "result_store: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
