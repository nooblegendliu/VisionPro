#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr auto pixels = MeasurementUnit::Pixel;
constexpr std::array original{50, 150, 220, 1, 126, 240};
constexpr std::array blank{0, 0, 0, 0, 0, 0};
constexpr std::array initial_binary{0, 255, 255, 0, 0, 255};
constexpr std::array edited_binary{0, 255, 0, 0, 255, 0};

void check(bool condition, std::string_view message)
{
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity T> T key(std::string_view name)
{ return take(SchemaIdFactory::parse<T>(name)); }

template<UuidIdentity T> T id(char suffix = '1')
{
    std::string text{"00000000-0000-4000-8000-000000000000"};
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

CoordinateSystemId frame() { return key<CoordinateSystemId>("s46.camera"); }
ToolTypeId threshold_type() { return key<ToolTypeId>("visionpro.image.threshold"); }

ImageFrame image(const std::array<int, 6>& samples, std::uint64_t sequence)
{
    std::array<std::byte, 6> bytes;
    std::ranges::transform(samples, bytes.begin(), [](int sample) { return static_cast<std::byte>(sample); });
    return {take(ImageHandle::copy({3, 2, 3, mono8}, bytes, frame())),
        FrameId{id<ResourceId>(), sequence}, std::chrono::sys_time<std::chrono::nanoseconds>{10ns}};
}

void expect_pixels(const ImageHandle& image, const std::array<int, 6>& expected)
{
    check(image.width() == 3 && image.height() == 2 && image.format() == mono8
        && image.coordinate_frame() == frame(), "The image retains its dimensions, format and coordinates.");
    check(std::ranges::equal(image.copy_pixels(), expected, [](std::byte actual, int wanted) {
        return std::to_integer<int>(actual) == wanted;
    }), "Pixels must equal the fixed expectation.");
}

class ThresholdGate {
public:
    void hold(const std::shared_ptr<const InvocationContext>& context)
    {
        std::unique_lock lock(mutex_);
        if (released_) { return; }
        context_ = context;
        computed_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    std::weak_ptr<const InvocationContext> await_computation()
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [this] { return computed_; }),
            "The real threshold did not reach the result gate.");
        return context_;
    }

    void release()
    {
        const std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::weak_ptr<const InvocationContext> context_;
    bool computed_{false};
    bool released_{false};
};

class GatedThreshold final : public ToolInstance {
public:
    GatedThreshold(const ToolExecutionConfiguration& configuration, std::shared_ptr<ToolInstance> tool,
        std::shared_ptr<ThresholdGate> gate)
        : ToolInstance(configuration), tool_(std::move(tool)), gate_(std::move(gate)) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        // Run the actual synchronous tool. The outer invocation remains in
        // begin until the client chooses when to return its real completion.
        auto started = InvocationOperation::start(tool_, *context);
        if (auto* failure = std::get_if<Diagnostic>(&started)) { return std::move(*failure); }
        auto progress = std::get<InvocationOperation>(started).poll();
        check(progress.physically_exited && progress.completion.has_value(), "A threshold must compute synchronously.");
        gate_->hold(context);
        return InvocationStart{std::move(*progress.completion)};
    }

    std::shared_ptr<ToolInstance> tool_;
    std::shared_ptr<ThresholdGate> gate_;
};

class GatedThresholdFactory final : public ToolInstanceFactory {
public:
    GatedThresholdFactory(std::shared_ptr<ToolInstanceFactory> factory, std::shared_ptr<ThresholdGate> gate)
        : factory_(std::move(factory)), gate_(std::move(gate)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        auto tool = create_tool_instance(*factory_, configuration);
        if (auto* failure = std::get_if<Diagnostic>(&tool)) { return std::move(*failure); }
        return std::shared_ptr<ToolInstance>{std::make_shared<GatedThreshold>(configuration,
            std::get<std::shared_ptr<ToolInstance>>(std::move(tool)), gate_)};
    }

private:
    std::shared_ptr<ToolInstanceFactory> factory_;
    std::shared_ptr<ThresholdGate> gate_;
};

