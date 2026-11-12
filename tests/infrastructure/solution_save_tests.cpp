#include "../../source/infrastructure/solution_save.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace save_detail = visionpro::infrastructure::detail;
using save_detail::SaveStage;

constexpr std::uint64_t initial_revision = 12;
constexpr std::string_view original_bytes = "abc";
constexpr std::string_view replacement_bytes = "xyz";
constexpr std::string_view original_digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
constexpr std::string_view replacement_digest = "3608bca1e44ea6c4d268eb6db02260269892c0b42b86bbf1e77a6fa16c3c9282";

int checks = 0;

void check(bool passed, std::string_view message)
{
    ++checks;
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> T id(char suffix = '1')
{
    std::string value = "00000000-0000-4000-8000-000000000001";
    value.back() = suffix;
    return take(IdFactory::parse<T>(value));
}

template<class T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }

template<class T> void rejected(const Result<T>& value, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    if (failure && failure->code != code) {
        throw std::runtime_error(std::string(message) + " Received: " + failure->message);
    }
    check(failure != nullptr, message);
}

struct ScratchDirectory {
    ScratchDirectory()
        : path(fs::current_path() / ("s38-save-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        if (!fs::create_directory(path)) { throw std::runtime_error("Could not reserve the fixture directory."); }
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    const fs::path path;
};

void write(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) { throw std::runtime_error("Could not write the fixture."); }
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Could not read the fixture."); }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

class Gate {
public:
    void pause()
    {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(lock, 10s, [this] { return released_; })) {
            throw std::runtime_error("The test did not release the waiting worker.");
        }
    }

    [[nodiscard]] bool await()
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 5s, [this] { return entered_; });
    }

    void release()
    {
        std::lock_guard lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

struct ReleaseGate {
    Gate& gate;
    ~ReleaseGate() { gate.release(); }
};

class ByteTool final : public ToolInstance {
public:
    ByteTool(const ToolExecutionConfiguration& configuration, std::shared_ptr<Gate> gate)
        : ToolInstance(configuration), gate_(std::move(gate)) {}
protected:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& resource = context->data().resources.at(0);
        const auto lease = std::dynamic_pointer_cast<const FileResourceLease>(resource.lease);
        if (!lease || lease->bytes().empty()) { throw std::logic_error("Expected the file lease."); }
        if (gate_) { gate_->pause(); }
        const auto byte = std::to_integer<std::int64_t>(lease->bytes().front());
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("byte"), DataValue::integer(byte)}}, {}}};
    }
private:
    std::shared_ptr<Gate> gate_;
};

class ByteFactory final : public ToolInstanceFactory {
public:
    explicit ByteFactory(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        return std::shared_ptr<ToolInstance>{std::make_shared<ByteTool>(configuration, gate_)};
    }
private:
    std::shared_ptr<Gate> gate_;
};

ToolRegistry registry(const std::shared_ptr<Gate>& gate)
{
    const auto schema = key<SchemaId>("s38.read");
    ToolDescriptor descriptor{
        .id = key<ToolTypeId>("s38.read"),
        .display_name = "Read one byte",
        .config_version = 1,
        .schema = {schema, 1, {}},
        .parameters = {},
        .inputs = {},
        .outputs = {{key<OutputId>("byte"), "Byte", DataType::integer(), {}}},
        .control_outputs = {},
        .state = {},
        .resources = {{key<SchemaId>("payload"), ResourceKind::Binary}},
        .region_capabilities = {},
        .image_formats = {},
        .execution = {},
        .state_parameters = {},
    };
    const std::array registrations{ToolRegistration{std::move(descriptor), {schema, 1, 1, {}},
        std::make_shared<ByteFactory>(gate)}};
    return take(ToolRegistry::create(registrations));
}

