#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>

#ifdef VISIONPRO_WITH_OPENCV
#include "simulation.hpp"
#include "vision/vision_tools.hpp"
#include "../tools/image_fixtures.hpp"

#include <array>
#include <numbers>
#endif

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using Json = nlohmann::json;
namespace fs = std::filesystem;

int checks = 0;

void check(bool passed, std::string_view message)
{
    ++checks;
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) {
        throw std::runtime_error(std::string(error_code_name(failure->code)) + ": " + failure->message);
    }
    return std::get<T>(std::move(value));
}

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view value) { return take(SchemaIdFactory::parse<T>(value)); }

std::string read_file(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Could not open a fixture file for reading."); }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) { throw std::runtime_error("Could not write a fixture file."); }
}

class ScratchDirectory final {
public:
    ScratchDirectory() : path(fs::current_path() / ("s39-cli-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        if (!fs::create_directory(path)) { throw std::runtime_error("Could not reserve the CLI fixture directory."); }
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const fs::path path;
};

class Handle final {
public:
    explicit Handle(HANDLE handle) : handle_(handle) {}
    ~Handle() { if (valid()) { CloseHandle(handle_); } }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(2 * slashes, L'\\');
    result.push_back(L'"');
    return result;
}

struct CommandResult { DWORD exit_code; std::string output; std::string error; };

enum class OutputMode { File, BrokenPipe };

HANDLE broken_output_pipe(SECURITY_ATTRIBUTES& attributes)
{
    HANDLE reader = nullptr;
    HANDLE writer = nullptr;
    if (!CreatePipe(&reader, &writer, &attributes, 0)) {
        throw std::runtime_error("Could not create the broken-output fixture.");
    }
    CloseHandle(reader);
    return writer;
}

CommandResult invoke(const fs::path& executable, const fs::path& working, const std::vector<std::wstring>& arguments,
    OutputMode output_mode = OutputMode::File)
{
    const auto stdout_path = working / L"stdout.txt";
    const auto stderr_path = working / L"stderr.txt";
    auto command_line = quote_argument(executable.native());
    for (const auto& argument : arguments) { command_line += L" " + quote_argument(argument); }
    PROCESS_INFORMATION information{};
    {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const Handle output(output_mode == OutputMode::BrokenPipe ? broken_output_pipe(inheritable)
            : CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        const Handle error(CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        const Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output.valid() || !error.valid() || !input.valid()) {
            throw std::runtime_error("Could not redirect the CLI test process.");
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input.get();
        startup.hStdOutput = output.get();
        startup.hStdError = error.get();
        if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, working.c_str(), &startup, &information)) {
            throw std::runtime_error("Could not start the CLI test process (Win32 " + std::to_string(GetLastError()) + ").");
        }
    }
    const Handle process(information.hProcess);
    const Handle thread(information.hThread);
    if (WaitForSingleObject(process.get(), 10000) != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 1000);
        throw std::runtime_error("The CLI test process did not exit in time.");
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) { throw std::runtime_error("Could not read the CLI exit code."); }
    return {exit_code, output_mode == OutputMode::File ? read_file(stdout_path) : std::string{}, read_file(stderr_path)};
}

Binding input_binding(std::string_view input, BindingSource source)
{ return {key<InputId>(input), {std::move(source)}}; }

Node tool(char id, std::string_view type, std::string name, std::vector<Binding> bindings = {})
{
    return {uuid<NodeId>(id), std::move(name), ToolConfiguration{key<ToolTypeId>(type), 1}, std::move(bindings), {}};
}

Node add(char id, BindingSource left, BindingSource right)
{
    return tool(id, "visionpro.sim.add", "加法", {input_binding("left", std::move(left)), input_binding("right", std::move(right))});
}

OutputReference output(WorkflowId flow, char node, std::string_view name = "value")
{ return {flow, uuid<NodeId>(node), key<OutputId>(name), {}}; }

ControlLink link(char id, char source, char target, std::optional<ControlOutputId> outlet = {})
{ return {uuid<ControlLinkId>(id), uuid<NodeId>(source), std::move(outlet), uuid<NodeId>(target)}; }

Node collect(WorkflowId flow, char id, std::string_view lanes, bool allow_absent = false)
{
    Binding binding{key<InputId>("values"), {}, BindingCardinality::MultipleSources};
    for (const char lane : lanes) { binding.sources.push_back(output(flow, lane)); }
    if (allow_absent) { binding.availability.accepted_absence.push_back(MissingReason::NotSelected); }
    return tool(id, "visionpro.sim.collect", "汇合", {std::move(binding)});
}

Node deliver(WorkflowId flow, char id, char source)
{ return tool(id, "visionpro.sim.deliver", "交付", {input_binding("values", output(flow, source, "values"))}); }

Workflow fork_flow(const ResourceReference& payload)
{
    const auto flow = uuid<WorkflowId>('1');
    const auto offset = key<InputId>("offset");
    auto entry = tool('1', "visionpro.sim.file_byte", "读文件");
    entry.resources.push_back({key<SchemaId>("payload"), payload});
    // Editable node order is deliberately different from the control order.
    return {flow, "五路汇合", {key<SchemaId>("s39.fork"), 1, {{offset, "Offset", DataType::integer()}}},
        {deliver(flow, '8', '7'), collect(flow, '7', "23456"),
            add('6', output(flow, '1'), ConfigValue::integer(5)),
            add('5', output(flow, '1'), ConfigValue::integer(4)),
            add('4', output(flow, '1'), ConfigValue::integer(3)),
            add('3', output(flow, '1'), ConfigValue::integer(2)),
            add('2', output(flow, '1'), ScopedInputReference{flow, {}, offset, {}}), std::move(entry)},
        {link('1', '1', '2'), link('2', '1', '3'), link('3', '1', '4'), link('4', '1', '5'), link('5', '1', '6'),
            link('6', '2', '7'), link('7', '3', '7'), link('8', '4', '7'), link('9', '5', '7'), link('a', '6', '7'),
            link('b', '7', '8')}, {}, {uuid<NodeId>('1')}, {}};
}

Workflow branch_flow()
{
    const auto flow = uuid<WorkflowId>('2');
    const auto mask = key<InputId>("mask");
    return {flow, "条件分支", {key<SchemaId>("s39.branch"), 1, {{mask, "Mask", DataType::integer()}}},
        {tool('9', "visionpro.sim.select", "选择", {input_binding("mask", ScopedInputReference{flow, {}, mask, {}})}),
            add('a', ConfigValue::integer(10), ConfigValue::integer(1)), add('b', ConfigValue::integer(20), ConfigValue::integer(2)),
            collect(flow, 'c', "ab", true), deliver(flow, 'd', 'c')},
        {link('c', '9', 'a', key<ControlOutputId>("left")), link('d', '9', 'b', key<ControlOutputId>("right")),
            link('e', 'a', 'c'), link('f', 'b', 'c'), link('0', 'c', 'd')}, {}, {uuid<NodeId>('9')}, {}};
}

enum class PayloadStorage { Managed, External };

struct Fixture {
    const fs::path executable;
    const ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"验收 方案";
    const fs::path manifest = package / "solution.json";
    const fs::path payload_file;
    const std::optional<std::uint64_t> payload_revision;

