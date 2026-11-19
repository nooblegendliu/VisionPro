#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/execution_plan.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <semaphore>
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

class ProbeGate {
public:
    void pause_before_read()
    {
        if (arrivals_.fetch_add(1) >= 2) { return; }
        entered_.release();
        if (!resumed_.try_acquire_for(10s)) { throw std::runtime_error("The test did not release the probe."); }
    }

    [[nodiscard]] bool await() { return entered_.try_acquire_for(5s); }
    void release() { resumed_.release(2); }

private:
    std::atomic<unsigned> arrivals_{0};
    std::counting_semaphore<2> entered_{0};
    std::counting_semaphore<2> resumed_{0};
};

struct ReleaseProbe {
    ProbeGate& gate;
    ~ReleaseProbe() { gate.release(); }
};

class ByteTool final : public ToolInstance {
public:
    ByteTool(const ToolExecutionConfiguration& configuration, bool use_file, std::shared_ptr<ProbeGate> gate)
        : ToolInstance(configuration), use_file_(use_file), gate_(std::move(gate)) {}
protected:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        if (gate_) { gate_->pause_before_read(); }
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
    std::shared_ptr<ProbeGate> gate_;
};

class ByteFactory final : public ToolInstanceFactory {
public:
    ByteFactory(std::shared_ptr<std::atomic<int>> calls, bool use_file, std::shared_ptr<ProbeGate> gate)
        : calls_(std::move(calls)), use_file_(use_file), gate_(std::move(gate)) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        ++*calls_;
        return std::shared_ptr<ToolInstance>{std::make_shared<ByteTool>(configuration, use_file_, gate_)};
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
    bool use_file_;
    std::shared_ptr<ProbeGate> gate_;
};

ToolRegistry catalog(const std::shared_ptr<std::atomic<int>>& calls, const std::shared_ptr<ProbeGate>& probe)
{
    const auto registration = [&](std::string_view name, bool use_file) {
        const auto schema = key<SchemaId>(name);
        ToolDescriptor descriptor{key<ToolTypeId>(name), std::string(name), 1, {schema, 1, {}}, {}, {},
            {{key<OutputId>("byte"), "Byte", DataType::integer()}}, {}};
        if (use_file) { descriptor.resources.push_back({key<SchemaId>("payload"), ResourceKind::Binary}); }
        return ToolRegistration{std::move(descriptor), {schema, 1, 1, {}},
            std::make_shared<ByteFactory>(calls, use_file, use_file ? nullptr : probe)};
    };
    auto selected = registration("s63.selected_file", false);
    InputDescriptor file{key<InputId>("file"), "Selected file", DataType::resource_reference(), true, {}, {}, {}, {}};
    file.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::RunOutput};
    file.binding.allow_dynamic_index = true;
    selected.descriptor.inputs = {file};
    const auto definitions = std::array{registration("s37.probe", false), registration("s37.read", true), std::move(selected)};
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

enum class ResourceFixtures { ProtectedFiles, ExternalFiles };

struct Fixture {
    ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"检测方案";
    const fs::path root = scratch.path / u8"命名根";
    const fs::path dynamic = scratch.path / u8"变化.bin";
    const fs::path managed = package / u8"resources/托管.bin";
    const fs::path external = root / u8"外部.bin";
    const std::array<NamedResourceRoot, 1> roots{NamedResourceRoot{key<SchemaId>("fixture.root"), root}};
    const std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0);
    const ToolRegistry registry;

    explicit Fixture(const std::shared_ptr<ProbeGate>& probe = {}) : registry(catalog(calls, probe))
    {
        write(managed, "abc");
        write(external, "abc");
        write(dynamic, "abc");
    }

    Solution model(ResourceFixtures files = ResourceFixtures::ProtectedFiles) const
    {
        const bool use_digests = files == ResourceFixtures::ProtectedFiles;
        const ResourcePath package_path{PathBase::SolutionDirectory, "resources/托管.bin", {}};
        const ResourcePath named_path{PathBase::NamedRoot, "外部.bin", roots.front().id};
        const auto per_invocation = [](ResourcePath path) -> ResourceLocation {
            return ExternalFileResource{std::move(path), ResourceReadPolicy::PerInvocation, {}, {}};
        };
        const ResourceDefinition package_file{id<ResourceId>('1'), use_digests ? "Managed" : "Package file", ResourceKind::Binary,
            use_digests ? ResourceLocation{ManagedResource{package_path, 7, std::string(digest)}} : per_invocation(package_path)};
        const ResourceDefinition named_file{id<ResourceId>('2'), use_digests ? "External" : "Named file", ResourceKind::Binary,
            use_digests ? ResourceLocation{ExternalFileResource{named_path, ResourceReadPolicy::SnapshotLocked, std::string(digest), 9}}
                : per_invocation(named_path)};
        const ResourceDefinition changing_file{id<ResourceId>('3'), "Dynamic", ResourceKind::Binary,
            per_invocation({PathBase::Absolute, utf8(dynamic), {}})};
        const auto version = [use_digests](std::uint64_t revision) {
            return use_digests ? std::optional{revision} : std::nullopt;
        };
        return {id<SolutionId>(), "文件验收", 1, 12,
            {flow('1', '1', '2', {package_file.id, ResourceKind::Binary, version(7)}),
                flow('2', '3', '4', {named_file.id, ResourceKind::Binary, version(9)}),
                flow('3', '5', '6', {changing_file.id, ResourceKind::Binary, {}})},
            {package_file, named_file, changing_file}, {}, {}, {}, {}};
    }
};