struct Fixture {
    ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"保存方案";
    const fs::path manifest = package / solution_manifest_name;
    const fs::path relative_managed = u8"resources/旧.bin";
    const fs::path managed = package / relative_managed;
    const fs::path external = scratch.path / u8"外部.bin";
    const ToolRegistry tools;

    explicit Fixture(const std::shared_ptr<Gate>& gate = {}) : tools(registry(gate))
    {
        write(managed, original_bytes);
        write(external, "external");
        const ResourceDefinition payload{id<ResourceId>('1'), "Managed", ResourceKind::Binary,
            ManagedResource{{PathBase::SolutionDirectory, utf8(relative_managed), {}}, 7, std::string(original_digest)}};
        const ResourceDefinition attachment{id<ResourceId>('2'), "External", ResourceKind::Binary,
            ExternalFileResource{{PathBase::Absolute, utf8(external), {}}, ResourceReadPolicy::PerInvocation, {}, {}}};
        const Node reader{id<NodeId>(), "Read", ToolConfiguration{key<ToolTypeId>("s38.read"), 1}, {},
            {{key<SchemaId>("payload"), {payload.id, ResourceKind::Binary, {}}}}};
        const Workflow flow{id<WorkflowId>(), "Read file", {key<SchemaId>("s38.flow"), 1, {}, {}, {}},
            {reader}, {}, {}, {reader.id}, {}, true, {}};
        take(write_new_solution(package, {id<SolutionId>(), "保存验收", solution_format_version, initial_revision,
            {flow}, {payload, attachment}, {}, {}, {}, {}}));
    }

    [[nodiscard]] SolutionDocument open() const { return take(load_solution(package, tools)); }
};

ManagedResourceWrite payload(std::string_view bytes = replacement_bytes, char resource = '1')
{
    return {id<ResourceId>(resource), std::as_bytes(std::span{bytes.data(), bytes.size()})};
}

std::array<ManagedResourceWrite, 1> replace(std::string_view bytes = replacement_bytes) { return {payload(bytes)}; }

RunHandle start(LoadedSolutionExecution& execution)
{
    const auto request = take(RunRequest::create(execution.snapshot, WorkflowTarget{id<WorkflowId>()}, {},
        {TriggerKind::Manual, key<SchemaId>("s38.test")}));
    auto submitted = execution.engine->submit(request);
    if (const auto* failure = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(failure->diagnostic.message); }
    return std::get<RunHandle>(std::move(submitted));
}

void output_is(const RunHandle& run, std::int64_t byte)
{
    const auto done = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(done.outcome == RunWaitOutcome::Satisfied && done.progress.status == ExecutionStatus::Succeeded
        && done.progress.nodes.size() == 1 && done.progress.nodes.front().invocation.has_value(),
        "The file-backed workflow finished.");
    const auto result = take(run.lookup(*done.progress.nodes.front().invocation));
    const auto* output = result ? result->output(key<OutputId>("byte")) : nullptr;
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && value->as_int64() && *value->as_int64() == byte, "The workflow received the expected file bytes.");
}

void current_output_is(const Fixture& fixture, std::int64_t byte)
{
    const auto document = fixture.open();
    check(document.can_save() && document.diagnostics().empty(), "The committed package reloads without diagnostics.");
    auto execution = take(open_solution_execution(document, fixture.tools));
    output_is(start(execution), byte);
}

fs::path verify_replacement(const Fixture& fixture, const Solution& solution)
{
    const auto& payload = std::get<ManagedResource>(solution.resources.front().location);
    check(solution.revision == initial_revision + 1 && payload.revision == 8,
        "The solution and replaced resource each advance one revision.");
    check(payload.path.base == PathBase::SolutionDirectory && payload.sha256 == replacement_digest,
        "The new resource records its package path and actual SHA-256.");
    const auto installed = fixture.package / fs::path(std::u8string(payload.path.path.begin(), payload.path.path.end()));
    check(installed != fixture.managed && read(installed) == replacement_bytes,
        "New bytes are stored under an independent managed path.");
    check(read(fixture.external) == "external", "The external file keeps its bytes.");
    return installed;
}

