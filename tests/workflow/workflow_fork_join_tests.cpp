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

int checks = 0;

void check(bool passed, std::string_view message)
{
    ++checks;
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T> T uuid(unsigned suffix = 1)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string value = "00000000-0000-4000-8000-000000000000";
    value[value.size() - 2] = hex.at(suffix >> 4);
    value.back() = hex.at(suffix & 0x0f);
    return take(IdFactory::parse<T>(value));
}

template<SchemaIdentity T> T key(std::string_view name)
{ return take(SchemaIdFactory::parse<T>(name)); }

const DataValue& input(const InvocationContext& context, std::string_view name)
{
    const auto& inputs = context.data().inputs;
    const auto found = std::ranges::find(inputs, key<InputId>(name), &FixedToolInput::id);
    if (found == inputs.end()) { throw std::runtime_error("Missing fixture tool input."); }
    return found->value;
}

const ImageFrame& image_input(const InvocationContext& context)
{
    const auto* frame = input(context, "image").as_image();
    if (!frame) { throw std::runtime_error("The measurement did not receive its image."); }
    return *frame;
}

struct MeasurementCall {
    InvocationCompletion token;
    ToolInvocationIdentity identity;
    std::weak_ptr<const InvocationContext> context;
};

class Driver {
public:
    void started(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        starts_.push_back(node);
    }

    std::size_t starts(NodeId node)
    {
        const std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::ranges::count(starts_, node));
    }

    bool observe(const MeasurementCall& call)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        calls_.push_back(call);
        changed_.notify_all();
        return true;
    }

    MeasurementCall await_call(NodeId node)
    {
        std::unique_lock lock(mutex_);
        const auto matches = [node](const auto& call) { return call.identity.invocation.node == node; };
        if (!changed_.wait_for(lock, 5s, [&] { return std::ranges::any_of(calls_, matches); })) {
            throw std::runtime_error("An activated measurement never started.");
        }
        return *std::ranges::find_if(calls_, matches);
    }

    static void close(const MeasurementCall& call)
    {
        static_cast<void>(call.token.complete({call.identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(call.token.mark_exited());
    }

    void drain()
    {
        std::vector<MeasurementCall> calls;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            calls.swap(calls_);
        }
        for (const auto& call : calls) { close(call); }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<MeasurementCall> calls_;
    std::vector<NodeId> starts_;
    bool draining_{false};
};

class MeasurementControl final : public InvocationControl {
public:
    void request_cancel() override {}
};

enum class ToolKind { Locate, Measure, Condition, Join, Send };

ToolCompletion completed(const ToolInvocationIdentity& identity, std::vector<ToolOutput> outputs,
    ToolJudgement judgement = ToolJudgement::NotEvaluated)
{ return {identity, ToolCompletionStatus::Succeeded, judgement, std::move(outputs), {}}; }

class FixtureTool final : public ToolInstance {
public:
    FixtureTool(ToolExecutionConfiguration configuration, ToolKind kind, std::shared_ptr<Driver> driver)
        : ToolInstance(std::move(configuration)), kind_(kind), driver_(std::move(driver)) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion token) override
    {
        const auto identity = context->data().identity;
        driver_->started(identity.invocation.node);
        switch (kind_) {
        case ToolKind::Locate:
            return InvocationStart{completed(identity, {{key<OutputId>("image"), input(*context, "image")}})};
        case ToolKind::Measure: {
            auto control = std::make_shared<MeasurementControl>();
            if (auto registered = token.register_control(control); const auto* failure = std::get_if<Diagnostic>(&registered)) {
                return *failure;
            }
            const MeasurementCall call{token, identity, context};
            if (!driver_->observe(call)) { Driver::close(call); }
            return InvocationStart{AsynchronousInvocation{std::move(control)}};
        }
        case ToolKind::Condition: {
            const auto* value = input(*context, "value").as_int64();
            if (!value) { throw std::runtime_error("The condition did not receive its reading."); }
            return InvocationStart{completed(identity, {{key<OutputId>("value"), DataValue::integer(*value + 1)}},
                *value < 35 ? ToolJudgement::NG : ToolJudgement::OK)};
        }
        case ToolKind::Join:
        case ToolKind::Send:
            return InvocationStart{completed(identity, {{key<OutputId>("values"), input(*context, "values")}})};
        }
        throw std::runtime_error("Unknown fixture tool.");
    }

    ToolKind kind_;
    std::shared_ptr<Driver> driver_;
};

class FixtureFactory final : public ToolInstanceFactory {
public:
    FixtureFactory(ToolKind kind, std::shared_ptr<Driver> driver) : kind_(kind), driver_(std::move(driver)) {}

private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<FixtureTool>(configuration, kind_, driver_)}; }

    ToolKind kind_;
    std::shared_ptr<Driver> driver_;
};

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr std::array<std::byte, 2> pixels{std::byte{20}, std::byte{37}};

