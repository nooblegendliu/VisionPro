#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/tool_execution.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

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
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T> T uuid(char digit = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = digit;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

template<RuntimeIdentity T> T number(std::uint64_t value)
{ return take(RuntimeIdFactory::from_value<T>(value)); }

void rejected(const Result<std::monostate>& result, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, message);
}

class CleanupGate {
public:
    explicit CleanupGate(bool gated) : open_(!gated) {}

    void enter()
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
    }

    void await_entry()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return entered_; })) {
            throw std::runtime_error("The tool did not enter physical cleanup.");
        }
    }

    void open()
    {
        const std::lock_guard lock(mutex_);
        open_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool open_;
    bool entered_{false};
};

struct OpenOnExit {
    CleanupGate& gate;
    ~OpenOnExit() { gate.open(); }
};

struct Call {
    InvocationCompletion completion;
    ToolInvocationIdentity identity;
    std::weak_ptr<const InvocationContext> context;
    std::weak_ptr<const ResourceLease> lease;
};

ToolCompletion successful(const ToolInvocationIdentity& identity, std::int64_t measurement,
    ToolJudgement judgement = ToolJudgement::OK)
{
    return {identity, ToolCompletionStatus::Succeeded, judgement,
        {{key<OutputId>("measurement"), DataValue::integer(measurement)}}, {}};
}

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

std::int64_t first_pixel(const InvocationContext& context)
{
    const auto* frame = context.data().inputs.front().value.as_image();
    if (!frame) { throw std::runtime_error("The invocation did not hold its image input."); }
    return std::to_integer<std::uint8_t>(take(frame->image.row(0)).front());
}

class AsyncDriver {
public:
    AsyncDriver(bool gated_cleanup, bool complete_in_begin)
        : complete_in_begin(complete_in_begin), cleanup(gated_cleanup)
    { calls_.reserve(3); }

    bool record(const Call& call)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        calls_.push_back(call);
        changed_.notify_all();
        return true;
    }

    Call await_call(std::size_t index)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return index < calls_.size(); })) {
            throw std::runtime_error("The worker did not start the asynchronous operation.");
        }
        return calls_[index];
    }

    void cancellation_requested()
    {
        const std::lock_guard lock(mutex_);
        ++cancellations_;
        changed_.notify_all();
    }

    unsigned await_cancellation()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return cancellations_ != 0; })) {
            throw std::runtime_error("The asynchronous tool did not receive cooperative cancellation.");
        }
        return cancellations_;
    }

    unsigned cancellation_count()
    {
        const std::lock_guard lock(mutex_);
        return cancellations_;
    }

    static void close(const Call& call)
    {
        // The first valid terminal report wins. Either way the fixture can
        // acknowledge exit once it no longer uses its context.
        static_cast<void>(call.completion.complete({call.identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(call.completion.mark_exited());
    }

    void drain()
    {
        cleanup.open();
        std::vector<Call> calls;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            calls.swap(calls_);
        }
        for (const auto& call : calls) { close(call); }
    }

    const bool complete_in_begin;
    CleanupGate cleanup;
    std::atomic<unsigned> live_instances{0};
    std::atomic<unsigned> successor_calls{0};

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Call> calls_;
    unsigned cancellations_{0};
    bool draining_{false};
};

class AsyncControl final : public InvocationControl {
public:
    explicit AsyncControl(const std::shared_ptr<AsyncDriver>& driver) : driver_(driver) {}
    void request_cancel() override
    {
        if (const auto driver = driver_.lock()) { driver->cancellation_requested(); }
    }
private:
    std::weak_ptr<AsyncDriver> driver_;
};

class AsyncTool final : public ToolInstance {
public:
    AsyncTool(const ToolExecutionConfiguration& configuration, const std::shared_ptr<AsyncDriver>& driver)
        : ToolInstance(configuration), driver_(driver)
    { ++driver->live_instances; }

    ~AsyncTool() override
    {
        if (const auto driver = driver_.lock()) {
            driver->cleanup.enter();
            --driver->live_instances;
        }
    }

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto driver = driver_.lock();
        if (!driver || context->data().resources.size() != 1) {
            return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "The asynchronous fixture requires its driver and image lease."};
        }
        const Call call{completion, context->data().identity, context, context->data().resources.front().lease};
        auto control = std::make_shared<AsyncControl>(driver);
        const auto registered = completion.register_control(control);
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        if (!driver->record(call)) { AsyncDriver::close(call); }
        else if (driver->complete_in_begin) {
            // Exercise completion and exit that both precede wakeup registration.
            take(completion.complete(successful(call.identity, first_pixel(*context))));
            take(completion.mark_exited());
        }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    std::weak_ptr<AsyncDriver> driver_;
};