std::size_t payload_files(const Fixture& fixture)
{
    const auto lock = fixture.package / ".visionpro-save.lock";
    std::size_t count = 0;
    for (const auto& item : fs::recursive_directory_iterator(fixture.package)) {
        if (item.is_regular_file() && item.path() != fixture.manifest && item.path() != lock) { ++count; }
    }
    return count;
}

void verify_rollback(const Fixture& fixture, const std::string& original_manifest)
{
    check(read(fixture.manifest) == original_manifest, "Failed saves preserve the old manifest byte for byte.");
    check(read(fixture.managed) == original_bytes && read(fixture.external) == "external",
        "Failed saves preserve both managed and external input bytes.");
    check(payload_files(fixture) == 1, "Failed saves leave only the original package payload.");
    current_output_is(fixture, 'a');
}

class HoldManifest final {
public:
    explicit HoldManifest(const fs::path& path)
        : file_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        if (file_ == INVALID_HANDLE_VALUE) { throw std::runtime_error("Could not hold the manifest without delete sharing."); }
    }
    ~HoldManifest() { CloseHandle(file_); }
    HoldManifest(const HoldManifest&) = delete;
    HoldManifest& operator=(const HoldManifest&) = delete;
private:
    HANDLE file_;
};

void failures()
{
    Fixture fixture;
    const auto source = fixture.open();
    const auto original = read(fixture.manifest);
    auto edited = source.solution();
    edited.name = "Edited";
    edited.resources.push_back({id<ResourceId>('3'), "New managed resource", ResourceKind::Binary, ManagedResource{}});
    const std::array writes{payload(), payload("another managed file", '3')};
    const std::array faults{std::pair{SaveStage::ResourceWrite, 2u}, std::pair{SaveStage::ManifestWrite, 3u},
        std::pair{SaveStage::Commit, 4u}};
    for (const auto& [fault, expected_files] : faults) {
        int resource_attempts = 0;
        std::size_t staged = 0;
        const auto saved = save_detail::save_solution(source, edited, writes, [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage != fault) { return {}; }
            if (stage == SaveStage::ResourceWrite && ++resource_attempts == 1) { return {}; }
            staged = payload_files(fixture);
            return Diagnostic{ErrorCode::SolutionIoFailure, Severity::Error, {}, "Injected save failure."};
        });
        check(staged == expected_files, "The selected failure stage was reached with the expected files staged.");
        rejected(saved, ErrorCode::SolutionIoFailure, "The injected failure prevents commit.");
        verify_rollback(fixture, original);
    }

    bool reached_commit = false;
    {
        const HoldManifest held(fixture.manifest);
        check(fixture.open().solution().revision == initial_revision, "The held manifest can still be read.");
        const auto saved = save_detail::save_solution(source, edited, writes, [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage == SaveStage::Commit) { reached_commit = true; }
            return {};
        });
        rejected(saved, ErrorCode::SolutionIoFailure, "Windows rejects replacement while the manifest denies delete sharing.");
    }
    check(reached_commit, "The operating system rejected a commit that reached manifest replacement.");
    verify_rollback(fixture, original);

    const auto external_write = save_solution(source, source.solution(), std::array{payload("changed", '2')});
    rejected(external_write, ErrorCode::InvalidResource, "Explicit payload writes are rejected for external files.");
    verify_rollback(fixture, original);

    const auto unknown_manifest = "{\"future_field\":true," + original.substr(1);
    write(fixture.manifest, unknown_manifest);
    const auto unsupported = fixture.open();
    check(!unsupported.can_save(), "An unknown field prevents overwriting a partially understood manifest.");
    rejected(save_solution(unsupported, unsupported.solution()), ErrorCode::UnsupportedSolutionContent,
        "Saving a partially understood manifest reports unsupported content.");
    check(read(fixture.manifest) == unknown_manifest, "Unsupported content stays byte for byte.");
}

