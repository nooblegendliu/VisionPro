#include "run_controller.hpp"

#include <QThread>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
using namespace infrastructure;
using Clock = std::chrono::steady_clock;

// Resource reads, submission preflight and physical-exit waits can block.
// Deliver their results only while the owning UI controller still exists.
template<class Task, class Finish>
void on_worker(QObject* owner, Task task, Finish finish)
{
    using Outcome = std::invoke_result_t<Task>;
    auto result = std::make_shared<std::optional<Outcome>>();
    auto* worker = QThread::create([task = std::move(task), result]() mutable {
        try { result->emplace(task()); }
        catch (const std::exception& error) {
            result->emplace(Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, error.what()});
        } catch (...) {
            result->emplace(Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Workflow trigger operation failed."});
        }
    });
    QObject::connect(worker, &QThread::finished, owner, [result, finish = std::move(finish)] { finish(std::move(**result)); });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

Result<std::shared_ptr<TriggerControl>> arm(std::shared_ptr<WorkflowSession> session, const Solution& draft,
    const RunDocument& document, WorkflowId id)
{
    if (draft.format_version != solution_format_version || (document.source && !document.source->can_save())) {
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
    return TriggerControl::start(std::move(session), id);
}
} // namespace

RunController::~RunController() { disarm_triggers(); }

void RunController::disarm_triggers()
{
    for (auto& item : triggers_) {
        auto& source = item.second;
        source.stopping = true;
        if (source.timer) { source.timer->stop(); }
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
    return found != triggers_.end() && !found->second.timer && !found->second.dispatching && !found->second.stopping;
}

void RunController::arm_trigger(Solution draft, RunDocument document, WorkflowId id)
{
    const auto* flow = find_workflow(draft, id);
    if (!flow || !can_arm_trigger(*flow) || !ensure_session()) { return; }
    const auto configuration = *flow->trigger;
    preparing_ = true;
    emit changed();
    on_worker(this, [session = session_, draft = std::move(draft), document = std::move(document), id] {
        return arm(session, draft, document, id);
    }, [this, configuration, id](Result<std::shared_ptr<TriggerControl>> result) {
        preparing_ = false;
        if (retiring_) { close_session(); return; }
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            emit operation_failed(*failure);
        } else {
            auto& source = triggers_[id];
            source.control = std::get<std::shared_ptr<TriggerControl>>(std::move(result));
            if (configuration.mode == TriggerMode::Continuous) {
                source.timer = std::make_unique<QChronoTimer>(std::chrono::milliseconds{configuration.interval_ms});
                source.timer->setTimerType(Qt::PreciseTimer);
                source.timer->setSingleShot(true);
                connect(source.timer.get(), &QChronoTimer::timeout, this, [this, id] { dispatch_trigger(id, {}); });
                source.timer->start();
            }
            subscribe();
            emit trigger_notice(QStringLiteral("Armed trigger %1.").arg(QString::fromUtf8(configuration.source.text())));
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
    const auto generation = document_generation_;
    const auto now = Clock::now();
    emit changed();
    on_worker(this, [control, frame, now]() -> Result<std::optional<TriggerReceipt>> {
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
            emit operation_failed(*failure);
        } else if (const auto& receipt = std::get<std::optional<TriggerReceipt>>(result)) {
            const auto* handle = std::get_if<RunHandle>(&receipt->submission);
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
    // Seal admission synchronously, without waiting for preflight or cleanup.
    control->disarm();
    emit changed();
    on_worker(this, [control, mode] { return control->stop(mode, Clock::time_point::max()); },
        [this, control, id](Result<RunWaitOutcome> result) {
            const auto it = triggers_.find(id);
            if (it == triggers_.end() || it->second.control != control) { return; }
            triggers_.erase(it);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) { emit operation_failed(*failure); }
            else { emit trigger_notice(QStringLiteral("Trigger stopped. Accepted runs have finished cleanup.")); }
            emit changed();
        });
}

} // namespace visionpro::ui
