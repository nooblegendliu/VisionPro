#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::infrastructure;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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
    auto value = std::string("00000000-0000-4000-8000-000000000001");
    value.back() = suffix;
    return take(IdFactory::parse<T>(value));
}

template<class T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }

template<class T> void rejected(const Result<T>& value, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code, message);
}

struct ScratchDirectory {
    ScratchDirectory()
        : path(fs::current_path() / ("s37-file-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        if (!fs::create_directory(path)) { throw std::runtime_error("Could not create a unique test directory."); }
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

constexpr std::string_view digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

class ByteTool final : public ToolInstance {
public:
    ByteTool(const ToolExecutionConfiguration& configuration, bool use_file)
        : ToolInstance(configuration), use_file_(use_file) {}
protected:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        std::int64_t byte = 0;
        if (use_file_) {
            const auto& resources = context->data().resources;
            if (resources.size() != 1) { throw std::logic_error("Expected one file lease."); }
            const auto lease = std::dynamic_pointer_cast<const FileResourceLease>(resources.front().lease);
            if (!lease || lease->bytes().empty() || !lease->path().is_absolute()) {
                throw std::logic_error("Expected a resolved file and its immutable bytes.");
            }
            byte = std::to_integer<std::int64_t>(lease->bytes().front());
        }
        return InvocationStart{ToolCompletion{context->data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("byte"), DataValue::integer(byte)}}, {}}};
    }
private:
    bool use_file_;
};

class ByteFactory final : public ToolInstanceFactory {
public:
    ByteFactory(std::shared_ptr<std::atomic<int>> calls, bool use_file) : calls_(std::move(calls)), use_file_(use_file) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        ++*calls_;
        return std::shared_ptr<ToolInstance>{std::make_shared<ByteTool>(configuration, use_file_)};
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
    bool use_file_;
};

ToolRegistry catalog(const std::shared_ptr<std::atomic<int>>& calls)
{
    const auto registration = [&](std::string_view name, bool use_file) {
        const auto schema = key<SchemaId>(name);
        ToolDescriptor descriptor{key<ToolTypeId>(name), std::string(name), 1, {schema, 1, {}}, {}, {},
            {{key<OutputId>("byte"), "Byte", DataType::integer()}}, {}};
        if (use_file) { descriptor.resources.push_back({key<SchemaId>("payload"), ResourceKind::Binary}); }
        return ToolRegistration{std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<ByteFactory>(calls, use_file)};
    };
    const auto definitions = std::array{registration("s37.probe", false), registration("s37.read", true)};
    return take(ToolRegistry::create(definitions));
}

Workflow flow(char suffix, char first, char second, const ResourceReference& resource)
{
    const Node probe{id<NodeId>(first), "Probe", ToolConfiguration{key<ToolTypeId>("s37.probe"), 1}, {}, {}};
    const Node reader{id<NodeId>(second), "Read", ToolConfiguration{key<ToolTypeId>("s37.read"), 1}, {},
        {{key<SchemaId>("payload"), resource}}};
    return {id<WorkflowId>(suffix), "Read file", {key<SchemaId>("s37.flow"), 1, {}, {}, {}},
        {probe, reader}, {{id<ControlLinkId>(suffix), probe.id, {}, reader.id}}, {}, {probe.id}, {}};
}

struct Fixture {
    ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"检测方案";
    const fs::path root = scratch.path / u8"命名根";
    const fs::path dynamic = scratch.path / u8"变化.bin";
    const fs::path managed = package / u8"resources/托管.bin";
    const fs::path external = root / u8"外部.bin";
    const std::array<NamedResourceRoot, 1> roots{NamedResourceRoot{key<SchemaId>("fixture.root"), root}};
    const std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    const ToolRegistry registry = catalog(calls);

    Fixture()
    {
        write(managed, "abc");
        write(external, "abc");
        write(dynamic, "abc");
    }