void conflict()
{
    Fixture fixture;
    const auto first_source = fixture.open();
    const auto second_source = fixture.open();
    const auto original_manifest = read(fixture.manifest);
    auto first_edit = first_source.solution();
    first_edit.name = "First edit";
    auto second_edit = second_source.solution();
    second_edit.name = "Second edit";
    Gate committing;
    std::promise<void> second_started;
    auto second_arrival = second_started.get_future();
    std::atomic<bool> second_staged{false};
    std::future<Result<SolutionSaveResult>> first;
    std::future<Result<SolutionSaveResult>> second;
    const ReleaseGate unblock{committing};

    first = std::async(std::launch::async, [&] {
        return save_detail::save_solution(first_source, first_edit, replace(), [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage == SaveStage::Commit) { committing.pause(); }
            return {};
        });
    });
    check(committing.await(), "The first writer has staged its payload and manifest.");
    second = std::async(std::launch::async, [&] {
        return save_detail::save_solution(second_source, second_edit, replace("different"), [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage == SaveStage::BeforeLock) { second_started.set_value(); }
            if (stage == SaveStage::ResourceWrite || stage == SaveStage::ManifestWrite) { second_staged = true; }
            return {};
        });
    });
    check(second_arrival.wait_for(5s) == std::future_status::ready, "The second writer starts before the first is released.");
    check(second.wait_for(150ms) == std::future_status::timeout && !second_staged,
        "The second writer waits before staging while the first holds the package lock.");
    check(read(fixture.manifest) == original_manifest, "The original manifest is still visible while the first writer is staged.");
    committing.release();

    const auto committed = take(first.get());
    check(committed.solution.name == first_edit.name && committed.cleanup.removed == 1,
        "The first writer commits and retires its predecessor.");
    rejected(second.get(), ErrorCode::SolutionSaveConflict, "The second writer is rejected with a stale revision conflict.");
    check(!second_staged && payload_files(fixture) == 1, "The stale writer stages no files.");
    const auto installed = verify_replacement(fixture, committed.solution);
    const auto current = fixture.open();
    check(current.solution().name == first_edit.name, "The winning edit is present after reloading.");
    current_output_is(fixture, 'x');

    const auto outside_edit = read(fixture.manifest) + " \n";
    write(fixture.manifest, outside_edit);
    const auto outside = fixture.open();
    check(outside.solution().revision == current.solution().revision, "The external change retains the numeric revision.");
    rejected(save_solution(current, current.solution()), ErrorCode::SolutionSaveConflict,
        "A manifest byte change conflicts even at the same numeric revision.");
    check(read(fixture.manifest) == outside_edit, "The externally edited manifest is preserved.");

    const auto late_edit = outside_edit + " ";
    const auto changed_during_save = save_detail::save_solution(outside, outside.solution(), replace("different"),
        [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage == SaveStage::Commit) { write(fixture.manifest, late_edit); }
            return {};
        });
    rejected(changed_during_save, ErrorCode::SolutionSaveConflict, "A non-cooperating edit during staging is detected before commit.");
    check(read(fixture.manifest) == late_edit && payload_files(fixture) == 1,
        "The late external edit is preserved and the staged files are discarded.");

    const auto refreshed = fixture.open();
    auto metadata_edit = refreshed.solution();
    metadata_edit.name = "Metadata only";
    const auto metadata_saved = take(save_solution(refreshed, metadata_edit));
    check(metadata_saved.solution.revision == refreshed.solution().revision + 1
        && fixture.open().solution().name == metadata_edit.name, "Reloading supplies the base for a new configuration save.");
    check(metadata_saved.cleanup.removed == 0 && metadata_saved.cleanup.deferred == 0 && metadata_saved.cleanup.diagnostics.empty()
        && payload_files(fixture) == 1 && read(installed) == replacement_bytes, "A configuration save preserves the existing managed version.");
}

