#include <visionpro/workflow/resolver.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
    if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(diagnostic->message); }
    return std::get<T>(std::move(result));
}
template<UuidIdentity T> T uuid(unsigned value = 1)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template<SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }
template<RuntimeIdentity T> T number(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }
InputDescriptor input(std::string_view id, DataType type, BindingSourceKind source)
{
    InputDescriptor result{key<InputId>(id), std::string(id), std::move(type)};
    result.binding.allowed_sources = {source};
    return result;
}
ToolDescriptor tool(std::string_view type, std::string_view schema,
    std::vector<InputDescriptor> inputs, OutputDescriptor output)
{
    ToolDescriptor result{key<ToolTypeId>(type), std::string(type), 1,
        {key<SchemaId>(schema), 1}, {}, std::move(inputs), {std::move(output)}, {}};
    result.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono}};
    return result;
}
OutputReference source(unsigned node, std::string_view output, Selector selector = {})
{ return {uuid<WorkflowId>(), uuid<NodeId>(node), key<OutputId>(output), std::move(selector)}; }
ResultIdentity identity(const RunIdentity& run, unsigned node)
{
    return {run, {run.run, uuid<NodeId>(node), number<ExecutionScopeId>(0), number<InvocationId>(node)}, number<AttemptId>(0)};
}
NodeResult successful(const RunIdentity& run, unsigned node, std::string_view output, DataValue value)
{
    const auto type = value.type();
    return take(NodeResult::create({identity(run, node), ExecutionStatus::Succeeded, Judgement::OK,
        {{key<OutputId>(output), type, std::move(value)}}, {}, std::chrono::nanoseconds{100 + node}}));
}
ResolutionContext context(const RunRequest& request, const RunIdentity& run, unsigned consumer,
    std::initializer_list<unsigned> producers)
{
    std::vector<RunInput> inputs;
    for (const auto& input : request.inputs()) { inputs.push_back({input.id, input.value}); }
    ResolutionContext result{run, identity(run, consumer).invocation,
        {{number<ExecutionScopeId>(0), {}, {}, std::move(inputs)}}, {}, {}, {}};
    for (const auto producer : producers) {
        result.sources.push_back({uuid<NodeId>(producer), number<ExecutionScopeId>(0), number<InvocationId>(producer)});
    }
    return result;
}
const ResolvedBinding* resolved(const BindingResolution& result, std::string_view description)
{
    const auto* value = std::get_if<ResolvedBinding>(&result);
    check(value != nullptr, description);
    return value;
}
void result_trace(const ResolvedBinding& value, const RunIdentity& run, unsigned producer,
    const OutputReference& reference)
{
    const auto* trace = value.sources.size() == 1 ? std::get_if<ResultTrace>(&value.sources.front().origin) : nullptr;
    check(trace && trace->run == run && trace->scope == number<ExecutionScopeId>(0)
        && trace->invocation == number<InvocationId>(producer) && trace->attempt == number<AttemptId>(0)
        && trace->reference == reference, "resolved source identifies exact workflow/run/scope/call/attempt and selector");
}

