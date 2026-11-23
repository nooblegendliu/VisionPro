#include "run_controller.hpp"
#include "background_result.hpp"

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
using namespace infrastructure;
using Clock = std::chrono::steady_clock;

struct TriggerSource {
    std::shared_ptr<TriggerControl> control;
    std::shared_ptr<CameraTriggerStream> camera;
};

QString camera_summary(const QString& state, const CameraTriggerStream& stream, const QString& last_frame = {})
{
    const auto stats = stream.statistics();
    return QStringLiteral("Camera %1 · received %2 · accepted %3 · rejected %4 · missing %5 · duplicates %6")
        .arg(state).arg(stats.received).arg(stats.accepted).arg(stats.rejected).arg(stats.missing).arg(stats.duplicates) + last_frame;
}

Result<TriggerSource> arm(std::shared_ptr<WorkflowSession> session, const Solution& draft,
    const RunDocument& document, WorkflowId id)
{
    if (!supports_solution_format(draft.format_version) || (document.source && !document.source->can_save())) {
        return Diagnostic{ErrorCode::UnsupportedSolutionContent, Severity::Error, {}, "This solution cannot be armed for triggers."};
    }
    if (document.source && draft.id != document.source->solution().id) {
        return Diagnostic{ErrorCode::ResultIdentityMismatch, Severity::Error, {}, "The trigger draft and source document differ."};
    }
    auto resources = document.source ? FileResourceProvider::create(draft, document.source->directory(), document.source->resource_roots())
        : FileResourceProvider::create(draft, document.directory, document.roots);
    if (auto* failure = std::get_if<Diagnostic>(&resources)) { return std::move(*failure); }
    auto provider = std::get<std::shared_ptr<FileResourceProvider>>(std::move(resources));
    auto base = session->inspect().active;
    auto published = base ? session->publish(*base, draft, std::move(provider)) : session->activate(draft, std::move(provider));
    if (auto* failure = std::get_if<Diagnostic>(&published)) { return std::move(*failure); }
    auto control = TriggerControl::start(std::move(session), id);
    if (auto* failure = std::get_if<Diagnostic>(&control)) { return std::move(*failure); }
    TriggerSource source{std::get<std::shared_ptr<TriggerControl>>(std::move(control)), {}};
    const auto* flow = find_workflow(draft, id);
    if (flow && flow->trigger && flow->trigger->camera) {
        auto camera = CameraTriggerStream::open(draft, id, source.control);
        if (auto* failure = std::get_if<Diagnostic>(&camera)) { return std::move(*failure); }
        source.camera = std::get<std::shared_ptr<CameraTriggerStream>>(std::move(camera));
    }
    return source;
}
} // namespace

RunController::~RunController()
{
    if (solution_stop_) { solution_stop_->request_stop(); }
    disarm_triggers();
    for (auto& [id, source] : triggers_) {
        if (source.camera) {
            QThreadPool::globalInstance()->start([camera = std::move(source.camera)] { (void)camera->close(); });
        }
    }
}

void RunController::disarm_triggers()
{
    for (auto& item : triggers_) {
        auto& source = item.second;
        source.stopping = true;
        if (source.timer) { source.timer->stop(); }
        if (source.camera) { source.camera->request_stop(); }
        // Revoke callbacks before exposing another document. The session
        // close worker drains any requests admitted before this point.
        source.control->disarm();
    }
}

bool RunController::can_arm_trigger(const Workflow& flow) const
{
    return !preparing_ && !retiring_ && flow.enabled && flow.trigger && !triggers_.contains(flow.id);
}

bool RunController::can_stop_trigger(WorkflowId id) const
{
    auto found = triggers_.find(id);
    return found != triggers_.end() && !found->second.stopping;
}

bool RunController::can_send_frame(WorkflowId id) const
{
    auto found = triggers_.find(id);
    return found != triggers_.end() && !found->second.camera && !found->second.timer
        && !found->second.dispatching && !found->second.stopping;
}

