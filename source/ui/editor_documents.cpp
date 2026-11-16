#include "editor_window.hpp"

#include "control_scene.hpp"

#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUndoStack>

#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <exception>
#include <iterator>
#include <type_traits>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
namespace fs = std::filesystem;

QString path_text(const fs::path& path) { return QString::fromStdWString(path.native()); }
fs::path file_path(const QString& text) { return fs::path{text.toStdWString()}; }

Diagnostic file_error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

std::vector<QString> file_issues(const SolutionDocument& source)
{
    std::vector<QString> issues;
    if (!source.can_save()) {
        issues.push_back(QStringLiteral("This solution contains unsupported content and is open read-only."));
    }
    for (const auto& diagnostic : source.diagnostics()) {
        if (diagnostic.location.resource || diagnostic.code == ErrorCode::UnsupportedSolutionContent) {
            issues.push_back(QString::fromStdString(diagnostic.message));
        }
    }
    return issues;
}

Result<SolutionDocument> current_baseline(std::optional<SolutionDocument> source, const fs::path& directory,
    const std::optional<Solution>& expected, const ToolRegistry& registry, std::span<const NamedResourceRoot> roots)
{
    if (source) { return std::move(*source); }
    if (expected) { return reload_saved_solution(directory, *expected, registry, roots); }
    return file_error(ErrorCode::SolutionSaveConflict, "The document has no confirmed save baseline. Open the saved solution again.");
}
} // namespace

struct EditorWindow::CommittedDocument {
    explicit CommittedDocument(SolutionDocument confirmed)
        : saved(confirmed.solution()), directory(confirmed.directory()), source(std::move(confirmed)),
          notices(file_issues(*source)) {}

    CommittedDocument(Solution saved_model, fs::path destination, const ToolRegistry& registry,
        std::span<const NamedResourceRoot> roots, std::vector<QString> messages)
        : saved(std::move(saved_model)), directory(std::move(destination)), notices(std::move(messages))
    {
        auto loaded = reload_saved_solution(directory, saved, registry, roots);
        if (const auto* issue = std::get_if<Diagnostic>(&loaded)) {
            notices.push_back(QStringLiteral("Saved, but the save baseline could not be refreshed: %1")
                .arg(QString::fromStdString(issue->message)));
        } else {
            source = std::get<SolutionDocument>(std::move(loaded));
            directory = source->directory();
            auto issues = file_issues(*source);
            notices.insert(notices.end(), std::make_move_iterator(issues.begin()), std::make_move_iterator(issues.end()));
        }
    }

    Solution saved;
    fs::path directory;
    std::optional<SolutionDocument> source;
    std::vector<QString> notices;
};

