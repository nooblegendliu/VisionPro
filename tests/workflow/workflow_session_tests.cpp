#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <array>
#include <atomic>
#include <chrono>
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

template<class T> void rejected(const Result<T>& result, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, message);
}

void rejected(const RunSubmission& submission, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<RunRejected>(&submission);
    check(failure && failure->diagnostic.code == code, message);
}

template<UuidIdentity T> T id()
{ return take(IdFactory::parse<T>("00000000-0000-4000-8000-000000000001")); }

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

class ExitGate {
public:
    void enter()
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }

    void wait_for_entry()
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return entered_; })) {
            throw std::runtime_error("The old backend did not enter physical cleanup.");
        }
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
    bool entered_{false};
    bool released_{false};
};

struct Call {
    InvocationCompletion token;
    ToolInvocationIdentity identity;
    std::int64_t value;
    std::weak_ptr<const InvocationContext> context;
    std::shared_ptr<ExitGate> exit;
};

ToolCompletion success(const Call& call)
{
    return {call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
        {{key<OutputId>("value"), DataValue::integer(call.value)}}, {}};
}

class Driver {
public:
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
        if (!changed_.wait_for(lock, 5s, [&] { return calls_.size() > index; })) {
            throw std::runtime_error("The requested session did not start its backend call.");
        }
        return calls_[index];
    }

    static void finish(const Call& call)
    {
        call.exit->release();
        static_cast<void>(call.token.complete({call.identity, ToolCompletionStatus::Cancelled,
            ToolJudgement::NotEvaluated, {}, {}}));
        static_cast<void>(call.token.mark_exited());
    }

    void drain()
    {
        std::vector<Call> calls;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            calls.swap(calls_);
        }
        for (const auto& call : calls) { finish(call); }
    }

    std::atomic<unsigned> live{0};
    std::atomic<unsigned> created{0};
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Call> calls_;
    bool draining_{false};
};

class Control final : public InvocationControl {
    void request_cancel() override {}
};

class Tool final : public ToolInstance {
public:
    Tool(const ToolExecutionConfiguration& configuration, const std::shared_ptr<Driver>& driver)
        : ToolInstance(configuration), driver_(driver)
    {
        ++driver->live;
        ++driver->created;
    }

    ~Tool() override
    {
        if (armed_) { exit_->enter(); }
        if (const auto driver = driver_.lock()) { --driver->live; }
    }
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion token) override
    {
        const auto driver = driver_.lock();
        if (!driver) { throw std::runtime_error("The test backend is absent."); }
        const Call call{token, context->data().identity, *context->data().inputs.front().value.as_int64(), context, exit_};
        auto control = std::make_shared<Control>();
        take(token.register_control(control));
        armed_ = driver->record(call);
        if (!armed_) { Driver::finish(call); }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    const std::weak_ptr<Driver> driver_;
    const std::shared_ptr<ExitGate> exit_{std::make_shared<ExitGate>()};
    bool armed_{false};
};

class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Driver> driver) : driver_(std::move(driver)) {}
private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Tool>(configuration, driver_)}; }
    const std::shared_ptr<Driver> driver_;
};

ToolRegistry registry_for(const std::shared_ptr<Driver>& driver)
{
    InputDescriptor input{key<InputId>("value"), "Input", DataType::integer()};
    input.binding.allowed_sources = {BindingSourceKind::Constant};
    ToolDescriptor descriptor{key<ToolTypeId>("test.session.read"), "Read", 1,
        {key<SchemaId>("test.session.read.schema"), 1}, {}, {input},
        {{key<OutputId>("value"), "Output", DataType::integer()}}};
    descriptor.execution.mode = ToolExecutionMode::Asynchronous;
    auto unsupported = descriptor;
    unsupported.id = key<ToolTypeId>("test.session.new.version");
    unsupported.schema.version = 2;
    const auto factory = std::make_shared<Factory>(driver);
    const SchemaSupport support{descriptor.schema.id, 1, 1, {}};
    const std::array registrations{ToolRegistration{descriptor, support, factory},
        ToolRegistration{unsupported, support, factory}};
    return take(ToolRegistry::create(registrations));
}

Solution solution(std::uint64_t revision, std::int64_t value)
{
    Workflow flow{id<WorkflowId>(), "Session flow", {key<SchemaId>("test.session.flow"), 1},
        {{id<NodeId>(), "Read", ToolConfiguration{key<ToolTypeId>("test.session.read"), 1},
            {{key<InputId>("value"), {ConfigValue::integer(value)}}}}}, {}, {}, {id<NodeId>()}};
    return {id<SolutionId>(), "Session fixture", 1, revision, {std::move(flow)}};
}

struct Fixture {
    Fixture() : session(take(WorkflowSession::create(registry))) {}
    ~Fixture() { driver->drain(); }

    const std::shared_ptr<Driver> driver{std::make_shared<Driver>()};
    const ToolRegistry registry{registry_for(driver)};
    const std::unique_ptr<WorkflowSession> session;
};