ToolRegistry real_registry(const std::shared_ptr<ThresholdGate>& gate)
{
    auto registrations = visionpro::tools::vision::registrations();
    const auto threshold = std::ranges::find_if(registrations, [](const auto& tool) {
        return tool.descriptor.id == threshold_type();
    });
    check(threshold != registrations.end(), "The production threshold is registered.");
    threshold->factory = std::make_shared<GatedThresholdFactory>(threshold->factory, gate);
    return take(ToolRegistry::create(registrations));
}

Node threshold_node(const ToolRegistry& registry, RegionShape shape, std::int64_t cutoff)
{
    const auto mode = key<ParameterId>("mode");
    const auto& parameters = registry.find_descriptor(threshold_type())->parameters;
    const auto mode_type = std::ranges::find(parameters, mode, &ParameterDescriptor::id)->type;
    const RegionDefinition roi{id<RegionId>(), frame(), pixels, RegionPrimitive{frame(), pixels, std::move(shape)}, {}};
    const auto input = key<InputId>("image");
    return {id<NodeId>(), "Threshold", ToolConfiguration{threshold_type(), 1},
        {{input, {ScopedInputReference{id<WorkflowId>(), {}, input, {}}}},
            {key<ParameterId>("roi"), {take(ConfigValue::region2d(roi))}},
            {key<ParameterId>("threshold"), {ConfigValue::integer(cutoff)}},
            {mode, {take(ConfigValue::enumeration(mode_type, key<EnumMemberId>("Binary")))}}}, {}};
}

Solution solution_for(const ToolRegistry& registry)
{
    const auto first = id<NodeId>();
    const auto second = id<NodeId>('2');
    const Node blob{second, "Blob", ToolConfiguration{key<ToolTypeId>("visionpro.image.blob"), 1},
        {{key<InputId>("binary"), {OutputReference{id<WorkflowId>(), first, key<OutputId>("binary"), {}}}},
            {key<ParameterId>("connectivity"), {ConfigValue::integer(4)}}}, {}};
    InputDescriptor input{key<InputId>("image"), "Image", DataType::image()};
    input.binding.allowed_sources = {BindingSourceKind::PublicInput};
    const Workflow workflow{id<WorkflowId>(), "Threshold and Blob", {key<SchemaId>("s46.lifecycle"), 1, {input}, {}, {}},
        {threshold_node(registry, FullImageRegion{}, 128), blob}, {{id<ControlLinkId>(), first, {}, second}}, {}, {first}};
    const CoordinateFrameDefinition coordinates{frame(), CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1},
        Handedness::LeftHanded};
    return {id<SolutionId>(), "Vision lifecycle", 1, 1, {workflow}, {}, {}, {}, {coordinates}};
}

struct Fixture {
    std::shared_ptr<ThresholdGate> gate{std::make_shared<ThresholdGate>()};
    ToolRegistry registry{real_registry(gate)};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry, {}, {1}))};
    Solution model{solution_for(registry)};

    ~Fixture() { gate->release(); }

    ConfigSnapshot capture(std::uint64_t revision) const
    {
        return take(ConfigSnapshot::capture(model, registry.descriptors(),
            {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(revision))}));
    }
};

RunRequest request(const ConfigSnapshot& snapshot, RunTarget target, std::string_view port, const ImageFrame& source)
{
    const auto trigger = std::holds_alternative<NodeTarget>(target) ? TriggerKind::Debug : TriggerKind::Manual;
    return take(RunRequest::create(snapshot, target, {{key<InputId>(port), take(DataValue::image(source))}},
        {trigger, key<SchemaId>("s46.lifecycle")}));
}

RunHandle accepted(WorkflowEngine& engine, const RunRequest& request)
{
    auto submitted = engine.submit(request);
    if (const auto* refused = std::get_if<RunRejected>(&submitted)) {
        throw std::runtime_error(std::string(error_code_name(refused->diagnostic.code)) + ": " + refused->diagnostic.message);
    }
    check(std::holds_alternative<RunHandle>(submitted), "An idle workflow must admit the request.");
    return std::get<RunHandle>(std::move(submitted));
}

