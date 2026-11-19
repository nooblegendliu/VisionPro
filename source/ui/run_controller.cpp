#include "run_controller.hpp"

#ifdef VISIONPRO_WITH_OPENCV
#include "visual_preview.hpp"
#endif

#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <exception>
#include <type_traits>
#include <utility>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
using namespace infrastructure;

Diagnostic issue(ErrorCode code, std::string message) { return {code, Severity::Error, {}, std::move(message)}; }

Diagnostic request_issue(Diagnostic failure, const Solution& draft, const RunTarget& target)
{
    failure.location.solution = draft.id;
    std::visit([&](const auto& concrete) {
        failure.location.workflow = concrete.workflow;
        if constexpr (std::is_same_v<std::decay_t<decltype(concrete)>, NodeTarget>) {
            failure.location.node = concrete.node;
        }
    }, target);
    return failure;
}

Result<std::monostate> read_inputs(RunArguments& arguments, const std::vector<RunImageInput>& images)
{
#ifdef VISIONPRO_WITH_OPENCV
    for (const auto& input : images) {
        auto image = app::read_preview_image(input.file, input.frame, input.grayscale);
        if (auto* failure = std::get_if<Diagnostic>(&image)) {
            failure->location.input = input.id;
            return std::move(*failure);
        }
        auto value = DataValue::image({std::get<ImageHandle>(std::move(image)), {}, {}});
        if (auto* failure = std::get_if<Diagnostic>(&value)) {
            failure->location.input = input.id;
            return std::move(*failure);
        }
        arguments.inputs.push_back({input.id, std::get<DataValue>(std::move(value))});
    }
#else
    if (!images.empty()) {
        auto failure = issue(ErrorCode::MissingCapability, "File image input requires OpenCV.");
        failure.location.input = images.front().id;
        return failure;
    }
    Q_UNUSED(arguments);
#endif
    return std::monostate{};
}

struct Admission {
    RunSubmission submission;
    std::optional<RunObservation> observation{};
};

Admission prepare_run(WorkflowSession& session, const Solution& draft, const RunDocument& document,
    RunArguments arguments, const std::vector<RunImageInput>& images, std::size_t subscribe_capacity)
{
    if (draft.format_version != solution_format_version || (document.source && !document.source->can_save())) {
        return {RunRejected{request_issue(issue(ErrorCode::UnsupportedSolutionContent,
            "This solution contains content that cannot be submitted from the editor."), draft, arguments.target)}};
    }
    if (auto checked = read_inputs(arguments, images); auto* failure = std::get_if<Diagnostic>(&checked)) {
        return {RunRejected{request_issue(std::move(*failure), draft, arguments.target)}};
    }
    auto base = session.inspect().active;
    if (!base) {
        // Establish the solution identity. Publication validates the requested
        // scope so an unrelated invalid workflow does not block a debug call.
        const Solution baseline{draft.id, draft.name, draft.format_version, draft.revision, {}, {}, {}, {}, {}, {}};
        auto activated = session.activate(baseline);
        if (auto* failure = std::get_if<Diagnostic>(&activated)) { return {RunRejected{std::move(*failure)}}; }
        base = std::get<ConfigSnapshot>(std::move(activated));
    }
    std::optional<RunObservation> observation;
    if (subscribe_capacity != 0) {
        auto subscribed = session.observe(subscribe_capacity);
        if (auto* observer = std::get_if<RunObservation>(&subscribed)) { observation.emplace(std::move(*observer)); }
    }
    if (document.source) {
        return {publish_solution_run(session, *base, *document.source, draft, arguments), std::move(observation)};
    }
    auto provider = FileResourceProvider::create(draft, document.directory, document.roots);
    if (auto* failure = std::get_if<Diagnostic>(&provider)) {
        return {RunRejected{std::move(*failure)}, std::move(observation)};
    }
    return {session.publish_and_submit(*base, draft, arguments, std::get<std::shared_ptr<FileResourceProvider>>(std::move(provider))),
        std::move(observation)};
}
} // namespace

