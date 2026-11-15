#include "vision/roi.hpp"
#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
namespace vision = visionpro::tools::vision;

constexpr auto pixels = MeasurementUnit::Pixel;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr std::size_t height = 9;
constexpr std::size_t target_width = 9;
using Pixel = std::array<std::size_t, 2>;

void check(bool passed, std::string_view message)
{
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* diagnostic = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(diagnostic->message); }
    return std::get<T>(std::move(value));
}

template<SchemaIdentity Id> Id key(std::string_view name) { return take(SchemaIdFactory::parse<Id>(name)); }

template<UuidIdentity Id> Id uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<Id>(text));
}

CoordinateSystemId camera() { return key<CoordinateSystemId>("s43.camera"); }

class EvaluationTool final : public ToolInstance {
public:
    explicit EvaluationTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto& image = *invocation.inputs.front().value.as_image();
        const auto& roi = invocation.parameters.front();
        auto evaluated = vision::evaluate_roi(image.image, *roi.value.as_region2d(), ToolValueTarget{roi.id},
            invocation.regions, invocation.resources);
        if (const auto* diagnostic = std::get_if<Diagnostic>(&evaluated)) { return *diagnostic; }
        auto& result = std::get<vision::RoiEvaluation>(evaluated);
        if (const auto* diagnostic = std::get_if<Diagnostic>(&result.effective_region)) { return *diagnostic; }
        auto effective = DataValue::region2d(std::get<RegionDefinition>(result.effective_region));
        if (const auto* diagnostic = std::get_if<Diagnostic>(&effective)) { return *diagnostic; }
        auto mask = DataValue::raster_mask(std::move(result.mask));
        if (const auto* diagnostic = std::get_if<Diagnostic>(&mask)) { return *diagnostic; }
        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
            {{key<OutputId>("effective_roi"), std::get<DataValue>(std::move(effective))},
                {key<OutputId>("roi_mask"), std::get<DataValue>(std::move(mask))}}, {}}};
    }
};

class EvaluationFactory final : public ToolInstanceFactory {
private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<EvaluationTool>(configuration)}; }
};

InputDescriptor public_image(std::string_view name)
{
    InputDescriptor input{key<InputId>(name), std::string(name), DataType::image()};
    input.binding.allowed_sources = {BindingSourceKind::PublicInput};
    return input;
}

ToolRegistry fixture_registry()
{
    const auto descriptors = vision::tool_descriptors();
    const auto threshold = std::ranges::find(descriptors, key<ToolTypeId>("visionpro.image.threshold"), &ToolDescriptor::id);
    check(threshold != descriptors.end(), "The vision catalog publishes the threshold contract.");
    const auto roi = std::ranges::find(threshold->parameters, key<ParameterId>("roi"), &ParameterDescriptor::id);
    check(roi != threshold->parameters.end(), "The vision contract declares a Region2D parameter.");
    const auto schema = key<SchemaId>("s43.evaluate.schema");
    ToolDescriptor tool{key<ToolTypeId>("s43.evaluate"), "Evaluate ROI", 1, {schema, 1}, {*roi}, {public_image("image")},
        {{key<OutputId>("effective_roi"), "Effective ROI", DataType::region2d(), {pixels, camera()}},
            {key<OutputId>("roi_mask"), "ROI mask", DataType::raster_mask(), {pixels, camera()}}}};
    tool.region_capabilities = threshold->region_capabilities;
    tool.image_formats = threshold->image_formats;
    const std::array registrations{ToolRegistration{std::move(tool), {schema, 1, 1, {}}, std::make_shared<EvaluationFactory>()}};
    return take(ToolRegistry::create(registrations));
}

InheritedRegion inherited_region()
{
    const auto frame = camera();
    Transform2D shift{frame, frame, pixels, pixels};
    shift.matrix[2] = 1;
    return {uuid<WorkflowId>(), uuid<NodeId>('1'), key<OutputId>("effective_roi"), frame, pixels, 0, shift};
}

