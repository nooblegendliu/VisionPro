#include "run_controller.hpp"
#include "background_result.hpp"
#include "image_source_fields.hpp"

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
using namespace infrastructure;
using Clock = std::chrono::steady_clock;

struct TriggerSource {
    std::shared_ptr<TriggerControl> control;
    std::shared_ptr<CameraTriggerStream> camera;
    std::shared_ptr<ImageDirectoryTrigger> directory;
};

constexpr std::uint32_t local_interval_ms = 100;

bool local_source(const Workflow& flow)
{
    return std::ranges::any_of(flow.nodes, [](const Node& node) {
        return node.enabled && is_image_source(&node) && source_enum(node, "source_type", "Local") == "Local";
    });
}

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
    auto resources = document.source ? FileResourceProvider::create(draft, document.source->directory(), document.source->resource_roots(), document.archives)
        : FileResourceProvider::create(draft, document.directory, document.roots, document.archives);
    if (auto* failure = std::get_if<Diagnostic>(&resources)) { return std::move(*failure); }
    auto provider = std::get<std::shared_ptr<FileResourceProvider>>(std::move(resources));
    auto base = session->inspect().active;
    auto published = base ? session->publish(*base, draft, std::move(provider)) : session->activate(draft, std::move(provider));
    if (auto* failure = std::get_if<Diagnostic>(&published)) { return std::move(*failure); }
    const auto* flow = find_workflow(draft, id);
    if (!flow) { return Diagnostic{ErrorCode::InvalidRunRequest, Severity::Error, {}, "Unknown trigger workflow."}; }
    TriggerSource source;
    if (ImageDirectoryTrigger::configured(*flow)) {
        auto directory = ImageDirectoryTrigger::open(session, id);
        if (auto* failure = std::get_if<Diagnostic>(&directory)) { return *failure; }
        source.directory = std::get<std::shared_ptr<ImageDirectoryTrigger>>(std::move(directory));
    }
    if (!flow->trigger && source.directory) { source.control = source.directory->control(); }
    else {
        auto control = flow->trigger ? TriggerControl::start(session, id)
            : TriggerControl::start_local(session, std::get<ConfigSnapshot>(published).identity(), id, local_interval_ms);
        if (auto* failure = std::get_if<Diagnostic>(&control)) { return *failure; }
        source.control = std::get<std::shared_ptr<TriggerControl>>(std::move(control));
    }
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
        if (source.directory_timer) { source.directory_timer->stop(); }
        if (source.directory) { source.directory->control()->disarm(); }
        if (source.camera) { source.camera->request_stop(); }
        // Revoke callbacks before exposing another document. The session
        // close worker drains any requests admitted before this point.
        source.control->disarm();
    }
}

bool RunController::can_arm_trigger(const Workflow& flow) const
{
    return !preparing_ && !retiring_ && flow.enabled && (flow.trigger || local_source(flow)) && !triggers_.contains(flow.id);
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
        && (!found->second.directory || found->second.control != found->second.directory->control())
        && !found->second.dispatching && !found->second.stopping;
}