    Fixture(fs::path cli, PayloadStorage storage)
        : executable(std::move(cli)),
          payload_file(storage == PayloadStorage::Managed ? package / u8"resources/托管 文件.bin"
                                                         : scratch.path / u8"外部 输入/文件.bin"),
          payload_revision(storage == PayloadStorage::Managed ? std::optional<std::uint64_t>{7} : std::nullopt)
    {
        write_file(payload_file, "abc");
        const auto relative = payload_file.lexically_relative(package).generic_u8string();
        const ResourcePath path{PathBase::SolutionDirectory, {relative.begin(), relative.end()}, {}};
        auto location = payload_revision
            ? ResourceLocation{ManagedResource{path, *payload_revision,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}}
            : ResourceLocation{ExternalFileResource{path, ResourceReadPolicy::PerInvocation, {}, {}}};
        const ResourceDefinition resource{uuid<ResourceId>(), "固定资源", ResourceKind::Binary, std::move(location)};
        const Solution solution{uuid<SolutionId>(), "命令行验收", solution_format_version, 12,
            {fork_flow({resource.id, resource.kind, payload_revision}), branch_flow()}, {resource}, {}, {}, {}, {}};
        take(write_new_solution(package, solution));
    }

    CommandResult command(std::initializer_list<std::wstring> arguments) const
    { return invoke(executable, scratch.path, arguments); }

    CommandResult run(char flow, std::wstring input) const
    {
        const auto id = uuid<WorkflowId>(flow).to_string();
        return command({L"run", L"--solution", manifest.lexically_relative(scratch.path).native(),
            L"--workflow", std::wstring(id.begin(), id.end()), L"--input", std::move(input)});
    }

