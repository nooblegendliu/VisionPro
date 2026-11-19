#include "../../source/infrastructure/solution_save.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/subgraph_package.hpp>

#include <algorithm>
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
#include <vector>

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

ToolRegistry registry(const std::shared_ptr<Gate>& gate, bool with_state = false)
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
    if (with_state) {
        InputDescriptor input{key<InputId>("offset"), "Offset", DataType::integer(), false, ConfigValue::integer(0)};
        input.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
        input.binding.allow_dynamic_index = true;
        descriptor.inputs.push_back(input);
        input.id = key<InputId>("second");
        input.display_name = "Second value";
        descriptor.inputs.push_back(std::move(input));
        descriptor.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0), StateScope::SolutionSession,
            StateResetPolicy::DomainStartAndExplicit, StateCompatibilityPolicy::PreserveCompatible,
            StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
    }
    const std::array registrations{ToolRegistration{std::move(descriptor), {schema, 1, 1, {}},
        std::make_shared<ByteFactory>(gate)}};
    return take(ToolRegistry::create(registrations));
}

enum class PayloadStorage { Managed, External };

struct Fixture {
    ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"保存方案";
    const fs::path manifest = package / solution_manifest_name;
    const fs::path relative_managed = u8"resources/旧.bin";
    const fs::path managed = package / relative_managed;
    const fs::path external = scratch.path / u8"外部.bin";
    const ToolRegistry tools;

