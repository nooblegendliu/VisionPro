#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_session.hpp>
#include "logic_tools.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::infrastructure;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void check(bool ok, std::string_view message)
{ if (!ok) { throw std::runtime_error(std::string(message)); } }

template<class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error->message); }
    return std::get<T>(std::move(result));
}
template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<UuidIdentity T> T fresh() { return take(IdFactory::generate<T>()); }

// Synchronous tools wait inside begin(). Asynchronous tools publish success
// immediately and use a separate gate for the backend's physical exit.
class Gates {
public:
    void hold(int ticket) { const std::lock_guard lock(mutex_); held_.insert(ticket); }
    void enter(int ticket, const std::optional<InvocationCompletion>& asynchronous_exit)
    {
        std::unique_lock lock(mutex_);
        entered_.insert(ticket);
        changed_.notify_all();
        if (asynchronous_exit) {
            if (held_.contains(ticket)) { pending_exit_.emplace(ticket, *asynchronous_exit); return; }
            lock.unlock();
            take(asynchronous_exit->mark_exited());
        } else {
            changed_.wait(lock, [&] { return !held_.contains(ticket); });
        }
    }
    void await(int ticket)
    {
        std::unique_lock lock(mutex_);
        check(changed_.wait_for(lock, 5s, [&] { return entered_.contains(ticket); }), "A tool did not reach its gate.");
    }
    bool entered(int ticket) const { const std::lock_guard lock(mutex_); return entered_.contains(ticket); }
    void release(int ticket)
    {
        std::unique_lock lock(mutex_);
        held_.erase(ticket);
        auto entry = pending_exit_.extract(ticket);
        lock.unlock();
        changed_.notify_all();
        if (!entry.empty()) { take(entry.mapped().mark_exited()); }
    }
    void release_all()
    {
        std::map<int, InvocationCompletion> pending;
        {
            const std::lock_guard lock(mutex_);
            held_.clear();
            pending.swap(pending_exit_);
        }
        changed_.notify_all();
        for (auto& [ticket, completion] : pending) { static_cast<void>(completion.mark_exited()); }
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::set<int> held_, entered_;
    std::map<int, InvocationCompletion> pending_exit_;
};

class Control final : public InvocationControl { void request_cancel() override {} };

class Probe final : public ToolInstance {
public:
    Probe(const ToolExecutionConfiguration& config, std::shared_ptr<Gates> gates)
        : ToolInstance(config), gates_(std::move(gates)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto& call = context->data();
        const auto& desc = call.configuration.data().tool;
        const auto ticket = static_cast<int>(*call.inputs.at(0).value.as_int64());
        const auto delta = *call.inputs.at(1).value.as_int64();
        auto value = std::int64_t{ticket} + delta;
        if (call.state) { value = *take(call.state->read()).as_int64() + delta; }
        ToolCompletion result{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
            {{key<OutputId>("value"), DataValue::integer(value)}}, {}};
        for (const auto& binding : call.resources) {
            if (const auto* file = dynamic_cast<const FileResourceLease*>(binding.lease.get())) {
                check(!file->bytes().empty(), "The fixture file is empty.");
                result.outputs.push_back({key<OutputId>(binding.slot.text()), DataValue::integer(std::to_integer<int>(file->bytes().front()))});
            }
        }
        if (desc.execution.mode == ToolExecutionMode::Asynchronous) {
            auto control = std::make_shared<Control>();
            take(completion.register_control(control));
            take(completion.complete(result));
            gates_->enter(ticket, completion);
            return InvocationStart{AsynchronousInvocation{std::move(control)}};
        }
        gates_->enter(ticket, {});
        if (call.state) { take(call.state->write(DataValue::integer(value))); }
        return InvocationStart{std::move(result)};
    }
    std::shared_ptr<Gates> gates_;
};
class Factory final : public ToolInstanceFactory {
public:
    explicit Factory(std::shared_ptr<Gates> gates) : gates_(std::move(gates)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<Probe>(config, gates_)}; }
private:
    std::shared_ptr<Gates> gates_;
};