struct DrainOnExit {
    Driver& driver;
    ~DrainOnExit() { driver.drain(); }
};

RunRequest request(const ConfigSnapshot& snapshot)
{
    return take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>()}, {},
        {TriggerKind::Manual, key<SchemaId>("test.session.manual")}));
}

RunHandle accepted(RunSubmission submission)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
    check(std::holds_alternative<RunHandle>(submission), "The active capture is admitted.");
    return std::get<RunHandle>(std::move(submission));
}

void wait_for(const RunHandle& run, RunWaitTarget target)
{
    const auto waited = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.status == ExecutionStatus::Succeeded,
        "The run reaches the requested successful terminal state.");
}

NodeResult lookup(const RunHandle& run, const Call& call)
{
    auto found = take(run.lookup(call.identity.invocation));
    check(found && found->data().identity.run == run.identity() && found->data().identity.attempt == call.identity.attempt,
        "The held run can read its exact accepted invocation.");
    return std::move(*found);
}

void value_is(const NodeResult& result, std::int64_t expected)
{
    const auto* output = result.output(key<OutputId>("value"));
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && *value == DataValue::integer(expected), "The exact snapshot's result remains readable.");
}

void observation_closed(const RunObservation& observation)
{
    const auto read = observation.wait(std::chrono::steady_clock::now() + 5s);
    check(read.closed && !read.event, "The session revoked the optional observation channel.");
}

void bad_candidate_keeps_active()
{
    Fixture fixture;
    auto& session = *fixture.session;
    check(session.inspect().status == WorkflowSessionStatus::Empty, "A new coordinator has no active solution.");
    const auto original = solution(1, 11);
    const auto snapshot = take(session.activate(original));
    auto observer = take(session.observe());
    const auto published = session.inspect();
    check(published.active && published.active->same_capture(snapshot), "Activation publishes the validated immutable capture.");

    const auto forged = take(ConfigSnapshot::capture(solution(2, 99), fixture.registry.descriptors(), snapshot.identity()));
    rejected(session.submit(request(forged)), ErrorCode::ResultIdentityMismatch,
        "Equal numeric snapshot identities do not authorize another capture.");
    const auto run = accepted(session.submit(request(snapshot)));
    const auto call = fixture.driver->await_call(0);

    auto unknown = solution(2, 22);
    std::get<ToolConfiguration>(unknown.workflows.front().nodes.front().configuration).type =
        key<ToolTypeId>("test.session.missing");
    rejected(session.activate(unknown), ErrorCode::UnknownTool, "An invalid model cannot replace the active candidate.");
    auto unsupported = solution(3, 33);
    std::get<ToolConfiguration>(unsupported.workflows.front().nodes.front().configuration).type =
        key<ToolTypeId>("test.session.new.version");
    rejected(session.activate(unsupported), ErrorCode::IncompatibleSchemaVersion,
        "A candidate with unsupported backend Schema cannot replace the active session.");
    const auto active = session.inspect();
    check(active.status == WorkflowSessionStatus::Active && active.active && active.active->same_capture(snapshot) && !active.operation,
        "Rejected candidates preserve the exact previous configuration and clear the operation.");
    const auto busy = session.submit(request(snapshot));
    const auto* occupied = std::get_if<RunBusy>(&busy);
    check(occupied && occupied->active == run.identity() && fixture.driver->created == 1,
        "Candidate validation has no factory or execution side effects.");

    take(call.token.complete(success(call)));
    call.exit->release();
    take(call.token.mark_exited());
    wait_for(run, RunWaitTarget::Quiescence);
    const auto event = observer.wait(std::chrono::steady_clock::now() + 5s);
    check(event.event && !event.closed && event.event->run == run.identity()
        && event.event->kind == RunObservationKind::ResultPublished,
        "The existing observer still receives the original run's result event.");
    auto retained = take(session.lookup_result(run.identity(), call.identity.invocation));
    check(retained.has_value(), "The active session can look up the indexed result.");
    value_is(*retained, 11);
}

