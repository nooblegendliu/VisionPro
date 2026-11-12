#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

class Gate {
public:
    void enter()
    {
        std::unique_lock lock(mutex_);
        ++arrivals_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return open_; });
    }

    void await(unsigned arrivals, const char* message)
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, 5s, [&] { return arrivals_ >= arrivals; })) {
            throw std::runtime_error(message);
        }
    }

    void open()
    {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned arrivals_{0};
    bool open_{false};
};

struct OpenOnExit {
    Gate& gate;
    ~OpenOnExit() { gate.open(); }
};

struct ToolCalls {
    explicit ToolCalls(NodeId cleanup_node) : cleanup_node(cleanup_node) {}
    const NodeId cleanup_node;
    Gate begin;
    Gate cleanup;
    std::atomic<unsigned> created{0};
    std::atomic<unsigned> started{0};
    std::atomic<unsigned> cleaned{0};
};

class GatedAdd final : public ToolInstance {
public:
    GatedAdd(const ToolExecutionConfiguration& configuration, std::shared_ptr<ToolCalls> calls)
        : ToolInstance(configuration), calls_(std::move(calls)),
          pause_begin_(configuration.data().node == uuid<NodeId>()),
          pause_cleanup_(configuration.data().node == calls_->cleanup_node) {}

    ~GatedAdd() override
    {
        if (pause_cleanup_) { calls_->cleanup.enter(); }
        ++calls_->cleaned;
    }

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        ++calls_->started;
        if (pause_begin_) { calls_->begin.enter(); }
        const auto value = *context->data().inputs.front().value.as_int64();
        const auto offset = *context->data().parameters.front().value.as_int64();
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded,
            ToolJudgement::NotEvaluated, {{key<OutputId>("sum"), DataValue::integer(value + offset)}}, {}}};
    }

    std::shared_ptr<ToolCalls> calls_;
    bool pause_begin_;
    bool pause_cleanup_;
};

class GatedAddFactory final : public ToolInstanceFactory {
public:
    explicit GatedAddFactory(std::shared_ptr<ToolCalls> calls) : calls_(std::move(calls)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        ++calls_->created;
        return std::shared_ptr<ToolInstance>{std::make_shared<GatedAdd>(configuration, calls_)};
    }

private:
    std::shared_ptr<ToolCalls> calls_;
};

ToolRegistry registry_for(const std::shared_ptr<ToolCalls>& calls)
{
    InputDescriptor input{key<InputId>("value"), "Value", DataType::integer()};
    input.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::RunOutput};
    ParameterDescriptor offset{key<ParameterId>("offset"), "Offset", DataType::integer()};
    offset.binding.time = BindingTime::SnapshotBuild;
    ToolDescriptor tool{key<ToolTypeId>("test.gated-add"), "Gated add", 1, {key<SchemaId>("test.gated-add.schema"), 1},
        {offset}, {input}, {{key<OutputId>("sum"), "Sum", DataType::integer()}}};
    const ToolRegistration registration{tool, {tool.schema.id, 1, 1, {}}, std::make_shared<GatedAddFactory>(calls)};
    return take(ToolRegistry::create(std::span{&registration, 1}));
}

Node add(const ToolDescriptor& tool, char id, BindingSource source, std::int64_t offset)
{
    return {uuid<NodeId>(id), "Add", ToolConfiguration{tool.id, 1},
        {{tool.inputs.front().id, {std::move(source)}}, {tool.parameters.front().id, {ConfigValue::integer(offset)}}}};
}

Solution solution_for(const ToolDescriptor& tool)
{
    const auto first = uuid<WorkflowId>();
    const auto other = uuid<WorkflowId>('2');
    const auto input = tool.inputs.front().id;
    const PublicInterface ports{key<SchemaId>("test.admission.inputs"), 1, tool.inputs};
    return {uuid<SolutionId>(), "Admission", 1, 1, {
        {first, "First flow", ports,
            {add(tool, '1', ScopedInputReference{first, {}, input, {}}, 1),
             add(tool, '2', OutputReference{first, uuid<NodeId>(), tool.outputs.front().id, {}}, 2)},
            {{uuid<ControlLinkId>(), uuid<NodeId>(), {}, uuid<NodeId>('2')}}, {}, {uuid<NodeId>()}},
        {other, "Second flow", ports, {add(tool, '3', ScopedInputReference{other, {}, input, {}}, -2)},
            {}, {}, {uuid<NodeId>('3')}}
    }};
}