void busy(WorkflowEngine& engine, const RunRequest& request, const RunHandle& running)
{
    const auto submission = engine.submit(request);
    const auto* occupied = std::get_if<RunBusy>(&submission);
    check(occupied && occupied->active == running.identity(), "The live call must retain its exact workflow slot.");
}

RunProgress quiescent(const RunHandle& run, ExecutionStatus expected)
{
    auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    if (expected == ExecutionStatus::Succeeded && !waited.progress.diagnostics.empty()) {
        const auto& failure = waited.progress.diagnostics.front();
        throw std::runtime_error(std::string(error_code_name(failure.code)) + ": " + failure.message);
    }
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent && waited.progress.status == expected,
        "The run must reach quiescence in the expected terminal state.");
    return std::move(waited.progress);
}

const RunNodeProgress& node_progress(const RunProgress& progress, NodeId id)
{
    const auto node = std::ranges::find(progress.nodes, id, &RunNodeProgress::node);
    check(node != progress.nodes.end(), "The run reports the requested node.");
    return *node;
}

NodeResult result_of(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "The dispatched node has an invocation key.");
    auto result = take(run.lookup(*node.invocation));
    check(result && result->data().identity.run == run.identity() && result->data().identity.invocation == *node.invocation,
        "The published result matches this run and invocation.");
    return std::move(*result);
}

const DataValue& output(const NodeResult& result, std::string_view name)
{
    const auto* slot = result.output(key<OutputId>(name));
    const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(value, "The expected output was produced.");
    return *value;
}

const ImageFrame& binary(const NodeResult& result)
{
    const auto* image = output(result, "binary").as_image();
    check(image, "The threshold output is an ImageFrame.");
    return *image;
}

void expect_independent(const RunHandle& run, const RunProgress& progress, NodeId target, std::string_view port)
{
    check(progress.call_order == std::vector{target} && progress.nodes.size() == 1 && progress.control_links.empty(),
        "An independent request dispatches only its target and no workflow links.");
    const auto input = key<InputId>(port);
    const auto& bindings = progress.nodes.front().bindings;
    const auto binding = std::ranges::find(bindings, BindingTarget{input}, &RunBindingTrace::target);
    check(binding != bindings.end() && binding->sources.size() == 1, "The injected input has one trace.");
    const auto* origin = std::get_if<IndependentInputTrace>(&binding->sources.front().origin);
    check(origin && origin->input == input && origin->run == run.identity() && origin->origin == InputOrigin::Provided,
        "The trace identifies the real independent input and run.");
}

void independent_snapshots_and_retention()
{
    Fixture fixture;
    const auto source = image(original, 11);
    const auto competitor = image(blank, 12);
    const NodeTarget threshold{id<WorkflowId>(), id<NodeId>()};
    const auto snapshot = fixture.capture(1);
    const auto first = accepted(*fixture.engine, request(snapshot, threshold, "image", source));
    const auto active_context = fixture.gate->await_computation();
    busy(*fixture.engine, request(snapshot, threshold, "image", competitor), first);

    fixture.model.workflows.front().nodes.front() = threshold_node(fixture.registry, AxisAlignedRect{0, 0, 2, 2}, 100);
    const auto edited = fixture.capture(2);
    fixture.gate->release();
    const auto first_progress = quiescent(first, ExecutionStatus::Succeeded);
    expect_independent(first, first_progress, threshold.node, "image");
    const auto retained = result_of(first, first_progress.nodes.front());
    expect_pixels(binary(retained).image, initial_binary);
    check(active_context.expired() && binary(retained).frame == source.frame && binary(retained).captured_at == source.captured_at
        && !binary(retained).image.shares_storage_with(source.image), "The original admitted frame is copied and survives actual exit.");

    const auto next = accepted(*fixture.engine, request(edited, threshold, "image", source));
    const auto next_progress = quiescent(next, ExecutionStatus::Succeeded);
    expect_independent(next, next_progress, threshold.node, "image");
    check(next.identity().run.value() == first.identity().run.value() + 1
        && next.identity().snapshot != first.identity().snapshot, "Busy admission queues no input and consumes no run ID.");
    const auto replacement = result_of(next, next_progress.nodes.front());
    expect_pixels(binary(replacement).image, edited_binary);
    const auto retired = fixture.engine->lookup_result(first.identity(), retained.data().identity.invocation);
    const auto* retirement = std::get_if<Diagnostic>(&retired);
    check(retirement && retirement->code == ErrorCode::ResultRetired, "The one-run history retires the original index entry.");

    const NodeTarget blob{id<WorkflowId>(), id<NodeId>('2')};
    const auto measured = accepted(*fixture.engine, request(edited, blob, "binary", binary(replacement)));
    const auto measured_progress = quiescent(measured, ExecutionStatus::Succeeded);
    expect_independent(measured, measured_progress, blob.node, "binary");
    const auto blob_result = result_of(measured, measured_progress.nodes.front());
    check(output(blob_result, "count") == DataValue::integer(1)
        && output(blob_result, "total_area_pixels") == DataValue::integer(2), "Blob measures the injected binary frame.");
    expect_pixels(source.image, original);

    fixture.engine.reset();
    expect_pixels(binary(retained).image, initial_binary);
    const auto lookup = take(first.lookup(retained.data().identity.invocation));
    check(lookup.has_value(), "The original run handle still owns its exact record after engine destruction.");
    expect_pixels(binary(*lookup).image, initial_binary);
}