InputDescriptor accepts(std::string_view name, DataType type, std::vector<BindingSourceKind> sources)
{
    InputDescriptor port{key<InputId>(name), std::string(name), std::move(type)};
    port.binding.allowed_sources = std::move(sources);
    return port;
}

InputDescriptor public_image()
{ return accepts("image", DataType::image(), {BindingSourceKind::PublicInput}); }

ToolRegistry registry_for(const std::shared_ptr<Driver>& driver)
{
    const auto integer = DataType::integer();
    const auto list = DataType::list(integer);
    const OutputDescriptor image{key<OutputId>("image"), "Image", DataType::image()};
    const OutputDescriptor value{key<OutputId>("value"), "Value", integer};
    const OutputDescriptor values{key<OutputId>("values"), "Values", list};
    auto multiple_values = accepts("values", list, {BindingSourceKind::RunOutput});
    multiple_values.binding.cardinality = BindingCardinality::MultipleSources;
    ToolDescriptor locate{key<ToolTypeId>("s34.locate"), "Locate", 1, {key<SchemaId>("s34.locate.schema"), 1}, {},
        {public_image()}, {image}};
    locate.image_formats = {mono8};
    ToolDescriptor measure{key<ToolTypeId>("s34.measure"), "Measure", 1, {key<SchemaId>("s34.measure.schema"), 1}, {},
        {accepts("image", DataType::image(), {BindingSourceKind::RunOutput})}, {value, image}};
    measure.image_formats = {mono8};
    measure.execution.mode = ToolExecutionMode::Asynchronous;
    measure.execution.cancellation = ToolCancellation::Cooperative;
    const ToolDescriptor condition{key<ToolTypeId>("s34.condition"), "Condition", 1, {key<SchemaId>("s34.condition.schema"), 1}, {},
        {accepts("value", integer, {BindingSourceKind::Constant, BindingSourceKind::RunOutput})}, {value}};
    const ToolDescriptor join{key<ToolTypeId>("s34.join"), "Join", 1, {key<SchemaId>("s34.join.schema"), 1}, {},
        {multiple_values}, {values}};
    ToolDescriptor send{key<ToolTypeId>("s34.send"), "Send", 1, {key<SchemaId>("s34.send.schema"), 1}, {},
        {accepts("values", list, {BindingSourceKind::RunOutput})}, {values}};
    send.execution.effects = ToolEffects::External;
    const auto registration = [&](const ToolDescriptor& descriptor, ToolKind kind) {
        return ToolRegistration{descriptor, {descriptor.schema.id, 1, 1, {}}, std::make_shared<FixtureFactory>(kind, driver)};
    };
    const std::array registrations{registration(locate, ToolKind::Locate), registration(measure, ToolKind::Measure),
        registration(condition, ToolKind::Condition), registration(join, ToolKind::Join), registration(send, ToolKind::Send)};
    return take(ToolRegistry::create(registrations));
}

OutputReference output_of(unsigned node, std::string_view output = "value")
{ return {uuid<WorkflowId>(), uuid<NodeId>(node), key<OutputId>(output), {}}; }