struct Fixture {
    explicit Fixture(char cleanup_node = '2') : calls(std::make_shared<ToolCalls>(uuid<NodeId>(cleanup_node))) {}

    std::shared_ptr<ToolCalls> calls;
    ToolRegistry registry{registry_for(calls)};
    std::unique_ptr<WorkflowEngine> engine{take(WorkflowEngine::create(registry))};
    Solution solution{solution_for(registry.descriptors().front())};

    ~Fixture()
    {
        // Release the worker before the engine joins, including test exceptions.
        calls->begin.open();
        calls->cleanup.open();
    }

    ConfigSnapshot capture(const Solution& source) const
    {
        return take(ConfigSnapshot::capture(source, registry.descriptors(),
            {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(source.revision))}));
    }

    void edit_successor(std::int64_t offset)
    {
        solution.workflows.front().nodes[1].bindings[1].sources.front() = ConfigValue::integer(offset);
        ++solution.revision;
    }
};

RunTrigger manual()
{ return {TriggerKind::Manual, key<SchemaId>("test.admission.manual")}; }

RunRequest request(const ConfigSnapshot& snapshot, std::int64_t value, char workflow = '1')
{
    return take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>(workflow)},
        {{key<InputId>("value"), DataValue::integer(value)}}, manual()));
}

RunHandle accepted(const RunSubmission& submission)
{
    if (const auto* rejected = std::get_if<RunRejected>(&submission)) {
        throw std::runtime_error(rejected->diagnostic.message);
    }
    check(std::holds_alternative<RunHandle>(submission), "Expected an accepted request.");
    return std::get<RunHandle>(submission);
}

void busy(const RunSubmission& submission, const RunIdentity& active)
{
    const auto* occupied = std::get_if<RunBusy>(&submission);
    check(occupied && occupied->active == active, "Busy must identify the already accepted run.");
}

RunProgress quiescence(const RunHandle& handle, ExecutionStatus status = ExecutionStatus::Succeeded)
{
    const auto wait = take(handle.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(wait.outcome == RunWaitOutcome::Satisfied && wait.progress.quiescent, "Run did not reach quiescence.");
    check(wait.progress.status == status, "Run reached an unexpected terminal status.");
    return wait.progress;
}

void deadline_expired(const RunHandle& handle, RunWaitTarget target, std::optional<ExecutionStatus> status = {})
{
    const auto wait = take(handle.wait(target, std::chrono::steady_clock::now()));
    check(wait.outcome == RunWaitOutcome::DeadlineExpired && !wait.progress.quiescent
        && (!status || wait.progress.status == *status),
        "Wait expiry must preserve the live run and its non-quiescent status.");
}

void check_output(const RunHandle& handle, const RunProgress& progress, std::int64_t expected)
{
    check(!progress.nodes.empty() && progress.nodes.back().invocation.has_value(),
        "The terminal node must expose its actual invocation identity.");
    const auto record = take(handle.lookup(*progress.nodes.back().invocation));
    check(record && record->data().identity.run == handle.identity(), "Output must belong to the accepted run.");
    const auto* output = record->output(key<OutputId>("sum"));
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && *value == DataValue::integer(expected), "Invocation used the wrong input or configuration snapshot.");
}