template<class Task, class Apply>
void EditorWindow::start_file_task(const QString& status, Task task, Apply apply)
{
    if (busy_) { return; }
    using Outcome = std::invoke_result_t<Task&>;
    auto outcome = std::make_shared<std::optional<Outcome>>();
    auto* worker = QThread::create([outcome, task = std::move(task)]() mutable {
        try { outcome->emplace(task()); }
        catch (const std::exception& failure) {
            outcome->emplace(file_error(ErrorCode::SolutionIoFailure, failure.what()));
        } catch (...) {
            outcome->emplace(file_error(ErrorCode::SolutionIoFailure, "The file operation failed."));
        }
    });
    connect(worker, &QThread::finished, this, [this, outcome, apply = std::move(apply)]() mutable {
        busy_ = false;
        refresh_actions();
        try {
            if (auto* issue = std::get_if<Diagnostic>(&outcome->value())) {
                report_error(QString::fromStdString(issue->message));
            } else {
                apply(std::get<0>(std::move(outcome->value())));
            }
        } catch (const std::exception& failure) {
            report_error(QString::fromUtf8(failure.what()));
        } catch (...) {
            report_error(QStringLiteral("Could not update the editor after the file operation."));
        }
        refresh_actions();
        if (close_when_idle_ && !busy_) {
            close_when_idle_ = false;
            QTimer::singleShot(0, this, [this] { close(); });
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    busy_ = true;
    refresh_actions();
    show_status(status);
    worker->start();
}

void EditorWindow::choose_open()
{
    const auto selected = QFileDialog::getOpenFileName(this, QStringLiteral("Open Solution"),
        directory_ ? path_text(*directory_) : QString{}, QStringLiteral("VisionPro solution (solution.json)"));
    if (selected.isEmpty()) { return; }
    const QFileInfo file(selected);
    if (file.fileName().compare(QStringLiteral("solution.json"), Qt::CaseInsensitive) != 0) {
        report_error(QStringLiteral("Select the solution.json manifest for the solution."));
        return;
    }
    const auto directory = file_path(file.absolutePath());
    confirm_replacement([this, directory] { open_document(directory); });
}

void EditorWindow::open_document(const fs::path& directory)
{
    start_file_task(QStringLiteral("Opening solution…"),
        [directory, registry = registry_, roots = roots_] { return load_solution(directory, registry, roots); },
        [this](SolutionDocument source) { accept_opened(std::move(source)); });
}

void EditorWindow::accept_opened(SolutionDocument source)
{
    auto notices = file_issues(source);
    replace_canvas(source.solution(), source.can_save());
    directory_ = source.directory();
    source_ = std::move(source);
    pending_baseline_.reset();
    file_notices_ = std::move(notices);
    render_diagnostics();
    show_status(QStringLiteral("Opened %1.").arg(path_text(*directory_)));
}

void EditorWindow::save_document() { begin_save(); }

void EditorWindow::begin_save(Continuation after)
{
    if (busy_ || !writable()) { return; }
    if (!directory_) {
        choose_transfer(SolutionTransfer::SaveAs, std::move(after));
        return;
    }
    start_file_task(QStringLiteral("Saving solution…"),
        [directory = *directory_, source = source_, baseline = pending_baseline_, candidate = draft(),
            registry = registry_, roots = roots_, dirty = !scene_->undoStack().isClean()]() mutable -> Result<CommittedDocument> {
            auto confirmed = current_baseline(std::move(source), directory, baseline, registry, roots);
            if (auto* issue = std::get_if<Diagnostic>(&confirmed)) { return std::move(*issue); }
            auto current = std::get<SolutionDocument>(std::move(confirmed));
            // A retry after a successful commit may only need the exact
            // baseline refresh. It need not publish another revision.
            if (!dirty) { return CommittedDocument{std::move(current)}; }
            auto saved = save_solution(current, candidate);
            if (auto* issue = std::get_if<Diagnostic>(&saved)) { return std::move(*issue); }
            auto result = std::get<SolutionSaveResult>(std::move(saved));
            std::vector<QString> notices;
            for (const auto& issue : result.cleanup.diagnostics) {
                notices.push_back(QStringLiteral("Saved. Resource cleanup: %1").arg(QString::fromStdString(issue.message)));
            }
            if (result.cleanup.deferred) {
                notices.push_back(QStringLiteral("Saved. Cleanup of %1 resource file(s) was deferred.").arg(result.cleanup.deferred));
            }
            return CommittedDocument{std::move(result.solution), directory, registry, roots, std::move(notices)};
        },
        [this, after = std::move(after)](CommittedDocument committed) {
            accept_committed(std::move(committed), false, after);
        });
}

void EditorWindow::choose_transfer(SolutionTransfer operation, Continuation after)
{
    if (busy_ || !writable()) { return; }
    const auto title = operation == SolutionTransfer::SaveAs
        ? QStringLiteral("Choose Folder for Solution") : QStringLiteral("Choose Folder for Independent Copy");
    const auto selected = QFileDialog::getExistingDirectory(this, title,
        directory_ ? path_text(directory_->parent_path()) : QString{});
    if (!selected.isEmpty()) { begin_transfer(file_path(selected), operation, std::move(after)); }
}

void EditorWindow::transfer_document(const fs::path& directory, SolutionTransfer operation)
{ begin_transfer(directory, operation, {}); }

void EditorWindow::begin_transfer(const fs::path& directory, SolutionTransfer operation, Continuation after)
{
    if (busy_ || !writable()) { return; }
    start_file_task(QStringLiteral("Preparing solution files…"),
        [directory, operation, old_directory = directory_, source = source_, baseline = pending_baseline_,
            candidate = draft(), registry = registry_, roots = roots_]() mutable -> Result<CommittedDocument> {
            if (old_directory && !source) {
                auto confirmed = current_baseline({}, *old_directory, baseline, registry, roots);
                if (auto* issue = std::get_if<Diagnostic>(&confirmed)) { return std::move(*issue); }
                source = std::get<SolutionDocument>(std::move(confirmed));
            }
            auto transfer = transfer_solution(directory, candidate, operation, source ? &*source : nullptr, roots);
            if (auto* issue = std::get_if<Diagnostic>(&transfer)) { return std::move(*issue); }
            auto result = std::get<SolutionTransferResult>(std::move(transfer));
            std::vector<QString> notices;
            for (const auto& external : result.external_dependencies) {
                notices.push_back(QStringLiteral("External resource: %1").arg(path_text(external)));
            }
            for (const auto& root : result.unresolved_roots) {
                notices.push_back(QStringLiteral("Named root requires a directory: %1")
                    .arg(QString::fromStdString(std::string(root.text()))));
            }
            return CommittedDocument{std::move(result.solution), directory, registry, roots, std::move(notices)};
        },
        [this, after = std::move(after)](CommittedDocument committed) {
            accept_committed(std::move(committed), true, after);
        });
}

void EditorWindow::accept_committed(CommittedDocument committed, bool replace, const Continuation& after)
{
    if (replace) { replace_canvas(committed.saved); }
    model_->set_saved_revision(committed.saved.revision);
    scene_->undoStack().setClean();
    directory_ = std::move(committed.directory);
    source_ = std::move(committed.source);
    pending_baseline_ = source_ ? std::nullopt : std::optional<Solution>{std::move(committed.saved)};
    file_notices_ = std::move(committed.notices);
    render_diagnostics();
    show_status(QStringLiteral("Saved %1.").arg(path_text(*directory_)));
    if (after) { after(); }
}

void EditorWindow::choose_resource_copy()
{
    if (busy_ || !source_ || !writable()) { return; }
    QStringList labels;
    std::vector<const ResourceDefinition*> resources;
    for (const auto& resource : draft().resources) {
        if (std::holds_alternative<ExternalFileResource>(resource.location)) {
            labels.push_back(QStringLiteral("%1  [%2]").arg(QString::fromStdString(resource.name),
                QString::fromStdString(resource.id.to_string())));
            resources.push_back(&resource);
        }
    }
    if (resources.empty()) { return; }
    bool accepted = false;
    const auto selected = QInputDialog::getItem(this, QStringLiteral("Copy Resource Into Solution"),
        QStringLiteral("External file"), labels, 0, false, &accepted);
    if (!accepted) { return; }
    const auto row = labels.indexOf(selected);
    if (row < 0) { return; }
    const auto& resource = *resources[static_cast<std::size_t>(row)];
    const auto id = resource.id;
    start_file_task(QStringLiteral("Copying resource…"),
        [source = *source_, file = std::get<ExternalFileResource>(resource.location)] {
            return copy_external_resource_into_solution(source, file);
        },
        [this, id](ResourcePath path) {
            if (scene_->edit_draft(QStringLiteral("Point resource to copied file"), [id, path](Solution& candidate) -> Result<std::monostate> {
                    auto resource = std::ranges::find(candidate.resources, id, &ResourceDefinition::id);
                    if (resource != candidate.resources.end()) {
                        if (auto* file = std::get_if<ExternalFileResource>(&resource->location)) {
                            file->path = path;
                            return std::monostate{};
                        }
                    }
                    return file_error(ErrorCode::ResourceNotFound, "The resource to update is missing from the draft.");
                })) {
                show_status(QStringLiteral("Copied resource. Save to publish the new reference."));
            }
        });
}

void EditorWindow::choose_resource_root()
{
    if (busy_) { return; }
    bool accepted = false;
    const auto key = QInputDialog::getText(this, QStringLiteral("Set Named Resource Root"), QStringLiteral("Root ID"),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || key.isEmpty()) { return; }
    const auto parsed = SchemaIdFactory::parse<SchemaId>(key.toStdString());
    if (const auto* issue = std::get_if<Diagnostic>(&parsed)) {
        report_error(QString::fromStdString(issue->message));
        return;
    }
    const auto folder = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Resource Root"));
    if (folder.isEmpty()) { return; }
    auto mappings = roots_;
    const auto id = std::get<SchemaId>(parsed);
    const auto existing = std::ranges::find(mappings, id, &NamedResourceRoot::id);
    if (existing == mappings.end()) { mappings.push_back({id, file_path(folder)}); }
    else { existing->directory = file_path(folder); }
    if (!directory_) {
        roots_ = std::move(mappings);
        show_status(QStringLiteral("Named root configured."));
        return;
    }
    const auto new_roots = mappings;
    start_file_task(QStringLiteral("Checking named resource root…"),
        [directory = *directory_, registry = registry_, mappings = new_roots, previous = source_, expected = pending_baseline_]()
            -> Result<SolutionDocument> {
            if (!previous && expected) { return reload_saved_solution(directory, *expected, registry, mappings); }
            auto loaded = load_solution(directory, registry, mappings);
            if (const auto* source = std::get_if<SolutionDocument>(&loaded); source && previous && !source->same_manifest(*previous)) {
                return file_error(ErrorCode::SolutionSaveConflict, "The saved manifest changed. Reopen it before changing resource roots.");
            }
            return loaded;
        },
        [this, mappings = std::move(mappings)](SolutionDocument source) {
            file_notices_ = file_issues(source);
            roots_ = std::move(mappings);
            source_ = std::move(source);
            pending_baseline_.reset();
            render_diagnostics();
            show_status(QStringLiteral("Named root configured."));
        });
}

} // namespace visionpro::ui