QString RunController::camera_status(WorkflowId id) const
{
    const auto status = camera_status_.find(id);
    return status == camera_status_.end() ? QString{} : status->second;
}

void RunController::arm_trigger(Solution draft, RunDocument document, WorkflowId id)
{
    const auto* flow = find_workflow(draft, id);
    if (!flow || !can_arm_trigger(*flow) || !ensure_session()) { return; }
    const auto configuration = *flow->trigger;
    if (configuration.camera) { camera_status_[id] = QStringLiteral("Opening camera…"); }
    else { camera_status_.erase(id); }
    preparing_ = true;
    emit changed();
    on_worker(this, [session = session_, draft = std::move(draft), document = std::move(document), id] {
        return arm(session, draft, document, id);
    }, [this, configuration, id](Result<TriggerSource> result) {
        preparing_ = false;
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            if (retiring_) { close_session(); return; }
            if (configuration.camera) { camera_status_[id] = QStringLiteral("Camera fault · %1").arg(QString::fromStdString(failure->message)); }
            emit operation_failed(*failure);
        } else {
            auto& source = triggers_[id];
            auto opened = std::get<TriggerSource>(std::move(result));
            source.control = std::move(opened.control);
            source.camera = std::move(opened.camera);
            if (retiring_) { disarm_triggers(); close_session(); return; }
            if (configuration.mode == TriggerMode::Continuous) {
                source.timer = std::make_unique<QChronoTimer>(std::chrono::milliseconds{configuration.interval_ms});
                source.timer->setTimerType(Qt::PreciseTimer);
                source.timer->setSingleShot(true);
                connect(source.timer.get(), &QChronoTimer::timeout, this, [this, id] { dispatch_trigger(id, {}); });
                source.timer->start();
            }
            subscribe();
            emit trigger_notice(QStringLiteral("Armed trigger %1.").arg(QString::fromUtf8(configuration.source.text())));
            if (source.camera) {
                camera_status_[id] = camera_summary(QStringLiteral("armed"), *source.camera);
                dispatch_trigger(id, {});
            }
        }
        emit changed();
    });
}

void RunController::send_frame(WorkflowId id, std::int64_t frame)
{
    if (can_send_frame(id)) { dispatch_trigger(id, frame); }
}