void atomic_admission()
{
    Fixture fixture;
    const auto original = fixture.capture(fixture.solution);
    fixture.edit_successor(5);
    const auto revised = fixture.capture(fixture.solution);
    const std::array requests{request(original, 11), request(revised, 90)};

    std::array<std::optional<RunSubmission>, 2> submissions;
    std::array<std::exception_ptr, 2> errors;
    Gate launch;
    std::array<std::jthread, 2> submitters;
    OpenOnExit release_launch{launch};
    for (std::size_t index = 0; index < submitters.size(); ++index) {
        submitters[index] = std::jthread([&, index] {
            try {
                launch.enter();
                submissions[index] = fixture.engine->submit(requests[index]);
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    launch.await(2, "Both submissions must reach the gate.");
    launch.open();
    for (auto& thread : submitters) { thread.join(); }
    for (const auto& error : errors) { if (error) { std::rethrow_exception(error); } }

    const auto winner = std::holds_alternative<RunHandle>(*submissions[0]) ? 0u : 1u;
    const auto active = accepted(*submissions[winner]);
    busy(*submissions[1u - winner], active.identity());
    check(active.identity().snapshot == requests[winner].snapshot().identity(), "Admission changed the winning snapshot.");
    fixture.calls->begin.await(1, "Accepted tool did not start.");
    deadline_expired(active, RunWaitTarget::Completion, ExecutionStatus::Running);

    const auto other = accepted(fixture.engine->submit(request(original, 7, '2')));
    check(other.identity().run.value() == active.identity().run.value() + 1, "Busy consumed a workflow run identity.");
    deadline_expired(other, RunWaitTarget::Completion, ExecutionStatus::Pending);

    fixture.edit_successor(100);
    const auto edited = fixture.capture(fixture.solution);
    const auto rejected_input = request(edited, 200);
    fixture.calls->begin.open();
    fixture.calls->cleanup.await(1, "The final tool must finish returning before its cleanup gate.");
    busy(fixture.engine->submit(rejected_input), active.identity());
    deadline_expired(active, RunWaitTarget::Quiescence);
    check(fixture.calls->started == 2 && fixture.calls->cleaned == 1, "Another flow started before physical cleanup.");

    fixture.calls->cleanup.open();
    const auto expected_first = winner == 0 ? 14 : 96;
    check_output(active, quiescence(active), expected_first);
    check_output(other, quiescence(other), 5);

    auto incompatible = fixture.solution;
    ++incompatible.revision;
    std::get<ToolConfiguration>(incompatible.workflows.front().nodes.front().configuration).config_version = 2;
    const auto rejected = fixture.engine->submit(request(fixture.capture(incompatible), 300));
    const auto* rejection = std::get_if<RunRejected>(&rejected);
    check(rejection && rejection->diagnostic.code == ErrorCode::IncompatibleSchemaVersion,
        "Invalid new configuration must be rejected before admission.");
    const auto missing_input = RunRequest::create(edited, WorkflowTarget{uuid<WorkflowId>()}, {}, manual());
    const auto* missing = std::get_if<Diagnostic>(&missing_input);
    check(missing && missing->code == ErrorCode::MissingRequiredValue, "A later request cannot reuse rejected or previous input.");

    const auto next = accepted(fixture.engine->submit(request(edited, 30)));
    check(next.identity().run.value() == other.identity().run.value() + 1, "Rejected requests consumed a workflow run identity.");
    check_output(next, quiescence(next), 131);
    check_output(active, take(active.inspect()), expected_first);
    check(fixture.calls->created == 5 && fixture.calls->started == 5 && fixture.calls->cleaned == 5,
        "Only the accepted invocations should create, start and destroy instances.");
}

void synchronous_cancellation()
{
    Fixture fixture('1');
    const auto snapshot = fixture.capture(fixture.solution);
    const auto active = accepted(fixture.engine->submit(request(snapshot, 20)));
    fixture.calls->begin.await(1, "Tool did not reach its synchronous call gate.");
    check(take(active.cancel()) == RunCancelDisposition::Requested, "First cancellation request was not accepted.");
    check(take(active.cancel()) == RunCancelDisposition::AlreadyRequested, "Repeated cancellation must be idempotent.");
    deadline_expired(active, RunWaitTarget::Quiescence, ExecutionStatus::Running);
    fixture.calls->begin.open();
    fixture.calls->cleanup.await(1, "Cancelled synchronous tool did not enter cleanup.");
    busy(fixture.engine->submit(request(snapshot, 21)), active.identity());
    fixture.calls->cleanup.open();

    const auto progress = quiescence(active, ExecutionStatus::Cancelled);
    check(progress.nodes.size() == 2 && progress.nodes[0].status == ExecutionStatus::Cancelled
        && progress.nodes[0].invocation.has_value(),
        "Cancellation must preserve the identity of the started invocation.");
    const auto result = take(active.lookup(*progress.nodes[0].invocation));
    check(result && result->data().status == ExecutionStatus::Cancelled, "The started invocation must have a cancelled record.");
    const auto* output = result->output(key<OutputId>("sum"));
    check(!output || !std::holds_alternative<DataValue>(output->content), "Late synchronous output must not be published.");
    check(progress.nodes[1].status == ExecutionStatus::Skipped && !progress.nodes[1].invocation
        && progress.nodes[1].skip_reason == NodeSkipReason::RunCancelled, "Cancellation must skip the unstarted successor.");
    check(fixture.calls->started == 1 && fixture.calls->cleaned == 1, "Cancelled run started an unexpected tool.");
    check(take(active.cancel()) == RunCancelDisposition::AlreadyCompleted, "A terminal run cannot be cancelled again.");
    const auto next = accepted(fixture.engine->submit(request(snapshot, 22)));
    check_output(next, quiescence(next), 25);
}

void dropped_handle()
{
    Fixture fixture;
    const auto snapshot = fixture.capture(fixture.solution);
    const auto identity = [&] {
        const auto handle = accepted(fixture.engine->submit(request(snapshot, 41)));
        fixture.calls->begin.await(1, "Tool did not start before the client dropped its handle.");
        return handle.identity();
    }();
    busy(fixture.engine->submit(request(snapshot, 42)), identity);
    const auto next_flow = accepted(fixture.engine->submit(request(snapshot, 14, '2')));
    fixture.calls->begin.open();
    fixture.calls->cleanup.open();
    check_output(next_flow, quiescence(next_flow), 12);
    check(fixture.calls->created == 3 && fixture.calls->started == 3 && fixture.calls->cleaned == 3,
        "Dropping the last client handle must let both accepted flows finish.");
    const auto next = accepted(fixture.engine->submit(request(snapshot, 43)));
    fixture.engine.reset();
    check_output(next, quiescence(next), 46);
}

void linear_scope_boundary()
{
    Fixture fixture;
    auto& flow = fixture.solution.workflows.front();
    flow.nodes.front().enabled = false;
    flow.nodes.back().bindings.front().sources = {
        ScopedInputReference{flow.id, {}, key<InputId>("value"), {}}};
    const auto snapshot = fixture.capture(fixture.solution);
    const auto independent = take(RunRequest::create(snapshot, NodeTarget{flow.id, flow.nodes.back().id},
        {{key<InputId>("value"), DataValue::integer(7)}}, manual()));
    const auto unsupported = fixture.engine->submit(independent);
    const auto* refusal = std::get_if<RunRejected>(&unsupported);
    check(refusal && refusal->diagnostic.code == ErrorCode::MissingCapability,
        "The linear workflow service rejects independent execution before it calls any tool.");

    const auto run = accepted(fixture.engine->submit(request(snapshot, 9)));
    const auto progress = quiescence(run);
    check(progress.nodes.size() == 2 && progress.nodes.front().status == ExecutionStatus::Skipped
        && progress.nodes.front().skip_reason == NodeSkipReason::Disabled && !progress.nodes.front().invocation,
        "A disabled entry closes the linear chain without an invocation.");
    check(progress.nodes.back().status == ExecutionStatus::Skipped
        && progress.nodes.back().skip_reason == NodeSkipReason::UpstreamClosed && !progress.nodes.back().invocation
        && fixture.calls->created == 0, "A closed unconditional predecessor cannot activate the next node.");
}
} // namespace

int main(int argc, char** argv)
{
    const std::string_view group = argc == 2 ? argv[1] : "";
    try {
        if (group == "atomic_admission") { atomic_admission(); }
        else if (group == "sync_run_lifecycle") { synchronous_cancellation(); dropped_handle(); linear_scope_boundary(); }
        else { throw std::runtime_error("Expected atomic_admission or sync_run_lifecycle."); }
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, 0 failures\n";
    return 0;
}
