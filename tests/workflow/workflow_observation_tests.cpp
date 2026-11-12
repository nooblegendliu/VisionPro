#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run_observation.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <array>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

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

template<UuidIdentity T> T uuid(char digit = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = digit;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

struct PendingCall {
    ToolInvocationIdentity identity;
    InvocationCompletion completion;

    void finish_if_pending() const
    {
        static_cast<void>(completion.complete({identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(completion.mark_exited());
    }
};

class AsyncDriver final {
public:
    bool record(const PendingCall& call)
    {
        {
            const std::lock_guard lock(mutex_);
            if (draining_) { return false; }
            pending_ = call;
        }
        changed_.notify_one();
        return true;
    }

    PendingCall wait_for(WorkflowRunId run)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return pending_ && pending_->identity.invocation.workflow_run == run; })) {
            throw std::runtime_error("The asynchronous fixture did not start the expected run.");
        }
        return *pending_;
    }

    void drain()
    {
        std::optional<PendingCall> call;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            call.swap(pending_);
        }
        if (call) { call->finish_if_pending(); }
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<PendingCall> pending_;
    bool draining_{false};
};

ToolCompletion successful(const ToolInvocationIdentity& identity, std::int64_t value)
{
    return {identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
        {{key<OutputId>("value"), DataValue::integer(value)}}, {}};
}

class Control final : public InvocationControl {
public:
    void request_cancel() override {}
};

class FixtureTool final : public ToolInstance {
public:
    FixtureTool(const ToolExecutionConfiguration& configuration, std::shared_ptr<AsyncDriver> driver)
        : ToolInstance(configuration), driver_(std::move(driver)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto& data = context->data();
        if (configuration().data().tool.execution.mode == ToolExecutionMode::Synchronous) {
            return InvocationStart{successful(data.identity, *data.inputs.front().value.as_int64() + 1)};
        }
        auto control = std::make_shared<Control>();
        const auto registered = completion.register_control(control);
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        const PendingCall call{data.identity, completion};
        if (!driver_->record(call)) { call.finish_if_pending(); }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    std::shared_ptr<AsyncDriver> driver_;
};

class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<AsyncDriver> driver) : driver_(std::move(driver)) {}
private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<FixtureTool>(configuration, driver_)}; }
    std::shared_ptr<AsyncDriver> driver_;
};

ToolRegistry registry_for(const std::shared_ptr<AsyncDriver>& driver)
{
    const OutputDescriptor output{key<OutputId>("value"), "Value", DataType::integer()};
    InputDescriptor input{key<InputId>("value"), "Value", DataType::integer()};
    input.binding.allowed_sources = {BindingSourceKind::RunOutput};
    ToolDescriptor source{key<ToolTypeId>("test.observation.source"), "Source", 1,
        {key<SchemaId>("test.observation.source.schema"), 1}, {}, {}, {output}};
    source.execution.mode = ToolExecutionMode::Asynchronous;
    const ToolDescriptor successor{key<ToolTypeId>("test.observation.successor"), "Successor", 1,
        {key<SchemaId>("test.observation.successor.schema"), 1}, {}, {input}, {output}};
    const auto factory = std::make_shared<Factory>(driver);
    const std::array registrations{
        ToolRegistration{source, {source.schema.id, 1, 1, {}}, factory},
        ToolRegistration{successor, {successor.schema.id, 1, 1, {}}, factory}};
    return take(ToolRegistry::create(registrations));
}

Solution solution_for()
{
    const auto flow = uuid<WorkflowId>();
    const auto source = uuid<NodeId>();
    const auto successor = uuid<NodeId>('2');
    const Binding from_source{key<InputId>("value"), {OutputReference{flow, source, key<OutputId>("value"), {}}}};
    Node first{source, "Source", ToolConfiguration{key<ToolTypeId>("test.observation.source"), 1}};
    Node second{successor, "Successor", ToolConfiguration{key<ToolTypeId>("test.observation.successor"), 1}, {from_source}};
    Workflow chain{flow, "Observed chain", {key<SchemaId>("test.observation.public"), 1},
        {std::move(first), std::move(second)},
        {{uuid<ControlLinkId>(), source, {}, successor}}, {}, {source}};
    return {uuid<SolutionId>(), "Run observations", 1, 1, {std::move(chain)}};
}

struct Fixture {
    ~Fixture() { driver->drain(); }

    RunHandle start(std::uint64_t snapshot_id)
    {
        const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(),
            {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(snapshot_id))}));
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>()}, {},
            {TriggerKind::Manual, key<SchemaId>("test.observation.manual")}));
        auto submission = engine->submit(request);
        if (const auto* rejection = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error(rejection->diagnostic.message);
        }
        check(std::holds_alternative<RunHandle>(submission), "The observed workflow is accepted.");
        return std::get<RunHandle>(std::move(submission));
    }

    std::shared_ptr<AsyncDriver> driver = std::make_shared<AsyncDriver>();
    ToolRegistry registry = registry_for(driver);
    std::unique_ptr<WorkflowEngine> engine = take(WorkflowEngine::create(registry));
    Solution solution = solution_for();
};

