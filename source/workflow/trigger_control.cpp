#include <visionpro/workflow/trigger_control.hpp>

#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace visionpro::workflow {
namespace {
using namespace core;

Diagnostic failure(ErrorCode code, WorkflowId workflow, std::string message)
{
    DiagnosticLocation location;
    location.workflow = workflow;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

std::optional<TriggerControl::Clock::time_point> next_tick(TriggerControl::Clock::time_point now,
    std::uint32_t interval_ms)
{
    const auto interval = std::chrono::milliseconds(interval_ms);
    if (now > TriggerControl::Clock::time_point::max() - interval) { return {}; }
    return now + interval;
}
} // namespace

struct TriggerControl::Impl {
    Impl(const std::shared_ptr<WorkflowSession>& owner, SolutionSessionId identity, WorkflowId flow,
        WorkflowTrigger entry, CorrelationDomainId domain, Clock::time_point now)
        : session(owner), session_id(identity), workflow(flow), entry(std::move(entry)), domain(std::move(domain)),
          due(next_tick(now, this->entry.interval_ms)) {}

    std::weak_ptr<WorkflowSession> session;
    const SolutionSessionId session_id;
    const WorkflowId workflow;
    const WorkflowTrigger entry;
    const CorrelationDomainId domain;
    std::mutex mutex;
    std::condition_variable idle;
    bool accepting{true};
    std::size_t preparing{};
    std::int64_t last_frame{};
    std::optional<Clock::time_point> due;
    std::vector<RunHandle> accepted;

    RunRejected stopped() const
    { return {failure(ErrorCode::InvalidState, workflow, "The trigger subscription has stopped.")}; }

    void prune_quiescent()
    {
        std::erase_if(accepted, [](const RunHandle& run) {
            const auto inspected = run.inspect();
            const auto* progress = std::get_if<RunProgress>(&inspected);
            return progress && progress->quiescent;
        });
    }

    RunSubmission submit(const RunTrigger& trigger, std::int64_t frame, const std::optional<CorrelationId>& correlation)
    try
    {
        const auto owner = session.lock();
        if (!owner) { return stopped(); }
        const auto current = owner->inspect();
        if (current.status != WorkflowSessionStatus::Active || !current.active
            || current.active->identity().session != session_id) {
            return RunRejected{failure(ErrorCode::ResultIdentityMismatch, workflow, "The trigger's solution session is no longer active.")};
        }
        const auto* flow = find_workflow(current.active->solution(), workflow);
        if (!flow || !flow->enabled || flow->trigger != entry) {
            return RunRejected{failure(ErrorCode::InvalidRunRequest, workflow, "The workflow's configured trigger entry has changed.")};
        }
        auto request = RunRequest::create(*current.active, WorkflowTarget{workflow},
            {{entry.frame_input, DataValue::integer(frame)}}, trigger, correlation);
        if (auto* error = std::get_if<Diagnostic>(&request)) { return RunRejected{std::move(*error)}; }

        // Share a single ordering point with stop, without holding the subscription
        // lock across file/provider preflight. An event still preparing at stop cannot enter a slot.
        const WorkflowSession::AdmissionGate gate = [&](const auto& admit) -> RunSubmission {
            const std::lock_guard lock(mutex);
            if (!accepting) { return stopped(); }
            if (accepted.size() == accepted.capacity()) {
                prune_quiescent();
                if (accepted.size() == accepted.capacity()) {
                    accepted.reserve(accepted.empty() ? 1 : 2 * accepted.size());
                }
            }
            auto result = admit();
            if (const auto* handle = std::get_if<RunHandle>(&result)) { accepted.push_back(*handle); }
            return result;
        };
        return owner->submit_guarded(std::get<RunRequest>(request), &gate);
    }
    catch (...) {
        return RunRejected{failure(ErrorCode::ExecutionBoundaryFailure, workflow, "Could not prepare the trigger event.")};
    }