void switch_and_close()
{
    Fixture fixture;
    auto& session = *fixture.session;
    const auto first = take(session.activate(solution(1, 41)));
    const auto first_request = request(first);
    auto old_observer = take(session.observe());
    const auto old_run = accepted(session.submit(first_request));
    const auto old_call = fixture.driver->await_call(0);
    const auto replacement = solution(2, 72);

    std::future<Result<ConfigSnapshot>> replacing;
    std::future<Result<std::monostate>> exiting;
    std::future<Result<std::monostate>> closing;
    const DrainOnExit unblock{*fixture.driver};
    replacing = std::async(std::launch::async, [&] { return session.activate(replacement); });
    observation_closed(old_observer);
    const auto switching = session.inspect();
    check(switching.status == WorkflowSessionStatus::Switching && switching.active && switching.active->same_capture(first)
        && switching.operation && switching.operation->kind == SessionOperationKind::Activate,
        "Switching exposes the old capture and a separate activation operation.");
    rejected(session.submit(first_request), ErrorCode::InvalidRunRequest, "Switching refuses new runs.");
    rejected(session.observe(), ErrorCode::InvalidState, "Switching refuses new observers.");
    rejected(session.close(), ErrorCode::InvalidState, "Concurrent management is rejected during a switch.");
    rejected(session.reset_node_state({id<WorkflowId>(), id<NodeId>()}), ErrorCode::InvalidState,
        "Switching refuses mutations of the old session.");

    take(old_call.token.complete(success(old_call)));
    wait_for(old_run, RunWaitTarget::Completion);
    const auto held_result = lookup(old_run, old_call);
    exiting = std::async(std::launch::async, [&] { return old_call.token.mark_exited(); });
    old_call.exit->wait_for_entry();
    check(!take(old_run.inspect()).quiescent && !old_call.context.expired() && fixture.driver->live == 1,
        "In-progress physical cleanup retains the old context.");
    check(replacing.wait_for(0ms) == std::future_status::timeout && fixture.driver->created == 1,
        "The replacement cannot publish or start a tool during old backend cleanup.");
    old_call.exit->release();
    take(exiting.get());
    const auto second = take(replacing.get());
    wait_for(old_run, RunWaitTarget::Quiescence);
    const auto active = session.inspect();
    check(active.status == WorkflowSessionStatus::Active && active.active && active.active->same_capture(second) && !active.operation
        && first.identity().session != second.identity().session && fixture.driver->live == 0,
        "The replacement publishes a new session only after the old call drains.");
    rejected(session.submit(first_request), ErrorCode::ResultIdentityMismatch, "The previous session capture is stale.");
    rejected(session.lookup_result(old_run.identity(), old_call.identity.invocation), ErrorCode::ResultIdentityMismatch,
        "The active index never resolves a previous session's identity.");
    value_is(held_result, 41);
    value_is(lookup(old_run, old_call), 41);

    auto new_observer = take(session.observe());
    const auto second_request = request(second);
    const auto new_run = accepted(session.submit(second_request));
    const auto new_call = fixture.driver->await_call(1);
    check(new_run.identity().run == old_run.identity().run && new_call.identity.invocation == old_call.identity.invocation,
        "The new session can reuse numeric identifiers without sharing identity.");
    rejected(old_call.token.complete(success(old_call)), ErrorCode::InvalidState, "The old backend cannot publish twice.");
    rejected(old_call.token.mark_exited(), ErrorCode::InvalidState, "The old backend cannot exit twice.");
    rejected(new_call.token.complete(success(old_call)), ErrorCode::InvalidValue,
        "An old completion cannot match the new invocation token.");
    const auto no_old_event = new_observer.wait(std::chrono::steady_clock::now());
    check(!no_old_event.closed && !no_old_event.event, "No old session completion reached the new observer.");

    take(new_call.token.complete(success(new_call)));
    wait_for(new_run, RunWaitTarget::Completion);
    const auto new_event = new_observer.wait(std::chrono::steady_clock::now() + 5s);
    check(new_event.event && new_event.event->run == new_run.identity()
        && new_event.event->kind == RunObservationKind::ResultPublished,
        "The new observer receives its own session's result.");
    // Completion already published to the original queue. An empty queue
    // signals the close boundary before we check that pending event was cleared.
    const auto close_observer = take(session.observe());
    closing = std::async(std::launch::async, [&] { return session.close(); });
    observation_closed(close_observer);
    observation_closed(new_observer);
    const auto closing_progress = session.inspect();
    check(closing_progress.status == WorkflowSessionStatus::Closing && closing_progress.operation
        && closing_progress.operation->kind == SessionOperationKind::Close
        && closing_progress.operation->id > switching.operation->id,
        "Close has a distinct management identity while waiting for exit.");
    rejected(session.submit(second_request), ErrorCode::InvalidRunRequest, "Close stops admission before cleanup.");
    rejected(session.observe(), ErrorCode::InvalidState, "Close stops observation before cleanup.");
    rejected(session.activate(replacement), ErrorCode::InvalidState, "Close cannot overlap another activation.");
    check(closing.wait_for(0ms) == std::future_status::timeout && !new_call.context.expired(),
        "Logical completion alone does not let close release the active backend.");
    new_call.exit->release();
    take(new_call.token.mark_exited());
    take(closing.get());
    wait_for(new_run, RunWaitTarget::Quiescence);
    const auto closed = session.inspect();
    check(closed.status == WorkflowSessionStatus::Closed && !closed.active && !closed.operation
        && fixture.driver->live == 0 && new_call.context.expired(),
        "Close returns after releasing the active configuration and backend.");
    rejected(session.submit(second_request), ErrorCode::InvalidRunRequest, "A closed coordinator cannot admit work.");
    take(session.close());
    value_is(lookup(new_run, new_call), 72);
}
} // namespace

int main()
{
    try {
        bad_candidate_keeps_active();
        switch_and_close();
    } catch (const std::exception& failure) {
        std::cerr << "FAIL after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks passed\n";
    return 0;
}