void composition()
{
    const std::vector<std::byte> expected_pixels{std::byte{3}, std::byte{17}, std::byte{29}, std::byte{251}};
    std::optional<DataValue> surviving_image;
    {
        const auto camera = key<CoordinateSystemId>("camera");
        const auto measured = key<CoordinateSystemId>("measurement");
        const auto record_type = take(DataType::record(key<SchemaId>("composition.measurement"), 1,
            {{key<FieldId>("radius"), DataType::real()}}));
        std::vector<ToolDescriptor> tools{
            tool("composition.image", "composition.image.schema",
                {input("seed", DataType::image(), BindingSourceKind::PublicInput)},
                {key<OutputId>("image"), "Image", DataType::image(), {MeasurementUnit::Pixel, camera}}),
            tool("composition.locate", "composition.locate.schema",
                {input("image", DataType::image(), BindingSourceKind::RunOutput)},
                {key<OutputId>("transform"), "Transform", DataType::transform2d()}),
            tool("composition.measure", "composition.measure.schema",
                {input("image", DataType::image(), BindingSourceKind::RunOutput),
                 input("transform", DataType::transform2d(), BindingSourceKind::RunOutput)},
                {key<OutputId>("measurement"), "Measurement", record_type, {MeasurementUnit::Pixel, {}}}),
            tool("composition.judge", "composition.judge.schema",
                {input("radius", DataType::real(), BindingSourceKind::RunOutput)},
                {key<OutputId>("accepted"), "Accepted", DataType::boolean()})};
        tools[3].inputs[0].semantics.unit = MeasurementUnit::Pixel;
        tools[3].parameters.push_back({key<ParameterId>("limit"), "Limit", DataType::real(), false, take(ConfigValue::real(13.0))});
        Workflow flow{uuid<WorkflowId>(), "A image to B location to C measure to D judgement",
            {key<SchemaId>("composition.public"), 1, {input("seed", DataType::image(), BindingSourceKind::Constant)}}};
        for (unsigned index = 0; index < tools.size(); ++index) {
            flow.nodes.push_back({uuid<NodeId>(index + 1), std::string(1, static_cast<char>('A' + index)),
                ToolConfiguration{tools[index].id, 1}});
            if (index != 0) { flow.control_links.push_back({uuid<ControlLinkId>(index), uuid<NodeId>(index), {}, uuid<NodeId>(index + 1)}); }
        }
        flow.entries = {uuid<NodeId>(1)};
        flow.nodes[0].bindings = {{key<InputId>("seed"), {ScopedInputReference{flow.id, {}, key<InputId>("seed"), {}}}}};
        flow.nodes[1].bindings = {{key<InputId>("image"), {source(1, "image")}}};
        flow.nodes[2].bindings = {{key<InputId>("image"), {source(1, "image")}},
            {key<InputId>("transform"), {source(2, "transform")}}};
        const auto radius_source = source(3, "measurement", {FieldSelection{key<FieldId>("radius")}});
        flow.nodes[3].bindings = {{key<InputId>("radius"), {radius_source}}};
        Solution editable{uuid<SolutionId>(), "Composition fixture", 1, 1, {flow}};
        editable.coordinates = {
            {camera, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded},
            {measured, CoordinateKind::Local, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, 1}, Handedness::RightHanded}};
        const auto graph = take(validate_solution(editable, tools));
        check(graph.scopes.size() == 1 && graph.scopes[0].reachable.size() == 4
            && graph.scopes[0].control_links.size() == 3 && graph.scopes[0].data.size() == 4,
            "constructed A/B/C/D model validates with separate control and data dependencies");
        check(std::ranges::any_of(graph.scopes[0].data, [](const auto& dependency) {
            return dependency.producer == uuid<NodeId>(1) && dependency.consumer == uuid<NodeId>(3)
                && dependency.target == BindingTarget{key<InputId>("image")};
        }), "non-adjacent C image binding is explicitly derived from A");

        const SnapshotIdentity snapshot_identity{uuid<SolutionSessionId>(), number<ConfigSnapshotId>(1)};
        const auto snapshot = take(ConfigSnapshot::capture(editable, tools, snapshot_identity));
        const auto image = take(ImageHandle::copy({2, 2, 2, {PixelDepth::UInt8, ChannelOrder::Mono}}, expected_pixels, camera));
        std::vector<RunInput> provided{{key<InputId>("seed"), take(DataValue::image({image, {}, {}}))}};
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, provided,
            {TriggerKind::Manual, key<SchemaId>("composition.fixture")}));
        provided[0].value = DataValue::boolean(false);
        check(request.input(key<InputId>("seed")) && request.input(key<InputId>("seed"))->value.as_image()
            && request.snapshot().identity() == snapshot_identity,
            "prepared request owns the typed image and exact configuration snapshot");
        const auto resolver = take(BindingResolver::create(request.snapshot()));
        ResultStore store(snapshot_identity.session);
        const RunIdentity first{snapshot_identity, flow.id, number<WorkflowRunId>(1)};

        const auto seed_result = resolver.resolve(context(request, first, 1, {}), key<InputId>("seed"), store);
        if (const auto* seed = resolved(seed_result, "A resolves the current request's public image input")) {
            const auto* trace = seed->sources.size() == 1 ? std::get_if<PublicInputTrace>(&seed->sources[0].origin) : nullptr;
            check(trace && trace->run == first && trace->scope == number<ExecutionScopeId>(0)
                && trace->origin == InputOrigin::Provided, "A public input trace is tied to this request's run scope");
            take(store.insert(snapshot, successful(first, 1, "image", seed->value)));
        }
        const auto locate_result = resolver.resolve(context(request, first, 2, {1}), key<InputId>("image"), store);
        if (const auto* image_input = resolved(locate_result, "B resolves A's published image")) {
            result_trace(*image_input, first, 1, source(1, "image"));
        }
        // This test supplies declared artificial records. It never invokes a tool or scheduler.
        Transform2D transform{camera, measured, MeasurementUnit::Pixel, MeasurementUnit::Pixel};
        transform.matrix = {1, 0, 4.5, 0, 1, -2.0, 0, 0, 1};
        take(store.insert(snapshot, successful(first, 2, "transform", take(DataValue::transform2d(transform)))));

        editable.revision = 2;
        editable.workflows[0].nodes[2].name = "Edited measurement";
        editable.workflows[0].nodes[2].bindings[0].sources = {source(2, "transform")};
        tools[3].parameters[0].default_value = take(ConfigValue::real(1.0));
        const auto c_context = context(request, first, 3, {1, 2});
        const auto c_image_result = resolver.resolve(c_context, key<InputId>("image"), store);
        if (const auto* image_input = resolved(c_image_result, "C keeps its captured non-adjacent A image binding after edits")) {
            result_trace(*image_input, first, 1, source(1, "image"));
            check(image_input->value.as_image() && image_input->value.as_image()->image.copy_pixels() == expected_pixels,
                "C receives the original image pixels without a copy through B");
            surviving_image = image_input->value;
        }
        const auto c_transform_result = resolver.resolve(c_context, key<InputId>("transform"), store);
        if (const auto* transform_input = resolved(c_transform_result, "C independently resolves B's transform")) {
            result_trace(*transform_input, first, 2, source(2, "transform"));
            check(transform_input->value.as_transform2d() && *transform_input->value.as_transform2d() == transform,
                "C retains transform coordinates, units and matrix values");
        }
        check(snapshot.solution().revision == 1 && snapshot.solution().workflows[0].nodes[2].name == "C",
            "configuration and tool edits do not mutate the captured revision");
        const auto measurement = take(DataValue::record(record_type,
            {{key<FieldId>("radius"), take(DataValue::real(12.4))}}));
        take(store.insert(snapshot, successful(first, 3, "measurement", measurement)));
        const auto d_context = context(request, first, 4, {3});
        const auto radius_result = resolver.resolve(d_context, key<InputId>("radius"), store);
        if (const auto* radius = resolved(radius_result, "D resolves C.radius from the current artificial record")) {
            check(radius->value.as_float64() && std::abs(*radius->value.as_float64() - 12.4) < 1e-12,
                "field-selected radius equals the fixture value 12.4");
            result_trace(*radius, first, 3, radius_source);
        }
        const auto limit_result = resolver.resolve(d_context, key<ParameterId>("limit"), store);
        if (const auto* limit = resolved(limit_result, "D parameter uses its captured default independently of model edits")) {
            check(limit->value == take(DataValue::real(13.0)), "captured limit remains 13.0 after descriptor was edited to 1.0");
        }
        take(store.insert(snapshot, successful(first, 4, "accepted", DataValue::boolean(true))));
        const auto d_record = take(store.lookup(first, identity(first, 4).invocation));
        check(d_record && d_record->data().judgement == Judgement::OK && d_record->data().status == ExecutionStatus::Succeeded,
            "artificial D terminal record is addressable separately from the resolved input values");

        const RunIdentity second{snapshot_identity, flow.id, number<WorkflowRunId>(2)};
        auto new_context = context(request, second, 4, {3});
        const auto waiting = resolver.resolve(new_context, key<InputId>("radius"), store);
        check(std::holds_alternative<PendingBinding>(waiting), "new run with no C record stays Pending instead of reusing old 12.4");
        if (const auto* pending = std::get_if<PendingBinding>(&waiting)) {
            const auto* trace = pending->sources.size() == 1 ? std::get_if<ResultTrace>(&pending->sources[0].origin) : nullptr;
            check(trace && trace->run == second && !trace->attempt, "Pending trace identifies the new run without inventing a completed attempt");
        }
        DiagnosticLocation origin; origin.workflow = flow.id; origin.node = uuid<NodeId>(3); origin.output = key<OutputId>("measurement");
        const Diagnostic source_error{ErrorCode::InvalidValue, Severity::Error, origin, "Artificial measurement failure"};
        const auto failed = take(NodeResult::create({identity(second, 3), ExecutionStatus::Failed, Judgement::NotEvaluated,
            {{key<OutputId>("measurement"), record_type, NotProduced{OutputAbsenceReason::Failed}}}, {source_error}}));
        take(store.insert(snapshot, failed));
        const auto failure_result = resolver.resolve(new_context, key<InputId>("radius"), store);
        const auto* failure = std::get_if<BindingFailure>(&failure_result);
        check(failure && failure->diagnostic.code == ErrorCode::SourceFailed,
            "failed C in the new run reports failure rather than returning prior radius");
        check(failure && failure->causes.size() == 1 && failure->causes[0].code == source_error.code
            && failure->causes[0].location.node == source_error.location.node,
            "new-run failure retains the artificial producer's original diagnostic");
        check(take(store.lookup(first, identity(first, 3).invocation)).has_value(),
            "old result may remain stored without being eligible for the new run binding");
        take(store.release_run(first)); take(store.release_run(second));
        check(store.size() == 0, "retiring both artificial runs releases their stored records");
    }
    check(surviving_image && surviving_image->as_image()
        && surviving_image->as_image()->image.copy_pixels() == expected_pixels,
        "resolved image retains readable ownership after store, request, snapshot and source handles leave scope");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string_view(argv[1]) != "composition") { return 2; }
    try { composition(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << "composition: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