    Solution model() const
    {
        const ResourceDefinition managed_file{id<ResourceId>('1'), "Managed", ResourceKind::Binary,
            ManagedResource{{PathBase::SolutionDirectory, "resources/托管.bin", {}}, 7, std::string(digest)}};
        const ResourceDefinition locked_file{id<ResourceId>('2'), "External", ResourceKind::Binary,
            ExternalFileResource{{PathBase::NamedRoot, "外部.bin", roots.front().id}, ResourceReadPolicy::SnapshotLocked,
                std::string(digest), 9}};
        const ResourceDefinition changing_file{id<ResourceId>('3'), "Dynamic", ResourceKind::Binary,
            ExternalFileResource{{PathBase::Absolute, utf8(dynamic), {}}, ResourceReadPolicy::PerInvocation, {}, {}}};
        return {id<SolutionId>(), "文件验收", 1, 12,
            {flow('1', '1', '2', {managed_file.id, ResourceKind::Binary, 7}),
                flow('2', '3', '4', {locked_file.id, ResourceKind::Binary, 9}),
                flow('3', '5', '6', {changing_file.id, ResourceKind::Binary, {}})},
            {managed_file, locked_file, changing_file}, {}, {}, {}, {}};
    }
};

RunRequest request(const ConfigSnapshot& snapshot, char workflow)
{
    return take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>(workflow)}, {},
        {TriggerKind::Manual, key<SchemaId>("s37.test")}));
}

void output_is(RunSubmission submission, std::int64_t byte)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
    const auto run = std::get<RunHandle>(std::move(submission));
    const auto done = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(done.outcome == RunWaitOutcome::Satisfied && done.progress.status == ExecutionStatus::Succeeded
        && done.progress.nodes.size() == 2 && done.progress.nodes.back().invocation.has_value(),
        "The two-node file workflow finished.");
    const auto result = take(run.lookup(*done.progress.nodes.back().invocation));
    const auto* output = result ? result->output(key<OutputId>("byte")) : nullptr;
    const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
    check(value && value->as_int64() && *value->as_int64() == byte, "The tool received the expected file bytes.");
}

void rejected_run(const RunSubmission& submission, ErrorCode code, std::string_view message)
{
    const auto* refusal = std::get_if<RunRejected>(&submission);
    check(refusal && refusal->diagnostic.code == code, message);
}

void execution()
{
    Fixture fixture;
    const auto model = fixture.model();
    take(write_new_solution(fixture.package, model));
    const auto document = take(load_solution(fixture.package, fixture.registry, fixture.roots));
    check(document.can_save() && document.diagnostics().empty(), "All three resource path bases resolve.");
    auto loaded = take(open_solution_execution(document, fixture.registry));
    auto session = take(WorkflowSession::create(fixture.registry));
    const auto active = take(activate_solution(*session, document));

    fs::remove(fixture.managed);
    write(fixture.external, "changed");
    write(fixture.dynamic, "xyz");
    output_is(loaded.engine->submit(request(loaded.snapshot, '1')), 'a');
    output_is(session->submit(request(active, '2')), 'a');
    output_is(loaded.engine->submit(request(loaded.snapshot, '3')), 'x');
    write(fixture.dynamic, "new");
    output_is(loaded.engine->submit(request(loaded.snapshot, '3')), 'n');

    fs::remove(fixture.dynamic);
    const int before = *fixture.calls;
    rejected_run(loaded.engine->submit(request(loaded.snapshot, '3')), ErrorCode::ResourceNotFound,
        "A disappeared dynamic file is diagnosed before the first node is started.");
    check(*fixture.calls == before, "Resource preflight creates no tool instances.");

    const auto damaged = take(load_solution(fixture.package, fixture.registry, fixture.roots));
    check(std::ranges::any_of(damaged.diagnostics(), [](const Diagnostic& item) {
        return item.code == ErrorCode::ResourceVersionMismatch && item.location.resource == id<ResourceId>('2');
    }), "A reloaded locked file reports its changed SHA-256.");
    rejected(activate_solution(*session, damaged), ErrorCode::ResourceNotFound, "A failed candidate leaves the active session available.");
    check(session->inspect().active->same_capture(active), "Resource diagnostics preserve the original active capture.");
    output_is(session->submit(request(active, '1')), 'a');
}