    CommandResult save() const { return command({L"save", L"--solution", manifest.native()}); }
};

Json success(const CommandResult& result)
{
    if (result.exit_code != 0) { throw std::runtime_error("CLI command failed: " + result.error + result.output); }
    check(result.error.empty(), "Successful commands leave stderr empty.");
    return Json::parse(result.output);
}

bool text_is(const Json& value, std::string_view expected)
{
    const auto* actual = value.get_ptr<const std::string*>();
    return actual && *actual == expected;
}

const Json& find_by(const Json& items, std::string_view field, std::string_view value)
{
    const auto found = std::ranges::find_if(items, [&](const Json& item) { return text_is(item.at(field), value); });
    if (found == items.end()) { throw std::runtime_error("The report is missing " + std::string(value) + '.'); }
    return *found;
}

const Json& node(const Json& report, char id) { return find_by(report.at("nodes"), "id", uuid<NodeId>(id).to_string()); }

const Json& sources(const Json& call, std::string_view input)
{ return find_by(call.at("bindings"), "target", input).at("sources"); }

const Json& produced(const Json& call, std::string_view name = "value")
{
    const auto& result = find_by(call.at("outputs"), "id", name);
    check(result.at("status") == "Produced", "The expected output was produced.");
    return result.at("value");
}

void edge(const Json& report, char id, bool arrived)
{
    const auto& state = find_by(report.at("control_links"), "id", uuid<ControlLinkId>(id).to_string());
    check(arrived ? state.at("status") == "Arrived" && state.at("close_reason").is_null()
                  : state.at("status") == "Closed" && state.at("close_reason") == "NotSelected",
        "A control edge reports its expected arrival or closure reason.");
}

void skipped(const Json& call)
{
    check(call.at("status") == "Skipped" && call.at("skip_reason") == "NotSelected"
        && call.at("invocation").is_null() && call.at("outputs").empty(), "A skipped node has no fabricated invocation or output.");
}

void check_run(const Json& report, char flow, std::string_view revision)
{
    check(report.at("solution") == uuid<SolutionId>().to_string() && text_is(report.at("revision"), revision)
        && report.at("workflow") == uuid<WorkflowId>(flow).to_string(), "The report addresses the requested on-disk revision and workflow.");
    check(report.at("status") == "Succeeded" && report.at("quiescent") == true && report.at("load_diagnostics").empty()
        && report.at("diagnostics").empty(), "The selected workflow reached quiescence successfully without diagnostics.");
    check(report.at("session").is_string() && report.at("snapshot").is_string() && report.at("run").is_string(),
        "The report includes a typed runtime identity.");

    for (const auto& call : report.at("nodes")) {
        for (const auto& binding : call.at("bindings")) {
            for (const auto& trace : binding.at("sources")) {
                const auto& kind = trace.at("kind");
                if (kind != "result" && kind != "public_input") { continue; }
                check(trace.at("session") == report.at("session") && trace.at("snapshot") == report.at("snapshot")
                    && trace.at("run") == report.at("run") && trace.at("workflow") == report.at("workflow"),
                    "The source addresses the current session, snapshot, workflow and run.");
                if (kind != "result" || trace.at("invocation").is_null()) { continue; }
                const auto& producer = find_by(report.at("nodes"), "id", trace.at("node").get_ref<const std::string&>());
                check(trace.at("scope") == producer.at("scope") && trace.at("invocation") == producer.at("invocation")
                    && trace.at("attempt") == producer.at("attempt"), "A produced value traces the exact producer invocation and attempt.");
            }
        }
    }
}

void verify_fork(const Json& report, std::string_view revision, std::optional<std::uint64_t> payload_revision)
{
    check_run(report, '1', revision);
    const auto& order = report.at("call_order");
    check(order.size() == 8 && order.front() == uuid<NodeId>('1').to_string() && order[6] == uuid<NodeId>('7').to_string()
        && order.back() == uuid<NodeId>('8').to_string(), "Five lane calls complete between the entry and one invocation each of join and delivery.");
    const auto& entry = node(report, '1');
    check(produced(entry) == "97", "The entry reads the first byte of the configured file.");
    const auto& leases = entry.at("resources");
    const auto expected_revision = payload_revision ? Json(std::to_string(*payload_revision)) : Json(nullptr);
    check(leases.size() == 1 && leases.front().at("slot") == "payload" && leases.front().at("resource") == uuid<ResourceId>().to_string()
        && leases.front().at("kind") == "Binary" && leases.front().at("revision") == expected_revision,
        "The acquired resource has the expected identity and revision.");
    for (const char id : std::string_view("23456")) {
        const auto identity = uuid<NodeId>(id).to_string();
        check(std::ranges::count_if(order, [&](const Json& call) { return call == identity; }) == 1, "Each measured lane runs exactly once.");
        const auto& trace = sources(node(report, id), "left").at(0);
        check(trace.at("kind") == "result" && trace.at("node") == uuid<NodeId>('1').to_string()
            && trace.at("output") == "value", "Each lane uses the entry's actual output as its left input.");
    }
    const auto& public_trace = sources(node(report, '2'), "right").at(0);
    check(public_trace.at("kind") == "public_input" && public_trace.at("input") == "offset" && public_trace.at("origin") == "Provided",
        "The first lane receives the explicit CLI input.");
    const auto& constant_trace = sources(node(report, '3'), "right").at(0);
    check(constant_trace.at("kind") == "constant" && constant_trace.at("defaulted") == false,
        "Other lanes can obtain their right input from saved constants.");
    const Json expected = Json::array({"104", "99", "100", "101", "102"});
    check(produced(node(report, '7'), "values") == expected && produced(node(report, '8'), "values") == expected,
        "Join and delivery preserve all five results in binding order.");
    const auto& join_sources = sources(node(report, '7'), "values");
    check(join_sources.size() == 5, "Join reports every source.");
    for (std::size_t index = 0; index < join_sources.size(); ++index) {
        const auto& trace = join_sources[index];
        check(trace.at("index") == index && trace.at("node") == uuid<NodeId>("23456"[index]).to_string(),
            "Join traces the five lane producers in binding order.");
    }
    check(sources(node(report, '8'), "values").at(0).at("node") == uuid<NodeId>('7').to_string(), "Delivery uses the joined result.");
    check(report.at("control_links").size() == 11, "The five-lane trace reports every control edge.");
    for (const char id : std::string_view("123456789ab")) { edge(report, id, true); }
}

void verify_branch(const Json& report, std::string_view revision, int mask)
{
    check_run(report, '2', revision);
    const bool left = (mask & 1) != 0;
    const bool right = (mask & 2) != 0;
    Json outlets = Json::array();
    Json expected = Json::array();
    if (left) { outlets.push_back("left"); expected.push_back("11"); }
    if (right) { outlets.push_back("right"); expected.push_back("22"); }
    check(node(report, '9').at("selected_outputs") == outlets, "The selector emits exactly the requested outlets.");
    check(report.at("control_links").size() == 5, "The conditional trace reports every control edge.");
    edge(report, 'c', left);
    edge(report, 'd', right);
    edge(report, 'e', left);
    edge(report, 'f', right);
    edge(report, '0', left || right);
    if (!left) { skipped(node(report, 'a')); }
    if (!right) { skipped(node(report, 'b')); }
    const auto& order = report.at("call_order");
    check(order.front() == uuid<NodeId>('9').to_string(), "The selector starts before every conditional branch.");
    if (!left && !right) {
        check(order.size() == 1, "Zero selection invokes only the selector.");
        skipped(node(report, 'c'));
        skipped(node(report, 'd'));
        return;
    }
    check(order.size() == 3u + static_cast<unsigned>(left) + static_cast<unsigned>(right)
        && order.at(order.size() - 2) == uuid<NodeId>('c').to_string() && order.back() == uuid<NodeId>('d').to_string(),
        "Join and delivery run once after the selected lanes.");
    check(produced(node(report, 'c'), "values") == expected && produced(node(report, 'd'), "values") == expected,
        "Join and delivery retain the selected values in binding order.");
    if (!right) {
        const auto& absent = sources(node(report, 'c'), "values").at(1);
        check(absent.at("node") == uuid<NodeId>('b').to_string() && absent.at("invocation").is_null()
            && absent.at("accepted_absence") == "NotSelected", "A closed branch stays in provenance without a call identity.");
    }
}

Json comparable(Json report)
{
    for (const auto key : {"revision", "session", "snapshot", "run"}) { report.erase(key); }
    for (auto& call : report.at("nodes")) {
        for (const auto key : {"scope", "invocation", "attempt", "elapsed", "elapsed_ns", "timings"}) { call.erase(key); }
        for (auto& binding : call.at("bindings")) {
            for (auto& source : binding.at("sources")) {
                for (const auto key : {"session", "snapshot", "run", "scope", "invocation", "attempt"}) { source.erase(key); }
            }
            if (binding.contains("regions")) {
                for (auto& region : binding.at("regions")) {
                    auto& producer = region.at("producer");
                    for (const auto key : {"session", "snapshot", "run", "scope", "invocation", "attempt"}) { producer.erase(key); }
                }
            }
        }
    }
    return report;
}

void roundtrip(const fs::path& executable, PayloadStorage storage)
{
    const Fixture fixture(executable, storage);
    const auto original = read_file(fixture.manifest);
    const auto fork = success(fixture.run('1', L"offset=7"));
    verify_fork(fork, "12", fixture.payload_revision);
    std::vector<std::pair<int, Json>> branches;
    for (const int mask : {3, 1, 0}) {
        auto trace = success(fixture.run('2', L"mask=" + std::to_wstring(mask)));
        verify_branch(trace, "12", mask);
        branches.emplace_back(mask, std::move(trace));
    }
    check(read_file(fixture.manifest) == original, "Run leaves the on-disk solution unchanged.");

    const auto saved = success(fixture.save());
    const auto& cleanup = saved.at("cleanup");
    check(saved.at("status") == "Committed" && saved.at("revision") == "13" && saved.at("solution") == uuid<SolutionId>().to_string(),
        "The CLI commits the next revision of the same solution.");
    check(cleanup.at("diagnostics").empty() && read_file(fixture.payload_file) == "abc", "The save retains the unchanged resource file.");
    check(Json::parse(read_file(fixture.manifest)).at("resources") == Json::parse(original).at("resources"),
        "The new solution revision retains the resource declaration.");
    const auto reloaded = success(fixture.run('1', L"offset=7"));
    verify_fork(reloaded, "13", fixture.payload_revision);
    check(fork.at("session") != reloaded.at("session"), "Reload and execution use a fresh process session.");
    check(comparable(fork) == comparable(reloaded), "Five-way business results, provenance and control agree after save and reload.");
    for (const auto& [mask, before] : branches) {
        const auto after = success(fixture.run('2', L"mask=" + std::to_wstring(mask)));
        verify_branch(after, "13", mask);
        check(comparable(before) == comparable(after), "Conditional results, source omissions and closure agree after save and reload.");
    }
}

void rejected(const CommandResult& result, DWORD exit_code, std::string_view code)
{
    if (result.exit_code != exit_code) { throw std::runtime_error("Unexpected CLI exit: " + result.error + result.output); }
    const auto report = Json::parse(result.error);
    check(report.at("stage").is_string() && !report.at("stage").get_ref<const std::string&>().empty(), "Errors identify the failing stage.");
    check(std::ranges::any_of(report.at("diagnostics"), [&](const Json& item) { return text_is(item.at("code"), code); }),
        "Errors preserve the diagnostic code.");
}

void calculations(const fs::path& executable)
{
    ScratchDirectory scratch;
    const auto manifest = scratch.path / "calculation" / "solution.json";
    write_file(manifest, read_file(VISIONPRO_CALCULATION_SAMPLE));
    const auto solution = Json::parse(read_file(manifest));
    const auto& workflows = solution.at("workflows");
    const auto run = [&](std::size_t workflow, std::wstring input = {}) {
        const auto id = workflows.at(workflow).at("id").get<std::string>();
        const std::wstring flow(id.begin(), id.end());
        if (input.empty()) {
            return invoke(executable, scratch.path, {L"run", L"--solution", manifest.native(), L"--workflow", flow});
        }
        return invoke(executable, scratch.path, {L"run", L"--solution", manifest.native(), L"--workflow", flow, L"--input", input});
    };
    const auto call = [&](const Json& report, std::size_t flow, std::size_t index) -> const Json& {
        const auto id = workflows.at(flow).at("nodes").at(index).at("id").get<std::string>();
        return find_by(report.at("nodes"), "id", id);
    };

    success(invoke(executable, scratch.path, {L"save", L"--solution", manifest.native()}));
    const auto selected = success(run(0));
    check(selected.at("status") == "Succeeded" && produced(call(selected, 0, 1)) == "9", "Area at dynamic index 1 must be 9.");
    const auto& selector = sources(call(selected, 0, 1), "left");
    const auto& list = find_by(selector, "kind", "public_input");
    const auto& index = find_by(selector, "kind", "result");
    const auto& producer = call(selected, 0, 0);
    check(selector.size() == 2 && list.at("input") == "areas" && list.at("index") == index.at("index")
        && list.at("selector").at(0).at("index_source").at("node") == producer.at("id")
        && index.at("node") == producer.at("id") && index.at("invocation") == producer.at("invocation")
        && index.at("attempt") == producer.at("attempt") && index.at("run") == selected.at("run")
        && index.at("selector_step") == 0 && index.at("index_value") == "1", "Index must trace the exact calculation used on this list.");
    const auto& formatter = call(selected, 0, 2);
    check(produced(formatter, "text") == "A=6|A=9" && produced(formatter, "used_indices") == Json::array({"0", "1"})
        && produced(formatter, "used_values") == Json::array({"6", "9"}), "Formatting records exactly the non-skipped selections.");
    check(produced(call(selected, 0, 3)) == "11" && produced(call(selected, 0, 4)) == "21", "The two counters start at their saved seeds.");
    const auto invalid = run(0, L"index=2");
    rejected(invalid, 1, "IndexOutOfRange");
    const auto failed = Json::parse(invalid.output);
    check(failed.at("status") == "Failed" && call(failed, 0, 2).at("status") == "Skipped", "A dynamic selection failure stops the control chain.");

    for (const bool passed : {false, true}) {
        const auto result = success(run(1, passed ? L"fifth=7" : L"fifth=9"));
        const auto& combined = call(result, 1, 5);
        const auto& lanes = sources(combined, "values");
        check(result.at("status") == "Succeeded" && produced(combined, "passed") == passed && lanes.size() == 5,
            "Five completed judgements produce one business decision.");
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            const auto& judge = call(result, 1, i);
            check(judge.at("status") == "Succeeded" && judge.at("judgement") == (i == 4 && !passed ? "NG" : "OK")
                && lanes.at(i).at("node") == judge.at("id") && lanes.at(i).at("index") == i,
                "The combined decision preserves lane order and treats NG as a valid result.");
        }
        check(call(result, 1, 6).at("selected_outputs") == Json::array({passed ? "true" : "false"})
            && produced(call(result, 1, passed ? 7 : 8)) == (passed ? "1" : "0"), "Only the selected successor executes.");
        skipped(call(result, 1, passed ? 8 : 7));
    }