QString RunController::trigger_status(WorkflowId id) const
{
    const auto found = triggers_.find(id);
    if (found == triggers_.end()) { return tr("未启动"); }
    return found->second.stopping ? tr("正在停止") : tr("已启动");
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
    document.archives = archives_;
    const auto configuration = flow->trigger;
    if (configuration && configuration->camera) { camera_status_[id] = QStringLiteral("Opening camera…"); }
    else { camera_status_.erase(id); }
    preparing_ = true;
    emit changed();
    const auto edit_generation = document.edit_generation;
    on_worker(this, [session = session_, draft = std::move(draft), document = std::move(document), id] {
        return arm(session, draft, document, id);
    }, [this, configuration, id, edit_generation](Result<TriggerSource> result) {
        preparing_ = false;
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            if (retiring_) { close_session(); return; }
            if (configuration && configuration->camera) { camera_status_[id] = QStringLiteral("Camera fault · %1").arg(QString::fromStdString(failure->message)); }
            emit operation_failed(*failure);
        } else {
            auto& source = triggers_[id];
            auto opened = std::get<TriggerSource>(std::move(result));
            source.control = std::move(opened.control);
            source.camera = std::move(opened.camera);
            source.directory = std::move(opened.directory);
            if (retiring_) { disarm_triggers(); close_session(); return; }
            if (const auto snapshot = session_->inspect().active) {
                emit snapshot_published(snapshot->identity(), edit_generation);
            }
            if (source.directory) {
                source.directory_timer = std::make_unique<QChronoTimer>(std::chrono::milliseconds{150});
                source.directory_timer->setSingleShot(true);
                connect(source.directory_timer.get(), &QChronoTimer::timeout, this, [this, id] { poll_directory(id); });
                source.directory_timer->start();
            }
            if (configuration ? configuration->mode == TriggerMode::Continuous : !source.directory) {
                source.timer = std::make_unique<QChronoTimer>(std::chrono::milliseconds{configuration ? configuration->interval_ms : local_interval_ms});
                source.timer->setTimerType(Qt::PreciseTimer);
                source.timer->setSingleShot(true);
                connect(source.timer.get(), &QChronoTimer::timeout, this, [this, id] { dispatch_trigger(id, {}); });
                source.timer->start();
            }
            subscribe();
            emit trigger_notice(configuration ? tr("已启动触发源 %1").arg(QString::fromUtf8(configuration->source.text()))
                : (source.directory ? tr("已启动订阅目录监听") : tr("已启动本地图像连续执行")));
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

void RunController::poll_directory(WorkflowId id)
{
    auto it = triggers_.find(id);
    if (it == triggers_.end() || it->second.stopping || !it->second.directory) { return; }
    const auto stream = it->second.directory;
    on_worker(this, [stream] { return stream->poll(); },
        [this, id, stream, generation = document_generation_](Result<std::optional<TriggerReceipt>> result) {
            if (generation != document_generation_) { return; }
            auto current = triggers_.find(id);
            if (const auto* event = std::get_if<std::optional<TriggerReceipt>>(&result); event && *event) {
                record_submission((*event)->submission, RunOrigin::Trigger, tr("订阅目录变更").toStdString());
                emit changed();
            }
            if (current == triggers_.end() || current->second.directory != stream || current->second.stopping) { return; }
            if (const auto* error = std::get_if<Diagnostic>(&result)) {
                stop_trigger(id, TriggerStopMode::Drain);
                emit operation_failed(*error);
            } else { current->second.directory_timer->start(); }
        });
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
                record_submission((*receipt)->submission, RunOrigin::Trigger);
                emit changed();
            }
            return;
        }
        auto& armed = current->second;
        armed.dispatching = false;
        if (armed.timer && !armed.stopping) {
            if (control->accepting()) { armed.timer->start(); }
            else { stop_trigger(id, TriggerStopMode::Drain); }
        }
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
            record_submission(receipt->submission, RunOrigin::Trigger, (description + QStringLiteral(" · ")).toStdString());
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
    if (source.directory_timer) { source.directory_timer->stop(); }
    const auto directory = source.directory;
    if (directory) { directory->control()->disarm(); }
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
    on_worker(this, [control, mode, camera, directory, archives=archives_]() -> Result<RunWaitOutcome> {
        const auto closed = camera ? camera->close() : Result<std::monostate>{std::monostate{}};
        auto stopped = control->stop(mode, Clock::time_point::max());
        if (directory && directory->control() != control) {
            auto done = directory->control()->stop(mode, Clock::time_point::max());
            if (const auto* failure = std::get_if<Diagnostic>(&done)) { return *failure; }
        }
        if (const auto* failure = std::get_if<Diagnostic>(&closed)) { return *failure; }
        if (archives && !std::holds_alternative<Diagnostic>(stopped)) {
            auto flushed = archives->flush();
            if (const auto* error = std::get_if<Diagnostic>(&flushed)) { return *error; }
        }
        return stopped;
    }, [this, control, id, generation = document_generation_](Result<RunWaitOutcome> result) {
        if (generation != document_generation_) { return; }
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