RunObservationEvent expect_event(const RunObservationRead& read, const RunHandle& run, std::uint64_t sequence,
    RunObservationKind kind, std::uint64_t dropped = 0)
{
    check(!read.closed && read.event.has_value(), "The observer receives a queued event while open.");
    check(read.dropped == dropped, "The observer reports its own cumulative overflow count.");
    const auto& event = *read.event;
    check(event.sequence == sequence && event.kind == kind, "The event has the expected sequence and kind.");
    check(event.run == run.identity() && event.status == ExecutionStatus::Succeeded,
        "The event identifies the successful transition in the exact session, snapshot and run.");
    check(event.result.has_value() == (kind == RunObservationKind::ResultPublished),
        "Only publication events identify an accepted result.");
    return event;
}

void expect_result(const RunObservationEvent& event, const RunHandle& run, NodeId node, std::int64_t expected)
{
    check(event.result && event.result->run == run.identity() && event.result->invocation.node == node,
        "The result event identifies the exact node and accepted run.");
    const auto published = take(run.lookup(event.result->invocation));
    check(published && published->data().identity == *event.result, "The result is queryable under the notified identity.");
    const auto* slot = published->output(key<OutputId>("value"));
    const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(value && value->as_int64() && *value->as_int64() == expected, "The accepted result has the expected value.");
}

void await_quiescence(const RunHandle& run)
{
    const auto waited = take(run.wait(RunWaitTarget::Quiescence, Clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent
        && waited.progress.status == ExecutionStatus::Succeeded && waited.progress.nodes.size() == 2,
        "Both calls succeed and the run reaches quiescence.");
}

void check_closed(const RunObservationRead& read)
{ check(read.closed && !read.event, "Unregistration discards queued events and closes the observer."); }

void bounded_delivery(Fixture& fixture)
{
    const auto normal = take(fixture.engine->observe(8));
    const auto slow = take(fixture.engine->observe(2));
    const auto run = fixture.start(1);
    const auto call = fixture.driver->wait_for(run.identity().run);
    take(call.completion.complete(successful(call.identity, 42)));

    const auto first = expect_event(normal.wait(Clock::now() + 5s), run, 1, RunObservationKind::ResultPublished);
    expect_result(first, run, uuid<NodeId>(), 42);
    const auto before_exit = normal.wait(Clock::now());
    check(!before_exit.closed && !before_exit.event, "Publication can be observed before the backend exits.");

    take(call.completion.mark_exited());
    await_quiescence(run);
    const auto second = expect_event(normal.wait(Clock::now() + 5s), run, 2, RunObservationKind::ResultPublished);
    expect_result(second, run, uuid<NodeId>('2'), 43);
    expect_event(normal.wait(Clock::now()), run, 3, RunObservationKind::RunCompleted);
    expect_event(normal.wait(Clock::now()), run, 4, RunObservationKind::RunQuiescent);

    // The slow observer has consumed nothing, so only the newest two events remain.
    expect_event(slow.wait(Clock::now()), run, 3, RunObservationKind::RunCompleted, 2);
    expect_event(slow.wait(Clock::now()), run, 4, RunObservationKind::RunQuiescent, 2);
    const auto empty = slow.wait(Clock::now());
    check(!empty.closed && !empty.event && empty.dropped == 2, "Overflow preserves the count after the queue is drained.");
}

void unsubscribe_during_async_run(Fixture& fixture)
{
    auto waiting = take(fixture.engine->observe(1));
    auto queued = take(fixture.engine->observe(1));
    const auto survivor = take(fixture.engine->observe(8));
    const auto closed_with_engine = take(fixture.engine->observe(8));
    const auto run = fixture.start(2);
    const auto call = fixture.driver->wait_for(run.identity().run);

    std::promise<void> ready;
    auto entered = ready.get_future();
    auto wait = std::async(std::launch::async, [&] {
        ready.set_value();
        return waiting.wait(Clock::now() + 5s);
    });
    entered.get();
    waiting.unsubscribe();
    check(wait.wait_for(2s) == std::future_status::ready, "Unregistration wakes the reader before its deadline.");
    check_closed(wait.get());

    take(call.completion.complete(successful(call.identity, 100)));
    const auto first = expect_event(survivor.wait(Clock::now() + 5s), run, 5, RunObservationKind::ResultPublished);
    expect_result(first, run, uuid<NodeId>(), 100);
    queued.unsubscribe();
    check_closed(queued.wait(Clock::now()));

    take(call.completion.mark_exited());
    await_quiescence(run);
    const auto successor = expect_event(survivor.wait(Clock::now()), run, 6, RunObservationKind::ResultPublished);
    expect_result(successor, run, uuid<NodeId>('2'), 101);
    expect_event(survivor.wait(Clock::now()), run, 7, RunObservationKind::RunCompleted);
    expect_event(survivor.wait(Clock::now()), run, 8, RunObservationKind::RunQuiescent);
    check_closed(waiting.wait(Clock::now()));
    check_closed(queued.wait(Clock::now()));

    take(fixture.engine->close());
    check_closed(closed_with_engine.wait(Clock::now()));
    check_closed(survivor.wait(Clock::now()));
    check(std::holds_alternative<Diagnostic>(fixture.engine->observe()), "The closed engine refuses new observers.");
}
} // namespace

int main()
{
    try {
        Fixture fixture;
        const auto invalid_capacity = fixture.engine->observe(0);
        const auto* diagnostic = std::get_if<Diagnostic>(&invalid_capacity);
        check(diagnostic && diagnostic->code == ErrorCode::InvalidValue, "A subscription requires positive capacity.");
        bounded_delivery(fixture);
        unsubscribe_during_async_run(fixture);
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