    const auto strict = success(run(2));
    check(produced(call(strict, 2, 1), "text") == "A=9", "The formatter accepts a value produced by ordinary list selection.");
    const auto upstream_failure = run(2, L"index=2");
    rejected(upstream_failure, 1, "IndexOutOfRange");
    const auto stopped = Json::parse(upstream_failure.output);
    const auto& downstream = call(stopped, 2, 1);
    check(stopped.at("status") == "Failed" && call(stopped, 2, 0).at("status") == "Failed"
        && downstream.at("status") == "Skipped" && downstream.at("invocation").is_null() && downstream.at("outputs").empty(),
        "Formatter tolerance never consumes an upstream technical failure.");
}

void shared_workflows(const fs::path& executable)
{
    ScratchDirectory scratch;
    const auto manifest = scratch.path / "shared" / "solution.json";
    write_file(manifest, read_file(VISIONPRO_SHARED_SAMPLE));
    const auto sample = Json::parse(read_file(manifest));
    success(invoke(executable, scratch.path, {L"save", L"--solution", manifest.native()}));
    const auto run = [&](std::size_t index) {
        const auto id = sample.at("workflows").at(index).at("id").get<std::string>();
        return success(invoke(executable, scratch.path, {L"run", L"--solution", manifest.native(), L"--workflow",
            std::wstring(id.begin(), id.end())}));
    };
    const auto delivery = run(0);
    const auto& received = delivery.at("nodes").back();
    const auto& messages = produced(received, "messages");
    check(delivery.at("status") == "Succeeded" && produced(received, "count") == "2" && messages.size() == 2,
        "Exactly two completed workpieces are received.");
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& message = messages.at(i);
        check(message.at("queue_id") == sample.at("queues").at(1).at("id") && message.at("domain") == "inspection.lot1"
            && message.at("sequence") == std::to_string(i + 1) && message.at("key") == (i == 0 ? "B" : "A")
            && message.at("payload") == Json{{"height", i == 0 ? "2" : "1"}, {"width", i == 0 ? "20" : "10"}},
            "A completion preserves FIFO sequence, business identity and measurements from the same workpiece.");
        const auto& sender = find_by(delivery.at("nodes"), "id",
            sample.at("workflows").at(0).at("nodes").at(i == 0 ? 8 : 10).at("id").get<std::string>());
        check(produced(sender, "message_id") == message.at("message_id"), "Received message traces its original send.");
    }
    const auto globals = run(1);
    const auto& nodes = globals.at("nodes");
    check(globals.at("status") == "Succeeded" && produced(nodes.at(0)) == "0" && produced(nodes.at(0), "revision") == "0"
        && produced(nodes.at(1)) == "10" && produced(nodes.at(1), "revision") == "1", "A fresh session initializes then advances a global.");
    const auto& entries = produced(nodes.at(2), "entries");
    check(entries.size() == 2 && entries.at(0).at("value") == "2" && entries.at(0).at("revision") == "0"
        && entries.at(1).at("value") == "10" && entries.at(1).at("revision") == "1",
        "The explicit multi-variable read observes the live values in declared order.");
    const auto& frozen = sources(nodes.back(), "left").at(0);
    check(produced(nodes.back()) == "0" && frozen.at("kind") == "global" && frozen.at("revision") == "0",
        "An ordinary binding keeps the global snapshot captured when preparing this run.");
}