void diagnostics()
{
    Fixture fixture;
    auto valid = fixture.model();
    take(write_new_solution(fixture.package, valid));
    const auto manifest = fixture.package / solution_manifest_name;
    const auto original = read(manifest);
    rejected(write_new_solution(fixture.package, valid), ErrorCode::SolutionAlreadyExists, "An existing manifest is protected.");
    check(read(manifest) == original, "A rejected publication preserves the original bytes.");

    const auto drafts = fixture.scratch.path / "draft";
    const auto missing = id<ResourceId>('8');
    std::get<ToolConfiguration>(valid.workflows[0].nodes[0].configuration).type = key<ToolTypeId>("s37.uninstalled");
    valid.workflows[1].nodes.back().resources.front().resource.id = missing;
    take(write_new_solution(drafts, valid));
    const auto document = take(load_solution(drafts, fixture.registry, fixture.roots));
    const auto issues = document.diagnostics();
    check(document.can_save() && document.solution().workflows.size() == 3, "Known drafts remain editable.");
    check(std::ranges::any_of(issues, [](const Diagnostic& item) {
        return item.code == ErrorCode::UnknownTool && item.location.node == id<NodeId>('1');
    }), "The editor sees the missing tool diagnostic.");
    check(std::ranges::any_of(issues, [&](const Diagnostic& item) {
        return item.code == ErrorCode::ResourceNotFound && item.location.resource == missing;
    }), "The editor sees the missing catalog reference.");
    auto scoped = take(open_solution_execution(document, fixture.registry));
    rejected_run(scoped.engine->submit(request(scoped.snapshot, '1')), ErrorCode::UnknownTool, "The selected missing tool is rejected.");
    rejected_run(scoped.engine->submit(request(scoped.snapshot, '2')), ErrorCode::ResourceNotFound, "The selected missing reference is rejected.");
    check(*fixture.calls == 0, "Invalid scopes start no tools.");
    output_is(scoped.engine->submit(request(scoped.snapshot, '3')), 'a');

    auto session = take(WorkflowSession::create(fixture.registry));
    const auto active = take(load_and_activate_solution(*session, fixture.package, fixture.registry, fixture.roots));
    const auto broken = fixture.scratch.path / "broken";
    const auto candidate = broken / solution_manifest_name;
    write(candidate, "{ truncated");
    rejected(load_and_activate_solution(*session, broken, fixture.registry, fixture.roots),
        ErrorCode::InvalidSolutionManifest, "Invalid JSON fails before replacing a session.");
    check(session->inspect().active->same_capture(active), "A damaged manifest preserves the active capture.");

    auto unknown = original;
    const auto config = unknown.find("\"config_version\": 1");
    check(config != std::string::npos, "The generated manifest has the expected tool configuration.");
    unknown.insert(config + std::string_view("\"config_version\": 1").size(), ", \"future_option\": true");
    write(candidate, unknown);
    const auto read_only = take(load_solution(broken, fixture.registry, fixture.roots));
    check(!read_only.can_save() && std::ranges::any_of(read_only.diagnostics(), [](const Diagnostic& item) {
        return item.code == ErrorCode::UnsupportedSolutionContent && item.message.find("/future_option") != std::string::npos;
    }), "Unknown nested fields give a path and retain a known read-only model.");
    const auto other = fixture.scratch.path / "save-as";
    rejected(write_new_solution(other, read_only, read_only.solution()), ErrorCode::UnsupportedSolutionContent,
        "A partial model cannot be saved as a complete manifest.");
    rejected(open_solution_execution(read_only, fixture.registry), ErrorCode::UnsupportedSolutionContent,
        "Unknown semantics cannot be executed from a partial model.");
    rejected(activate_solution(*session, read_only), ErrorCode::UnsupportedSolutionContent, "Unknown semantics cannot replace the active session.");
    check(!fs::exists(other / solution_manifest_name) && read(candidate) == unknown
        && session->inspect().active->same_capture(active), "Both the original file and active session are preserved.");

    auto version = original;
    const auto marker = version.find("\"format_version\": 1");
    check(marker != std::string::npos, "The manifest declares its version.");
    version[marker + std::string_view("\"format_version\": ").size()] = '2';
    write(candidate, version);
    rejected(load_solution(broken, fixture.registry, fixture.roots), ErrorCode::UnsupportedSolutionFormat, "Unknown manifest versions are explicit.");
    write(candidate, std::string("{\"format\":\"other\",") + original.substr(original.find('{') + 1));
    rejected(load_solution(broken, fixture.registry, fixture.roots), ErrorCode::InvalidSolutionManifest, "Duplicate object keys cannot mask configuration.");
    rejected(load_solution(fs::path("relative"), fixture.registry, fixture.roots), ErrorCode::InvalidPath,
        "Loading requires an explicit absolute package directory.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "execution") { execution(); }
        else if (test == "diagnostics") { diagnostics(); }
        else { throw std::runtime_error("Expected execution or diagnostics."); }
        std::cout << test << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