void cleanup()
{
    {
        const auto gate = std::make_shared<Gate>();
        Fixture fixture(gate);
        const auto source = fixture.open();
        auto execution = take(open_solution_execution(source, fixture.tools));
        const ReleaseGate unblock{*gate};
        const auto running = start(execution);
        check(gate->await(), "The running tool holds its lease before save.");
        const auto saved = take(save_solution(source, source.solution(), replace()));
        const auto installed = verify_replacement(fixture, saved.solution);
        check(saved.cleanup.removed == 0 && saved.cleanup.deferred == 1,
            "The retired version is deferred while the run holds it.");
        check(saved.cleanup.diagnostics.empty(), "An active lease does not produce a cleanup failure diagnostic.");
        check(read(fixture.managed) == original_bytes, "Cleanup retains the active version on disk.");
        gate->release();
        output_is(running, 'a');
        const auto reclaimed = take(cleanup_solution_resources(source));
        check(reclaimed.removed == 1 && reclaimed.deferred == 0 && reclaimed.diagnostics.empty(),
            "Cleanup retry removes the retired version after quiescence.");
        check(!fs::exists(fixture.managed) && read(installed) == replacement_bytes, "Cleanup retains the committed version.");
        check(take(cleanup_solution_resources(source)).removed == 0, "Repeated cleanup is idempotent.");
        current_output_is(fixture, 'x');
    }

    Fixture fixture;
    const auto source = fixture.open();
    const auto saved = take(save_detail::save_solution(source, source.solution(), replace(),
        [](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage != SaveStage::Cleanup) { return {}; }
            return Diagnostic{ErrorCode::SolutionCleanupFailure, Severity::Error, {}, "Injected cleanup failure."};
        }));
    const auto installed = verify_replacement(fixture, saved.solution);
    check(saved.cleanup.removed == 0 && saved.cleanup.deferred == 0 && saved.cleanup.diagnostics.size() == 1
        && saved.cleanup.diagnostics.front().code == ErrorCode::SolutionCleanupFailure,
        "A cleanup failure is reported after successful commit.");
    check(read(fixture.managed) == original_bytes, "The failed cleanup leaves the retired version available for retry.");
    current_output_is(fixture, 'x');

    write(fixture.managed, "changed outside the transaction");
    const auto mismatched = take(cleanup_solution_resources(source));
    check(mismatched.removed == 0 && mismatched.deferred == 0 && mismatched.diagnostics.size() == 1
        && mismatched.diagnostics.front().code == ErrorCode::SolutionCleanupFailure,
        "Cleanup reports that the file no longer matches the retired digest.");
    check(read(fixture.managed) == "changed outside the transaction", "Cleanup preserves changed bytes at a retired path.");
    write(fixture.managed, original_bytes);
    check(take(cleanup_solution_resources(source)).removed == 1 && !fs::exists(fixture.managed),
        "The previous document can retry failed cleanup.");

    const auto current = fixture.open();
    auto edited = current.solution();
    auto& alias = std::get<ExternalFileResource>(edited.resources[1].location);
    alias.path.path = utf8(installed);
    const auto aliased = take(save_solution(current, edited, replace("new")));
    check(aliased.cleanup.removed == 0 && read(installed) == replacement_bytes,
        "An external reference in the committed manifest protects the retired file.");
    check(take(cleanup_solution_resources(current)).removed == 0 && read(installed) == replacement_bytes,
        "Cleanup retry honors the current external reference.");
    current_output_is(fixture, 'n');
    check(read(fixture.external) == "external", "Cleanup never rewrites the external file.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view group = argc == 2 ? argv[1] : "";
        if (group == "failures") { failures(); }
        else if (group == "conflict") { conflict(); }
        else if (group == "cleanup") { cleanup(); }
        else { throw std::runtime_error("Expected failures, conflict, or cleanup."); }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