InputDescriptor input(std::string_view name)
{
    InputDescriptor port{key<InputId>(name), std::string(name), DataType::integer()};
    port.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput};
    return port;
}
ToolDescriptor descriptor(std::string_view name)
{
    const auto schema = std::string("s65.") + std::string(name);
    return {key<ToolTypeId>(schema), schema, 1, {key<SchemaId>(schema), 1}, {}, {input("ticket"), input("delta")},
        {{key<OutputId>("value"), "Value", DataType::integer()}}};
}
ToolRegistry catalog(const std::shared_ptr<Gates>& gates)
{
    const auto factory = std::make_shared<Factory>(gates);
    auto registrations = visionpro::tools::logic_registrations();
    const auto add = [&](ToolDescriptor desc) {
        registrations.push_back({desc, {desc.schema.id, 1, 1, {}}, factory});
    };
    add(descriptor("sync"));
    auto device = descriptor("exclusive");
    device.resources = {{key<SchemaId>("instrument"), ResourceKind::Binary, true, true}};
    device.execution.mode = ToolExecutionMode::Asynchronous;
    add(device);
    auto file = descriptor("files");
    for (auto name : {"pinned", "current"}) {
        file.resources.push_back({key<SchemaId>(name), ResourceKind::Binary});
        file.outputs.push_back({key<OutputId>(name), name, DataType::integer()});
    }
    add(file);
    for (auto scope : {StateScope::WorkflowRun, StateScope::SolutionSession}) {
        auto counter = descriptor(scope == StateScope::WorkflowRun ? "per_run" : "session");
        counter.execution.effects = ToolEffects::State;
        counter.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0), scope, StateResetPolicy::DomainStart,
            StateCompatibilityPolicy::PreserveCompatible, StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
        add(counter);
    }
    return take(ToolRegistry::create(registrations));
}
Workflow flow(std::string_view tool, std::vector<NodeResourceBinding> resources = {})
{
    const auto id = fresh<WorkflowId>();
    const auto ticket = key<InputId>("ticket"), delta = key<InputId>("delta");
    Node probe{fresh<NodeId>(), "Probe", ToolConfiguration{key<ToolTypeId>(std::string("s65.") + std::string(tool)), 1},
        {{ticket, {ScopedInputReference{id, {}, ticket, {}}}}, {delta, {ScopedInputReference{id, {}, delta, {}}}}}, std::move(resources)};
    return {id, std::string(tool), {key<SchemaId>("s65.flow"), 1, {input("ticket"), input("delta")}}, {probe}, {}, {}, {probe.id}};
}
Solution solution(std::vector<Workflow> flows, std::vector<ResourceDefinition> resources = {})
{ return {fresh<SolutionId>(), "Parallel test", 1, 1, std::move(flows), std::move(resources)}; }

RunArguments arguments(WorkflowId flow, int ticket, int delta)
{
    return {WorkflowTarget{flow}, {{key<InputId>("ticket"), DataValue::integer(ticket)}, {key<InputId>("delta"), DataValue::integer(delta)}},
        {TriggerKind::Manual, key<SchemaId>("s65.test")}};
}
RunHandle accepted(RunSubmission submission)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
    check(std::holds_alternative<RunHandle>(submission), "Unexpected Busy response.");
    return std::get<RunHandle>(std::move(submission));
}
struct Fixture {
    std::shared_ptr<Gates> gates = std::make_shared<Gates>();
    const ToolRegistry registry = catalog(gates);
    const std::unique_ptr<WorkflowSession> session = take(WorkflowSession::create(registry));
    ~Fixture() { gates->release_all(); }
    RunSubmission submit(const ConfigSnapshot& snapshot, WorkflowId flow, int ticket, int delta = 1) const
    {
        const auto args = arguments(flow, ticket, delta);
        return session->submit(take(RunRequest::create(snapshot, args.target, args.inputs, args.trigger)));
    }
};
RunProgress wait(const RunHandle& run, RunWaitTarget target = RunWaitTarget::Quiescence)
{
    auto result = take(run.wait(target, std::chrono::steady_clock::now() + 5s));
    check(result.outcome == RunWaitOutcome::Satisfied, "Run did not reach the required milestone before the barrier was released.");
    return result.progress;
}
NodeResult output(const RunHandle& run)
{
    auto end = wait(run);
    if (!end.diagnostics.empty()) { throw std::runtime_error(end.diagnostics.front().message); }
    check(end.status == ExecutionStatus::Succeeded && end.nodes.front().invocation.has_value(), "Expected a succeeded invocation.");
    return take(run.lookup(*end.nodes.front().invocation)).value();
}
std::int64_t number(const NodeResult& result, std::string_view port = "value")
{ return *std::get<DataValue>(result.output(key<OutputId>(port))->content).as_int64(); }

