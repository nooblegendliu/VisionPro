#pragma once

#include <visionpro/core/result.hpp>

#include <QFutureWatcher>
#include <QPromise>
#include <QThreadPool>

#include <exception>
#include <type_traits>
#include <utility>

namespace visionpro::ui {

// The worker owns the task and its values. Delivery ends with the GUI owner.
template<class Task, class Finish>
void on_worker(QObject* owner, Task task, Finish finish)
{
    using Outcome = std::invoke_result_t<Task>;
    auto* watcher = new QFutureWatcher<Outcome>(owner);
    QObject::connect(watcher, &QFutureWatcher<Outcome>::finished, owner, [watcher, finish = std::move(finish)] {
        auto result = watcher->future().takeResult();
        watcher->deleteLater();
        finish(std::move(result));
    });
    QPromise<Outcome> completion;
    watcher->setFuture(completion.future());
    QThreadPool::globalInstance()->start([task = std::move(task), completion = std::move(completion)]() mutable {
        completion.start();
        try { completion.addResult(task()); }
        catch (const std::exception& error) {
            completion.addResult(Outcome{core::Diagnostic{core::ErrorCode::ExecutionBoundaryFailure, core::Severity::Error, {}, error.what()}});
        } catch (...) {
            completion.addResult(Outcome{core::Diagnostic{core::ErrorCode::ExecutionBoundaryFailure, core::Severity::Error, {},
                "The background operation failed."}});
        }
        completion.finish();
    });
}

} // namespace visionpro::ui