class IncrementTool final : public ToolInstance {
public:
    IncrementTool(const ToolExecutionConfiguration& configuration, const std::shared_ptr<AsyncDriver>& driver)
        : ToolInstance(configuration), driver_(driver) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        if (configuration().data().node == uuid<NodeId>('2')) { ++driver_->successor_calls; }
        const auto value = *context->data().inputs.front().value.as_int64();
        return InvocationStart{successful(context->data().identity, value + 1)};
    }
    std::shared_ptr<AsyncDriver> driver_;
};

template<class Tool> class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<AsyncDriver> driver) : driver_(std::move(driver)) {}
private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Tool>(configuration, driver_)}; }
    std::shared_ptr<AsyncDriver> driver_;
};

class ImageLease final : public ResourceLease {
public:
    explicit ImageLease(ResourceReference reference) : ResourceLease(std::move(reference)) {}
};

class ImageResourceProvider final : public InvocationResourceProvider {
public:
    std::span<const ResourceKind> kinds() const noexcept override { return kinds_; }

    Result<std::shared_ptr<const ResourceLease>> acquire(
        const ToolExecutionConfiguration&, const ToolResourceBinding& binding) override
    { return std::shared_ptr<const ResourceLease>{std::make_shared<ImageLease>(binding.resource)}; }
private:
    const std::array<ResourceKind, 1> kinds_{ResourceKind::Image};
};

ToolRegistry registry_for(const std::shared_ptr<AsyncDriver>& driver)
{
    InputDescriptor image{key<InputId>("image"), "Image", DataType::image()};
    image.binding.allowed_sources = {BindingSourceKind::PublicInput};
    InputDescriptor value{key<InputId>("value"), "Value", DataType::integer()};
    value.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput};
    const OutputDescriptor output{key<OutputId>("measurement"), "Measurement", DataType::integer()};
    ToolDescriptor async{key<ToolTypeId>("test.async.measure"), "Async measure", 1,
        {key<SchemaId>("test.async.measure.schema"), 1}, {}, {image}, {output}};
    async.execution.mode = ToolExecutionMode::Asynchronous;
    async.execution.cancellation = ToolCancellation::Cooperative;
    async.resources = {{key<SchemaId>("image.slot"), ResourceKind::Image}};
    async.image_formats = {mono8};
    const ToolDescriptor increment{key<ToolTypeId>("test.async.increment"), "Increment", 1,
        {key<SchemaId>("test.async.increment.schema"), 1}, {}, {value}, {output}};
    const std::array registrations{
        ToolRegistration{async, {async.schema.id, 1, 1, {}}, std::make_shared<Factory<AsyncTool>>(driver)},
        ToolRegistration{increment, {increment.schema.id, 1, 1, {}}, std::make_shared<Factory<IncrementTool>>(driver)}};
    return take(ToolRegistry::create(registrations));
}