void parallel_dispatch()
{
    Fixture f;
    auto a = flow("sync"), b = flow("sync");
    const auto snapshot = take(f.session->activate(solution({a,b})));
    f.gates->hold(10);
    const auto blocked = accepted(f.submit(snapshot, a.id, 10));
    f.gates->await(10);
    const auto busy = f.submit(snapshot, a.id, 11);
    check(std::holds_alternative<RunBusy>(busy) && std::get<RunBusy>(busy).active == blocked.identity(), "A legacy flow keeps its Busy response.");
    const auto independent = accepted(f.submit(snapshot, b.id, 20));
    check(number(output(independent)) == 21 && !take(blocked.inspect()).quiescent, "B must finish before A is released.");
    f.gates->release(10);
    check(number(output(blocked)) == 11, "A must keep its original inputs.");
}

struct LeaseCounts { std::atomic<int> current{}, peak{}, acquired{}; };
class CountedLease final : public ResourceLease {
public:
    CountedLease(const ResourceReference& ref, std::shared_ptr<LeaseCounts> counts) : ResourceLease(ref), counts_(std::move(counts))
    {
        ++counts_->acquired;
        const int count = ++counts_->current;
        auto peak = counts_->peak.load();
        while (count > peak && !counts_->peak.compare_exchange_weak(peak, count)) {}
    }
    ~CountedLease() override { --counts_->current; }
private:
    std::shared_ptr<LeaseCounts> counts_;
};
class Provider : public InvocationResourceProvider {
public:
    std::span<const ResourceKind> kinds() const noexcept override
    { static constexpr std::array kinds{ResourceKind::Binary}; return kinds; }
    Result<std::shared_ptr<const ResourceLease>> acquire(const ToolExecutionConfiguration&, const ToolResourceBinding& binding) override
    { return std::shared_ptr<const ResourceLease>{std::make_shared<CountedLease>(binding.resource, counts)}; }
    std::shared_ptr<LeaseCounts> counts = std::make_shared<LeaseCounts>();
};

struct CleanupGate {
    std::promise<void> entered, release;
    const std::shared_future<void> resume = release.get_future().share();
    std::atomic<bool> timed_out{};
};
class CleanupProvider final : public Provider {
public:
    explicit CleanupProvider(std::shared_ptr<CleanupGate> gate) : gate_(std::move(gate)) {}
    ~CleanupProvider() override
    {
        gate_->entered.set_value();
        gate_->timed_out = gate_->resume.wait_for(5s) != std::future_status::ready;
    }
private:
    std::shared_ptr<CleanupGate> gate_;
};

void exclusive_resources()
{
    Fixture f;
    const auto device = fresh<ResourceId>();
    const NodeResourceBinding exclusive{key<SchemaId>("instrument"), {device, ResourceKind::Binary, {}}};
    auto a = flow("exclusive", {exclusive}), b = flow("exclusive", {exclusive}), c = flow("sync");
    const auto provider = std::make_shared<Provider>();
    const auto snapshot = take(f.session->activate(solution({a,b,c},
        {{device, "Instrument", ResourceKind::Binary, ExternalFileResource{{PathBase::SolutionDirectory, "instrument.bin"}, ResourceReadPolicy::PerInvocation}}}), provider));
    f.gates->hold(1);
    const auto holding = accepted(f.submit(snapshot, a.id, 1));
    f.gates->await(1);
    const auto logical = wait(holding, RunWaitTarget::Completion);
    check(logical.status == ExecutionStatus::Succeeded && !logical.quiescent, "Logical completion must precede the controlled physical exit.");
    const auto cancelled = accepted(f.submit(snapshot, b.id, 2));
    check(number(output(accepted(f.submit(snapshot, c.id, 3)))) == 4, "Unrelated C did not complete during exclusive contention.");
    check(!f.gates->entered(2) && provider->counts->acquired == 1, "Contending B acquired a lease before A exited.");
    take(cancelled.cancel());
    check(wait(cancelled).status == ExecutionStatus::Cancelled && !f.gates->entered(2), "Cancelling a resource waiter must not start its tool.");
    const auto second = accepted(f.submit(snapshot, b.id, 4));
    f.gates->release(1);
    check(number(output(second)) == 5 && number(output(holding)) == 2, "B should run once A physically exits.");
    check(provider->counts->peak == 1 && provider->counts->acquired == 2 && provider->counts->current == 0,
        "Exactly one physical lease may exist for the shared exclusive resource.");
}