void trigger_replay(const fs::path& executable)
{
    ScratchDirectory scratch;
    const auto manifest = scratch.path / "trigger" / "solution.json";
    write_file(manifest, read_file(VISIONPRO_TRIGGER_SAMPLE));
    success(invoke(executable, scratch.path, {L"save", L"--solution", manifest.native()}));
    const auto flows = Json::parse(read_file(manifest)).at("workflows");
    const auto replay = [&](std::size_t flow, std::initializer_list<std::wstring> options) {
        const auto id = flows.at(flow).at("id").get<std::string>();
        // Launch a separate CLI process against the saved package, outside its directory.
        std::vector<std::wstring> args{L"trigger", L"--solution", manifest.native(), L"--workflow", {id.begin(), id.end()}};
        args.insert(args.end(), options.begin(), options.end());
        return invoke(executable, scratch.path, args);
    };
    const auto verify = [&](const Json& report, std::size_t index, std::initializer_list<std::string_view> frames) {
        const auto& config = flows.at(index).at("trigger");
        const auto& events = report.at("events");
        check(report.at("stop") == "drain" && report.at("quiescent") == true
            && events.size() == frames.size(), "Drain returns a terminal record for each supplied event.");
        std::set<std::string> runs;
        auto event = events.begin();
        for (const auto frame : frames) {
            const auto& result = event->at("result");
            check(event->at("mode") == config.at("mode") && event->at("source") == config.at("source")
                && event->at("frame_input") == config.at("frame_input") && text_is(event->at("frame"), frame)
                && event->at("correlation") == Json{{"domain", config.at("source")}, {"key", frame}},
                "Each receipt retains its source and associated frame.");
            check((event->at("admission") == "immediate" || event->at("admission") == "queued")
                && result.at("quiescent") == true && result.at("status") == "Succeeded"
                && runs.insert(result.at("run").get<std::string>()).second
                && result.at("trigger").at("correlation") == event->at("correlation")
                && text_is(result.at("inputs").at(0).at("value"), frame)
                && text_is(produced(result.at("nodes").at(0)), frame), "Each accepted run executes with its own frozen frame.");
            ++event;
        }
    };
    verify(success(replay(0, {L"--frame", L"71", L"--frame", L"-4", L"--frame", L"9007199254740993"})),
        0, {"71", "-4", "9007199254740993"});
    verify(success(replay(1, {L"--advance-ms", L"99", L"--advance-ms", L"1", L"--advance-ms", L"250",
        L"--advance-ms", L"100"})), 1, {"1", "2", "3"});
    const auto empty = success(replay(0, {L"--stop", L"cancel"}));
    check(empty.at("events").empty() && empty.at("stop") == "cancel" && empty.at("quiescent") == true,
        "Stopping an idle controlled source submits no work.");
    rejected(replay(0, {L"--advance-ms", L"100"}), 1, "InvalidRunRequest");
    rejected(replay(1, {L"--frame", L"3"}), 1, "InvalidRunRequest");
    auto limited = Json::parse(read_file(manifest));
    limited.at("workflows").at(0).at("interface").at("inputs").at(0).at("constraints")["integer_range"]
        = {{"minimum", "0"}, {"maximum", "10"}};
    write_file(manifest, limited.dump());
    const auto mixed = replay(0, {L"--frame", L"3", L"--frame", L"11", L"--frame", L"5"});
    rejected(mixed, 1, "ConstraintViolation");
    const auto events = Json::parse(mixed.output).at("events");
    check(events.size() == 3 && produced(events.at(0).at("result").at("nodes").at(0)) == "3"
        && events.at(1).at("admission") == "rejected" && events.at(1).at("correlation").at("key") == "11"
        && !events.at(1).contains("result") && produced(events.at(2).at("result").at("nodes").at(0)) == "5",
        "A rejected frame remains traceable without suppressing accepted frames on either side.");
}

void output_failures(const fs::path& executable)
{
    const Fixture fixture(executable, PayloadStorage::External);
    const auto expect_output_failure = [&](std::initializer_list<std::wstring> arguments) {
        const auto result = invoke(executable, fixture.scratch.path, arguments, OutputMode::BrokenPipe);
        rejected(result, 1, "ExecutionBoundaryFailure");
        const auto report = Json::parse(result.error);
        check(report.at("stage") == "report", "An output failure is distinguished from workflow or save failure.");
        check(report.at("diagnostics").at(0).at("message").get<std::string>().find("standard output") != std::string::npos,
            "The diagnostic identifies the failed output stream.");
    };

    // The short help text fits in the CRT buffer, so failure must also be checked when flushing.
    expect_output_failure({L"--help"});
    const auto workflow = uuid<WorkflowId>('1').to_string();
    expect_output_failure({L"run", L"--solution", fixture.manifest.native(), L"--workflow",
        std::wstring(workflow.begin(), workflow.end()), L"--input", L"offset=7"});
    expect_output_failure({L"save", L"--solution", fixture.manifest.native()});
    check(Json::parse(read_file(fixture.manifest)).at("revision") == "13",
        "Reporting failure does not undo an already committed save.");
}