Node fixture_node(unsigned id, std::string_view type, std::vector<Binding> bindings = {})
{ return {uuid<NodeId>(id), std::string(type), ToolConfiguration{key<ToolTypeId>(type), 1}, std::move(bindings)}; }

ControlLink edge(unsigned id, unsigned source, unsigned target)
{ return {uuid<ControlLinkId>(id), uuid<NodeId>(source), {}, uuid<NodeId>(target)}; }

Node locator()
{
    const auto image = key<InputId>("image");
    return fixture_node(1, "s34.locate", {{image, {ScopedInputReference{uuid<WorkflowId>(), {}, image, {}}}}});
}

Node measurement(unsigned id)
{ return fixture_node(id, "s34.measure", {{key<InputId>("image"), {output_of(1, "image")}}}); }

Node condition(unsigned id, BindingSource source)
{ return fixture_node(id, "s34.condition", {{key<InputId>("value"), {std::move(source)}}}); }

Workflow pipeline(std::string name, std::vector<Node> nodes, std::vector<ControlLink> links,
    std::vector<NodeId> entries, bool with_image)
{
    auto inputs = with_image ? std::vector{public_image()} : std::vector<InputDescriptor>{};
    return {uuid<WorkflowId>(), std::move(name), {key<SchemaId>("s34.main"), 1, std::move(inputs)},
        std::move(nodes), std::move(links), {}, std::move(entries)};
}

RunHandle accepted(RunSubmission submission)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) {
        throw std::runtime_error(std::string(error_code_name(failure->diagnostic.code)) + ": " + failure->diagnostic.message);
    }
    check(std::holds_alternative<RunHandle>(submission), "Expected an accepted run.");
    return std::get<RunHandle>(std::move(submission));
}

RunProgress wait_for(const RunHandle& run, RunWaitTarget target)
{
    const auto waited = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied, "The run did not reach the requested stage.");
    if (waited.progress.status != ExecutionStatus::Succeeded && !waited.progress.diagnostics.empty()) {
        throw std::runtime_error(waited.progress.diagnostics.front().message);
    }
    check(waited.progress.status == ExecutionStatus::Succeeded, "Expected a successful run.");
    return waited.progress;
}

struct Fixture {
    std::shared_ptr<Driver> driver{std::make_shared<Driver>()};
    ToolRegistry registry{registry_for(driver)};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    IdSequence<ConfigSnapshotId> ids;

    ~Fixture() { driver->drain(); }

    RunSubmission submit(Workflow flow, std::vector<RunInput> inputs = {})
    {
        const auto workflow = flow.id;
        Solution solution{uuid<SolutionId>(), "Control scheduling", 1, 1, {std::move(flow)}};
        solution.coordinates.push_back({key<CoordinateSystemId>("s34.frame"), CoordinateKind::Image, MeasurementUnit::Pixel,
            {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
        const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), take(ids.next())}));
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{workflow}, inputs,
            {TriggerKind::Manual, key<SchemaId>("s34.test")}));
        return engine->submit(request);
    }

    void checkpoint()
    {
        // Older ready work is handled before a newly admitted synchronous probe.
        Workflow probe{uuid<WorkflowId>(2), "Independent probe", {key<SchemaId>("s34.probe"), 1},
            {condition(14, ConfigValue::integer(100))}, {}, {}, {uuid<NodeId>(14)}};
        const auto run = accepted(submit(std::move(probe)));
        static_cast<void>(wait_for(run, RunWaitTarget::Quiescence));
    }
};

DataValue fixture_image()
{
    auto image = take(ImageHandle::copy({2, 1, 2, mono8}, pixels, key<CoordinateSystemId>("s34.frame")));
    return take(DataValue::image({std::move(image), FrameId{uuid<ResourceId>(), 1}, {}}));
}