void bounded_admission()
{
    Fixture f;
    auto a = flow("sync");
    a.admission = {1, 1, AdmissionOverflow::Reject};
    const auto snapshot = take(f.session->activate(solution({a})));
    f.gates->hold(10);
    const auto first = accepted(f.submit(snapshot, a.id, 10));
    f.gates->await(10);
    const auto second = accepted(f.submit(snapshot, a.id, 20));
    const auto third = f.submit(snapshot, a.id, 30);
    check(first.admission() == RunAdmission::Immediate && second.admission() == RunAdmission::Queued
        && take(second.inspect()).status == ExecutionStatus::Pending, "The accepted waiting request must remain observable as queued.");
    check(std::holds_alternative<RunRejected>(third) && std::get<RunRejected>(third).diagnostic.code == ErrorCode::RunQueueFull,
        "A full waiting queue must reject the third request.");
    f.gates->release(10);
    check(number(output(first)) == 11 && number(output(second)) == 21 && !f.gates->entered(30), "Only the two accepted inputs may execute.");

    f.gates->hold(40);
    const auto occupied = accepted(f.submit(snapshot, a.id, 40));
    f.gates->await(40);
    const auto removed = accepted(f.submit(snapshot, a.id, 50));
    take(removed.cancel());
    const auto end = wait(removed);
    check(end.status == ExecutionStatus::Cancelled && end.call_order.empty() && !f.gates->entered(50), "Cancelling a pending run must skip every invocation.");
    const auto replacement = accepted(f.submit(snapshot, a.id, 60));
    check(replacement.admission() == RunAdmission::Queued && !take(occupied.inspect()).quiescent, "The cancelled queue slot must be reusable before the active run finishes.");
    f.gates->release(40);
    check(number(output(replacement)) == 61 && number(output(occupied)) == 41, "A new waiting request must keep its own input.");

    a.admission = {1, 2, AdmissionOverflow::Busy};
    const auto fifo = take(f.session->activate(solution({a})));
    f.gates->hold(70);
    f.gates->hold(71);
    const auto leader = accepted(f.submit(fifo, a.id, 70));
    f.gates->await(70);
    const auto head = accepted(f.submit(fifo, a.id, 71)), tail = accepted(f.submit(fifo, a.id, 72));
    check(std::holds_alternative<RunBusy>(f.submit(fifo, a.id, 73)), "The configured Busy policy must also apply to a nonempty full queue.");
    f.gates->release(70);
    f.gates->await(71);
    check(!f.gates->entered(72), "The third accepted input overtook the head of the waiting queue.");
    f.gates->release(71);
    check(number(output(leader)) == 71 && number(output(head)) == 72 && number(output(tail)) == 73, "All three FIFO requests must keep their input.");
}