void diagnostics(const fs::path& executable, PayloadStorage storage)
{
    const Fixture fixture(executable, storage);
    const auto help = fixture.command({L"--help"});
    check(help.exit_code == 0 && help.error.empty() && help.output.find("--workflow") != std::string::npos
        && help.output.find("save") != std::string::npos,
        "Help describes the executable commands.");
    const auto usage = fixture.command({L"run", L"--solution", fixture.manifest.native()});
    check(usage.exit_code == 2 && usage.output.empty() && !Json::parse(usage.error).at("diagnostics").empty(),
        "Missing CLI arguments use the usage error exit code.");

    const auto flow = uuid<WorkflowId>('1').to_string();
    const auto missing = fixture.command({L"run", L"--solution", fixture.manifest.native(), L"--workflow", std::wstring(flow.begin(), flow.end())});
    rejected(missing, 1, "MissingRequiredValue");
    check(missing.output.empty(), "An unaccepted request does not produce a run summary.");
    rejected(fixture.run('1', L"offset=9223372036854775808"), 2, "InvalidRunRequest");

    const auto original = read_file(fixture.manifest);
    const auto failed = fixture.run('1', L"offset=9223372036854775807");
    rejected(failed, 1, "InvalidValue");
    const auto trace = Json::parse(failed.output);
    check(trace.at("status") == "Failed" && trace.at("quiescent") == true && !trace.at("diagnostics").empty(),
        "An accepted execution failure still reports its terminal run and diagnostics.");
    check(node(trace, '7').at("skip_reason") == "FailFast" && node(trace, '8').at("invocation").is_null(),
        "A failed measurement cannot trigger successful join and delivery.");
    check(read_file(fixture.manifest) == original, "A failed run leaves the manifest unchanged.");

    const auto unknown = "{\"future_field\":true," + original.substr(1);
    write_file(fixture.manifest, unknown);
    rejected(fixture.save(), 1, "UnsupportedSolutionContent");
    check(read_file(fixture.manifest) == unknown && read_file(fixture.payload_file) == "abc", "A rejected save preserves original bytes.");
    write_file(fixture.manifest, "{invalid");
    rejected(fixture.run('1', L"offset=7"), 1, "InvalidSolutionManifest");
    write_file(fixture.manifest, original);

    check(fs::remove(fixture.payload_file), "The fixture removed the resource file before another request.");
    rejected(fixture.run('1', L"offset=7"), 1, "ResourceNotFound");
    check(read_file(fixture.manifest) == original, "A missing resource leaves the loaded manifest unchanged.");
}

#ifdef VISIONPRO_WITH_OPENCV
namespace vision = visionpro::tools::vision;

CoordinateSystemId camera() { return key<CoordinateSystemId>("camera"); }
constexpr auto pixel = MeasurementUnit::Pixel;

Binding setting(std::string_view name, ConfigValue value)
{ return {key<ParameterId>(name), {std::move(value)}}; }

Binding enum_setting(std::string_view type, std::string_view name, std::string_view member)
{
    const auto descriptors = vision::tool_descriptors();
    const auto descriptor = std::ranges::find_if(descriptors, [&](const ToolDescriptor& item) { return item.id.text() == type; });
    check(descriptor != descriptors.end(), "The fixture uses a declared vision tool.");
    const auto parameter = std::ranges::find(descriptor->parameters, key<ParameterId>(name), &ParameterDescriptor::id);
    check(parameter != descriptor->parameters.end(), "The fixture uses a declared enum parameter.");
    return setting(name, take(ConfigValue::enumeration(parameter->type, key<EnumMemberId>(member))));
}

Node image_reader(char id, const ResourceReference& source)
{
    constexpr auto type = "visionpro.image.read_file";
    auto node = tool(id, type, "读图", {enum_setting(type, "decode_mode", "Gray8"),
        setting("frame_origin", take(ConfigValue::point2d({camera(), pixel, 0, 0})))});
    node.resources.push_back({key<SchemaId>("source"), source});
    return node;
}

Node threshold(WorkflowId flow, char id, char reader, RegionShape shape)
{
    constexpr auto type = "visionpro.image.threshold";
    const RegionDefinition roi{uuid<RegionId>(id), camera(), pixel, RegionPrimitive{camera(), pixel, std::move(shape)}, {}};
    return tool(id, type, "二值化", {input_binding("image", output(flow, reader, "image")),
        setting("roi", take(ConfigValue::region2d(roi))), setting("threshold", ConfigValue::integer(128)),
        enum_setting(type, "mode", "Binary")});
}

Node blob(WorkflowId flow, char id, char binary)
{
    return tool(id, "visionpro.image.blob", "连通域", {input_binding("binary", output(flow, binary, "binary")),
        setting("connectivity", ConfigValue::integer(8))});
}

Node judge(char id, OutputReference source, std::int64_t expected)
{
    return tool(id, "visionpro.judge.integer_range", "范围判断", {input_binding("value", std::move(source)),
        setting("minimum", ConfigValue::integer(expected)), setting("maximum", ConfigValue::integer(expected))});
}

Workflow image_chain(const ResourceReference& source)
{
    const auto flow = uuid<WorkflowId>('1');
    return {flow, "视觉单链", {key<SchemaId>("s46.single"), 1},
        {image_reader('1', source), threshold(flow, '2', '1', FullImageRegion{}), blob(flow, '3', '2'),
            judge('4', output(flow, '3', "count"), 2)},
        {link('1', '1', '2'), link('2', '2', '3'), link('3', '3', '4')}, {}, {uuid<NodeId>('1')}, {}};
}