ToolCompletion measure(const MeasurementCall& call, unsigned lane)
{
    const auto context = call.context.lock();
    if (!context) { throw std::runtime_error("The in-flight measurement lost its input."); }
    const auto& frame = image_input(*context);
    auto marked = frame.image.copy_pixels();
    marked.front() = static_cast<std::byte>(120 + lane);
    const auto output_image = take(ImageHandle::copy({2, 1, 2, mono8}, marked, frame.image.coordinate_frame()));
    const auto input_row = take(frame.image.row(0));
    const auto reading = std::to_integer<std::uint8_t>(input_row.front()) + 10 * lane;
    return completed(call.identity, {{key<OutputId>("value"), DataValue::integer(reading)},
        {key<OutputId>("image"), take(DataValue::image({output_image, frame.frame, frame.captured_at}))}});
}

const RunNodeProgress& node_progress(const RunProgress& progress, unsigned node)
{
    const auto found = std::ranges::find(progress.nodes, uuid<NodeId>(node), &RunNodeProgress::node);
    if (found == progress.nodes.end()) { throw std::runtime_error("Missing fixture node progress."); }
    return *found;
}

void expect_link(const RunProgress& progress, unsigned link, ControlLinkStatus status)
{
    const auto found = std::ranges::find(progress.control_links, uuid<ControlLinkId>(link), &RunControlLinkProgress::link);
    check(found != progress.control_links.end() && found->status == status && !found->close_reason,
        "The unconditional link has the expected progress.");
}

DataValue output(const RunHandle& run, const RunProgress& progress, unsigned node, std::string_view port)
{
    const auto& state = node_progress(progress, node);
    check(state.status == ExecutionStatus::Succeeded && state.invocation.has_value(), "The producing node must succeed.");
    const auto result = take(run.lookup(*state.invocation));
    check(result && result->data().identity.run == run.identity() && result->data().identity.invocation == *state.invocation,
        "The queried result belongs to the exact call.");
    const auto* result_port = result->output(key<OutputId>(port));
    if (!result_port) { throw std::runtime_error("Missing fixture tool output."); }
    return std::get<DataValue>(result_port->content);
}