    explicit Fixture(const std::shared_ptr<Gate>& gate = {}, PayloadStorage storage = PayloadStorage::Managed)
        : tools(registry(gate))
    {
        write(managed, original_bytes);
        write(external, "external");
        const ResourcePath payload_path{PathBase::SolutionDirectory, utf8(relative_managed), {}};
        const ResourceLocation payload_location = storage == PayloadStorage::Managed
            ? ResourceLocation{ManagedResource{payload_path, 7, std::string(original_digest)}}
            : ResourceLocation{ExternalFileResource{payload_path, ResourceReadPolicy::PerInvocation, {}, {}}};
        const ResourceDefinition payload{id<ResourceId>('1'),
            storage == PayloadStorage::Managed ? "Managed" : "Package payload", ResourceKind::Binary, payload_location};
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

RunHandle start(LoadedSolutionExecution& execution, WorkflowId workflow = id<WorkflowId>())
{
    const auto request = take(RunRequest::create(execution.snapshot, WorkflowTarget{workflow}, {},
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

void transfer_packages()
{
    Fixture fixture;
    const auto root_id = key<SchemaId>("test.external");
    const auto unresolved_id = key<SchemaId>("test.unresolved");
    const std::array roots{NamedResourceRoot{root_id, fixture.scratch.path}};
    const auto source = take(load_solution(fixture.package, fixture.tools, roots));
    const auto original = read(fixture.manifest);
    auto draft = source.solution();
    const auto workflow = draft.workflows.front().id;
    const auto node = draft.workflows.front().nodes.front().id;
    const ClassPalette palette{{17, {15, 160, 250}}};
    draft.layouts = {{workflow, {{node, 23, -17, palette}}}};
    draft.name = "已编辑";
    draft.resources.push_back({id<ResourceId>('3'), "Named external", ResourceKind::Binary, ExternalFileResource{
        {PathBase::NamedRoot, utf8(fixture.external.filename()), root_id}, ResourceReadPolicy::PerInvocation, {}, {}}});
    draft.resources.push_back({id<ResourceId>('4'), "Relative external", ResourceKind::Binary, ExternalFileResource{
        {PathBase::SolutionDirectory, "../" + utf8(fixture.external.filename()), {}}, ResourceReadPolicy::PerInvocation, {}, {}}});
    draft.resources.push_back({id<ResourceId>('5'), "Unconfigured root", ResourceKind::Binary, ExternalFileResource{
        {PathBase::NamedRoot, "camera.bin", unresolved_id}, ResourceReadPolicy::PerInvocation, {}, {}}});
    draft.resources.push_back({id<ResourceId>('6'), "Package sequence", ResourceKind::DirectorySequence, ImageSequenceResource{
        DirectorySelection{{PathBase::SolutionDirectory, "images", {}}, "*.bin"}, SequenceSort::NameAscending,
        SequenceAdvance::PerInvocation, SequenceRepeat::Stop, SequenceMissing::Fail, SequenceTrigger::Manual, 0}});
    draft.resources.push_back({id<ResourceId>('7'), "Package output", ResourceKind::OutputPath, OutputPathResource{
        {PathBase::SolutionDirectory, "outputs", {}}, OutputNaming::FixedName, "image", "bin", OutputOverwrite::Fail, 1000}});
    const auto& relative = std::get<ManagedResource>(draft.resources.front().location).path;
    const auto alias_path = relative.path;
    draft.resources.push_back({id<ResourceId>('8'), "Aliased payload", ResourceKind::Binary, ExternalFileResource{
        {PathBase::SolutionDirectory, alias_path, {}}, ResourceReadPolicy::PerInvocation, {}, {}}});
    write(fixture.package / "images" / u8"帧.bin", "selected");
    write(fixture.package / "images" / "notes.txt", "unselected");
    write(fixture.package / "outputs" / "previous.bin", "historical data");
    write(fixture.package / "unrelated.bin", "not a resource");

    const auto destination = fixture.scratch.path / u8"另存方案";
    write(destination / "sentinel.txt", "unrelated destination file");
    const auto saved = take(transfer_solution(destination, draft, SolutionTransfer::SaveAs, &source));
    const auto document = take(reload_saved_solution(destination, saved.solution, fixture.tools, roots));
    const auto& restored = document.solution();
    check(restored.id == draft.id && restored.revision == draft.revision && restored.name == draft.name
        && restored.workflows.front().id == workflow && restored.workflows.front().nodes.front().id == node,
        "Save As retains stable identities and persists the draft without incrementing its old revision.");
    check(restored.layouts.front().workflow == workflow && restored.layouts.front().nodes.front().node == node
        && restored.layouts.front().nodes.front().x == 23 && restored.layouts.front().nodes.front().y == -17
        && restored.layouts.front().nodes.front().class_palette == palette,
        "Layout round-trips with the same workflow and node identities.");
    const auto relative_to = [&](const ResourcePath& path) { return destination / fs::path(std::u8string(path.path.begin(), path.path.end())); };
    const auto& copied = std::get<ManagedResource>(restored.resources[0].location);
    const auto& old = std::get<ManagedResource>(draft.resources[0].location);
    check(copied.path.base == PathBase::SolutionDirectory && copied.path.path != old.path.path
        && copied.revision == old.revision && copied.sha256 == old.sha256
        && read(relative_to(copied.path)) == original_bytes, "The package copy relocates the referenced managed bytes.");
    const auto& alias = std::get<ExternalFileResource>(restored.resources.back().location);
    check(alias.path.base == PathBase::SolutionDirectory && alias.path.path == copied.path.path,
        "Different declarations for the same payload keep a shared target.");
    const auto& outside = std::get<ExternalFileResource>(restored.resources[3].location);
    check(outside.path.base == PathBase::Absolute
        && fs::equivalent(fs::path(std::u8string(outside.path.path.begin(), outside.path.path.end())), fixture.external)
        && std::get<ExternalFileResource>(restored.resources[2].location).path.named_root == root_id
        && saved.unresolved_roots == std::vector{unresolved_id}
        && std::ranges::all_of(saved.external_dependencies, [&](const auto& path) { return fs::equivalent(path, fixture.external); })
        && !saved.external_dependencies.empty(), "Outside files keep their targets and named-root requirements are reported.");
    const auto& selection = std::get<DirectorySelection>(std::get<ImageSequenceResource>(restored.resources[5].location).selection);
    const auto& outputs = std::get<OutputPathResource>(restored.resources[6].location);
    check(selection.filename_pattern == "*.bin" && read(relative_to(selection.directory) / u8"帧.bin") == "selected"
        && !fs::exists(relative_to(selection.directory) / "notes.txt")
        && fs::is_empty(relative_to(outputs.directory)) && !fs::exists(destination / "unrelated.bin"),
        "Sequence transfer keeps only matching files and output directories start empty.");
    check(read(destination / "sentinel.txt") == "unrelated destination file"
        && read(fixture.manifest) == original && read(fixture.managed) == original_bytes,
        "The source package and pre-existing destination files keep their bytes.");

    auto execution = take(open_solution_execution(document, fixture.tools));
    output_is(start(execution), 'a');
    const auto independent_dir = fixture.scratch.path / u8"独立副本";
    const auto independent = take(transfer_solution(independent_dir, draft, SolutionTransfer::IndependentCopy, &source));
    const auto clone = take(reload_saved_solution(independent_dir, independent.solution, fixture.tools, roots));
    const auto& cloned_flow = clone.solution().workflows.front();
    const auto& cloned_node = cloned_flow.nodes.front();
    check(clone.solution().id != draft.id && clone.solution().revision == 1 && cloned_flow.id != workflow
        && cloned_node.id != node && cloned_flow.entries == std::vector{cloned_node.id}
        && clone.solution().resources.front().id != draft.resources.front().id
        && cloned_node.resources.front().resource.id == clone.solution().resources.front().id,
        "Independent copies allocate identities and remap entries plus resource-slot references.");
    check(clone.solution().layouts.front().workflow == cloned_flow.id && clone.solution().layouts.front().nodes.front().node == cloned_node.id
        && clone.solution().layouts.front().nodes.front().class_palette == palette,
        "Independent copies remap layout identities with the domain objects.");
    auto copied_execution = take(open_solution_execution(clone, fixture.tools));
    output_is(start(copied_execution, cloned_flow.id), 'a');

    const auto later = take(save_solution(document, restored));
    rejected(reload_saved_solution(destination, saved.solution, fixture.tools, roots), ErrorCode::SolutionSaveConflict,
        "Refreshing a stale save baseline cannot adopt a later manifest.");
    const auto current = take(reload_saved_solution(destination, later.solution, fixture.tools, roots));
    check(current.same_manifest(take(load_solution(destination, fixture.tools, roots))) && !current.same_manifest(document),
        "Refresh identifies the exact committed manifest bytes.");

    const auto empty_package = fixture.scratch.path / u8"首次显式目录";
    auto new_draft = draft;
    new_draft.resources.erase(new_draft.resources.begin() + 3, new_draft.resources.end());
    new_draft.resources.front().location = ExternalFileResource{{PathBase::SolutionDirectory, "payload.bin", {}},
        ResourceReadPolicy::PerInvocation, {}, {}};
    write(empty_package / "payload.bin", "first");
    const auto new_save = take(transfer_solution(empty_package, new_draft, SolutionTransfer::SaveAs, nullptr, roots));
    const auto new_document = take(reload_saved_solution(empty_package, new_save.solution, fixture.tools, roots));
    auto new_execution = take(open_solution_execution(new_document, fixture.tools));
    output_is(start(new_execution), 'f');
    check(new_save.solution.id == new_draft.id && new_save.unresolved_roots.empty()
        && std::get<ExternalFileResource>(new_save.solution.resources.front().location).path.path == "payload.bin",
        "Initial publication uses its explicit destination as the first relative base.");
}

void transfer_failures_and_explicit_copy()
{
    Fixture fixture;
    const auto source = fixture.open();
    const auto manifest = read(fixture.manifest);
    const auto& external = std::get<ExternalFileResource>(source.solution().resources.back().location);
    const auto path = take(copy_external_resource_into_solution(source, external));
    const auto installed = fixture.package / fs::path(std::u8string(path.path.begin(), path.path.end()));
    check(path.base == PathBase::SolutionDirectory && read(installed) == "external"
        && read(fixture.external) == "external" && read(fixture.manifest) == manifest,
        "An explicit copy creates a separate package file and leaves declarations unchanged until save.");
    write(fixture.external, "changed input");
    check(read(installed) == "external", "The copied file does not follow writes to the original external file.");
    rejected(transfer_solution(fixture.package, source.solution(), SolutionTransfer::SaveAs, &source),
        ErrorCode::SolutionAlreadyExists, "A transfer cannot replace an existing manifest.");

    auto draft = source.solution();
    draft.resources.push_back({id<ResourceId>('3'), "Digest mismatch after staging", ResourceKind::Binary,
        ExternalFileResource{{PathBase::SolutionDirectory, path.path, {}}, ResourceReadPolicy::PerInvocation,
            std::string(original_digest), {}}});
    const auto existing_target = fixture.scratch.path / "existing-destination";
    write(existing_target / "sentinel.bin", "keep");
    rejected(transfer_solution(existing_target, draft, SolutionTransfer::SaveAs, &source),
        ErrorCode::ResourceVersionMismatch, "A content mismatch fails the staged transfer.");
    check(std::distance(fs::directory_iterator(existing_target), fs::directory_iterator{}) == 1
        && read(existing_target / "sentinel.bin") == "keep", "Rollback removes staged assets without touching pre-existing contents.");
    const auto absent_parent = fixture.scratch.path / "absent-parent";
    rejected(transfer_solution(absent_parent / "new-destination", draft, SolutionTransfer::SaveAs, &source),
        ErrorCode::ResourceVersionMismatch, "A failed transfer can target an absent package directory.");
    check(!fs::exists(absent_parent) && read(fixture.manifest) == manifest && read(fixture.managed) == original_bytes,
        "Failed publication removes newly created empty directories and preserves the source files.");

    const auto competing_target = fixture.scratch.path / "competing-transfer";
    const auto export_model = [&] {
        return transfer_solution(competing_target, source.solution(), SolutionTransfer::SaveAs, &source);
    };
    auto first = std::async(std::launch::async, export_model);
    auto second = std::async(std::launch::async, export_model);
    const auto a = first.get();
    const auto b = second.get();
    check(std::holds_alternative<SolutionTransferResult>(a) != std::holds_alternative<SolutionTransferResult>(b),
        "Competing transfers admit a single package publication.");
    const auto& committed = std::holds_alternative<SolutionTransferResult>(a) ? a : b;
    const auto& refused = std::holds_alternative<SolutionTransferResult>(a) ? b : a;
    rejected(refused, ErrorCode::SolutionAlreadyExists, "The competing transfer reports the occupied destination.");
    const auto loaded = take(reload_saved_solution(competing_target, std::get<SolutionTransferResult>(committed).solution, fixture.tools));
    check(loaded.diagnostics().empty()
        && std::distance(fs::directory_iterator(competing_target), fs::directory_iterator{}) == 2,
        "The published manifest only refers to the winning transfer's assets.");

    const auto partial = std::string{"{\n  \"future_option\": true,"} + manifest.substr(1);
    write(fixture.manifest, partial);
    const auto unknown = fixture.open();
    const auto untouched = fixture.scratch.path / "uncreated";
    rejected(transfer_solution(untouched, unknown.solution(), SolutionTransfer::IndependentCopy, &unknown),
        ErrorCode::UnsupportedSolutionContent, "Unknown fields are protected from lossy transfer.");
    rejected(copy_external_resource_into_solution(unknown, external), ErrorCode::UnsupportedSolutionContent,
        "Unknown metadata also prevents explicit package resource changes.");
    check(!fs::exists(untouched) && read(fixture.manifest) == partial,
        "Rejected transfers leave the source manifest and target untouched.");
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
        "Failed saves preserve both input files.");
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

void failures(PayloadStorage storage)
{
    Fixture fixture({}, storage);
    const auto source = fixture.open();
    const auto original = read(fixture.manifest);
    auto edited = source.solution();
    edited.name = "Edited";
    std::vector<ManagedResourceWrite> writes;
    std::vector<std::pair<SaveStage, std::size_t>> faults{{SaveStage::ManifestWrite, 1}, {SaveStage::Commit, 2}};
    if (storage == PayloadStorage::Managed) {
        edited.resources.push_back({id<ResourceId>('3'), "New managed resource", ResourceKind::Binary, ManagedResource{}});
        writes = {payload(), payload("another managed file", '3')};
        faults = {{SaveStage::ResourceWrite, 2}, {SaveStage::ManifestWrite, 3}, {SaveStage::Commit, 4}};
    }
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

void conflict(PayloadStorage storage)
{
    Fixture fixture({}, storage);
    const bool replaces_payload = storage == PayloadStorage::Managed;
    const auto first_payload = replace();
    const auto second_payload = replace("different");
    const std::span<const ManagedResourceWrite> first_writes = replaces_payload ? std::span<const ManagedResourceWrite>{first_payload}
        : std::span<const ManagedResourceWrite>{};
    const std::span<const ManagedResourceWrite> second_writes = replaces_payload ? std::span<const ManagedResourceWrite>{second_payload}
        : std::span<const ManagedResourceWrite>{};
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
        return save_detail::save_solution(first_source, first_edit, first_writes, [&](SaveStage stage) -> std::optional<Diagnostic> {
            if (stage == SaveStage::Commit) { committing.pause(); }
            return {};
        });
    });
    check(committing.await(), "The first writer has staged its manifest.");
    second = std::async(std::launch::async, [&] {
        return save_detail::save_solution(second_source, second_edit, second_writes, [&](SaveStage stage) -> std::optional<Diagnostic> {
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
    check(committed.solution.name == first_edit.name && committed.solution.revision == initial_revision + 1
        && committed.cleanup.removed == (replaces_payload ? 1u : 0u), "The first writer commits the expected new revision.");
    rejected(second.get(), ErrorCode::SolutionSaveConflict, "The second writer is rejected with a stale revision conflict.");
    check(!second_staged && payload_files(fixture) == 1, "The stale writer stages no files.");
    const auto installed = replaces_payload ? verify_replacement(fixture, committed.solution) : fixture.managed;
    const auto current = fixture.open();
    check(current.solution().name == first_edit.name, "The winning edit is present after reloading.");
    current_output_is(fixture, replaces_payload ? 'x' : 'a');

    const auto outside_edit = read(fixture.manifest) + " \n";
    write(fixture.manifest, outside_edit);
    const auto outside = fixture.open();
    check(outside.solution().revision == current.solution().revision, "The external change retains the numeric revision.");
    rejected(save_solution(current, current.solution()), ErrorCode::SolutionSaveConflict,
        "A manifest byte change conflicts even at the same numeric revision.");
    check(read(fixture.manifest) == outside_edit, "The externally edited manifest is preserved.");

    const auto late_edit = outside_edit + " ";
    const auto changed_during_save = save_detail::save_solution(outside, outside.solution(), second_writes,
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
        && payload_files(fixture) == 1 && read(installed) == (replaces_payload ? replacement_bytes : original_bytes)
        && read(fixture.external) == "external", "A configuration save preserves the existing file bytes.");
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
void reusable_packages()
{
    const Fixture fixture;
    const auto tools = registry({}, true);
    const auto source = fixture.open();
    auto draft = source.solution();
    auto& flow = draft.workflows.front();
    auto& reader = flow.nodes.front();
    flow.admission = {2, 3, AdmissionOverflow::Reject};
    const auto frame_input = key<InputId>("frame");
    flow.interface.inputs.push_back({frame_input, "Frame", DataType::integer()});
    flow.trigger = WorkflowTrigger{TriggerMode::External, key<SchemaId>("subgraph.frames"), frame_input};
    const auto child = reader.id, group = id<NodeId>('2');
    const auto port = key<OutputId>("byte");
    const auto variable = id<VariableId>('6');
    draft.variables.push_back({variable, "Offset", DataType::integer(), ConfigValue::integer(2)});
    const auto numbers = key<InputId>("numbers");
    const auto index = key<InputId>("index");
    reader.bindings = {
        {key<InputId>("offset"), {ScopedInputReference{flow.id, group, numbers, {DynamicElementSelection{IndexGlobalReference{variable, 0, {}}}}}}},
        {key<InputId>("second"), {ScopedInputReference{flow.id, group, numbers, {DynamicElementSelection{IndexInputReference{flow.id, group, index, {}}}}}}}};
    const auto frame = key<CoordinateSystemId>("subgraph.pixels");
    draft.coordinates.push_back({frame, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded});
    auto unused = draft.coordinates.front();
    unused.id = key<CoordinateSystemId>("subgraph.unused");
    draft.coordinates.push_back(unused);
    const InputDescriptor position{key<InputId>("origin"), "Origin", DataType::point2d(), false,
        take(ConfigValue::point2d({frame, MeasurementUnit::Pixel, 0, 0}))};
    const InputDescriptor list{numbers, "Numbers", DataType::list(DataType::integer()), false,
        take(ConfigValue::list(DataType::integer(), {ConfigValue::integer(10), ConfigValue::integer(20)}))};
    const InputDescriptor cursor{index, "Index", DataType::integer(), false, ConfigValue::integer(0)};
    const PublicInterface ports{key<SchemaId>("reusable.byte"), 3, {position, list, cursor}, {{port, "Byte", DataType::integer(), {}}}, {}};
    flow.nodes.push_back({group, "Read group", GroupConfiguration{ports, {child}, {{port, {flow.id, child, port, {}}}}, {}}});
    flow.entries = {group};
    flow.containment = {{group, child}};
    const ClassPalette palette{{255, {128, 0, 255}}};
    draft.layouts = {{flow.id, {{child, 14, 25, palette}}}};
    const auto directory = fixture.scratch.path / "group-export";
    take(export_subgraph_package(directory, draft, flow.id, group, tools, &source));
    rejected(export_subgraph_package(directory, draft, flow.id, group, tools, &source), ErrorCode::SolutionAlreadyExists,
        "Export never overwrites an existing reusable package.");
    const auto wf_package = fixture.scratch.path / "workflow-export";
    take(export_subgraph_package(wf_package, draft, flow.id, {}, tools, &source));

    Solution destination{id<SolutionId>('7'), "Host", 1, 1,
        {{id<WorkflowId>('8'), "Host workflow", {key<SchemaId>("subgraph.host"), 1, {}, {}, {}}}}};
    destination.variables = draft.variables;
    destination.coordinates = {draft.coordinates.front()};
    const auto host_directory = fixture.scratch.path / ("host-" + std::string(64, 'h'));
    fs::create_directory(host_directory);
    take(write_new_solution(host_directory, destination));
    const auto host = take(load_solution(host_directory, tools));
    const auto target = destination.workflows.front().id;
    const auto baseline = read(host_directory / solution_manifest_name);
    const auto count_files = [&] { return std::distance(fs::recursive_directory_iterator(host_directory), fs::recursive_directory_iterator{}); };
    const auto before = count_files();
    auto wrong_host = destination;
    wrong_host.variables.front().id = id<VariableId>('3');
    const auto missing = import_subgraph_package(directory, wrong_host, target, {}, tools, &host);
    rejected(missing, ErrorCode::MissingCapability, "A matching display name cannot replace a required host identity.");
    check(std::get<Diagnostic>(missing).location.variable == variable, "The diagnostic identifies the required variable.");
    rejected(import_subgraph_package(directory, destination, target, {}, tools, &host, {false, true}),
        ErrorCode::MissingCapability, "A standalone host without node state rejects the package.");
    rejected(import_subgraph_package(directory, destination, target, {}, tools, &host, {true, false}),
        ErrorCode::MissingCapability, "A host without file leases rejects the package.");
    wrong_host = destination;
    wrong_host.coordinates.front().origin.x = 100;
    rejected(import_subgraph_package(directory, wrong_host, target, {}, tools, &host), ErrorCode::CoordinateMismatch,
        "Conflicting definitions under one stable coordinate ID cannot be merged.");
    wrong_host = destination;
    wrong_host.workflows.front().interface.id = ports.id;
    wrong_host.workflows.front().interface.version = ports.version;
    rejected(import_subgraph_package(directory, wrong_host, target, {}, tools, &host), ErrorCode::InvalidSchema,
        "A host cannot reuse the imported public Schema identity/version with a different port set.");
    check(read(host_directory / solution_manifest_name) == baseline && count_files() == before,
        "Validation and late import failures preserve the host manifest and leave no copied assets.");

    const auto first = take(import_subgraph_package(directory, destination, target, {}, tools, &host));
    const auto second = take(import_subgraph_package(directory, first.solution, target, {}, tools, &host));
    check(first.group && second.group && first.group != second.group && second.solution.coordinates.size() == 1
        && second.solution.resources.size() == 2 && second.solution.resources[0].id != second.solution.resources[1].id,
        "Repeated imports create different Group/resource identities and share only the compatible coordinate definition.");
    auto final = second.solution;
    auto& imported_flow = final.workflows.front();
    check(imported_flow.admission == WorkflowAdmission{}, "Importing a Group preserves the host workflow's own admission settings.");
    check(!imported_flow.trigger, "Importing a Group does not copy the source workflow's entry trigger.");
    const auto& instance = std::get<GroupConfiguration>(find_node(imported_flow, *first.group)->configuration);
    const auto instance_child = instance.entries.at(0);
    check(instance.interface.id == ports.id && instance.interface.version == 3 && instance_child != child
        && instance.outputs.front().source.node == instance_child && instance.outputs.front().source.workflow == target
        && final.layouts.front().nodes.front().node == instance_child && final.layouts.front().nodes.front().class_palette == palette,
        "Public Schema/version, body, exported result and layout rebase on the new owning workflow.");
    const auto& bindings = find_node(imported_flow, instance_child)->bindings;
    const auto& outer = std::get<ScopedInputReference>(bindings[0].sources.front());
    const auto& nested = std::get<DynamicElementSelection>(outer.selector.front()).source;
    check(outer.workflow == target && outer.group == first.group && std::get<IndexGlobalReference>(nested).variable == variable
        && std::get<IndexGlobalReference>(nested).required_revision == 0,
        "A variable used only by a dynamic index remains a versioned reference to the exact host identity.");
    const auto& second_outer = std::get<ScopedInputReference>(bindings[1].sources.front());
    const auto& nested_input = std::get<IndexInputReference>(std::get<DynamicElementSelection>(second_outer.selector.front()).source);
    check(second_outer.group == first.group && nested_input.workflow == target && nested_input.group == first.group && nested_input.input == index,
        "Group input and nested index input both rebase to the destination instance.");
    imported_flow.entries = {*first.group};
    const auto saved = take(save_solution(host, final));
    check(saved.solution.resources.size() == 2 && std::get<ManagedResource>(saved.solution.resources.front().location).revision == 7,
        "The normal save transaction can publish imported immutable managed versions.");
    const auto& asset_path = std::get<ManagedResource>(saved.solution.resources.front().location).path.path;
    const auto long_asset = host_directory / fs::path(std::u8string(asset_path.begin(), asset_path.end()));
    check(long_asset.native().size() > MAX_PATH && take(read_file_bytes(long_asset))->size() == original_bytes.size(),
        "An imported managed asset longer than MAX_PATH is published and readable at its full path.");
    const auto as_workflow = take(import_subgraph_package(wf_package, saved.solution, {}, {}, tools, &host));
    check(!as_workflow.group && as_workflow.workflow != flow.id && as_workflow.solution.workflows.size() == 2,
        "A workflow package becomes a separate workflow in the target solution.");
    check(find_workflow(as_workflow.solution, as_workflow.workflow)->admission == flow.admission,
        "A complete workflow carries its in-flight limit, waiting capacity and overflow response across the package boundary.");
    check(find_workflow(as_workflow.solution, as_workflow.workflow)->trigger == flow.trigger,
        "An imported workflow keeps its trigger mapping to its own public frame input.");
    const auto after = count_files();
    const auto saved_manifest = read(host_directory / solution_manifest_name);
    fs::path bundled_asset;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().filename() != subgraph_manifest_name) {
            check(bundled_asset.empty(), "The fixture exports exactly one asset.");
            bundled_asset = entry.path();
        }
    }
    check(!bundled_asset.empty(), "The exported package contains its reader's asset.");
    fs::rename(bundled_asset, fixture.scratch.path / "held-package-asset");
    const auto missing_asset = import_subgraph_package(directory, saved.solution, target, {}, tools, &host);
    rejected(missing_asset, ErrorCode::ResourceNotFound, "A missing package asset must reject the import.");
    check(std::get<Diagnostic>(missing_asset).location.resource == id<ResourceId>('1')
        && count_files() == after && read(host_directory / solution_manifest_name) == saved_manifest,
        "A missing file identifies its resource and leaves the host intact.");
    write(bundled_asset, "bad");
    rejected(import_subgraph_package(directory, saved.solution, target, {}, tools, &host), ErrorCode::ResourceVersionMismatch,
        "Tampered package bytes reject the import.");
    check(count_files() == after, "A failed integrity check removes all staged files.");
    fs::rename(directory, fixture.scratch.path / "moved-group");
    fs::rename(wf_package, fixture.scratch.path / "moved-workflow");
    fs::rename(fixture.managed, fixture.scratch.path / "moved-input");
    auto execution = take(open_solution_execution(take(load_solution(host_directory, tools)), tools));
    const auto no_values = start(execution, target);
    const auto initial = take(no_values.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(initial.progress.status == ExecutionStatus::Failed && no_values.request().globals().size() == 1
        && no_values.request().globals().front().variable == variable && no_values.request().globals().front().revision == 0
        && std::ranges::any_of(initial.progress.diagnostics, [](const auto& issue) { return issue.code == ErrorCode::IndexOutOfRange; }),
        "The new host session supplies the configured initial index 2, outside its two-element Group input.");
    const auto origin = key<SchemaId>("subgraph.test.host");
    const std::array globals{GlobalVariableEntry{variable, DataValue::integer(1), 0,
        ExternalUpdateOrigin{execution.engine->session(), origin}}};
    const auto request = take(RunRequest::create(execution.snapshot, WorkflowTarget{target}, {},
        {TriggerKind::Manual, origin}, {}, {}, globals));
    auto submitted = execution.engine->submit(request);
    if (const auto* rejected = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(rejected->diagnostic.message); }
    const auto run = std::get<RunHandle>(submitted);
    const auto completed = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    if (completed.progress.status != ExecutionStatus::Succeeded && !completed.progress.diagnostics.empty()) {
        throw std::runtime_error(completed.progress.diagnostics.front().message);
    }
    check(completed.progress.status == ExecutionStatus::Succeeded, "The imported Group still runs after moving the source and both packages.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view group = argc == 2 ? argv[1] : "";
        if (group == "failures") { failures(PayloadStorage::Managed); }
        else if (group == "conflict") { conflict(PayloadStorage::Managed); }
        else if (group == "cleanup") { cleanup(); }
        else if (group == "external-failures") { failures(PayloadStorage::External); }
        else if (group == "external-conflict") { conflict(PayloadStorage::External); }
        else if (group == "subgraph") { reusable_packages(); }
        else if (group == "transfer") {
            transfer_packages();
            transfer_failures_and_explicit_copy();
        } else { throw std::runtime_error("Expected failures, conflict, cleanup, external-failures, external-conflict or transfer."); }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