RunController::RunController(ToolRegistry registry, QObject* parent)
    : QObject(parent), registry_(std::move(registry))
{
    setObjectName(QStringLiteral("runController"));
    refresh_.setInterval(50);
    connect(&refresh_, &QTimer::timeout, this, &RunController::refresh_progress);
}

bool RunController::workflow_active(WorkflowId workflow) const noexcept
{
    return std::ranges::any_of(active_, [&](const auto& entry) { return entry.first.workflow == workflow; });
}

bool RunController::can_submit(const Workflow& flow) const noexcept
{
    // Reserve publications one at a time. Accepted work continues independently.
    return !preparing_ && !retiring_ && (flow.admission != WorkflowAdmission{} || !workflow_active(flow.id));
}

bool RunController::can_cancel(const RunHandle& run) const noexcept
{
    const auto found = active_.find(run.identity());
    return found != active_.end() && !found->second.cancelling
        && (found->second.status == ExecutionStatus::Pending || found->second.status == ExecutionStatus::Running);
}

bool RunController::ensure_session()
{
    if (!session_) {
        // Completed runs are owned by the window's configurable handle history.
        auto created = WorkflowSession::create(registry_, {}, {0});
        if (auto* failure = std::get_if<Diagnostic>(&created)) {
            emit operation_failed(*failure);
            return false;
        }
        // Normal closing drains the session explicitly. Direct destruction of
        // a window also releases the session away from the GUI thread.
        session_ = std::shared_ptr<WorkflowSession>(std::get<std::unique_ptr<WorkflowSession>>(std::move(created)).release(),
            [](WorkflowSession* session) {
                QThreadPool::globalInstance()->start([owner = std::unique_ptr<WorkflowSession>(session)] {});
            });
    }
    return true;
}