Solution fixture_solution(bool exclude_source_pixel)
{
    const auto flow = uuid<WorkflowId>();
    const auto frame = camera();
    RegionDefinition local{uuid<RegionId>('1'), frame, pixels,
        RegionPrimitive{frame, pixels, RotatedRect{4, 4, 4, 2, std::numbers::pi / 4}},
        {}, RegionBoundaryPolicy::Intersect};
    if (exclude_source_pixel) {
        local.exclusions.emplace_back(RegionPrimitive{frame, pixels, AxisAlignedRect{4, 5, 1, 1}});
    }
    const RegionDefinition inherited{uuid<RegionId>('2'), frame, pixels, inherited_region(),
        {RegionPrimitive{frame, pixels, AxisAlignedRect{5, 4, 1, 1}}}};
    const auto node = [&](char suffix, std::string_view image_input, const RegionDefinition& roi) {
        return Node{uuid<NodeId>(suffix), std::string(image_input), ToolConfiguration{key<ToolTypeId>("s43.evaluate"), 1},
            {{key<InputId>("image"), {ScopedInputReference{flow, {}, key<InputId>(image_input), {}}}},
                {key<ParameterId>("roi"), {take(ConfigValue::region2d(roi))}}}};
    };
    const Workflow workflow{flow, "ROI inheritance", {key<SchemaId>("s43.flow"), 1, {public_image("narrow"), public_image("wide")}},
        {node('1', "narrow", local), node('2', "wide", inherited)},
        {{uuid<ControlLinkId>(), uuid<NodeId>('1'), {}, uuid<NodeId>('2')}}, {}, {uuid<NodeId>('1')}};
    const CoordinateFrameDefinition coordinates{frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded};
    return {uuid<SolutionId>(), "ROI runtime fixture", 1, 1, {workflow}, {}, {}, {}, {coordinates}};
}

DataValue fixture_image(std::size_t width)
{
    const std::vector<std::byte> bytes(width * height, std::byte{255});
    auto image = take(ImageHandle::copy({width, height, width, mono8}, bytes, camera()));
    return take(DataValue::image({std::move(image), {}, {}}));
}

RunHandle submit(WorkflowEngine& engine, const ToolRegistry& registry, bool exclude_source_pixel,
    std::size_t producer_width, std::uint64_t revision)
{
    const auto solution = fixture_solution(exclude_source_pixel);
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(),
        {engine.session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(revision))}));
    const auto request = take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>()},
        {{key<InputId>("narrow"), fixture_image(producer_width)}, {key<InputId>("wide"), fixture_image(target_width)}},
        {TriggerKind::Manual, key<SchemaId>("s43.test")}));
    auto submitted = engine.submit(request);
    if (const auto* rejection = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(rejection->diagnostic.message); }
    check(std::holds_alternative<RunHandle>(submitted), "The ROI workflow is admitted.");
    return std::get<RunHandle>(std::move(submitted));
}

RunProgress finish(const RunHandle& run)
{
    auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    if (!waited.progress.diagnostics.empty()) { throw std::runtime_error(waited.progress.diagnostics.front().message); }
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent
        && waited.progress.status == ExecutionStatus::Succeeded, "The ROI workflow reaches successful quiescence.");
    return std::move(waited.progress);
}

const RunNodeProgress& progress_of(const RunProgress& progress, char suffix)
{
    const auto node = std::ranges::find(progress.nodes, uuid<NodeId>(suffix), &RunNodeProgress::node);
    check(node != progress.nodes.end() && node->invocation.has_value(), "The evaluator node was called.");
    return *node;
}

const DataValue& output(const NodeResult& result, std::string_view name)
{
    const auto* slot = result.output(key<OutputId>(name));
    const auto* data = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(data != nullptr, "The evaluator publishes its declared output.");
    return *data;
}

void check_mask(const NodeResult& result, std::size_t width, std::span<const Pixel> expected)
{
    const auto* mask = output(result, "roi_mask").as_raster_mask();
    check(mask && mask->pixels.width() == width && mask->pixels.height() == height
        && mask->pixels.coordinate_frame() == camera() && mask->pixels.format() == mono8,
        "The member mask has the invocation target's layout and coordinates.");
    std::vector<Pixel> members;
    for (std::size_t y = 0; y < height; ++y) {
        const auto row = take(mask->pixels.row(y));
        for (std::size_t x = 0; x < width; ++x) {
            check(row[x] == std::byte{0} || row[x] == std::byte{255}, "The member mask is binary.");
            if (row[x] != std::byte{0}) { members.push_back({x, y}); }
        }
    }
    check(std::ranges::equal(members, expected), "The effective ROI has the expected member pixels.");
}

