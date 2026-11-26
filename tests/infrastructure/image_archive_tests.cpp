#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/infrastructure/image_archive.hpp>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <set>

namespace
{
using namespace visionpro::core;
using namespace visionpro::infrastructure;
namespace fs = std::filesystem;
void check(bool passed, const char* reason)
{
    if (!passed) {
        throw std::runtime_error(reason);
    }
}
template <class T> T take(Result<T> r)
{
    if (auto* e = std::get_if<Diagnostic>(&r)) {
        throw std::runtime_error(e->message);
    }
    return std::get<T>(std::move(r));
}
void success(const ImageSaveReport& r)
{
    if (r.failure) {
        throw std::runtime_error(r.failure->message);
    }
}
struct Temp {
    fs::path path = fs::temp_directory_path() / ("vp-images-" + take(IdFactory::generate<MessageId>()).to_string());
    Temp()
    {
        fs::create_directories(path);
    }
    ~Temp()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
struct Calls {
    SolutionSessionId session = take(IdFactory::generate<SolutionSessionId>());
    NodeId node = take(IdFactory::generate<NodeId>());
    unsigned next{};
    ToolInvocationIdentity call()
    {
        return {session,
                take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1)),
                {take(RuntimeIdFactory::from_value<WorkflowRunId>(1)), node,
                 take(RuntimeIdFactory::from_value<ExecutionScopeId>(0)),
                 take(RuntimeIdFactory::from_value<InvocationId>(++next))},
                take(RuntimeIdFactory::from_value<AttemptId>(0))};
    }
};
ArchiveImage image(const fs::path& dir, unsigned char pixel = 1)
{
    return {ArchiveKind::Original,
            dir,
            "sample",
            ArchiveSuffix::Sequence,
            "png",
            std::vector<std::uint8_t>(512, pixel),
            20};
}
std::size_t count(const fs::path& dir, const fs::path& ext)
{
    if (!fs::exists(dir)) {
        return 0;
    }
    std::size_t n{};
    for (const auto& f : fs::recursive_directory_iterator(dir)) {
        if (f.is_regular_file() && f.path().extension() == ext) {
            ++n;
        }
    }
    return n;
}
void local()
{
    Temp t;
    Calls calls;
    ImageArchive archive;
    ImageSaveOptions opts;
    opts.interval = 2;
    opts.date_directories = true;
    auto first = archive.save(calls.call(), opts, {image(t.path)});
    success(first);
    check(first.images.empty() && count(t.path, ".png") == 0, "Interval wrote prematurely.");
    auto second = archive.save(calls.call(), opts, {image(t.path, 2)});
    success(second);
    check(second.images.size() == 1 && count(t.path, ".png") == 2, "Interval did not commit both images.");
    const auto local = *second.images.front().local_file;
    check(local.parent_path().parent_path().parent_path() == t.path && fs::exists(local),
          "Dated directory mismatch.");
    opts.interval = 11;
    check(archive.save(calls.call(), opts, {image(t.path)}).failure.has_value(), "Interval above ten passed.");
    opts.interval = 3;
    auto narrow = image(t.path);
    narrow.cache_limit = 2;
    check(archive.save(calls.call(), opts, {narrow}).failure.has_value(), "Interval above queue capacity passed.");
    opts.interval = 0;
    narrow.cache_limit = 21;
    check(archive.save(calls.call(), opts, {narrow}).failure.has_value(), "Oversized queue passed.");
    opts.date_directories = false;
    const auto dir = t.path / "limits";
    auto original = image(dir);
    opts.max_files = 1;
    original.suffix = ArchiveSuffix::None;
    auto stored = archive.save(calls.call(), opts, {original});
    success(stored);
    const auto path = *stored.images.front().local_file;
    check(archive.save(calls.call(), opts, {original}).failure.has_value(), "Stop policy replaced a file.");
    std::ofstream(dir / "unrelated.png") << "other";
    opts.overwrite_oldest = true;
    auto replacement = archive.save(calls.call(), opts, {original});
    success(replacement);
    check(fs::exists(path) && fs::file_size(dir / "unrelated.png") == 5, "Overwrite removed unrelated files.");
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(48));
    opts.max_files = 0;
    opts.retention_days = 1;
    original.prefix = "new";
    success(archive.save(calls.call(), opts, {original}));
    check(!fs::exists(path), "Expired owned file was retained.");
    opts.retention_days = 501;
    check(archive.save(calls.call(), opts, {original}).failure.has_value(), "Invalid retention passed.");
    opts.retention_days = 0;
    opts.minimum_free_bytes = std::numeric_limits<std::uint64_t>::max();
    opts.overwrite_oldest = false;
    check(archive.save(calls.call(), opts, {original}).failure.has_value(), "Free-space floor was ignored.");
    opts.minimum_free_bytes = 0;
    original.capacity_bytes = 2;
    check(archive.save(calls.call(), opts, {original}).failure.has_value(),
          "Output path byte capacity was ignored.");
    original.capacity_bytes = 0;
    original.prefix = "../bad";
    check(archive.save(calls.call(), opts, {original}).failure.has_value(), "Unsafe file name was accepted.");
    const auto root = t.path.wstring();
    auto boundary = fs::path(root + L"/" + std::wstring(255 - root.size() - 1, L'a'));
    opts.max_files = 0;
    auto valid = archive.save(calls.call(), opts, {image(boundary)});
    success(valid);
    check(!valid.images.empty(), "255-character directory did not write.");
    check(archive.save(calls.call(), opts, {image(fs::path(boundary.wstring() + L"b"))}).failure.has_value(),
          "256-character directory accepted.");
    opts.sync_storage = false;
    auto async = archive.save(calls.call(), opts, {image(t.path / "async")});
    success(async);
    check(async.images.empty(), "Asynchronous operation reported uncommitted artifact.");
    take(archive.flush());
    check(count(t.path / "async", ".png") == 1, "Asynchronous flush lost image.");
    opts.sync_storage = true;
    auto cancelled = archive.save(calls.call(), opts, {image(t.path / "cancel")}, [] { return true; });
    check(cancelled.failure && !fs::exists(t.path / "cancel"), "Cancellation created a file.");

    // An older accepted frame survives an I/O failure in the call that fills its interval.
    const auto blocked = t.path / "blocked";
    std::ofstream(blocked) << "existing file";
    opts.interval = 2;
    success(archive.save(calls.call(), opts, {image(blocked, 8)}));
    auto failed = archive.save(calls.call(), opts, {image(blocked, 9)});
    check(failed.failure.has_value(), "A file in place of an output directory did not fail.");
    fs::remove(blocked);
    take(archive.flush());
    check(count(blocked, ".png") == 1,
          "A failed flush lost the earlier accepted image or stored the rejected frame.");
    opts.interval = 0;
    opts.sync_storage = false;
    std::ofstream(t.path / "async-blocked") << "existing file";
    success(archive.save(calls.call(), opts, {image(t.path / "async-blocked")}));
    check(std::holds_alternative<Diagnostic>(archive.flush()), "Asynchronous storage failure was not reported.");
}
void packages()
{
    Temp t;
    Calls a, b;
    const auto dir = t.path / "packages";
    ImageArchive writer(ArchivePackagePolicy{dir, 2});
    ImageSaveOptions opts;
    opts.ftp = ImageFtpSettings{"invalid.invalid"};
    auto first = writer.save(a.call(), opts, {image(t.path / "raw", 8)});
    success(first);
    check(first.images.empty() && count(dir, ".dat") == 0, "Partial package published before threshold.");
    auto second = writer.save(b.call(), opts, {image(t.path / "rendered", 9)});
    success(second);
    check(second.images.size() == 1 && second.images[0].package.has_value(), "Package path missing.");
    const auto file = *second.images[0].package;
    auto entries = take(list_image_package(file));
    check(entries.size() == 2 && entries[0].origin != entries[1].origin, "Package lost node identity.");
    check(take(read_image_package_entry(file, 0)) == image({}, 8).bytes &&
              take(read_image_package_entry(file, 1)) == image({}, 9).bytes,
          "Package image content changed.");
    check(!fs::exists(t.path / "raw") && !fs::exists(t.path / "rendered"),
          "Global storage wrote node directories.");
    auto extracted = take(extract_image_packages(file, true));
    check(extracted.size() == 2 && extracted[0].parent_path() == dir / file.stem(),
          "Single package extraction failed.");
    success(writer.save(a.call(), opts, {image({}, 11)}));
    take(writer.flush());
    check(count(dir, ".dat") == 2, "Final partial package lost.");
    auto all = take(extract_image_packages(dir, false));
    check(all.size() == 3, "Directory extraction failed.");
    std::ofstream bad(file, std::ios::binary | std::ios::app);
    bad << "corruption";
    bad.close();
    check(std::holds_alternative<Diagnostic>(list_image_package(file)), "Corrupt package accepted.");
    opts.minimum_free_bytes = std::numeric_limits<std::uint64_t>::max();
    check(writer.save(a.call(), opts, {image({}, 12)}).failure.has_value(),
          "Global storage ignored free-space floor.");
}
void retention()
{
    Temp temp;
    Calls calls;
    const auto ng = temp.path / "ng";
    const auto ok = temp.path / "ok";
    const auto old = fs::file_time_type::clock::now() - std::chrono::days(3);
    fs::path retained, previously_bound;
    {
        ImageArchive archive;
        ImageSaveOptions options;
        options.retention_days = 1;
        options.verdict = "NG";
        auto original = image(ng);
        auto rendered = image(ng);
        rendered.kind = ArchiveKind::Rendered;
        auto issue_result = archive.save(calls.call(), options, {original, rendered});
        success(issue_result);
        const auto expired = *issue_result.images[0].local_file;
        retained = *issue_result.images[1].local_file;
        fs::last_write_time(expired, old);
        std::ofstream(retained, std::ios::trunc) << "replacement";
        fs::last_write_time(retained, old);
        auto unrelated = ng / "unrelated.png";
        std::ofstream(unrelated) << "unowned";
        fs::last_write_time(unrelated, old);
        options.verdict = "OK";
        auto ok_result = archive.save(calls.call(), options, {image(ok)});
        success(ok_result);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (fs::exists(expired) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(!fs::exists(expired) && fs::exists(retained) && fs::exists(unrelated),
              "Idle retention failed across the inactive NG directory or removed an unowned file.");
        retained = *ok_result.images[0].local_file;
        auto previous = archive.save(calls.call(), options, {image(temp.path / "previous")});
        success(previous);
        previously_bound = *previous.images.front().local_file;
        take(archive.flush());
    }
    fs::last_write_time(retained, old);
    fs::last_write_time(previously_bound, old);
    ImageArchive reopened;
    reopened.register_output_directory(ok);
    reopened.register_output_directory(previously_bound.parent_path());
    reopened.register_output_node(calls.node); // Last persisted policy applies to a dynamically bound period.
    const std::array active{ImageRetentionLocation{calls.node, ok}};
    reopened.set_retention_locations(active);
    take(reopened.flush());
    check(!fs::exists(retained) && fs::is_regular_file(previously_bound),
          "Reopened retention did not respect the node's currently bound output directories.");
}

void package_receipts()
{
    Temp temp;
    Calls calls;
    auto original = image({}, 7);
    auto rendered = image({}, 11);
    rendered.kind = ArchiveKind::Rendered;
    original.suffix = ArchiveSuffix::None;
    rendered.suffix = ArchiveSuffix::None;
    for (const unsigned per_file : {1u, 2u}) {
        ImageArchive archive(ArchivePackagePolicy{temp.path / std::to_string(per_file), per_file});
        const auto saved = archive.save(calls.call(), {}, {original, rendered});
        success(saved);
        check(saved.images.size() == 2 && saved.images[0].package && saved.images[1].package,
              "An image committed in the second half of an invocation lost its receipt.");
        check((*saved.images[0].package == *saved.images[1].package) == (per_file == 2),
              "The two published images did not identify their actual packages.");
        for (const auto& receipt : saved.images) {
            const auto entries = take(list_image_package(*receipt.package));
            const auto entry = std::ranges::find(entries, receipt.name, &ImagePackageEntry::name);
            check(entry != entries.end(), "The receipt name was absent from its referenced package.");
            const auto bytes =
                take(read_image_package_entry(*receipt.package, static_cast<std::size_t>(entry - entries.begin())));
            check(bytes == (receipt.kind == ArchiveKind::Original ? original.bytes : rendered.bytes),
                  "A package receipt traced the wrong image.");
        }
    }
    ImageArchive archive(ArchivePackagePolicy{temp.path / "boundary", 2});
    success(archive.save(calls.call(), {}, {original}));
    auto incomplete = archive.save(calls.call(), {}, {original, rendered});
    success(incomplete);
    check(incomplete.images.size() == 1 && incomplete.images.front().kind == ArchiveKind::Original,
          "Image awaiting the next package received a premature receipt.");
    take(archive.flush());
}
void concurrent_packages()
{
    Temp temp;
    ImageArchivePool pool;
    const auto dir = temp.path / "shared";
    const ArchivePackagePolicy policy{dir, 7};
    auto writer = pool.writer(policy);
    check(writer == pool.writer(policy), "The same policy did not reuse the session writer.");
    Calls first, second;
    auto produce = [&](Calls& calls, unsigned base) {
        for (unsigned i = 0; i < 25; ++i) {
            ImageSaveOptions options;
            options.sync_storage = i % 2 == 0;
            success(writer->save(calls.call(), options, {image({}, static_cast<unsigned char>(base + i))}));
        }
    };
    auto a = std::async(std::launch::async, [&] { produce(first, 1); });
    auto b = std::async(std::launch::async, [&] { produce(second, 26); });
    a.get();
    b.get();
    take(pool.flush());
    check(count(dir, ".dat") == 8, "Incorrect number of shared or partial packages.");
    std::set<unsigned> pixels;
    std::set<std::string> origins;
    for (const auto& file : fs::directory_iterator(dir)) {
        auto entries = take(list_image_package(file.path()));
        check(entries.size() <= 7, "Package exceeded the configured image count.");
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto bytes = take(read_image_package_entry(file.path(), i));
            check(bytes.size() == 512 && std::ranges::all_of(bytes, [&](auto b) { return b == bytes.front(); }),
                  "Concurrent package payload changed.");
            const auto& node = bytes.front() < 26 ? first.node : second.node;
            check(entries[i].origin.find(node.to_string()) != std::string::npos,
                  "Image is associated with another producer.");
            pixels.insert(bytes.front());
            origins.insert(entries[i].origin);
        }
    }
    check(pixels.size() == 50 && *pixels.begin() == 1 && *pixels.rbegin() == 50 && origins.size() == 50,
          "Concurrent archiving duplicated or lost an invocation.");
}
void ftp(int argc, char** argv)
{
    check(argc == 5, "FTP mode expects directory,port,and connected/disconnected.");
    fs::path root = argv[2];
    ImageFtpSettings settings{"127.0.0.1", static_cast<std::uint16_t>(std::stoi(argv[3])), "anonymous", "", "UTF-8",
                              "/质量/图像"};
    const std::string mode = argv[4];
    const bool online = mode == "online" || mode == "no-local";
    struct CredentialCleanup {
        std::string ref;
        ~CredentialCleanup()
        {
            if (!ref.empty()) {
                (void)remove_image_ftp_password(ref);
            }
        }
    } cleanup;
    std::array<char, 256> password{};
    std::size_t password_length{};
    if (getenv_s(&password_length, password.data(), password.size(), "VISIONPRO_TEST_FTP_SECRET") == 0 &&
        password_length > 1) {
        settings.user = "test";
        cleanup.ref = "archive-test-" + take(IdFactory::generate<MessageId>()).to_string();
        settings.credential = cleanup.ref;
        take(save_image_ftp_password(settings.credential, password.data()));
    }
    auto status = check_image_ftp(settings);
    if (mode == "probe-delete") {
        const auto* failure = std::get_if<Diagnostic>(&status);
        check(failure && failure->message.find(".visionpro-probe-") != std::string::npos &&
                  failure->message.find("550") != std::string::npos,
              "Check did not identify the inaccessible disposable FTP probe.");
        return;
    }
    if (mode == "offline" || mode == "idle" || mode == "both-fail") {
        check(std::holds_alternative<Diagnostic>(status), "Offline FTP returned success.");
    } else if (mode == "reject") {
        const auto* failure = std::get_if<Diagnostic>(&status);
        check(failure && failure->message.find("RNTO") != std::string::npos,
              "FTP probe did not report rejected publication.");
    } else {
        take(std::move(status));
    }
    ImageArchive archive;
    if (mode == "online") {
        archive.register_output_directory(root.parent_path() / "previous");
    }
    Calls calls;
    calls.node = take(IdFactory::parse<NodeId>("00000000-0000-4000-8000-000000000001"));
    ImageSaveOptions o;
    o.ftp = settings;
    auto input = image(root, online ? 2 : mode == "reject" ? 3 : 1);
    input.prefix = mode + "-样件";
    if (mode == "online" || mode == "no-local" || mode == "both-fail") {
        o.minimum_free_bytes = std::numeric_limits<std::uint64_t>::max();
        input.capacity_bytes = 1;
    }
    if (mode == "no-local" || mode == "both-fail") {
        input.directory.clear();
    }
    if (mode == "idle") {
        o.retention_days = 1;
    }
    auto result = archive.save(calls.call(), o, {input});
    if (mode == "both-fail") {
        check(result.failure && result.images.empty() &&
                  result.failure->message.find("Local FTP fallback failed") != std::string::npos,
              "When FTP and local storage both fail, diagnostics must identify the failed fallback.");
        return;
    }
    success(result);
    check(result.images.size() == 1 && result.images[0].local_file.has_value() != online,
          "Unexpected FTP local receipt.");
    if (mode == "idle") {
        const auto cached = *result.images[0].local_file;
        fs::last_write_time(cached, fs::file_time_type::clock::now() - std::chrono::days(3));
        take(archive.flush());
        check(fs::exists(cached), "Retention deleted the only pending FTP image.");
        std::ofstream(root / "ready").put('1');
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (fs::exists(cached) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        check(!fs::exists(cached), "Live FTP cache was not recovered without another submission.");
    }
    take(archive.flush());
}
} // namespace
int main(int argc, char** argv)
{
    try {
        if (argc < 2) {
            return 1;
        }
        auto mode = std::string_view(argv[1]);
        if (mode == "local") {
            local();
            retention();
        } else if (mode == "packages") {
            packages();
            package_receipts();
            concurrent_packages();
        } else if (mode == "ftp") {
            ftp(argc, argv);
        } else {
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
