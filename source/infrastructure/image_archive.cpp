#include "image_archive_internal.hpp"
#include "windows_path.hpp"
#include <visionpro/infrastructure/file_resources.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>
#include <windows.h>

namespace visionpro::infrastructure
{
namespace
{
using namespace archive_detail;
using Json = nlohmann::json;
using core::Diagnostic;
using core::ToolInvocationIdentity;
void record_failure(std::optional<Diagnostic>& previous, Diagnostic failure)
{
    if (previous) {
        previous->message += " " + failure.message;
    } else {
        previous = std::move(failure);
    }
}
// Each outstanding image holds one reference, from queue admission through interval buffering.
struct QueueDomain {
};
using QueueSlot = std::shared_ptr<const QueueDomain>;
struct Task {
    ToolInvocationIdentity identity;
    ImageSaveOptions options;
    std::vector<ArchiveImage> images;
    std::vector<QueueSlot> slots{};
};
std::string origin(const ToolInvocationIdentity& id, ArchiveKind kind)
{
    const auto& key = id.invocation;
    return id.session.to_string() + "/" + std::to_string(key.workflow_run.value()) + "/" + key.node.to_string() +
           "/" + std::to_string(key.scope.value()) + "/" + std::to_string(key.invocation.value()) + "/" +
           (kind == ArchiveKind::Original ? "original" : "rendered");
}
std::string clock_label(const char* format)
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream s;
    s << std::put_time(&local, format);
    return s.str();
}
std::string file_name(const ArchiveImage& image, const ImageSaveOptions& options, std::uint64_t seq)
{
    auto name = image.prefix;
    if (!options.verdict.empty()) {
        name += "-" + options.verdict;
    }
    if (image.suffix == ArchiveSuffix::Sequence) {
        name += "-" + std::to_string(seq);
    }
    if (image.suffix == ArchiveSuffix::Time) {
        name += "-" + clock_label("%Y%m%d-%H%M%S");
    }
    return name + "." + image.extension;
}
ArchiveCheck validate(const ImageSaveOptions& o, const ArchiveImage& i, bool packaged)
{
    if (!safe_name(i.prefix) || i.bytes.empty() || i.bytes.size() > 512ULL * 1024 * 1024 ||
        (i.extension != "png" && i.extension != "jpeg" && i.extension != "jpg" && i.extension != "bmp")) {
        return issue("Image requires a valid file prefix, supported extension and encoded pixels.",
                     core::ErrorCode::InvalidValue);
    }
    if (!packaged && !o.ftp && (!i.directory.is_absolute() || i.directory.native().size() > 255)) {
        return issue("Image output directory must be absolute and at most 255 characters.",
                     core::ErrorCode::InvalidPath);
    }
    if (i.cache_limit == 0 || i.cache_limit > 20 || o.interval > 10 || o.interval > i.cache_limit ||
        o.retention_days > 500 || (o.verdict != "" && o.verdict != "OK" && o.verdict != "NG")) {
        return issue("Invalid image storage interval, queue capacity, expiry or verdict.",
                     core::ErrorCode::InvalidValue);
    }
    if (o.ftp && !packaged) {
        return validate_ftp_settings(*o.ftp);
    }
    return std::monostate{};
}
std::string ftp_target(const ImageFtpSettings& s)
{
    return Json::array({s.host, s.port, s.user, s.path, s.encoding}).dump();
}
fs::path contained(const fs::path& root, const std::string& relative)
{
    const auto path = path_from(relative);
    if (path.empty() || path.has_root_path()) {
        throw std::runtime_error("Invalid stored image record.");
    }
    for (const auto& segment : path) {
        if (!safe_name(utf8(segment))) {
            throw std::runtime_error("Invalid stored image component.");
        }
    }
    const auto canonical = fs::weakly_canonical(root / path);
    auto remaining = canonical.lexically_relative(fs::canonical(root));
    if (remaining.empty() || *remaining.begin() == "..") {
        throw std::runtime_error("Stored image moved outside its archive directory.");
    }
    return root / path;
}
bool owned_file(const fs::path& path, const Json& record)
{
    if (!fs::is_regular_file(path)) {
        return false;
    }
    auto bytes = read_file_bytes(detail::windows_display_path(path), record.at("sha").get<std::string>());
    return !std::holds_alternative<Diagnostic>(bytes);
}
struct DirectoryIndex {
    fs::path root, file;
    Json data;
    explicit DirectoryIndex(const fs::path& dir, std::string key) : root(dir)
    {
        const auto metadata = dir / ".visionpro-image-index";
        fs::create_directories(metadata);
        if (fs::canonical(metadata).parent_path() != fs::canonical(dir)) {
            throw std::runtime_error("Image index is outside its storage directory.");
        }
        file = metadata / (key + ".json");
        if (fs::exists(file)) {
            std::ifstream in(file, std::ios::binary);
            data = Json::parse(in);
            if (data.at("version") != 1 || !data.at("files").is_array()) {
                throw std::runtime_error("Invalid image archive index.");
            }
        } else {
            data = {{"version", 1}, {"sequence", 0}, {"files", Json::array()}};
        }
    }
    void persist()
    {
        const auto body = data.dump();
        auto r = atomic_file(file, std::as_bytes(std::span(body)), fs::exists(file));
        if (auto* e = std::get_if<Diagnostic>(&r)) {
            throw std::runtime_error(e->message);
        }
    }
    void expire(unsigned days)
    {
        if (!days) {
            return;
        }
        auto& rows = data.at("files");
        const auto before = rows.size();
        for (auto it = rows.begin(); it != rows.end();) {
            if (!it->at("ftp").get_ref<const std::string&>().empty()) {
                ++it;
                continue;
            }
            const auto path = contained(root, it->at("file").get<std::string>());
            if (!fs::exists(path)) {
                it = rows.erase(it);
                continue;
            }
            if (fs::file_time_type::clock::now() - fs::last_write_time(path) < std::chrono::days(days)) {
                ++it;
                continue;
            }
            if (owned_file(path, *it)) {
                fs::remove(path);
            }
            it = rows.erase(it);
        }
        if (rows.size() != before) {
            persist();
        }
    }
    void cleanup(const ArchiveImage& image, const ImageSaveOptions& options)
    {
        const auto volume = fs::space(root);
        if ((image.capacity_bytes && image.bytes.size() > image.capacity_bytes) ||
            volume.capacity < image.bytes.size() ||
            volume.capacity - image.bytes.size() < options.minimum_free_bytes) {
            throw std::runtime_error("Image cannot fit the configured output capacity or free-space floor.");
        }
        auto& rows = data.at("files");
        std::uint64_t count{}, total{};
        expire(options.retention_days);
        for (auto it = rows.begin(); it != rows.end();) {
            auto& row = *it;
            auto path = contained(root, row.at("file").get<std::string>());
            if (!fs::exists(path)) {
                it = rows.erase(it);
                continue;
            }
            ++count;
            total += fs::file_size(path);
            ++it;
        }
        auto limited = [&] {
            const auto available = fs::space(root).available;
            return (options.max_files && count >= options.max_files) ||
                   (image.capacity_bytes && (image.bytes.size() > image.capacity_bytes ||
                                             total > image.capacity_bytes - image.bytes.size())) ||
                   available < image.bytes.size() || available - image.bytes.size() < options.minimum_free_bytes;
        };
        for (auto it = rows.begin(); limited() && it != rows.end();) {
            if (!options.overwrite_oldest) {
                break;
            }
            auto path = contained(root, it->at("file").get<std::string>());
            if (!it->at("ftp").get<std::string>().empty() || !owned_file(path, *it)) {
                ++it;
                continue;
            }
            auto size = fs::file_size(path);
            fs::remove(path);
            --count;
            total -= size;
            it = rows.erase(it);
        }
        if (limited()) {
            persist();
            throw std::runtime_error("Image storage reached the file, byte or free-space limit.");
        }
    }
    bool transfer(const ImageFtpSettings& settings, std::stop_token stop = {})
    {
        const auto target = ftp_target(settings);
        for (auto& row : data.at("files")) {
            if (row.at("ftp") != target) {
                continue;
            }
            if (stop.stop_requested()) {
                return false;
            }
            const auto path = contained(root, row.at("file").get<std::string>());
            if (!row.contains("remote")) {
                auto read = read_file_bytes(detail::windows_display_path(path), row.at("sha").get<std::string>());
                const auto* bytes = std::get_if<std::shared_ptr<const std::vector<std::byte>>>(&read);
                if (!bytes) {
                    continue;
                }
                auto sent = upload_image(settings, **bytes, utf8(path.filename()), stop);
                if (std::holds_alternative<Diagnostic>(sent)) {
                    return false;
                }
                row["remote"] = std::get<std::string>(std::move(sent));
                persist(); // A later cleanup retry must not publish the same acknowledged image again.
            }
            if (owned_file(path, row)) {
                fs::remove(path);
            }
            row["ftp"] = "";
            persist();
        }
        return true;
    }
};
ImageSaveReceipt store_local(const Task& task, const ArchiveImage& image)
{
    if (!image.directory.is_absolute() || image.directory.native().size() > 255) {
        throw std::runtime_error("Image output directory must be absolute and at most 255 characters.");
    }
    const auto root = detail::windows_access_path(image.directory, true);
    fs::create_directories(root);
    const auto key =
        task.identity.invocation.node.to_string() + (image.kind == ArchiveKind::Original ? "-raw" : "-rendered");
    const auto metadata = root / ".visionpro-image-index";
    fs::create_directories(metadata);
    if (fs::canonical(metadata).parent_path() != fs::canonical(root)) {
        throw std::runtime_error("Image index is outside its storage directory.");
    }
    auto locked = detail::PackageWriteLock::acquire(metadata, false);
    if (auto* e = std::get_if<Diagnostic>(&locked)) {
        throw std::runtime_error(e->message);
    }
    DirectoryIndex index(root, key);
    const auto& opts = task.options;
    const auto token = origin(task.identity, image.kind);
    for (const auto& row : index.data.at("files")) {
        if (row.at("origin") == token) {
            auto path = contained(root, row.at("file").get<std::string>());
            return {image.kind,
                    row.value("remote", utf8(path.filename())),
                    fs::exists(path) ? std::optional(detail::windows_display_path(path)) : std::nullopt,
                    {}};
        }
    }
    index.cleanup(image, opts);
    const auto seq = index.data.at("sequence").get<std::uint64_t>() + 1;
    auto dir = root;
    if (opts.date_directories) {
        dir /= clock_label("%Y-%m");
        dir /= clock_label("%Y-%m-%d");
        fs::create_directories(dir);
    }
    const auto target = unique_path(dir / path_from(file_name(image, opts, seq)));
    const auto relative = utf8(target.lexically_relative(root));
    (void)contained(root, relative);
    auto digest = std::get<std::string>(detail::sha256(std::as_bytes(std::span(image.bytes))));
    auto& rows = index.data.at("files");
    rows.push_back({{"origin", token},
                    {"file", relative},
                    {"sha", digest},
                    {"created", std::time(nullptr)},
                    {"ftp", opts.ftp ? ftp_target(*opts.ftp) : ""}});
    index.data["sequence"] = seq;
    index.data["retention_days"] = opts.retention_days;
    // Journal first. A missing file left by a failed write is pruned on the next access.
    index.persist();
    auto written = atomic_file(target, std::as_bytes(std::span(image.bytes)));
    if (auto* failure = std::get_if<Diagnostic>(&written)) {
        rows.erase(rows.end() - 1);
        index.persist();
        throw std::runtime_error(failure->message);
    }
    return {image.kind, utf8(target.filename()), detail::windows_display_path(target), {}};
}
struct Pending {
    Task task;
    ArchiveImage image;
    QueueSlot slot;
};
struct WindowsNameLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
    {
        return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};

// A durable row belongs to one output node and one FTP endpoint. The monitor opens only the
// solution's registered indexes; a successful login never authorizes uploading arbitrary files.
class ArchiveMaintenance
{
  public:
    ~ArchiveMaintenance()
    {
        stop();
    }
    void stop()
    {
        std::jthread worker;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            if (!worker_.joinable()) {
                return;
            }
            worker_.request_stop();
            changed_.notify_all();
            worker = std::move(worker_);
        }
        worker.join();
    }
    void directory(const fs::path& path)
    {
        if (path.is_absolute()) {
            std::lock_guard lock(mutex_);
            if (roots_.insert(detail::windows_access_path(path, true)).second) {
                changed_catalog();
            }
        }
    }
    void node(const core::NodeId& id, std::optional<unsigned> days)
    {
        std::lock_guard lock(mutex_);
        bool changed = false;
        for (const auto suffix : {"-raw", "-rendered"}) {
            auto [it, added] = indexes_.try_emplace(id.to_string() + suffix, days);
            if (!added && days && it->second != days) {
                it->second = days;
                changed = true;
            }
            changed |= added;
        }
        if (changed) {
            changed_catalog();
        }
    }
    void retention_locations(std::span<const ImageRetentionLocation> locations)
    {
        RetentionScope scope;
        for (const auto& selected : locations) {
            auto& allowed = scope[detail::windows_access_path(selected.directory, true)];
            for (const auto suffix : {"-raw", "-rendered"}) {
                allowed.insert(selected.node.to_string() + suffix);
            }
        }
        std::lock_guard lock(mutex_);
        if (scope_ != scope) {
            scope_ = std::move(scope);
            changed_catalog();
        }
    }
    void stored(const ImageFtpSettings& settings, bool local_pending)
    {
        const auto key = ftp_target(settings);
        std::unique_lock lock(mutex_);
        if (closed_) {
            return;
        }
        const bool first_use = known_.insert(key).second;
        if (!local_pending && !first_use && !pending_.contains(key)) {
            return;
        }
        const auto revision = ++revision_;
        pending_[key] = {settings, revision};
        if (!local_pending) {
            const auto roots = roots_;
            const auto indexes = indexes_;
            lock.unlock();
            const bool complete = replay(settings, roots, indexes, {});
            lock.lock();
            if (complete) {
                acknowledge(key, revision);
            }
        }
        if (!closed_ && !pending_.empty() && !worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token stop) { monitor(stop); });
        }
        changed_.notify_one();
    }
    ArchiveCheck expire()
    {
        std::unique_lock lock(mutex_);
        const auto roots = roots_;
        const auto indexes = indexes_;
        const auto scope = scope_;
        lock.unlock();
        return expire(roots, indexes, scope, {});
    }

  private:
    struct Target {
        ImageFtpSettings settings;
        std::uint64_t revision;
    };
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::set<fs::path> roots_;
    using IndexPolicies = std::map<std::string, std::optional<unsigned>>;
    using RetentionScope = std::map<fs::path, std::set<std::string>>;
    std::optional<RetentionScope> scope_;
    IndexPolicies indexes_;
    std::set<std::string> known_;
    std::map<std::string, Target> pending_;
    std::uint64_t revision_{};
    bool closed_{};
    std::jthread worker_;
    std::chrono::steady_clock::time_point next_sweep_{std::chrono::steady_clock::time_point::max()};
    void changed_catalog()
    {
        const bool retention = (!scope_ || !scope_->empty()) && std::ranges::any_of(indexes_, [](const auto& item) {
                                   return !item.second || *item.second > 0;
                               });
        next_sweep_ = retention && !roots_.empty() ? std::chrono::steady_clock::now() + std::chrono::seconds(1)
                                                   : std::chrono::steady_clock::time_point::max();
        if (!closed_ && retention && !roots_.empty() && !worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token stop) { monitor(stop); });
        }
        changed_.notify_one();
    }

    void acknowledge(const std::string& key, std::uint64_t revision)
    {
        const auto entry = pending_.find(key);
        if (entry != pending_.end() && entry->second.revision == revision) {
            pending_.erase(entry);
        }
    }
    template <class Visit>
    static ArchiveCheck visit_indexes(const std::set<fs::path>& roots, const IndexPolicies& indexes,
                                      std::stop_token stop, bool retry_busy, const RetentionScope* scope,
                                      Visit visit)
    {
        std::optional<Diagnostic> failure;
        for (const auto& root : roots) {
            if (stop.stop_requested()) {
                break;
            }
            const std::set<std::string>* allowed = nullptr;
            if (scope) {
                const auto found = scope->find(root);
                if (found == scope->end()) {
                    continue;
                }
                allowed = &found->second;
            }
            try {
                const auto metadata = root / ".visionpro-image-index";
                std::error_code ec;
                if (!fs::is_directory(metadata, ec) ||
                    fs::canonical(metadata).parent_path() != fs::canonical(root)) {
                    continue;
                }
                // The idle monitor can retry later if another process currently owns the index.
                auto locked = detail::PackageWriteLock::acquire(metadata, false, false);
                if (std::holds_alternative<Diagnostic>(locked)) {
                    if (retry_busy) {
                        record_failure(failure, issue("The archive index is busy."));
                    }
                    continue;
                }
                for (const auto& entry : fs::directory_iterator(metadata)) {
                    const auto key = utf8(entry.path().stem());
                    const auto selected = indexes.find(key);
                    if (entry.path().extension() != ".json" || selected == indexes.end() ||
                        (allowed && !allowed->contains(key))) {
                        continue;
                    }
                    DirectoryIndex index(root, key);
                    if (!visit(index, selected->second)) {
                        return issue("An image archive operation remains pending.");
                    }
                }
            } catch (const std::exception& e) {
                record_failure(failure, issue(e.what()));
            }
        }
        if (failure) {
            return *failure;
        }
        return std::monostate{};
    }
    static bool replay(const ImageFtpSettings& settings, const std::set<fs::path>& roots,
                       const IndexPolicies& indexes, std::stop_token stop)
    {
        return std::holds_alternative<std::monostate>(
            visit_indexes(roots, indexes, stop, true, nullptr,
                          [&](DirectoryIndex& index, auto) { return index.transfer(settings, stop); }));
    }
    static ArchiveCheck expire(const std::set<fs::path>& roots, const IndexPolicies& indexes,
                               const std::optional<RetentionScope>& scope, std::stop_token stop)
    {
        auto active = indexes;
        std::erase_if(active, [](const auto& entry) { return entry.second && *entry.second == 0; });
        if (active.empty()) {
            return std::monostate{};
        }
        return visit_indexes(roots, active, stop, false, scope ? &*scope : nullptr,
                             [](DirectoryIndex& index, std::optional<unsigned> configured) {
                                 const auto days = configured.value_or(index.data.value("retention_days", 0u));
                                 if (days > 500) {
                                     throw std::runtime_error("Invalid retention period in archive index.");
                                 }
                                 index.expire(days);
                                 return true;
                             });
    }
    void monitor(std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            const auto now = std::chrono::steady_clock::now();
            const auto wake = pending_.empty() ? next_sweep_ : std::min(next_sweep_, now + std::chrono::seconds(1));
            if (wake == std::chrono::steady_clock::time_point::max()) {
                changed_.wait(lock);
                continue;
            }
            // Catalog updates, FTP work and shutdown can all shorten this wait.
            if (changed_.wait_until(lock, wake) != std::cv_status::timeout) {
                continue;
            }
            if (stop.stop_requested()) {
                return;
            }
            const auto targets = pending_;
            const auto roots = roots_;
            const auto indexes = indexes_;
            const auto scope = scope_;
            const bool sweep = std::chrono::steady_clock::now() >= next_sweep_;
            if (sweep) {
                next_sweep_ = std::chrono::steady_clock::now() + std::chrono::minutes(1);
            }
            lock.unlock();
            if (sweep) {
                (void)expire(roots, indexes, scope, stop);
            }
            for (const auto& [key, target] : targets) {
                if (stop.stop_requested()) {
                    break;
                }
                if (replay(target.settings, roots, indexes, stop)) {
                    std::lock_guard completed(mutex_);
                    acknowledge(key, target.revision);
                }
            }
            lock.lock();
        }
    }
};
} // namespace