void RunController::dispatch_trigger(WorkflowId id, std::optional<std::int64_t> frame)
{
    auto it = triggers_.find(id);
    if (it == triggers_.end() || it->second.dispatching || it->second.stopping) { return; }
    auto& source = it->second;
    source.dispatching = true;
    const auto control = source.control;
    const auto camera = source.camera;
    const auto generation = document_generation_;
    const auto now = Clock::now();
    if (!camera) { emit changed(); }
    on_worker(this, [control, camera, frame, now]() -> Result<std::optional<TriggerReceipt>> {
        if (camera) {
            auto receipt = camera->next();
            if (auto* failure = std::get_if<Diagnostic>(&receipt)) { return std::move(*failure); }
            return std::optional{std::get<TriggerReceipt>(std::move(receipt))};
        }
        if (frame) { return std::optional{control->external(*frame)}; }
        return control->tick(now);
    }, [this, generation, id, control](Result<std::optional<TriggerReceipt>> result) {
        if (generation != document_generation_) { return; }
        auto current = triggers_.find(id);
        if (current == triggers_.end() || current->second.control != control) {
            // Preserve an admitted run even if the user has since disarmed
            // this entry. Its callback cannot change the new source's status.
            const auto* receipt = std::get_if<std::optional<TriggerReceipt>>(&result);
            if (receipt && *receipt && std::holds_alternative<RunHandle>((*receipt)->submission)) {
                record_submission((*receipt)->submission);
                emit changed();
            }
            return;
        }
        auto& armed = current->second;
        armed.dispatching = false;
        if (armed.timer && !armed.stopping) { armed.timer->start(); }
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            if (armed.stopping) { return; }
            if (armed.camera) {
                armed.camera_fault = QString::fromStdString(failure->message);
                stop_trigger(id, TriggerStopMode::Drain);
            }
            emit operation_failed(*failure);
        } else if (const auto& receipt = std::get<std::optional<TriggerReceipt>>(result)) {
            const auto* handle = std::get_if<RunHandle>(&receipt->submission);
            if (const auto& camera = receipt->camera) {
                const auto number = [](auto value) { return value ? QString::number(*value) : QStringLiteral("unknown"); };
                armed.camera_last_frame = QStringLiteral("\nFrame %1 · device frame %2 · host capture %3 µs · timestamp %4 · device ticks %5 at %6 Hz")
                    .arg(receipt->frame).arg(number(camera->device_frame_number)).arg(camera->capture_duration.count())
                    .arg(QString::fromStdString(camera->timestamp_source), number(camera->device_ticks), number(camera->tick_frequency_hz));
            }
            auto description = QStringLiteral("%1 · %2 = %3").arg(QString::fromUtf8(receipt->trigger.source.text()),
                QString::fromUtf8(receipt->frame_input.text())).arg(receipt->frame);
            if (const auto& key = receipt->correlation) {
                description += QStringLiteral(" · %1 / %2").arg(QString::fromUtf8(key->domain().text()), QString::fromUtf8(key->key()));
            }
            record_submission(receipt->submission, (description + QStringLiteral(" · ")).toStdString());
            if (handle) {
                emit trigger_notice(description + (handle->admission() == RunAdmission::Queued
                    ? QStringLiteral(" · Queued") : QStringLiteral(" · Accepted")));
            }
        }
        current = triggers_.find(id);
        if (current != triggers_.end() && current->second.control == control && current->second.camera && !current->second.stopping) {
            camera_status_[id] = camera_summary(QStringLiteral("armed"), *current->second.camera, current->second.camera_last_frame);
            dispatch_trigger(id, {});
        }
        emit changed();
    });
}

void RunController::stop_trigger(WorkflowId id, TriggerStopMode mode)
{
    if (!can_stop_trigger(id)) { return; }
    auto& source = triggers_.at(id);
    source.stopping = true;
    if (source.timer) { source.timer->stop(); }
    const auto control = source.control;
    const auto camera = source.camera;
    if (camera) {
        camera->request_stop();
        camera_status_[id] = camera_summary(source.camera_fault.isEmpty() ? QStringLiteral("stopping")
            : QStringLiteral("fault · %1").arg(source.camera_fault), *camera, source.camera_last_frame);
    }
    // Seal admission synchronously, without waiting for preflight or cleanup.
    control->disarm();
    emit changed();
    on_worker(this, [control, mode, camera]() -> Result<RunWaitOutcome> {
        const auto closed = camera ? camera->close() : Result<std::monostate>{std::monostate{}};
        auto stopped = control->stop(mode, Clock::time_point::max());
        if (const auto* failure = std::get_if<Diagnostic>(&closed)) { return *failure; }
        return stopped;
    }, [this, control, id](Result<RunWaitOutcome> result) {
        const auto it = triggers_.find(id);
        if (it == triggers_.end() || it->second.control != control) { return; }
        if (it->second.camera) {
            auto state = QStringLiteral("stopped");
            if (!it->second.camera_fault.isEmpty()) { state += QStringLiteral(" · fault · ") + it->second.camera_fault; }
            if (const auto* failure = std::get_if<Diagnostic>(&result)) {
                state += QStringLiteral(" · stop fault · ") + QString::fromStdString(failure->message);
            }
            camera_status_[id] = camera_summary(state, *it->second.camera, it->second.camera_last_frame);
        }
        triggers_.erase(it);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) { emit operation_failed(*failure); }
        else { emit trigger_notice(QStringLiteral("Trigger stopped. Accepted runs have finished cleanup.")); }
        emit changed();
    });
}

} // namespace visionpro::ui