ResultIdentity verify(const RunHandle& run, const RunProgress& progress, std::size_t producer_width,
    std::span<const Pixel> source_members, std::span<const Pixel> consumer_members, std::size_t source_exclusion_count)
{
    const auto& producer = progress_of(progress, '1');
    const auto& consumer = progress_of(progress, '2');
    check(progress.call_order == std::vector{producer.node, consumer.node}, "The consumer runs after its source publishes.");
    const auto produced = take(run.lookup(*producer.invocation));
    const auto consumed = take(run.lookup(*consumer.invocation));
    check(produced && consumed && produced->data().identity.run == run.identity() && consumed->data().identity.run == run.identity(),
        "Both results address the current run.");
    check_mask(*produced, producer_width, source_members);
    check_mask(*consumed, target_width, consumer_members);
    const auto* published_region = output(*produced, "effective_roi").as_region2d();
    const auto* effective_region = output(*consumed, "effective_roi").as_region2d();
    check(published_region && published_region->id == uuid<RegionId>('1') && published_region->exclusions.size() == source_exclusion_count,
        "The source publishes its evaluated Region2D identity and source exclusions.");
    check(effective_region && effective_region->id == uuid<RegionId>('2')
        && std::holds_alternative<RegionPrimitive>(effective_region->source)
        && effective_region->exclusions.size() == source_exclusion_count + 1,
        "The consumer publishes its own region identity and the complete flattened exclusions.");

    const auto trace = std::ranges::find(consumer.bindings, BindingTarget{key<ParameterId>("roi")}, &RunBindingTrace::target);
    check(trace != consumer.bindings.end() && trace->regions.size() == 1, "The ROI binding records one frozen source.");
    const auto& frozen = trace->regions.front();
    check(frozen.reference == inherited_region() && frozen.value == *published_region
        && frozen.identity == produced->data().identity && frozen.identity.invocation == *producer.invocation
        && frozen.source_index == 0 && frozen.path == std::vector<DiagnosticPathStep>{key<FieldId>("source")},
        "The trace owns the full frozen result, correction and exact producer call at the original binding path.");
    return frozen.identity;
}

void runtime_inheritance()
{
    const auto registry = fixture_registry();
    const auto engine = take(WorkflowEngine::create(registry));
    const auto first = submit(*engine, registry, false, 5, 1);
    const auto first_progress = finish(first);
    const auto second = submit(*engine, registry, true, target_width, 2);
    const auto second_progress = finish(second);

    constexpr std::array clipped_source{Pixel{3, 3}, Pixel{4, 3}, Pixel{3, 4}, Pixel{4, 4}, Pixel{4, 5}};
    constexpr std::array inherited_members{Pixel{4, 3}, Pixel{5, 3}, Pixel{4, 4}, Pixel{6, 4}, Pixel{5, 5}, Pixel{6, 5}};
    constexpr std::array excluded_source{Pixel{3, 3}, Pixel{4, 3}, Pixel{3, 4}, Pixel{4, 4}, Pixel{5, 4}, Pixel{5, 5}};
    constexpr std::array excluded_members{Pixel{4, 3}, Pixel{5, 3}, Pixel{4, 4}, Pixel{6, 4}, Pixel{6, 5}};
    // A wider target re-evaluates the source expression, including pixels clipped from the source image.
    const auto first_source = verify(first, first_progress, 5, clipped_source, inherited_members, 0);
    // This run adds an upstream exclusion, which the consumer applies after the same correction.
    const auto second_source = verify(second, second_progress, target_width, excluded_source, excluded_members, 1);
    check(first_source != second_source && first_source.run == first.identity() && second_source.run == second.identity(),
        "Each consumer resolves only its own run's producer result.");
}
}

int main()
{
    try {
        runtime_inheritance();
        std::cout << "ROI inheritance through WorkflowEngine and frozen-source tracing passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