    TriggerReceipt dispatch(TriggerKind kind, std::int64_t frame, std::optional<CorrelationId> correlation)
    {
        const RunTrigger trigger{kind, entry.source};
        if (!correlation) {
            auto key = RuntimeIdFactory::correlation(domain, std::to_string(frame));
            if (auto* error = std::get_if<Diagnostic>(&key)) {
                return {trigger, entry.frame_input, frame, {}, RunRejected{std::move(*error)}};
            }
            correlation = std::get<CorrelationId>(std::move(key));
        }
        {
            const std::lock_guard lock(mutex);
            if (!accepting) { return {trigger, entry.frame_input, frame, correlation, stopped()}; }
            if ((kind == TriggerKind::External) != (entry.mode == TriggerMode::External)) {
                return {trigger, entry.frame_input, frame, correlation,
                    RunRejected{failure(ErrorCode::InvalidRunRequest, workflow, "Event kind does not match the trigger entry.")}};
            }
            ++preparing;
        }
        struct Finish {
            Impl& self;
            ~Finish() {
                const std::lock_guard lock(self.mutex);
                if (--self.preparing == 0) { self.idle.notify_all(); }
            }
        } finish{*this};
        return {trigger, entry.frame_input, frame, correlation, submit(trigger, frame, correlation)};
    }
};

TriggerControl::TriggerControl(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TriggerControl::~TriggerControl() = default;

Result<std::shared_ptr<TriggerControl>> TriggerControl::start(std::shared_ptr<WorkflowSession> session,
    WorkflowId workflow, Clock::time_point now)
{
    if (!session) { return failure(ErrorCode::InvalidState, workflow, "Starting a trigger requires a session."); }
    const auto current = session->inspect();
    if (current.status != WorkflowSessionStatus::Active || !current.active) {
        return failure(ErrorCode::InvalidState, workflow, "Starting a trigger requires an active solution.");
    }
    const auto* flow = find_workflow(current.active->solution(), workflow);
    if (!flow || !flow->enabled || !flow->trigger) {
        return failure(ErrorCode::InvalidRunRequest, workflow, "Starting requires an enabled workflow with a trigger entry.");
    }
    auto validated = validate_workflow_trigger(*flow);
    if (auto* error = std::get_if<Diagnostic>(&validated)) { return std::move(*error); }
    auto domain = SchemaIdFactory::parse<CorrelationDomainId>(flow->trigger->source.text());
    if (auto* error = std::get_if<Diagnostic>(&domain)) { return std::move(*error); }
    return std::shared_ptr<TriggerControl>(new TriggerControl(std::make_unique<Impl>(session,
        current.active->identity().session, workflow, *flow->trigger, std::get<CorrelationDomainId>(domain), now)));
}

TriggerReceipt TriggerControl::external(std::int64_t frame, std::optional<CorrelationId> correlation)
{ return impl_->dispatch(TriggerKind::External, frame, std::move(correlation)); }

std::optional<TriggerReceipt> TriggerControl::tick(Clock::time_point now)
{
    std::int64_t frame;
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting || impl_->entry.mode != TriggerMode::Continuous || !impl_->due || now < *impl_->due) {
            return {};
        }
        frame = ++impl_->last_frame;
        impl_->due = frame == std::numeric_limits<std::int64_t>::max() ? std::nullopt : next_tick(now, impl_->entry.interval_ms);
    }
    return impl_->dispatch(TriggerKind::Continuous, frame, {});
}

void TriggerControl::disarm()
{
    const std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
}

Result<RunWaitOutcome> TriggerControl::stop(TriggerStopMode mode, Clock::time_point deadline)
{
    if (mode != TriggerStopMode::Drain && mode != TriggerStopMode::Cancel) {
        return failure(ErrorCode::InvalidEnumValue, impl_->workflow, "Unknown trigger stop mode.");
    }
    std::vector<RunHandle> runs;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->accepting = false;
        runs = impl_->accepted;
    }
    if (mode == TriggerStopMode::Cancel) {
        // Request cancellation of later FIFO entries before an active run can release its slot.
        std::optional<Diagnostic> first_error;
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            auto cancelled = it->cancel();
            if (auto* error = std::get_if<Diagnostic>(&cancelled); error && !first_error) { first_error = std::move(*error); }
        }
        if (first_error) { return std::move(*first_error); }
    }
    for (const auto& run : runs) {
        auto waited = run.wait(RunWaitTarget::Quiescence, deadline);
        if (auto* error = std::get_if<Diagnostic>(&waited)) { return std::move(*error); }
        if (std::get<RunWaitResult>(waited).outcome == RunWaitOutcome::DeadlineExpired) { return RunWaitOutcome::DeadlineExpired; }
    }
    std::unique_lock lock(impl_->mutex);
    impl_->accepted.clear(); // Every admitted handle reached physical exit.
    if (!impl_->idle.wait_until(lock, deadline, [&] { return impl_->preparing == 0; })) { return RunWaitOutcome::DeadlineExpired; }
    return RunWaitOutcome::Satisfied;
}

} // namespace visionpro::workflow