RunArguments arguments(char workflow)
{
    return {WorkflowTarget{id<WorkflowId>(workflow)}, {}, {TriggerKind::Manual, key<SchemaId>("s37.test")}};
}

RunRequest request(const ConfigSnapshot& snapshot, char workflow)
{
    const auto run = arguments(workflow);
    return take(RunRequest::create(snapshot, run.target, run.inputs, run.trigger));
}

RunHandle accepted(RunSubmission submission)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
    return std::get<RunHandle>(std::move(submission));
}

void output_is(RunSubmission submission, std::int64_t byte)
{
    const auto run = accepted(std::move(submission));
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

void external_execution()
{
    Fixture fixture;
    const auto model = fixture.model(ResourceFixtures::ExternalFiles);
    write(fixture.external, "named root");
    write(fixture.dynamic, "direct file");
    take(write_new_solution(fixture.package, model));
    const auto document = take(load_solution(fixture.package, fixture.registry, fixture.roots));
    check(document.can_save() && document.diagnostics().empty(), "All three external path bases resolve.");
    auto session = take(WorkflowSession::create(fixture.registry));
    const auto active = take(activate_solution(*session, document));
    output_is(session->submit(request(active, '1')), 'a');
    output_is(session->submit(request(active, '2')), 'n');
    output_is(session->submit(request(active, '3')), 'd');

    const auto plan = take(ExecutionPlan::create(request(active, '1'), fixture.registry,
        {.resource_leases = {ResourceKind::Binary}}));
    const auto provider = take(FileResourceProvider::create(model, fixture.package, fixture.roots));
    const auto& reader = model.workflows.front().nodes.back();
    const auto* planned = plan.tool(reader.id);
    check(planned != nullptr, "The external reader is present in the execution plan.");
    const auto& configuration = planned->configuration();
    const auto lease = std::dynamic_pointer_cast<const FileResourceLease>(
        take(provider->acquire(configuration, configuration.data().resources.front())));
    check(lease && fs::equivalent(lease->path(), fixture.managed), "The lease resolves to the package-relative file.");
    write(fixture.managed, "xyz");
    constexpr std::string_view previous_bytes = "abc";
    check(std::ranges::equal(lease->bytes(), std::as_bytes(std::span(previous_bytes))),
        "An acquired external lease retains its bytes after the disk file changes.");
    output_is(session->submit(request(active, '1')), 'x');

    fs::remove(fixture.dynamic);
    const int before = *fixture.calls;
    rejected_run(session->submit(request(active, '3')), ErrorCode::ResourceNotFound,
        "A missing external file is rejected during preflight.");
    check(*fixture.calls == before, "Resource preflight creates no tool instances.");
    rejected(load_and_activate_solution(*session, fixture.package, fixture.registry, fixture.roots), ErrorCode::ResourceNotFound,
        "A candidate with a missing external file cannot replace the session.");
    check(session->inspect().active->same_capture(active), "The missing file leaves the active capture intact.");
    output_is(session->submit(request(active, '2')), 'n');
}

void dynamic_resources()
{
    Fixture fixture;
    auto model = fixture.model(ResourceFixtures::ExternalFiles);
    const auto index = key<InputId>("index");
    const auto assets = key<InputId>("assets");
    const auto selected = key<InputId>("file");
    const auto asset_type = DataType::resource_reference();
    for (auto& flow : model.workflows) {
        const auto producer = flow.nodes.front().id;
        auto& consumer = flow.nodes.back();
        consumer.configuration = ToolConfiguration{key<ToolTypeId>("s63.selected_file"), 1};
        consumer.resources.clear();
        flow.interface.inputs = {{assets, "Assets", DataType::list(asset_type)}, {index, "Index", DataType::integer()}};
        std::optional<NodeId> group;
        if (flow.id == model.workflows.back().id) { group = id<NodeId>('7'); }
        const IndexSource source = flow.id == id<WorkflowId>('2')
            ? IndexSource{IndexOutputReference{flow.id, producer, key<OutputId>("byte"), {}}}
            : IndexSource{IndexInputReference{flow.id, group, index, {}}};
        consumer.bindings = {{selected, {ScopedInputReference{flow.id, group, assets, {DynamicElementSelection{source}}}}}};
        if (group) {
            const auto consumer_id = consumer.id;
            auto ports = flow.interface;
            ports.id = key<SchemaId>("s63.pick_file");
            for (auto& port : ports.inputs) { port.binding.allowed_sources = {BindingSourceKind::PublicInput}; }
            flow.nodes.push_back({*group, "Pick a file", GroupConfiguration{ports, {producer}},
                {{assets, {ScopedInputReference{flow.id, {}, assets, {}}}}, {index, {ScopedInputReference{flow.id, {}, index, {}}}}}});
            flow.containment = {{*group, producer}, {*group, consumer_id}};
            flow.entries = {*group};
        }
    }
    take(write_new_solution(fixture.package, model));
    const auto document = take(load_solution(fixture.package, fixture.registry, fixture.roots));
    const auto execution = take(open_solution_execution(document, fixture.registry));
    fs::remove(fixture.dynamic);
    const auto present = take(DataValue::resource_reference({id<ResourceId>('1'), ResourceKind::Binary, {}}));
    const auto missing = take(DataValue::resource_reference({id<ResourceId>('3'), ResourceKind::Binary, {}}));
    const auto run = [&](char workflow, std::int64_t position, const std::vector<DataValue>& files, bool debug_group = false) {
        auto args = arguments(workflow);
        if (debug_group) { args.target = NodeTarget{id<WorkflowId>(workflow), id<NodeId>('7')}; }
        args.inputs = {{index, DataValue::integer(position)}, {assets, take(DataValue::list(asset_type, files))}};
        return execution.engine->submit(take(RunRequest::create(execution.snapshot, args.target, args.inputs, args.trigger)));
    };
    output_is(run('1', 0, {present, missing}), 0);
    const auto before = fixture.calls->load();
    const auto refused = run('1', 1, {present, missing});
    rejected_run(refused, ErrorCode::ResourceNotFound, "A request-fixed index validates exactly the selected resource before admission.");
    check(*fixture.calls == before && std::get<RunRejected>(refused).diagnostic.location.resource == id<ResourceId>('3'),
        "Preflight identifies the chosen missing asset without creating tools.");
    output_is(run('2', 1, {present, missing}), 0);
    const auto late = accepted(run('2', 1, {missing, present}));
    const auto done = take(late.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    const auto failure = std::ranges::find(done.progress.diagnostics, ErrorCode::ResourceNotFound, &Diagnostic::code);
    check(done.progress.status == ExecutionStatus::Failed && failure != done.progress.diagnostics.end()
        && failure->location.resource == id<ResourceId>('3') && failure->location.input == selected,
        "An upstream-produced index selects only its actual file and reports missing bytes at the consuming binding.");
    for (auto independent : {false, true}) {
        const auto group = accepted(run('3', 0, {present, missing}, independent));
        const auto status = take(group.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
        check(status.progress.status == ExecutionStatus::Succeeded,
            "A Group can pass the resource candidates to a child without probing unused files, including in independent debugging.");
        rejected_run(run('3', 1, {present, missing}, independent), ErrorCode::ResourceNotFound,
            "A Group child still preflights the actual missing item when its list and index are fixed by this request.");
    }
}

void publication()
{
    const auto gate = std::make_shared<ProbeGate>();
    Fixture fixture(gate);
    auto model = fixture.model(ResourceFixtures::ExternalFiles);
    // Both selected workflows use the same resource ID from their own captures.
    model.workflows[0].nodes.back().resources.front().resource.id = id<ResourceId>('2');
    take(write_new_solution(fixture.package, model));
    const auto manifest = fixture.package / solution_manifest_name;
    const auto original = read(manifest);
    const auto source = take(load_solution(fixture.package, fixture.registry, fixture.roots));
    auto session = take(WorkflowSession::create(fixture.registry));
    const auto base = take(activate_solution(*session, source));

    auto draft = source.solution();
    draft.name = "Edited file-backed draft";
    auto& named = std::get<ExternalFileResource>(draft.resources[1].location);
    named.path.path = "edited.bin";
    const auto edited_file = fixture.root / named.path.path;
    write(edited_file, "initial new content");
    const auto run = arguments('2');

    const auto old = accepted(session->submit(request(base, '1')));
    const auto current = [&] {
        const ReleaseProbe release{*gate};
        check(gate->await(), "The previous run waits before its file-reading successor.");
        fs::remove(fixture.dynamic);
        const auto admitted = accepted(publish_solution_run(*session, base, source, draft, run));
        check(gate->await(), "The published workflow reaches its own probe while the old workflow is blocked.");
        const auto active = session->inspect().active;
        check(active && active->same_capture(admitted.request().snapshot())
            && active->solution().name == draft.name && active->solution().revision == source.solution().revision,
            "Admission publishes the exact unsaved capture.");
        check(old.request().snapshot().same_capture(base) && admitted.identity().snapshot.session == base.identity().session
            && admitted.identity().snapshot.snapshot > base.identity().snapshot,
            "Publication keeps the session and the in-flight capture.");
        write(fixture.external, "old path read at invocation");
        write(edited_file, "new path read at invocation");
        return admitted;
    }();

    output_is(old, 'o');
    output_is(current, 'n');
    check(read(manifest) == original, "Publishing the draft leaves the manifest bytes intact.");

    const auto& active = current.request().snapshot();
    const int before = *fixture.calls;
    check(std::holds_alternative<RunRejected>(publish_solution_run(*session, base, source, draft, run)),
        "Preparation for a stale base cannot publish again.");
    auto missing = draft;
    std::get<ExternalFileResource>(missing.resources[1].location).path.path = "missing.bin";
    const auto refused = publish_solution_run(*session, active, source, missing, run);
    rejected_run(refused, ErrorCode::ResourceNotFound, "A missing file in the chosen workflow prevents admission.");
    check(std::get<RunRejected>(refused).diagnostic.location.resource == id<ResourceId>('2'),
        "Resource preflight reports the chosen resource rather than the missing file outside that scope.");
    auto unrelated = draft;
    unrelated.id = id<SolutionId>('2');
    rejected_run(publish_solution_run(*session, active, source, unrelated, run), ErrorCode::ResultIdentityMismatch,
        "A source document may provide paths only for its own solution.");
    check(*fixture.calls == before && session->inspect().active.value().same_capture(active),
        "Refused drafts start no tools and preserve the previously published capture.");
    output_is(session->submit(request(active, '2')), 'n');
    check(read(manifest) == original, "Failed drafts and resubmission preserve the on-disk revision.");
}

void diagnostics(ResourceFixtures files)
{
    Fixture fixture;
    auto valid = fixture.model(files);
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
    rejected_run(publish_solution_run(*session, active, read_only, read_only.solution(), arguments('1')),
        ErrorCode::UnsupportedSolutionContent, "Unknown semantics cannot be published from a partial model.");
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
        else if (test == "diagnostics") { diagnostics(ResourceFixtures::ProtectedFiles); }
        else if (test == "external-execution") { external_execution(); }
        else if (test == "dynamic-resources") { dynamic_resources(); }
        else if (test == "external-diagnostics") { diagnostics(ResourceFixtures::ExternalFiles); }
        else if (test == "publication") { publication(); }
        else { throw std::runtime_error("Unknown solution file test case."); }
        std::cout << test << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "FAIL after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