void fork_join()
{
    constexpr unsigned lanes = 5;
    constexpr unsigned join_id = 12;
    constexpr unsigned send_id = 13;
    constexpr std::array<unsigned, lanes> binding_order{3, 0, 4, 1, 2};
    constexpr std::array<unsigned, lanes - 1> finish_order{4, 0, 3, 2};
    constexpr unsigned last_lane = 1;
    std::vector<Node> nodes{locator()};
    std::vector<ControlLink> links;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        nodes.push_back(measurement(2 + lane));
        nodes.push_back(condition(7 + lane, output_of(2 + lane)));
        links.push_back(edge(1 + lane, 1, 2 + lane));
        links.push_back(edge(6 + lane, 2 + lane, 7 + lane));
        links.push_back(edge(11 + lane, 7 + lane, join_id));
    }
    Binding aggregate{key<InputId>("values"), {}, BindingCardinality::MultipleSources};
    for (const auto lane : binding_order) { aggregate.sources.emplace_back(output_of(7 + lane)); }
    nodes.push_back(fixture_node(join_id, "s34.join", {std::move(aggregate)}));
    nodes.push_back(fixture_node(send_id, "s34.send", {{key<InputId>("values"), {output_of(join_id, "values")}}}));
    links.push_back(edge(16, join_id, send_id));
    std::ranges::reverse(nodes);

    Fixture fixture;
    const auto input_image = fixture_image();
    const auto run = accepted(fixture.submit(pipeline("Five lanes", std::move(nodes), std::move(links), {uuid<NodeId>()}, true),
        {{key<InputId>("image"), input_image}}));
    for (unsigned lane = 0; lane < lanes; ++lane) {
        const auto call = fixture.driver->await_call(uuid<NodeId>(2 + lane));
        const auto context = call.context.lock();
        check(context && image_input(*context).image.shares_storage_with(input_image.as_image()->image)
            && image_input(*context).frame == input_image.as_image()->frame,
            "Each lane holds the same immutable located image and frame identity.");
    }

    for (const auto lane : finish_order) {
        const auto call = fixture.driver->await_call(uuid<NodeId>(2 + lane));
        const auto completion = measure(call, lane);
        take(call.token.complete(completion));
        const auto duplicate = call.token.complete(completion);
        const auto* failure = std::get_if<Diagnostic>(&duplicate);
        check(failure && failure->code == ErrorCode::InvalidState, "A duplicate completion is rejected.");
        take(call.token.mark_exited());
    }
    const auto last = fixture.driver->await_call(uuid<NodeId>(2 + last_lane));
    const auto last_completion = measure(last, last_lane);
    take(last.token.complete(last_completion));
    fixture.checkpoint();
    auto progress = take(run.inspect());
    check(node_progress(progress, 2 + last_lane).status == ExecutionStatus::Succeeded,
        "The last lane has published success while its tool still awaits physical exit.");
    for (const auto lane : finish_order) {
        expect_link(progress, 11 + lane, ControlLinkStatus::Arrived);
        check(fixture.driver->starts(uuid<NodeId>(7 + lane)) == 1, "Each exited lane runs its condition once.");
    }
    expect_link(progress, 6 + last_lane, ControlLinkStatus::Pending);
    expect_link(progress, 11 + last_lane, ControlLinkStatus::Pending);
    check(fixture.driver->starts(uuid<NodeId>(join_id)) == 0 && fixture.driver->starts(uuid<NodeId>(send_id)) == 0,
        "The join and simulated send wait for the final lane.");

    take(last.token.mark_exited());
    const auto repeated_exit = last.token.mark_exited();
    const auto* failure = std::get_if<Diagnostic>(&repeated_exit);
    check(failure && failure->code == ErrorCode::InvalidState, "A duplicate exit is rejected.");
    progress = wait_for(run, RunWaitTarget::Quiescence);
    check(progress.nodes.size() == 13 && progress.control_links.size() == 16, "The run reports the complete five-lane graph.");
    for (unsigned link = 1; link <= 16; ++link) { expect_link(progress, link, ControlLinkStatus::Arrived); }
    for (unsigned node = 1; node <= send_id; ++node) {
        check(fixture.driver->starts(uuid<NodeId>(node)) == 1, "The location, measurements, conditions, join and send execute once.");
    }
    const auto ng = take(run.lookup(node_progress(progress, 7).invocation.value()));
    check(ng && ng->data().judgement == Judgement::NG, "A successful NG condition still contributes its branch.");
    std::vector<DataValue> expected;
    for (const auto lane : binding_order) { expected.push_back(DataValue::integer(21 + 10 * lane)); }
    const auto expected_values = take(DataValue::list(DataType::integer(), expected));
    check(output(run, progress, join_id, "values") == expected_values
        && output(run, progress, send_id, "values") == expected_values,
        "The join and send use binding order independently of branch completion order.");
    for (unsigned lane = 0; lane < lanes; ++lane) {
        const auto marked = output(run, progress, 2 + lane, "image");
        const auto* frame = marked.as_image();
        check(frame && !frame->image.shares_storage_with(input_image.as_image()->image)
            && take(frame->image.row(0)).front() == static_cast<std::byte>(120 + lane),
            "Each measurement produces a distinct image buffer.");
    }
    check(std::ranges::equal(take(input_image.as_image()->image.row(0)), pixels),
        "The original shared image is intact after independent output processing.");
}

