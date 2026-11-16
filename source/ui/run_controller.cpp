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

bool RunController::can_cancel() const noexcept
{
    return active_ && !cancelling_
        && (active_status_ == ExecutionStatus::Pending || active_status_ == ExecutionStatus::Running);
}

void RunController::start(Solution draft, RunDocument document, RunArguments arguments, std::vector<RunImageInput> images)
{
    if (!can_start()) { return; }
    runs_.reserve(runs_.size() + 1);
    if (!session_) {
        // Completed runs are owned by the window's configurable handle history.
        auto created = WorkflowSession::create(registry_, {}, {0});
        if (auto* failure = std::get_if<Diagnostic>(&created)) {
            emit operation_failed(*failure);
            return;
        }
        // Normal closing drains the session explicitly. Direct destruction of
        // a window also releases the session away from the GUI thread.
        session_ = std::shared_ptr<WorkflowSession>(std::get<std::unique_ptr<WorkflowSession>>(std::move(created)).release(),
            [](WorkflowSession* session) {
                QThreadPool::globalInstance()->start([owner = std::unique_ptr<WorkflowSession>(session)] {});
            });
    }
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
        if (const auto* handle = std::get_if<RunHandle>(&admitted.submission)) {
            active_ = *handle;
            runs_.push_back(*handle);
            active_status_ = ExecutionStatus::Pending;
            cancelling_ = false;
            inspection_failed_ = false;
            refresh_.start();
        } else if (const auto* rejected = std::get_if<RunRejected>(&admitted.submission)) {
            emit operation_failed(rejected->diagnostic);
        } else {
            emit operation_failed(issue(ErrorCode::InvalidState, "A run is already active in this session."));
        }
        emit changed();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    preparing_ = true;
    emit changed();
    worker->start();
}

void RunController::cancel()
{
    if (!can_cancel()) { return; }
    auto cancelled = active_->cancel();
    if (const auto* failure = std::get_if<Diagnostic>(&cancelled)) {
        emit operation_failed(*failure);
        return;
    }
    cancelling_ = true;
    emit changed();
}

void RunController::reset_document()
{
    if (retiring_) { return; }
    unsubscribe();
    dropped_ = 0;
    refresh_.stop();
    active_.reset();
    runs_.clear();
    cancelling_ = false;
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
    auto failure = std::make_shared<std::optional<Diagnostic>>();
    auto* worker = QThread::create([session = std::exchange(session_, {}), failure] {
        auto closed = session->close();
        if (auto* diagnostic = std::get_if<Diagnostic>(&closed)) { *failure = std::move(*diagnostic); }
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
    const auto completed = runs_.size() - (active_ ? 1 : 0);
    if (completed > history_limit_) {
        runs_.erase(runs_.begin(), runs_.begin() + static_cast<std::ptrdiff_t>(completed - history_limit_));
    }
}

void RunController::refresh_progress()
{
    if (!active_) { return; }
    auto inspected = active_->inspect();
    if (const auto* progress = std::get_if<RunProgress>(&inspected)) {
        inspection_failed_ = false;
        active_status_ = progress->status;
        if (progress->quiescent) {
            active_.reset();
            cancelling_ = false;
            trim_history();
            refresh_.stop();
        }
    } else if (!inspection_failed_) {
        inspection_failed_ = true;
        emit operation_failed(std::get<Diagnostic>(inspected));
    }
    // A single refresh covers the buffered notifications. Inspecting the
    // admitted handle above also recovers exact progress after overflow or
    // while the panel is detached.
    if (observation_) {
        for (std::size_t count = 0; count < observation_capacity_; ++count) {
            const auto next = observation_->wait(std::chrono::steady_clock::now());
            dropped_ += next.dropped - subscription_dropped_;
            subscription_dropped_ = next.dropped;
            if (next.closed || !next.event) { break; }
        }
    }
    emit changed();
}

} // namespace visionpro::ui