void admission_during_cancelled_cleanup()
{
    Fixture f;
    const auto gate = std::make_shared<CleanupGate>();
    auto cleaning = gate->entered.get_future();
    auto a = flow("sync"), b = flow("sync");
    a.admission = {1, 1, AdmissionOverflow::Busy};
    auto draft = solution({a, b});
    const auto initial = take(f.session->activate(draft));
    f.gates->hold(100);
    const auto running = accepted(f.submit(initial, a.id, 100));
    f.gates->await(100);
    const auto queued = accepted(f.session->publish_and_submit(initial, draft, arguments(a.id, 101, 1),
        std::make_shared<CleanupProvider>(gate)));
    {
        struct ReleaseOnExit { CleanupGate& gate; ~ReleaseOnExit() { gate.release.set_value(); } } release{*gate};
        check(queued.admission() == RunAdmission::Queued, "The controlled provider must belong to a queued run.");
        // Publish a different provider so only the queued Work owns the old one.
        const auto next = accepted(f.session->publish_and_submit(*f.session->inspect().active, draft, arguments(b.id, 103, 1), {}));
        check(number(output(next)) == 104, "Another workflow must publish while the original slot is busy.");
        take(queued.cancel());
        check(cleaning.wait_for(5s) == std::future_status::ready, "Queued cancellation did not enter provider cleanup.");
        f.gates->release(100);
        check(number(output(running)) == 101 && !take(queued.inspect()).quiescent,
            "The running slot must be free while the cancelled queue entry is still physically retiring.");
        const auto current = *f.session->inspect().active;
        f.gates->hold(104);
        const auto available = accepted(f.submit(current, a.id, 104));
        check(available.admission() == RunAdmission::Immediate,
            "A cancelled entry in cleanup cannot turn an available slot into a queue admission.");
        f.gates->await(104);
        const auto busy = f.submit(current, a.id, 106);
        check(std::holds_alternative<RunBusy>(busy) && std::get<RunBusy>(busy).active == available.identity(),
            "Busy must identify the run holding the active slot, not a cancelled predecessor retiring its queue entry.");
        f.gates->release(104);
        check(number(output(available)) == 105, "Rejecting a new request must preserve the active input.");
        draft.workflows.front().admission.queue_capacity = 0;
        const auto no_queue = accepted(f.session->publish_and_submit(current, draft, arguments(a.id, 105, 1), {}));
        check(no_queue.admission() == RunAdmission::Immediate && number(output(no_queue)) == 106,
            "Lowering queue capacity to zero must still allow a run that can start immediately.");
    }
    check(wait(queued).status == ExecutionStatus::Cancelled && !gate->timed_out && !f.gates->entered(101),
        "The cancelled run must retire without invoking its tool or timing out its cleanup barrier.");
}

void reentrant_state()
{
    Fixture f;
    auto local = flow("per_run"), shared = flow("session"), other = flow("sync");
    local.admission = shared.admission = {2, 0, AdmissionOverflow::Reject};
    auto accumulator = flow("sync");
    const auto& builtin = *f.registry.find_descriptor(key<ToolTypeId>("visionpro.calc.int64_accumulate"));
    const auto& mode = *std::ranges::find(builtin.parameters, key<ParameterId>("mode"), &ParameterDescriptor::id);
    auto& node = accumulator.nodes.front();
    node.configuration = ToolConfiguration{builtin.id, builtin.config_version};
    node.bindings = {{key<InputId>("value"), {ScopedInputReference{accumulator.id, {}, key<InputId>("delta"), {}}}},
        {mode.id, {take(ConfigValue::enumeration(mode.type, key<EnumMemberId>("Session")))}}};
    const auto snapshot = take(f.session->activate(solution({local, shared, other, accumulator})));
    f.gates->hold(1);
    f.gates->hold(2);
    const auto first = accepted(f.submit(snapshot, local.id, 1, 10));
    f.gates->await(1);
    const auto second = accepted(f.submit(snapshot, local.id, 2, 20));
    f.gates->await(2);
    const auto excess = f.submit(snapshot, local.id, 9, 90);
    check(std::holds_alternative<RunRejected>(excess) && std::get<RunRejected>(excess).diagnostic.code == ErrorCode::RunQueueFull,
        "The third request must see both in-flight slots occupied.");
    f.gates->release(2);
    const auto b = output(second);
    check(number(b) == 20 && second.admission() == RunAdmission::Immediate, "Two per-run state cells must progress independently.");
    f.gates->release(1);
    const auto a = output(first);
    check(number(a) == 10 && a.data().identity.invocation.workflow_run == first.identity().run
        && b.data().identity.invocation.workflow_run == second.identity().run && first.identity().run != second.identity().run,
        "Concurrent reentry must preserve input, state and producing run identity.");
    check(std::holds_alternative<Diagnostic>(first.lookup(b.data().identity.invocation)), "A handle must reject an invocation belonging to another run.");

    f.gates->hold(3);
    const auto increment = accepted(f.submit(snapshot, shared.id, 3));
    f.gates->await(3); // The first invocation has already read zero from session state.
    const auto again = accepted(f.submit(snapshot, shared.id, 4));
    check(number(output(accepted(f.submit(snapshot, other.id, 5)))) == 6 && !f.gates->entered(4), "Only contenders for the shared state cell may wait.");
    f.gates->release(3);
    check(number(output(increment)) == 1 && number(output(again)) == 2, "Serializing read-modify-write across reentry must preserve both increments.");
    check(number(output(accepted(f.submit(snapshot, accumulator.id, 6)))) == 1
        && number(output(accepted(f.submit(snapshot, accumulator.id, 7)))) == 2,
        "The production session accumulator must preserve its accepted value between serial runs.");
}

