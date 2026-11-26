#include <visionpro/infrastructure/image_directory_trigger.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/core/shared_data.hpp>
#include "windows_path.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace workflow;
namespace fs = std::filesystem;

const BindingSource* parameter(const Node& node, std::string_view name)
{
    for (const auto& binding : node.bindings) {
        const auto* id = std::get_if<ParameterId>(&binding.target);
        if (id && id->text() == name && binding.sources.size() == 1) { return &binding.sources.front(); }
    }
    return nullptr;
}

bool watches(const Node& node)
{
    const auto* type = std::get_if<ToolConfiguration>(&node.configuration);
    if (!node.enabled || !type || type->type.text() != "visionpro.image.source") { return false; }
    const auto* mode = std::get_if<ConfigValue>(parameter(node, "source_type"));
    const auto enabled = [&](std::string_view field) {
        const auto* value = std::get_if<ConfigValue>(parameter(node, field));
        return value && value->as_bool() && *value->as_bool();
    };
    return (!mode || (mode->as_enum() && mode->as_enum()->text() == "Local"))
        && enabled("subscribe_folder") && enabled("trigger_flow");
}

Diagnostic issue(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Result<std::string> folder(const Node& node, const WorkflowSession& session)
{
    const auto* source = parameter(node, "folder_path");
    if (const auto* fixed = std::get_if<ConfigValue>(source); fixed && fixed->as_string()) { return *fixed->as_string(); }
    if (const auto* global = std::get_if<GlobalReference>(source); global && global->selector.empty()) {
        auto service = session.shared_preview();
        if (const auto* error = std::get_if<Diagnostic>(&service)) { return *error; }
        auto read = std::get<std::shared_ptr<const SharedData>>(service)->read(global->variable);
        if (const auto* error = std::get_if<Diagnostic>(&read)) { return *error; }
        const auto& entry = std::get<GlobalVariableEntry>(read);
        if ((!global->required_revision || entry.revision == *global->required_revision) && entry.current_value.as_string()) {
            return *entry.current_value.as_string();
        }
    }
    auto bad = issue(ErrorCode::InvalidBinding, "Arming a directory needs an absolute path or an available global String before the run starts.");
    bad.location.node = node.id;
    return bad;
}

using FileStamp = std::pair<std::uintmax_t, fs::file_time_type>;
using Listing = std::map<fs::path, FileStamp>;
using Directories = std::map<NodeId, Listing>;
}

struct ImageDirectoryTrigger::Impl {
    std::shared_ptr<WorkflowSession> session;
    ConfigSnapshot snapshot;
    WorkflowId workflow;
    std::shared_ptr<TriggerControl> source;
    Directories baseline, pending;
    std::int64_t batch{};
    std::mutex mutex;

    Result<Directories> scan() const {
        Directories scanned;
        for (const auto& node : find_workflow(snapshot.solution(), workflow)->nodes) {
            if (!watches(node)) { continue; }
            auto path = folder(node, *session);
            if (const auto* bad = std::get_if<Diagnostic>(&path)) { return *bad; }
            const auto& text = std::get<std::string>(path);
            auto paths = image_directory_files(fs::path(std::u8string(text.begin(), text.end())));
            if (const auto* bad = std::get_if<Diagnostic>(&paths)) { return *bad; }
            auto& entries = scanned[node.id];
            for (const auto& file : std::get<std::vector<fs::path>>(paths)) {
                std::error_code ec;
                const auto access = detail::windows_access_path(file);
                const auto size = fs::file_size(access, ec);
                if (ec) { continue; }
                const auto stamp = fs::last_write_time(access, ec);
                if (!ec) { entries.emplace(file, FileStamp{size, stamp}); }
            }
        }
        return scanned;
    }
};

ImageDirectoryTrigger::ImageDirectoryTrigger(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ImageDirectoryTrigger::~ImageDirectoryTrigger() = default;
bool ImageDirectoryTrigger::configured(const Workflow& flow) { return std::ranges::any_of(flow.nodes, watches); }
const std::shared_ptr<TriggerControl>& ImageDirectoryTrigger::control() const { return impl_->source; }

Result<std::shared_ptr<ImageDirectoryTrigger>> ImageDirectoryTrigger::open(std::shared_ptr<WorkflowSession> session, WorkflowId workflow)
{
    const auto snapshot = session ? session->inspect().active : std::nullopt;
    const auto* flow = snapshot ? find_workflow(snapshot->solution(), workflow) : nullptr;
    if (!flow || !configured(*flow)) { return issue(ErrorCode::InvalidRunRequest, "No directory trigger is configured in this workflow."); }
    auto armed = TriggerControl::start_local(session, snapshot->identity(), workflow);
    if (const auto* bad = std::get_if<Diagnostic>(&armed)) { return *bad; }
    auto impl = std::unique_ptr<Impl>(new Impl{session, *snapshot, workflow, std::get<std::shared_ptr<TriggerControl>>(armed), {}, {}, 0, {}});
    auto before = impl->scan();
    if (const auto* bad = std::get_if<Diagnostic>(&before)) { return *bad; }
    impl->baseline = std::get<Directories>(std::move(before));
    impl->pending = impl->baseline;
    return std::shared_ptr<ImageDirectoryTrigger>(new ImageDirectoryTrigger(std::move(impl)));
}

Result<std::optional<TriggerReceipt>> ImageDirectoryTrigger::poll()
{
    const std::lock_guard lock(impl_->mutex);
    if (!impl_->source->accepting()) { return std::optional<TriggerReceipt>{}; }
    const auto current = impl_->session->inspect().active;
    if (!current || !current->same_capture(impl_->snapshot)) {
        impl_->source->disarm();
        return issue(ErrorCode::ResultIdentityMismatch, "Directory subscription belongs to a previous configuration. Arm the current configuration.");
    }
    auto scanned = impl_->scan();
    if (const auto* bad = std::get_if<Diagnostic>(&scanned)) { return *bad; }
    auto& observed = std::get<Directories>(scanned);
    if (observed != impl_->pending) {
        impl_->pending = std::move(observed);
        return std::optional<TriggerReceipt>{};
    }
    if (observed == impl_->baseline) { return std::optional<TriggerReceipt>{}; }
    std::vector<DirectoryChange> changes;
    for (const auto& [node, files] : observed) {
        const auto& previous = impl_->baseline.at(node);
        DirectoryChange change{node, {}};
        for (const auto& [file, stamp] : files) {
            const auto old = previous.find(file);
            if (old != previous.end() && old->second == stamp) { continue; }
            std::ifstream readable(detail::windows_access_path(file), std::ios::binary);
            if (!readable || !stamp.first) { return std::optional<TriggerReceipt>{}; }
            const auto path = file.generic_u8string();
            change.changed_files.emplace_back(path.begin(), path.end());
        }
        changes.push_back(std::move(change));
    }
    auto receipt = impl_->source->directory_changed(++impl_->batch, std::move(changes));
    if (!std::holds_alternative<RunBusy>(receipt.submission)) { impl_->baseline = std::move(observed); }
    return std::optional{std::move(receipt)};
}
} // namespace visionpro::infrastructure