Solution solution_for(bool with_successor)
{
    const auto main = uuid<WorkflowId>();
    const auto input = key<InputId>("image");
    Workflow chain{main, "Async chain", {key<SchemaId>("test.async.public"), 1, {{input, "Image", DataType::image()}}},
        {{uuid<NodeId>(), "Measure", ToolConfiguration{key<ToolTypeId>("test.async.measure"), 1},
            {{input, {ScopedInputReference{main, {}, input, {}}}}},
            {{key<SchemaId>("image.slot"), {uuid<ResourceId>(), ResourceKind::Image, {}}}}}},
        {}, {}, {uuid<NodeId>()}};
    if (with_successor) {
        chain.nodes.push_back({uuid<NodeId>('2'), "Successor", ToolConfiguration{key<ToolTypeId>("test.async.increment"), 1},
            {{key<InputId>("value"), {OutputReference{main, uuid<NodeId>(), key<OutputId>("measurement"), {}}}}}});
        chain.control_links.push_back({uuid<ControlLinkId>(), uuid<NodeId>(), {}, uuid<NodeId>('2')});
    }
    Workflow probe{uuid<WorkflowId>('2'), "Independent probe", {key<SchemaId>("test.async.probe"), 1},
        {{uuid<NodeId>('3'), "Probe", ToolConfiguration{key<ToolTypeId>("test.async.increment"), 1},
            {{key<InputId>("value"), {ConfigValue::integer(40)}}}}},
        {}, {}, {uuid<NodeId>('3')}};
    Solution solution{uuid<SolutionId>(), "Asynchronous execution", 1, 1, {std::move(chain), std::move(probe)}};
    solution.resources = {{uuid<ResourceId>(), "Fixture image", ResourceKind::Image,
        ManagedResource{{PathBase::SolutionDirectory, "fixture.png", {}}, 7, std::string(64, 'a')}}};
    for (const auto frame : {"async.frame", "async.crop"}) {
        solution.coordinates.push_back({key<CoordinateSystemId>(frame), CoordinateKind::Image, MeasurementUnit::Pixel,
            {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    }
    return solution;
}

RunHandle accepted(const RunSubmission& submission)
{
    if (const auto* refusal = std::get_if<RunRejected>(&submission)) {
        throw std::runtime_error(refusal->diagnostic.message);
    }
    check(std::holds_alternative<RunHandle>(submission), "The workflow must be accepted.");
    return std::get<RunHandle>(submission);
}

RunProgress wait_for(const RunHandle& run, RunWaitTarget target, ExecutionStatus status)
{
    const auto waited = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.status == status,
        "The run must reach the requested state.");
    return waited.progress;
}

void still_waiting(const RunHandle& run, RunWaitTarget target, ExecutionStatus status)
{
    const auto waited = take(run.wait(target, std::chrono::steady_clock::now()));
    check(waited.outcome == RunWaitOutcome::DeadlineExpired && waited.progress.status == status && !waited.progress.quiescent,
        "A client deadline must preserve the outstanding operation and its state.");
}

void busy(const RunSubmission& submission, const RunIdentity& run)
{
    const auto* occupied = std::get_if<RunBusy>(&submission);
    check(occupied && occupied->active == run, "The same workflow remains Busy under the exact original run identity.");
}

NodeResult result(const RunHandle& run, const InvocationKey& invocation)
{
    auto published = take(run.lookup(invocation));
    check(published && published->data().identity.run == run.identity() && published->data().identity.invocation == invocation,
        "Lookup returns the record belonging to the exact invocation.");
    return std::move(*published);
}

void measurement(const NodeResult& record, std::int64_t expected, Judgement judgement = Judgement::OK)
{
    const auto* output = record.output(key<OutputId>("measurement"));
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(record.data().status == ExecutionStatus::Succeeded && record.data().judgement == judgement
        && value && *value == DataValue::integer(expected), "The accepted measurement and business judgement are immutable.");
}

struct Fixture {
    explicit Fixture(bool with_successor = true, bool gated_cleanup = false, bool complete_in_begin = false)
        : driver(std::make_shared<AsyncDriver>(gated_cleanup, complete_in_begin)), registry(registry_for(driver)),
          engine(take(WorkflowEngine::create(registry, std::make_shared<ImageResourceProvider>()))),
          solution(solution_for(with_successor)) {}
    ~Fixture() { driver->drain(); }

    ConfigSnapshot snapshot(std::uint64_t version) const
    {
        return take(ConfigSnapshot::capture(solution, registry.descriptors(),
            {engine->session(), number<ConfigSnapshotId>(version)}));
    }

    RunSubmission submit(std::uint8_t pixel, std::uint64_t version = 1)
    {
        std::array bytes{std::byte{17}, static_cast<std::byte>(pixel)};
        const auto parent = take(ImageHandle::copy({2, 1, 2, mono8}, bytes, key<CoordinateSystemId>("async.frame")));
        const auto cropped = take(parent.crop({1, 0, 1, 1}, key<CoordinateSystemId>("async.crop")));
        bytes.back() = std::byte{0};
        const auto request = take(RunRequest::create(snapshot(version), WorkflowTarget{uuid<WorkflowId>()},
            {{key<InputId>("image"), take(DataValue::image({cropped, FrameId{uuid<ResourceId>(), pixel}, {}}))}},
            {TriggerKind::Manual, key<SchemaId>("test.async.manual")}));
        return engine->submit(request);
    }

    void probe()
    {
        const auto request = take(RunRequest::create(snapshot(1), WorkflowTarget{uuid<WorkflowId>('2')}, {},
            {TriggerKind::Manual, key<SchemaId>("test.async.probe")}));
        const auto run = accepted(engine->submit(request));
        const auto progress = wait_for(run, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
        check(progress.nodes.size() == 1 && progress.nodes.front().invocation.has_value(),
            "An independent synchronous flow completes while asynchronous work is suspended.");
        measurement(result(run, *progress.nodes.front().invocation), 41);
    }

    std::shared_ptr<AsyncDriver> driver;
    ToolRegistry registry;
    std::unique_ptr<WorkflowEngine> engine;
    Solution solution;
};

void live_inputs(const Call& call, std::uint8_t pixel)
{
    const auto context = call.context.lock();
    const auto lease = call.lease.lock();
    check(context && lease, "The suspended operation still owns its context and lease.");
    check(lease->reference() == ResourceReference{uuid<ResourceId>(), ResourceKind::Image, 7},
        "The owned lease addresses the fixed resource revision.");
    const auto* frame = context->data().inputs.front().value.as_image();
    check(frame && frame->frame == FrameId{uuid<ResourceId>(), pixel}
        && frame->image.origin_x() == 1 && frame->image.root_frame() == key<CoordinateSystemId>("async.frame")
        && first_pixel(*context) == pixel, "The fixed cropped image and frame identity survive until the backend exits.");
}

void mismatched_identity(const Call& call)
{
    const auto& exact = call.identity;
    std::vector<ToolInvocationIdentity> mismatches(7, exact);
    mismatches[0].session = exact.session == uuid<SolutionSessionId>('a') ? uuid<SolutionSessionId>('b') : uuid<SolutionSessionId>('a');
    mismatches[1].snapshot = number<ConfigSnapshotId>(exact.snapshot.value() + 1);
    mismatches[2].invocation.workflow_run = number<WorkflowRunId>(exact.invocation.workflow_run.value() + 1);
    mismatches[3].invocation.scope = number<ExecutionScopeId>(exact.invocation.scope.value() + 1);
    mismatches[4].invocation.node = uuid<NodeId>('f');
    mismatches[5].invocation.invocation = number<InvocationId>(exact.invocation.invocation.value() + 1);
    mismatches[6].attempt = number<AttemptId>(exact.attempt.value() + 1);
    for (const auto& wrong : mismatches) {
        rejected(call.completion.complete(successful(wrong, 99)), ErrorCode::InvalidValue,
            "A completion with any mismatched identity component is rejected.");
    }
}

void asynchronous_completion()
{
    Fixture fixture;
    const auto run = accepted(fixture.submit(11));
    const auto call = fixture.driver->await_call(0);
    check(call.identity.session == run.identity().snapshot.session && call.identity.snapshot == run.identity().snapshot.snapshot
        && call.identity.invocation.workflow_run == run.identity().run && call.identity.invocation.scope == number<ExecutionScopeId>(0)
        && call.identity.attempt == number<AttemptId>(0), "The operation carries the exact accepted run and attempt identity.");
    mismatched_identity(call);
    auto invalid = successful(call.identity, 99);
    invalid.outputs.front().content = DataValue::boolean(true);
    rejected(call.completion.complete(invalid), ErrorCode::TypeMismatch, "Asynchronous results obey the output schema.");
    still_waiting(run, RunWaitTarget::Completion, ExecutionStatus::Running);
    fixture.probe();
    check(!take(run.lookup(call.identity.invocation)), "Invalid and wrong-identity reports do not publish a result.");
    live_inputs(call, 11);

    take(call.completion.complete(successful(call.identity, 11, ToolJudgement::NG)));
    rejected(call.completion.complete(successful(call.identity, 99)), ErrorCode::InvalidState,
        "A duplicate completion cannot replace an accepted terminal payload.");
    // The older run's wakeup was delivered before this probe was admitted.
    fixture.probe();
    measurement(result(run, call.identity.invocation), 11, Judgement::NG);
    still_waiting(run, RunWaitTarget::Quiescence, ExecutionStatus::Running);
    check(fixture.driver->successor_calls == 0 && fixture.driver->live_instances == 1,
        "The successor waits while its predecessor still owns the instance.");
    busy(fixture.submit(99, 2), run.identity());

    take(call.completion.mark_exited());
    const auto finished = wait_for(run, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
    check(finished.nodes.size() == 2 && finished.nodes.back().invocation.has_value(), "The chain runs its successor.");
    measurement(result(run, *finished.nodes.back().invocation), 12);
    check(fixture.driver->successor_calls == 1 && fixture.driver->live_instances == 0 && call.context.expired() && call.lease.expired(),
        "The accepted completion advances control exactly once and quiescence includes cleanup.");

    const auto next = accepted(fixture.submit(23, 2));
    const auto current = fixture.driver->await_call(1);
    check(next.identity().run != run.identity().run, "Slot reuse allocates a new run identity.");
    rejected(call.completion.complete(successful(call.identity, 44)), ErrorCode::InvalidState, "Late completion on a retired token is rejected.");
    rejected(call.completion.complete(successful(current.identity, 45)), ErrorCode::InvalidState, "A retired token cannot address the new operation.");
    rejected(current.completion.complete(successful(call.identity, 46)), ErrorCode::InvalidValue, "The new token rejects an old invocation identity.");
    rejected(call.completion.mark_exited(), ErrorCode::InvalidState, "A late exit cannot release the new run's slot.");
    busy(fixture.submit(98, 3), next.identity());
    check(!take(next.lookup(current.identity.invocation)) && fixture.driver->successor_calls == 1,
        "Stale reports do not affect the new run or advance control.");
    take(current.completion.complete(successful(current.identity, 23)));
    take(current.completion.mark_exited());
    const auto next_finished = wait_for(next, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
    check(next_finished.nodes.back().invocation.has_value(), "The new valid run advances its own successor.");
    measurement(result(next, *next_finished.nodes.back().invocation), 24);
    measurement(result(run, call.identity.invocation), 11, Judgement::NG);
    check(fixture.driver->successor_calls == 2, "There is one actual successor call per accepted successful run.");
}

void completion_before_registration()
{
    Fixture fixture(true, false, true);
    const auto run = accepted(fixture.submit(30));
    const auto finished = wait_for(run, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
    check(finished.nodes.size() == 2 && finished.nodes.back().invocation.has_value(), "An early completion and exit still advance the chain.");
    measurement(result(run, *finished.nodes.back().invocation), 31);
    check(fixture.driver->successor_calls == 1 && fixture.driver->live_instances == 0, "Early notification does not repeat or leak an invocation.");
}

void check_cancelled_successor(const RunProgress& progress)
{
    check(progress.nodes.size() == 2 && progress.nodes.back().status == ExecutionStatus::Skipped
        && !progress.nodes.back().invocation && progress.nodes.back().skip_reason == NodeSkipReason::RunCancelled,
        "Cancellation prevents the unstarted successor from acquiring a call identity.");
}

void cancellation_before_submission()
{
    Fixture fixture;
    const auto run = accepted(fixture.submit(14));
    const auto call = fixture.driver->await_call(0);
    check(take(run.cancel()) == RunCancelDisposition::Requested, "The first cancellation request is accepted.");
    const auto cancelled = wait_for(run, RunWaitTarget::Completion, ExecutionStatus::Cancelled);
    check(!cancelled.quiescent && fixture.driver->await_cancellation() == 1 && call.completion.cancellation_requested()
        && fixture.driver->live_instances == 1,
        "Logical cancellation is published and forwarded once before the backend exits.");
    check_cancelled_successor(cancelled);
    auto record = result(run, call.identity.invocation);
    check(record.data().status == ExecutionStatus::Cancelled
        && std::ranges::none_of(record.data().outputs, [](const auto& slot) { return std::holds_alternative<DataValue>(slot.content); }),
        "The cancelled invocation owns a terminal record without algorithm output.");
    check(take(run.cancel()) == RunCancelDisposition::AlreadyCompleted, "Repeated cancellation cannot revise the terminal run.");
    still_waiting(run, RunWaitTarget::Quiescence, ExecutionStatus::Cancelled);
    busy(fixture.submit(99, 2), run.identity());
    live_inputs(call, 14);

    take(call.completion.complete(successful(call.identity, 88)));
    rejected(call.completion.complete(successful(call.identity, 89)), ErrorCode::InvalidState,
        "Duplicate late completion is rejected by the operation.");
    fixture.probe();
    record = result(run, call.identity.invocation);
    check(record.data().status == ExecutionStatus::Cancelled && fixture.driver->successor_calls == 0,
        "A late backend result cannot replace cancellation or trigger a successor.");
    take(call.completion.mark_exited());
    wait_for(run, RunWaitTarget::Quiescence, ExecutionStatus::Cancelled);
    check(fixture.driver->live_instances == 0 && call.context.expired() && call.lease.expired()
        && fixture.driver->cancellation_count() == 1, "Actual exit releases ownership after one cooperative cancellation request.");
}

void cancellation_after_submission()
{
    Fixture fixture;
    const auto run = accepted(fixture.submit(15));
    const auto call = fixture.driver->await_call(0);
    take(call.completion.complete(successful(call.identity, 15)));
    fixture.probe();
    measurement(result(run, call.identity.invocation), 15);
    check(take(run.cancel()) == RunCancelDisposition::Requested, "The chain is still cancellable after its first node submits a result.");
    const auto cancelled = wait_for(run, RunWaitTarget::Completion, ExecutionStatus::Cancelled);
    check_cancelled_successor(cancelled);
    check(cancelled.nodes.front().status == ExecutionStatus::Succeeded, "Cancellation preserves the already accepted node state.");
    measurement(result(run, call.identity.invocation), 15);
    still_waiting(run, RunWaitTarget::Quiescence, ExecutionStatus::Cancelled);
    take(call.completion.mark_exited());
    wait_for(run, RunWaitTarget::Quiescence, ExecutionStatus::Cancelled);
    check(fixture.driver->successor_calls == 0, "Actual exit cannot start a successor after cancellation.");
}

void detached_handle_and_cleanup()
{
    Fixture fixture(false, true);
    auto observer = std::optional{accepted(fixture.submit(19))};
    const auto identity = observer->identity();
    const auto call = fixture.driver->await_call(0);
    take(call.completion.complete(successful(call.identity, 19)));
    const auto completed = wait_for(*observer, RunWaitTarget::Completion, ExecutionStatus::Succeeded);
    check(!completed.quiescent, "The last node can publish completion while the physical operation is live.");
    still_waiting(*observer, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
    observer.reset();
    busy(fixture.submit(80, 2), identity);
    live_inputs(call, 19);

    std::optional<Result<std::monostate>> exit_result;
    std::exception_ptr exit_error;
    std::jthread exiting;
    OpenOnExit open_cleanup{fixture.driver->cleanup};
    exiting = std::jthread([&] {
        try { exit_result.emplace(call.completion.mark_exited()); }
        catch (...) { exit_error = std::current_exception(); }
    });
    fixture.driver->cleanup.await_entry();
    check(fixture.driver->live_instances == 1, "The instance destructor is still executing.");
    live_inputs(call, 19);
    busy(fixture.submit(81, 2), identity);
    fixture.probe();
    fixture.driver->cleanup.open();
    exiting.join();
    if (exit_error) { std::rethrow_exception(exit_error); }
    take(std::move(*exit_result));

    fixture.probe();
    check(call.context.expired() && call.lease.expired() && fixture.driver->live_instances == 0,
        "The engine releases the context, lease and instance even after the client drops its handle.");
    const auto next = accepted(fixture.submit(25, 2));
    const auto replacement = fixture.driver->await_call(1);
    take(replacement.completion.complete(successful(replacement.identity, 25)));
    take(replacement.completion.mark_exited());
    wait_for(next, RunWaitTarget::Quiescence, ExecutionStatus::Succeeded);
    measurement(result(next, replacement.identity.invocation), 25);
}
} // namespace

int main(int argc, char** argv)
{
    const std::string_view group = argc == 2 ? argv[1] : "";
    try {
        if (group == "async_completion") { asynchronous_completion(); completion_before_registration(); }
        else if (group == "async_cancellation") { cancellation_before_submission(); cancellation_after_submission(); }
        else if (group == "async_lifetime") { detached_handle_and_cleanup(); }
        else { throw std::runtime_error("Expected async_completion, async_cancellation, or async_lifetime."); }
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, 0 failures\n";
    return 0;
}