void RunController::start(Solution draft, RunDocument document, RunArguments arguments, std::vector<RunImageInput> images)
{
    if (preparing_ || retiring_ || !ensure_session()) { return; }
    auto outcome = std::make_shared<std::optional<Admission>>();
    const auto subscribe_capacity = observing_ && !observation_ ? observation_capacity_ : 0;
    const auto generation = observation_generation_;
    auto* worker = QThread::create([outcome, session = session_, draft = std::move(draft), document = std::move(document),
        arguments = std::move(arguments), images = std::move(images), subscribe_capacity]() mutable {
        try {
            outcome->emplace(prepare_run(*session, draft, document, std::move(arguments), images, subscribe_capacity));
        } catch (const std::exception& failure) {
            outcome->emplace(Admission{RunRejected{issue(ErrorCode::ExecutionBoundaryFailure, failure.what())}});
        } catch (...) {
            outcome->emplace(Admission{RunRejected{issue(ErrorCode::ExecutionBoundaryFailure, "Could not prepare the run.")}});
        }
    });
    connect(worker, &QThread::finished, this, [this, outcome, generation] {
        preparing_ = false;
        auto& admitted = outcome->value();
        if (retiring_) {
            admitted.observation.reset();
            close_session();
            return;
        }
        if (observing_ && generation == observation_generation_ && admitted.observation) {
            observation_ = std::move(admitted.observation);
        } else {
            subscribe();
        }
        record_submission(admitted.submission);
        emit changed();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    preparing_ = true;
    emit changed();
    worker->start();
}

void RunController::record_submission(const RunSubmission& submission, std::string description)
{
    if (const auto* handle = std::get_if<RunHandle>(&submission)) {
        active_.emplace(handle->identity(), ActiveRun{*handle});
        runs_.push_back(*handle);
        refresh_.start();
    } else if (const auto* rejected = std::get_if<RunRejected>(&submission)) {
        auto failure = rejected->diagnostic;
        failure.message = std::move(description) + failure.message;
        emit operation_failed(failure);
    } else {
        const auto& busy = std::get<RunBusy>(submission);
        auto failure = issue(ErrorCode::InvalidState, std::move(description) + "Workflow is busy (active run "
            + std::to_string(busy.active.run.value()) + ").");
        failure.location.workflow = busy.active.workflow;
        emit operation_failed(failure);
    }
}

void RunController::cancel(const RunHandle& run)
{
    if (!can_cancel(run)) { return; }
    auto cancelled = run.cancel();
    if (const auto* failure = std::get_if<Diagnostic>(&cancelled)) {
        emit operation_failed(*failure);
        return;
    }
    active_.at(run.identity()).cancelling = true;
    emit changed();
}

Result<std::optional<SessionSharedPreview>> RunController::shared_preview() const
{
    if (preparing_ || retiring_) { return issue(ErrorCode::InvalidState, "Shared data is changing sessions. Refresh after publication."); }
    const auto active = session_ ? session_->inspect().active : std::nullopt;
    if (!active) { return std::optional<SessionSharedPreview>{}; }
    auto view = session_->shared_preview();
    if (const auto* failure = std::get_if<Diagnostic>(&view)) { return *failure; }
    auto joins = session_->correlation_preview();
    if (const auto* failure = std::get_if<Diagnostic>(&joins)) { return *failure; }
    return std::optional{SessionSharedPreview{*active, std::get<std::shared_ptr<const SharedData>>(std::move(view)),
        std::get<std::shared_ptr<const CorrelationJoinAccess>>(std::move(joins))}};
}

bool RunController::can_reset_state(const NodeTarget& target) const
{
    if (!session_ || preparing_ || retiring_ || workflow_active(target.workflow)) { return false; }
    const auto active = session_->inspect().active;
    const auto* flow = active ? find_workflow(active->solution(), target.workflow) : nullptr;
    const auto* node = flow ? find_node(*flow, target.node) : nullptr;
    const auto* configuration = node ? std::get_if<ToolConfiguration>(&node->configuration) : nullptr;
    const auto* tool = configuration ? active->find_tool(configuration->type) : nullptr;
    if (!tool || !tool->state) { return false; }
    const auto resettable = [](StateScope scope, StateResetPolicy reset) {
        return scope == StateScope::SolutionSession && reset == StateResetPolicy::DomainStartAndExplicit;
    };
    const auto& policy = tool->state_parameters.policy;
    if (!policy) { return resettable(tool->state->scope, tool->state->reset); }
    const auto parameter = std::ranges::find(tool->parameters, policy->id, &ParameterDescriptor::id);
    const auto binding = std::ranges::find(node->bindings, BindingTarget{policy->id}, &Binding::target);
    const ConfigValue* value = nullptr;
    if (binding == node->bindings.end()) {
        if (parameter != tool->parameters.end() && parameter->default_value) { value = &*parameter->default_value; }
    } else if (binding->sources.size() == 1) { value = std::get_if<ConfigValue>(&binding->sources.front()); }
    if (!value || !value->as_enum()) { return false; }
    const auto choice = std::ranges::find(policy->choices, *value->as_enum(), &StatePolicyChoice::choice);
    return choice != policy->choices.end() && resettable(choice->scope, choice->reset);
}

Result<std::monostate> RunController::reset_state(const NodeTarget& target)
{
    if (!session_ || preparing_ || retiring_) { return issue(ErrorCode::InvalidState, "State reset requires an active session."); }
    return session_->reset_node_state(target);
}

void RunController::reset_document()
{
    if (retiring_) { return; }
    ++document_generation_;
    disarm_triggers();
    camera_status_.clear();
    unsubscribe();
    dropped_ = 0;
    refresh_.stop();
    active_.clear();
    runs_.clear();
    retiring_ = session_ || preparing_;
    if (retiring_ && !preparing_) { close_session(); }
    emit changed();
}

void RunController::close_session()
{
    if (!session_) {
        retiring_ = false;
        emit changed();
        return;
    }
    std::vector<std::shared_ptr<TriggerControl>> retired;
    std::vector<std::shared_ptr<CameraTriggerStream>> cameras;
    for (auto& entry : triggers_) {
        retired.push_back(std::move(entry.second.control));
        if (entry.second.camera) { cameras.push_back(std::move(entry.second.camera)); }
    }
    triggers_.clear(); // Timers are GUI-thread objects.
    auto failure = std::make_shared<std::optional<Diagnostic>>();
    auto* worker = QThread::create([session = std::exchange(session_, {}), retired = std::move(retired),
            cameras = std::move(cameras), failure] {
        for (const auto& camera : cameras) {
            auto released = camera->close();
            if (auto* diagnostic = std::get_if<Diagnostic>(&released)) { *failure = std::move(*diagnostic); }
        }
        auto closed = session->close();
        if (auto* diagnostic = std::get_if<Diagnostic>(&closed)) { *failure = std::move(*diagnostic); }
        // A disarmed event can still be returning from a resource preflight.
        // It owns no run, so closing the session alone cannot wait for it.
        for (const auto& source : retired) {
            auto stopped = source->stop(TriggerStopMode::Drain, std::chrono::steady_clock::time_point::max());
            if (auto* diagnostic = std::get_if<Diagnostic>(&stopped)) { *failure = std::move(*diagnostic); }
        }
    });
    connect(worker, &QThread::finished, this, [this, failure] {
        retiring_ = false;
        if (*failure) { emit operation_failed(**failure); }
        emit changed();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void RunController::set_observing(bool visible)
{
    if (observing_ == visible) { return; }
    observing_ = visible;
    if (visible) { subscribe(); }
    else { unsubscribe(); }
}

void RunController::set_limits(std::size_t history, std::size_t notifications)
{
    history_limit_ = history;
    trim_history();
    const auto capacity = std::max<std::size_t>(1, notifications);
    if (observation_capacity_ != capacity) {
        unsubscribe();
        observation_capacity_ = capacity;
        subscribe();
    }
    emit changed();
}

void RunController::subscribe()
{
    if (!observing_ || observation_ || !session_ || preparing_ || retiring_) { return; }
    auto observed = session_->observe(observation_capacity_);
    if (auto* observer = std::get_if<RunObservation>(&observed)) { observation_.emplace(std::move(*observer)); }
}

void RunController::unsubscribe()
{
    if (observation_) {
        const auto state = observation_->wait(std::chrono::steady_clock::now());
        dropped_ += state.dropped - subscription_dropped_;
    }
    observation_.reset();
    subscription_dropped_ = 0;
    ++observation_generation_;
}

void RunController::trim_history()
{
    const auto completed = runs_.size() - active_.size();
    if (completed > history_limit_) {
        auto excess = completed - history_limit_;
        std::erase_if(runs_, [&](const RunHandle& run) {
            if (!excess || active_.contains(run.identity())) { return false; }
            --excess;
            return true;
        });
    }
}

void RunController::refresh_progress()
{
    std::vector<Diagnostic> failures;
    for (auto it = active_.begin(); it != active_.end();) {
        auto& tracked = it->second;
        auto inspected = tracked.handle.inspect();
        if (const auto* progress = std::get_if<RunProgress>(&inspected)) {
            tracked.inspection_failed = false;
            tracked.status = progress->status;
            if (progress->quiescent) { it = active_.erase(it); continue; }
        } else if (!std::exchange(tracked.inspection_failed, true)) {
            failures.push_back(std::get<Diagnostic>(std::move(inspected)));
        }
        ++it;
    }
    trim_history();
    if (active_.empty()) { refresh_.stop(); }
    // A single refresh covers the buffered notifications. Inspecting the
    // admitted handles also recovers exact progress after overflow or
    // while the panel is detached.
    if (observation_) {
        for (std::size_t count = 0; count < observation_capacity_; ++count) {
            const auto next = observation_->wait(std::chrono::steady_clock::now());
            dropped_ += next.dropped - subscription_dropped_;
            subscription_dropped_ = next.dropped;
            if (next.closed || !next.event) { break; }
        }
    }
    for (const auto& failure : failures) { emit operation_failed(failure); }
    emit changed();
}

} // namespace visionpro::ui