struct Scratch {
    const fs::path directory = fs::temp_directory_path() / ("visionpro-s65-" + fresh<SolutionId>().to_string());
    Scratch() { fs::create_directories(directory); }
    ~Scratch() { std::error_code ignored; fs::remove_all(directory, ignored); }
};
void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << bytes;
    file.close();
    check(file.good(), "Could not stage fixture file.");
}

void queued_file_snapshots()
{
    Scratch old_files, new_files;
    Fixture f;
    const auto locked = fresh<ResourceId>(), dynamic = fresh<ResourceId>();
    auto reader = flow("files", {{key<SchemaId>("pinned"), {locked, ResourceKind::Binary, {}}},
        {key<SchemaId>("current"), {dynamic, ResourceKind::Binary, {}}}});
    reader.admission = {1, 1, AdmissionOverflow::Reject};
    const auto other = flow("sync");
    auto draft = solution({reader,other},
        {{locked, "Fixed file", ResourceKind::Binary, ExternalFileResource{{PathBase::SolutionDirectory, "fixed.bin"},
            ResourceReadPolicy::SnapshotLocked, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}},
         {dynamic, "Live file", ResourceKind::Binary, ExternalFileResource{{PathBase::SolutionDirectory, "live.bin"}, ResourceReadPolicy::PerInvocation}}});
    write(old_files.directory / "fixed.bin", "abc");
    write(old_files.directory / "live.bin", "1");
    const auto original = take(f.session->activate(draft, take(FileResourceProvider::create(draft, old_files.directory))));
    f.gates->hold(10);
    const auto active = accepted(f.submit(original, reader.id, 10));
    f.gates->await(10);
    const auto queued = accepted(f.submit(original, reader.id, 20, 7));
    check(queued.admission() == RunAdmission::Queued, "File probe must be queued before editing its files.");
    write(old_files.directory / "fixed.bin", "changed");
    write(old_files.directory / "live.bin", "2");
    write(new_files.directory / "fixed.bin", "ABC");
    write(new_files.directory / "live.bin", "3");
    std::get<ExternalFileResource>(draft.resources.front().location).expected_sha256 = "b5d4045c3f466fa91fe2cc6abe79232a1a57cdf104f7a26e716e0a1e2789df78";
    draft.workflows.front().nodes.front().bindings.at(1).sources = {ConfigValue::integer(100)};
    const auto replacement = take(FileResourceProvider::create(draft, new_files.directory));
    const auto publication = accepted(f.session->publish_and_submit(original, draft, arguments(other.id, 30, 1), replacement));
    check(number(output(publication)) == 31, "Another workflow must publish the replacement while the old reader waits.");
    f.gates->release(10);
    const auto started = output(active), waiting = output(queued);
    check(number(started, "current") == '1' && number(waiting, "pinned") == 'a' && number(waiting, "current") == '2'
        && number(waiting) == 27 && queued.request().snapshot().same_capture(original), "A queued request must freeze the old plan and provider, then acquire dynamic bytes at invocation.");
    const auto updated = output(accepted(f.submit(*f.session->inspect().active, reader.id, 40, 7)));
    check(number(updated, "pinned") == 'A' && number(updated, "current") == '3' && number(updated) == 140,
        "Subsequent work must use the new provider and edited binding.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "parallel_dispatch") { parallel_dispatch(); }
        else if (test == "exclusive_resources") { exclusive_resources(); }
        else if (test == "bounded_admission") { bounded_admission(); admission_during_cancelled_cleanup(); }
        else if (test == "reentrant_state") { reentrant_state(); }
        else if (test == "queued_file_snapshots") { queued_file_snapshots(); }
        else { throw std::runtime_error("Unknown concurrency test."); }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