struct ImageArchive::State {
    std::optional<ArchivePackagePolicy> policy;
    ArchiveMaintenance maintenance;
    std::mutex queue_mutex, write_mutex;
    std::condition_variable activity;
    std::deque<Task> queue;
    std::map<std::pair<core::NodeId, ArchiveKind>, QueueSlot> queue_domains;
    bool running{}, closing{};
    std::optional<Diagnostic> async_error;
    std::thread worker;
    std::map<std::pair<core::NodeId, ArchiveKind>, std::deque<Pending>> waiting;
    std::vector<PackageImage> package;
    std::map<std::wstring, std::uint32_t, WindowsNameLess> package_names;
    fs::path package_path;
    std::uint64_t package_reserve{}, package_bytes{}, sequence{};
    std::map<std::pair<core::NodeId, ArchiveKind>, std::uint64_t> ftp_sequences;

    explicit State(std::optional<ArchivePackagePolicy> p) : policy(std::move(p))
    {
        if (policy) {
            policy->directory = detail::windows_access_path(policy->directory, true);
        }
    }
    ArchiveCheck finish_package()
    {
        if (package.empty()) {
            return std::monostate{};
        }
        fs::create_directories(policy->directory);
        const auto available = fs::space(policy->directory).available;
        if (available < package_bytes || available - package_bytes < package_reserve) {
            return issue("Insufficient free space for the image package.");
        }
        auto r = write_package(package_path, package);
        if (!std::holds_alternative<Diagnostic>(r)) {
            package.clear();
            package_names.clear();
            package_path.clear();
            package_bytes = 0;
            package_reserve = 0;
        }
        return r;
    }
    ImageSaveReceipt write(const Task& task, const ArchiveImage& image)
    {
        if (!policy) {
            maintenance.directory(image.directory);
            maintenance.node(task.identity.invocation.node, task.options.retention_days);
            if (!task.options.ftp) {
                return store_local(task, image);
            }
            const auto& target = *task.options.ftp;
            auto& seq = ftp_sequences[{task.identity.invocation.node, image.kind}];
            auto remote =
                upload_image(target, std::as_bytes(std::span(image.bytes)), file_name(image, task.options, ++seq));
            if (auto* name = std::get_if<std::string>(&remote)) {
                maintenance.stored(target, false);
                return {image.kind, std::move(*name), {}, {}};
            }
            try {
                auto receipt = store_local(task, image);
                maintenance.stored(target, true);
                return receipt;
            } catch (const std::exception& e) {
                throw std::runtime_error(std::get<Diagnostic>(remote).message +
                                         " Local FTP fallback failed: " + e.what());
            }
        }
        if (!policy->directory.is_absolute() || !policy->images_per_file || policy->images_per_file > 65536) {
            throw std::runtime_error("Invalid global image package settings.");
        }
        if (package.size() >= policy->images_per_file) {
            auto r = finish_package();
            if (auto* e = std::get_if<Diagnostic>(&r)) {
                throw std::runtime_error(e->message);
            }
        }
        fs::create_directories(policy->directory);
        if (package.empty()) {
            package_path = policy->directory / ("images-" + uuid() + ".dat");
        }
        auto name = file_name(image, task.options, ++sequence);
        const auto available = fs::space(policy->directory).available;
        const auto needed = package_bytes + image.bytes.size();
        if (available < needed || available - needed < std::max(package_reserve, task.options.minimum_free_bytes)) {
            throw std::runtime_error("Insufficient free space for the image package.");
        }
        const auto requested = path_from(name);
        auto [entry, first_use] = package_names.try_emplace(requested.native(), 1);
        if (!first_use) {
            std::wstring alternate;
            do {
                alternate = requested.stem().native() + L"-" + std::to_wstring(++entry->second) +
                            requested.extension().native();
            } while (!package_names.try_emplace(alternate, 1).second);
            name = utf8(alternate);
        }
        package.push_back({name, origin(task.identity, image.kind), image.bytes});
        package_bytes = needed;
        package_reserve = std::max(package_reserve, task.options.minimum_free_bytes);
        const auto file = detail::windows_display_path(package_path);
        if (package.size() == policy->images_per_file) {
            auto r = finish_package();
            if (auto* d = std::get_if<Diagnostic>(&r)) {
                package_bytes -= package.back().bytes.size();
                package_names.erase(path_from(package.back().name).native());
                package.pop_back();
                throw std::runtime_error(d->message);
            }
            return {image.kind, std::move(name), {}, file};
        }
        return {image.kind, std::move(name), {}, file};
    }
    ImageSaveReport process(Task& task, const std::function<bool()>& cancelled = {})
    {
        ImageSaveReport report;
        try {
            for (std::size_t index = 0; index < task.images.size(); ++index) {
                auto& image = task.images[index];
                if (cancelled && cancelled()) {
                    report.failure = issue("Image storage was cancelled.", core::ErrorCode::SourceCancelled);
                    break;
                }
                auto& pending = waiting[{task.identity.invocation.node, image.kind}];
                // The originating identity and settings travel with every buffered image.
                const auto kind = image.kind;
                if (pending.size() >= image.cache_limit) {
                    throw std::runtime_error("Image storage queue is full.");
                }
                pending.push_back(
                    {{task.identity, task.options, {}}, std::move(image), std::move(task.slots[index])});
                if (pending.size() < std::max<std::size_t>(1, task.options.interval)) {
                    continue;
                }
                try {
                    while (!pending.empty()) {
                        auto& front = pending.front();
                        auto receipt = write(front.task, front.image);
                        if (front.task.identity == task.identity && receipt.kind == kind) {
                            report.images.push_back(std::move(receipt));
                        }
                        pending.pop_front();
                    }
                } catch (...) {
                    // This call reports failure. Previously accepted buffered frames remain available for flush.
                    std::erase_if(pending,
                                  [&](const Pending& item) { return item.task.identity == task.identity; });
                    throw;
                }
            }
        } catch (const std::exception& e) {
            report.failure = issue(e.what());
            report.failure->location.invocation = task.identity.invocation;
            report.failure->location.node = task.identity.invocation.node;
        }
        std::erase_if(report.images, [&](const ImageSaveReceipt& r) {
            return r.package && !package.empty() && *r.package == detail::windows_display_path(package_path);
        });
        return report;
    }
    void work()
    {
        std::unique_lock lock(queue_mutex);
        while (true) {
            activity.wait(lock, [&] { return closing || !queue.empty(); });
            if (queue.empty() && closing) {
                return;
            }
            auto task = std::move(queue.front());
            queue.pop_front();
            running = true;
            lock.unlock();
            ImageSaveReport result;
            try {
                std::lock_guard write_lock(write_mutex);
                result = process(task);
            } catch (...) {
                result.failure = issue("Unexpected failure in the image storage worker.");
            }
            lock.lock();
            if (result.failure) {
                for (const auto& receipt : result.images) {
                    result.failure->message += " Committed image: " + receipt.name + ".";
                }
                record_failure(async_error, std::move(*result.failure));
            }
            running = false;
            activity.notify_all();
        }
    }
};
ImageArchive::ImageArchive(std::optional<ArchivePackagePolicy> policy)
    : state_(std::make_unique<State>(std::move(policy)))
{
}
void ImageArchive::register_output_directory(const fs::path& directory)
{
    state_->maintenance.directory(directory);
}
void ImageArchive::register_output_node(const core::NodeId& node, std::optional<unsigned> days)
{
    state_->maintenance.node(node, days);
}
void ImageArchive::set_retention_locations(std::span<const ImageRetentionLocation> locations)
{
    state_->maintenance.retention_locations(locations);
}
ImageArchive::~ImageArchive()
{
    state_->maintenance.stop();
    {
        std::lock_guard lock(state_->queue_mutex);
        state_->closing = true;
        state_->activity.notify_all();
    }
    if (state_->worker.joinable()) {
        state_->worker.join();
    }
    (void)flush();
}
ImageSaveReport ImageArchive::save(const core::ToolInvocationIdentity& identity, const ImageSaveOptions& opts,
                                   std::vector<ArchiveImage> images, const std::function<bool()>& cancelled)
{
    try {
        for (const auto& image : images) {
            auto r = validate(opts, image, state_->policy.has_value());
            if (auto* d = std::get_if<Diagnostic>(&r)) {
                return {{}, *d};
            }
        }
        if (cancelled && cancelled()) {
            return {{}, issue("Image storage was cancelled.", core::ErrorCode::SourceCancelled)};
        }
        Task task{identity, opts, std::move(images)};
        std::unique_lock lock(state_->queue_mutex);
        if (state_->async_error) {
            auto error = std::exchange(state_->async_error, {});
            return {{}, std::move(error)};
        }
        for (const auto& image : task.images) {
            auto& domain = state_->queue_domains[{identity.invocation.node, image.kind}];
            if (!domain) {
                domain = std::make_shared<QueueDomain>();
            }
            if (static_cast<std::size_t>(domain.use_count() - 1) >= image.cache_limit) {
                return {{}, issue("Image storage queue is full.", core::ErrorCode::QueueFull)};
            }
            task.slots.push_back(domain);
        }
        if (opts.sync_storage) {
            state_->activity.wait(lock, [this] { return state_->queue.empty() && !state_->running; });
            lock.unlock();
            std::lock_guard write_lock(state_->write_mutex);
            return state_->process(task, cancelled);
        }
        if (!state_->worker.joinable()) {
            state_->worker = std::thread([this] { state_->work(); });
        }
        state_->queue.push_back(std::move(task));
        state_->activity.notify_one();
        return {};
    } catch (const std::exception& e) {
        return {{}, issue(e.what())};
    }
}
ArchiveCheck ImageArchive::flush()
{
    try {
        std::optional<Diagnostic> failure;
        {
            std::unique_lock lock(state_->queue_mutex);
            state_->activity.wait(lock, [this] { return state_->queue.empty() && !state_->running; });
            failure = std::exchange(state_->async_error, {});
        }
        std::lock_guard write_lock(state_->write_mutex);
        for (auto& [key, pending] : state_->waiting) {
            (void)key;
            while (!pending.empty()) {
                auto& front = pending.front();
                try {
                    (void)state_->write(front.task, front.image);
                } catch (const std::exception& e) {
                    auto diagnostic = issue(e.what());
                    diagnostic.location.node = front.task.identity.invocation.node;
                    diagnostic.location.invocation = front.task.identity.invocation;
                    record_failure(failure, std::move(diagnostic));
                    break;
                }
                pending.pop_front();
            }
        }
        auto packed = state_->finish_package();
        if (auto* error = std::get_if<Diagnostic>(&packed)) {
            record_failure(failure, std::move(*error));
        }
        auto expired = state_->maintenance.expire();
        if (auto* error = std::get_if<Diagnostic>(&expired)) {
            record_failure(failure, std::move(*error));
        }
        if (failure) {
            return *failure;
        }
        return std::monostate{};
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
std::shared_ptr<ImageArchive> ImageArchivePool::writer(const std::optional<ArchivePackagePolicy>& policy)
{
    std::lock_guard lock(mutex_);
    for (const auto& [key, service] : writers_) {
        if (key == policy) {
            return service;
        }
    }
    auto service = std::make_shared<ImageArchive>(policy);
    writers_.emplace_back(policy, service);
    return service;
}

ArchiveCheck ImageArchivePool::flush()
{
    std::vector<std::shared_ptr<ImageArchive>> services;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, service] : writers_) {
            (void)key;
            services.push_back(service);
        }
    }
    ArchiveCheck result = std::monostate{};
    for (const auto& service : services) {
        auto checked = service->flush();
        if (auto* failure = std::get_if<Diagnostic>(&checked)) {
            if (auto* previous = std::get_if<Diagnostic>(&result)) {
                previous->message += " " + failure->message;
            } else {
                result = std::move(*failure);
            }
        }
    }
    return result;
}
} // namespace visionpro::infrastructure