void sibling_dependencies()
{
    constexpr unsigned consumer = 2;
    constexpr unsigned gate = 3;
    constexpr unsigned producer = 4;
    Fixture fixture;
    const auto run = accepted(fixture.submit(pipeline("Sibling dependency",
        {condition(consumer, output_of(producer)), measurement(producer), locator(), measurement(gate)},
        {edge(1, 1, consumer), edge(2, 1, gate), edge(3, gate, producer)}, {uuid<NodeId>(), uuid<NodeId>(producer)}, true),
        {{key<InputId>("image"), fixture_image()}}));
    const auto blocked = fixture.driver->await_call(uuid<NodeId>(gate));
    fixture.checkpoint();
    const auto pending = take(run.inspect());
    expect_link(pending, 1, ControlLinkStatus::Arrived);
    expect_link(pending, 3, ControlLinkStatus::Pending);
    check(fixture.driver->starts(uuid<NodeId>(consumer)) == 0 && fixture.driver->starts(uuid<NodeId>(producer)) == 0,
        "A control-ready consumer waits for data. The source still needs its real incoming edge despite its entry permit.");
    take(blocked.token.complete(measure(blocked, 0)));
    take(blocked.token.mark_exited());

    const auto source = fixture.driver->await_call(uuid<NodeId>(producer));
    take(source.token.complete(measure(source, 2)));
    const auto completed_run = wait_for(run, RunWaitTarget::Completion);
    check(!completed_run.quiescent && fixture.driver->starts(uuid<NodeId>(producer)) == 1
        && fixture.driver->starts(uuid<NodeId>(consumer)) == 1,
        "The accepted sibling result wakes the consumer while the producer still awaits exit.");
    check(output(run, completed_run, consumer, "value") == DataValue::integer(41),
        "The consumer uses the accepted sibling reading.");
    const auto& bindings = node_progress(completed_run, consumer).bindings;
    const auto& binding = bindings.at(0);
    check(binding.target == BindingTarget{key<InputId>("value")} && binding.sources.size() == 1,
        "The consumer records its actual binding source.");
    const auto& trace = std::get<ResultTrace>(binding.sources.front().origin);
    check(trace.run == run.identity() && trace.reference.node == uuid<NodeId>(producer)
        && trace.invocation == source.identity.invocation.invocation && trace.attempt == source.identity.attempt,
        "The sibling trace identifies the exact current-run invocation and attempt.");
    take(source.token.mark_exited());
    static_cast<void>(wait_for(run, RunWaitTarget::Quiescence));
}

void reject_invalid_dependencies()
{
    Fixture fixture;
    const auto require_rejection = [&](Workflow flow, ErrorCode code, unsigned node) {
        const auto submission = fixture.submit(std::move(flow));
        const auto* rejected = std::get_if<RunRejected>(&submission);
        check(rejected && rejected->diagnostic.code == code && rejected->diagnostic.location.node == uuid<NodeId>(node)
            && rejected->diagnostic.location.input == key<InputId>("value"),
            "Planning rejects the invalid data dependency and identifies its consumer.");
    };
    require_rejection(pipeline("Reverse dependency", {condition(1, output_of(2)), condition(2, ConfigValue::integer(3))},
        {edge(1, 1, 2)}, {uuid<NodeId>()}, false), ErrorCode::ControlOrderConflict, 1);

    // Control edges 2 -> 3 and 4 -> 5 combine with data edges 3 -> 4 and 5 -> 2.
    require_rejection(pipeline("Mixed wait cycle", {condition(1, ConfigValue::integer(3)), condition(2, output_of(5)),
        condition(3, ConfigValue::integer(3)), condition(4, output_of(3)), condition(5, ConfigValue::integer(3))},
        {edge(1, 1, 2), edge(2, 1, 4), edge(3, 2, 3), edge(4, 4, 5)}, {uuid<NodeId>()}, false),
        ErrorCode::DependencyCycle, 4);
    for (unsigned node = 1; node <= 5; ++node) {
        check(fixture.driver->starts(uuid<NodeId>(node)) == 0, "An invalid plan runs no tools.");
    }
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view scenario = argc == 2 ? argv[1] : "";
        if (scenario == "fork_join") { fork_join(); }
        else if (scenario == "sibling_dependencies") { sibling_dependencies(); reject_invalid_dependencies(); }
        else { throw std::runtime_error("Expected fork_join or sibling_dependencies."); }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