Workflow inherited_chain(const ResourceReference& source)
{
    const auto flow = uuid<WorkflowId>('3');
    const Transform2D shift{camera(), camera(), pixel, pixel, TransformModel::Affine, {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    const RegionDefinition roi{uuid<RegionId>('8'), camera(), pixel,
        InheritedRegion{flow, uuid<NodeId>('7'), key<OutputId>("effective_roi"), camera(), pixel, 0, shift},
        {RegionPrimitive{camera(), pixel, AxisAlignedRect{5, 4, 1, 1}}}};
    auto measured = blob(flow, '8', '6');
    measured.bindings.push_back(setting("roi", ConfigValue::some(take(ConfigValue::region2d(roi)))));
    return {flow, "继承旋转 ROI", {key<SchemaId>("s46.inherited"), 1},
        {image_reader('5', source), threshold(flow, '6', '5', FullImageRegion{}),
            threshold(flow, '7', '5', RotatedRect{4, 4, 4, 2, std::numbers::pi / 4}), std::move(measured),
            judge('e', output(flow, '8', "total_area_pixels"), 6)},
        {link('4', '5', '6'), link('5', '5', '7'), link('6', '6', '8'), link('7', '7', '8'), link('8', '8', 'e')},
        {}, {uuid<NodeId>('5')}, {}};
}

std::string white_bmp()
{
    // 9 x 9 uncompressed BGR8. Each bottom-up row has 27 sample bytes and one padding byte.
    constexpr unsigned char header[]{
        0x42, 0x4d, 0x32, 0x01, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, 9, 0, 0, 0, 9, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0,
        252, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static_assert(sizeof(header) == 54);
    std::string bytes(reinterpret_cast<const char*>(header), sizeof(header));
    for (int row = 0; row < 9; ++row) {
        bytes.append(27, static_cast<char>(255));
        bytes.push_back(0);
    }
    return bytes;
}

struct VisionFixture {
    const fs::path cli;
    const ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"视觉 方案";
    const fs::path manifest = package / solution_manifest_name;
    const fs::path first = scratch.path / u8"保存 前";
    const fs::path second = scratch.path / u8"重载 后";
    const fs::path png_path = package / u8"图像/灰度 图.png";
    const fs::path bmp_path = package / u8"图像/旋转 图.bmp";

    explicit VisionFixture(fs::path executable) : cli(std::move(executable))
    {
        fs::create_directories(first);
        fs::create_directories(second);
        const auto& png = visionpro::test_fixtures::png;
        write_file(png_path, {reinterpret_cast<const char*>(png), sizeof(png)});
        write_file(bmp_path, white_bmp());
        const auto resource = [&](char id, const fs::path& path) {
            const auto relative = path.lexically_relative(package).generic_u8string();
            return ResourceDefinition{uuid<ResourceId>(id), "外部图像", ResourceKind::Image,
                ExternalFileResource{{PathBase::SolutionDirectory, {relative.begin(), relative.end()}, {}},
                    ResourceReadPolicy::PerInvocation, {}, {}}};
        };
        const auto png_resource = resource('1', png_path);
        const auto bmp_resource = resource('2', bmp_path);
        const CoordinateFrameDefinition coordinates{camera(), CoordinateKind::Image, pixel, {0, 0}, {1, 0}, {0, -1},
            Handedness::LeftHanded};
        const Solution solution{uuid<SolutionId>(), "真实视觉往返", solution_format_version, 12,
            {image_chain({png_resource.id, png_resource.kind, {}}), branch_flow(), inherited_chain({bmp_resource.id, bmp_resource.kind, {}})},
            {png_resource, bmp_resource}, {}, {}, {coordinates}, {}};
        take(write_new_solution(package, solution));
    }

    CommandResult run(char flow, const fs::path& working) const
    {
        const auto id = uuid<WorkflowId>(flow).to_string();
        const auto manifest_arg = manifest.lexically_relative(working).native();
        const std::wstring flow_arg(id.begin(), id.end());
        if (flow == '2') {
            return invoke(cli, working, {L"run", L"--solution", manifest_arg, L"--workflow", flow_arg, L"--input", L"mask=3"});
        }
        return invoke(cli, working, {L"run", L"--solution", manifest_arg, L"--workflow", flow_arg});
    }
};

void check_vision_report(const Json& report, char flow, std::string_view revision)
{
    if (flow == '2') { verify_branch(report, revision, 3); return; }
    check_run(report, flow, revision);
    const bool inherited = flow == '3';
    const auto& reader = node(report, inherited ? '5' : '1');
    const auto& measured = node(report, inherited ? '8' : '3');
    const auto& judged = node(report, inherited ? 'e' : '4');
    const auto& layout = produced(reader, "image");
    const auto side = inherited ? 9 : 2;
    check(layout.at("width") == side && layout.at("height") == side && layout.at("depth") == "UInt8"
        && layout.at("channel_order") == "Mono" && layout.at("current_frame") == "camera"
        && layout.at("root_frame") == "camera", "The CLI identifies the full decoded image and coordinates.");
    check(layout.at("samples") == Json{{"minimum", inherited ? 255 : 0}, {"maximum", 255}, {"nonzero_count", inherited ? 81 : 3}},
        "The CLI summarizes the valid samples of both file formats.");
    const auto& lease = find_by(reader.at("resources"), "slot", "source");
    check(lease.at("resource") == uuid<ResourceId>(inherited ? '2' : '1').to_string() && lease.at("kind") == "Image"
        && lease.at("revision").is_null(),
        "The CLI keeps the exact external image resource identity.");
    check(produced(measured, "count") == "1" && produced(measured, "total_area_pixels") == (inherited ? "6" : "2")
        && produced(measured, "largest_area_pixels") == (inherited ? "6" : "2"), "The CLI publishes the fixed component statistics.");
    check(judged.at("status") == "Succeeded" && judged.at("judgement") == (inherited ? "OK" : "NG")
        && produced(judged, "passed") == inherited, "The CLI accepts the business judgement as successful execution.");
    for (const char id : std::string_view(inherited ? "45678" : "123")) { edge(report, id, true); }

    if (inherited) {
        check(sources(measured, "binary").at(0).at("node") == uuid<NodeId>('6').to_string(),
            "Blob uses the whole binary image from the full-image threshold.");
        const auto& frozen = find_by(measured.at("bindings"), "target", "roi").at("regions").at(0);
        const auto& reference = frozen.at("reference");
        const auto& producer = frozen.at("producer");
        const auto& source = node(report, '7');
        check(frozen.at("region") == uuid<RegionId>('7').to_string() && reference.at("kind") == "inherited"
            && reference.at("transform").at("matrix") == Json::array({1, 0, 1, 0, 1, 0, 0, 0, 1}),
            "The CLI preserves the frozen region and the position correction.");
        check(producer.at("node") == source.at("id") && producer.at("output") == "effective_roi"
            && producer.at("scope") == source.at("scope") && producer.at("invocation") == source.at("invocation")
            && producer.at("attempt") == source.at("attempt") && producer.at("run") == report.at("run")
            && producer.at("snapshot") == report.at("snapshot") && producer.at("session") == report.at("session")
            && producer.at("workflow") == report.at("workflow"), "The inherited ROI references this exact producer and run.");
    }
}

ToolRegistry full_catalog()
{
    auto registrations = visionpro::tools::simulation_registrations();
    for (auto& item : vision::registrations()) { registrations.push_back(std::move(item)); }
    return take(ToolRegistry::create(registrations));
}

void check_pixels(const SolutionDocument& document, const ToolRegistry& registry, char flow)
{
    const auto execution = take(open_solution_execution(document, registry));
    const auto request = take(RunRequest::create(execution.snapshot, WorkflowTarget{uuid<WorkflowId>(flow)}, {},
        {TriggerKind::Manual, key<SchemaId>("s46.pixels")}));
    auto submitted = execution.engine->submit(request);
    if (const auto* refused = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(refused->diagnostic.message); }
    const auto& run = std::get<RunHandle>(submitted);
    const auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.status == ExecutionStatus::Succeeded,
        "The loaded configuration succeeds when checking image data directly.");
    const auto value = [&](char id, std::string_view port) {
        const auto invocation = std::ranges::find(waited.progress.nodes, uuid<NodeId>(id), &RunNodeProgress::node);
        check(invocation != waited.progress.nodes.end() && invocation->invocation.has_value(), "The producer was invoked.");
        const auto result = take(run.lookup(*invocation->invocation));
        check(result.has_value(), "The producer result is retained.");
        const auto* slot = result->output(key<OutputId>(port));
        check(slot && std::holds_alternative<DataValue>(slot->content), "The binary image or mask was published.");
        return std::get<DataValue>(slot->content);
    };
    const auto check_image = [&](const ImageHandle& image, std::size_t side, std::span<const unsigned char> samples) {
        check(image.width() == side && image.height() == side && image.format() == PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}
            && image.coordinate_frame() == camera() && image.root_frame() == camera() && image.origin_x() == 0 && image.origin_y() == 0
            && std::ranges::equal(image.copy_pixels(), std::as_bytes(samples)), "Valid pixels, layout and coordinates match the fixed expectation.");
    };
    if (flow == '1') {
        constexpr unsigned char binary[]{0, 0, 255, 255};
        check_image(value('1', "image").as_image()->image, 2, visionpro::test_fixtures::mono_pixels);
        check_image(value('2', "binary").as_image()->image, 2, binary);
    } else {
        std::array<unsigned char, 81> white;
        white.fill(255);
        check_image(value('5', "image").as_image()->image, 9, white);
        check_image(value('6', "binary").as_image()->image, 9, white);
        std::array<unsigned char, 81> rotated{};
        for (const auto index : {30, 31, 39, 40, 41, 49, 50}) { rotated[index] = 255; }
        check_image(value('7', "binary").as_image()->image, 9, rotated);
        check_image(value('7', "roi_mask").as_raster_mask()->pixels, 9, rotated);
    }
}

void vision_roundtrip(const fs::path& executable)
{
    const VisionFixture fixture(executable);
    const auto registry = full_catalog();
    const auto original = read_file(fixture.manifest);
    const auto original_png = read_file(fixture.png_path);
    const auto original_bmp = read_file(fixture.bmp_path);
    const auto loaded = take(load_solution(fixture.package, registry));
    check_pixels(loaded, registry, '1');
    check_pixels(loaded, registry, '3');

    std::vector<std::pair<char, Json>> before;
    for (const char flow : {'1', '2', '3'}) {
        auto report = success(fixture.run(flow, fixture.first));
        check_vision_report(report, flow, "12");
        before.emplace_back(flow, std::move(report));
    }
    check(read_file(fixture.manifest) == original, "Executing the package leaves its manifest unchanged.");

    const auto changed = original + "\n";
    write_file(fixture.manifest, changed);
    const auto stale = save_solution(loaded, loaded.solution());
    const auto* conflict = std::get_if<Diagnostic>(&stale);
    check(conflict && conflict->code == ErrorCode::SolutionSaveConflict && read_file(fixture.manifest) == changed,
        "A byte change in the same revision prevents a stale document from overwriting the manifest.");

    const auto saved = success(invoke(executable, fixture.first, {L"save", L"--solution", fixture.manifest.native()}));
    check(saved.at("status") == "Committed" && saved.at("revision") == "13" && saved.at("cleanup").at("diagnostics").empty(),
        "The independent save process commits a new revision.");
    auto expected = Json::parse(original);
    expected["revision"] = "13";
    check(Json::parse(read_file(fixture.manifest)) == expected, "The manifest keeps configuration and resource references across revisions.");
    check(read_file(fixture.png_path) == original_png && read_file(fixture.bmp_path) == original_bmp,
        "The external files retain their content after save.");

    const auto reloaded = take(load_solution(fixture.package, registry));
    check_pixels(reloaded, registry, '1');
    check_pixels(reloaded, registry, '3');
    for (const auto& [flow, previous] : before) {
        const auto after = success(fixture.run(flow, fixture.second));
        check_vision_report(after, flow, "13");
        check(previous.at("session") != after.at("session") && comparable(previous) == comparable(after),
            "Fresh CLI processes agree on values, binding sources and control from different working directories.");
    }
}

void vision_diagnostics(const fs::path& executable)
{
    const VisionFixture fixture(executable);
    const auto original = read_file(fixture.manifest);
    const auto prior = success(fixture.run('1', fixture.first));
    check_vision_report(prior, '1', "12");

    write_file(fixture.png_path, "damaged image");
    const auto damaged = fixture.run('1', fixture.second);
    rejected(damaged, 1, "InvalidImage");
    const auto report = Json::parse(damaged.output);
    check(report.at("status") == "Failed" && report.at("quiescent") == true && report.at("session") != prior.at("session")
        && node(report, '1').at("status") == "Failed", "An accepted decode failure reports the terminal state of this run.");
    for (const auto& output : node(report, '1').at("outputs")) {
        check(output.at("status") != "Produced", "The failed reader has no produced value.");
    }
    for (const char id : {'2', '3', '4'}) {
        const auto& successor = node(report, id);
        check(successor.at("status") == "Skipped" && successor.at("skip_reason") == "FailFast"
            && successor.at("invocation").is_null() && successor.at("outputs").empty(),
            "FailFast closes each successor without a stale result.");
    }

    check(fs::remove(fixture.png_path), "The fixture removed its external image.");
    const auto missing = fixture.run('1', fixture.second);
    rejected(missing, 1, "ResourceNotFound");
    check(missing.output.empty(), "A missing required image fails before acceptance.");
    check(read_file(fixture.manifest) == original, "Failed runs preserve the original manifest.");
}
#endif

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 3) { throw std::runtime_error("Expected CLI executable and test group."); }
        const auto cli = fs::absolute(argv[1]);
        const std::wstring_view group(argv[2]);
        if (group == L"roundtrip") { roundtrip(cli, PayloadStorage::Managed); }
        else if (group == L"diagnostics") { diagnostics(cli, PayloadStorage::Managed); }
        else if (group == L"external-roundtrip") { roundtrip(cli, PayloadStorage::External); }
        else if (group == L"external-diagnostics") { diagnostics(cli, PayloadStorage::External); }
        else if (group == L"output-failures") { output_failures(cli); }
        else if (group == L"calculations") { calculations(cli); }
        else if (group == L"shared") { shared_workflows(cli); }
        else if (group == L"trigger") { trigger_replay(cli); }
#ifdef VISIONPRO_WITH_OPENCV
        else if (group == L"vision-roundtrip") { vision_roundtrip(cli); }
        else if (group == L"vision-diagnostics") { vision_diagnostics(cli); }
#endif
        else { throw std::runtime_error("Unknown CLI test group."); }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