void cancellation_before_publication()
{
    Fixture fixture;
    const auto snapshot = fixture.capture(1);
    const WorkflowTarget chain{id<WorkflowId>()};
    const auto source = image(original, 21);
    const auto running = accepted(*fixture.engine, request(snapshot, chain, "image", source));
    const auto weak_context = fixture.gate->await_computation();
    const auto before = take(running.inspect());
    const auto& pending = node_progress(before, id<NodeId>());
    check(before.call_order == std::vector{id<NodeId>()} && pending.invocation
        && !take(running.lookup(*pending.invocation)), "A real computed image is still unpublished at the gate.");
    check(take(running.cancel()) == RunCancelDisposition::Requested, "Cancellation wins while the real result is gated.");
    const auto next_request = request(snapshot, chain, "image", image(blank, 22));
    busy(*fixture.engine, next_request, running);
    const auto waiting = take(running.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now()));
    check(waiting.outcome == RunWaitOutcome::DeadlineExpired && !waiting.progress.quiescent,
        "Cancellation cannot release the slot before the invocation actually exits.");
    {
        const auto context = weak_context.lock();
        check(context && context->data().inputs.front().value.as_image()->frame == source.frame,
            "The cancelling invocation retains its admitted frame until exit.");
        expect_pixels(context->data().inputs.front().value.as_image()->image, original);
    }

    fixture.gate->release();
    const auto stopped = quiescent(running, ExecutionStatus::Cancelled);
    const auto cancelled = result_of(running, node_progress(stopped, id<NodeId>()));
    check(cancelled.data().status == ExecutionStatus::Cancelled
        && std::ranges::none_of(cancelled.data().outputs, [](const auto& slot) { return std::holds_alternative<DataValue>(slot.content); })
        && stopped.call_order == std::vector{id<NodeId>()}, "The late threshold completion publishes no values or successor calls.");
    const auto& successor = node_progress(stopped, id<NodeId>('2'));
    check(successor.status == ExecutionStatus::Skipped && !successor.invocation && stopped.control_links.size() == 1
        && stopped.control_links.front().status == ControlLinkStatus::Closed && weak_context.expired(),
        "Cancellation closes the successor edge and releases the context at physical exit.");

    const auto next = accepted(*fixture.engine, next_request);
    const auto next_progress = quiescent(next, ExecutionStatus::Succeeded);
    check(next_progress.call_order == std::vector{id<NodeId>(), id<NodeId>('2')},
        "The released slot can run a fresh threshold and Blob.");
    expect_pixels(binary(result_of(next, node_progress(next_progress, id<NodeId>()))).image, blank);
    const auto blob = result_of(next, node_progress(next_progress, id<NodeId>('2')));
    check(output(blob, "count") == DataValue::integer(0), "The fresh Blob does not inherit the cancelled image.");
}
} // namespace

int main()
{
    try {
        independent_snapshots_and_retention();
        cancellation_before_publication();
        std::cout << "Real vision independent calls, cancellation and retained results passed.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
