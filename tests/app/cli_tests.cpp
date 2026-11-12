#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>

#include <nlohmann/json.hpp>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
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

CommandResult invoke(const fs::path& executable, const fs::path& working, std::initializer_list<std::wstring> arguments)
{
    const auto stdout_path = working / L"stdout.txt";
    const auto stderr_path = working / L"stderr.txt";
    auto command_line = quote_argument(executable.native());
    for (const auto& argument : arguments) { command_line += L" " + quote_argument(argument); }
    PROCESS_INFORMATION information{};
    {
        SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        const Handle output(CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
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
    return {exit_code, read_file(stdout_path), read_file(stderr_path)};
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

struct Fixture {
    const fs::path executable;
    const ScratchDirectory scratch;
    const fs::path package = scratch.path / u8"验收 方案";
    const fs::path manifest = package / "solution.json";
    const fs::path managed = package / u8"resources/托管 文件.bin";

    explicit Fixture(fs::path cli) : executable(std::move(cli))
    {
        write_file(managed, "abc");
        const ResourceDefinition resource{uuid<ResourceId>(), "固定资源", ResourceKind::Binary,
            ManagedResource{{PathBase::SolutionDirectory, "resources/托管 文件.bin", {}}, 7,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}};
        const Solution solution{uuid<SolutionId>(), "命令行验收", solution_format_version, 12,
            {fork_flow({resource.id, resource.kind, 7}), branch_flow()}, {resource}, {}, {}, {}, {}};
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

void verify_fork(const Json& report, std::string_view revision)
{
    check_run(report, '1', revision);
    const auto& order = report.at("call_order");
    check(order.size() == 8 && order.front() == uuid<NodeId>('1').to_string() && order[6] == uuid<NodeId>('7').to_string()
        && order.back() == uuid<NodeId>('8').to_string(), "Five lane calls complete between the entry and one invocation each of join and delivery.");
    const auto& entry = node(report, '1');
    check(produced(entry) == "97", "The entry reads the first byte of the managed file.");
    const auto& leases = entry.at("resources");
    check(leases.size() == 1 && leases.front().at("slot") == "payload" && leases.front().at("resource") == uuid<ResourceId>().to_string()
        && leases.front().at("kind") == "Binary" && leases.front().at("revision") == "7", "The acquired resource has the expected identity and revision.");
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
        }
    }
    return report;
}

void roundtrip(const fs::path& executable)
{
    const Fixture fixture(executable);
    const auto original = read_file(fixture.manifest);
    const auto fork = success(fixture.run('1', L"offset=7"));
    verify_fork(fork, "12");
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
    check(cleanup.at("diagnostics").empty() && read_file(fixture.managed) == "abc", "The save retains the unchanged managed resource.");
    const auto reloaded = success(fixture.run('1', L"offset=7"));
    verify_fork(reloaded, "13");
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

void diagnostics(const fs::path& executable)
{
    const Fixture fixture(executable);
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
    check(read_file(fixture.manifest) == unknown && read_file(fixture.managed) == "abc", "A rejected save preserves original bytes.");
    write_file(fixture.manifest, "{invalid");
    rejected(fixture.run('1', L"offset=7"), 1, "InvalidSolutionManifest");
    write_file(fixture.manifest, original);

    check(fs::remove(fixture.managed), "The fixture removed the managed source before another request.");
    rejected(fixture.run('1', L"offset=7"), 1, "ResourceNotFound");
    check(read_file(fixture.manifest) == original, "A missing resource leaves the loaded manifest unchanged.");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 3) { throw std::runtime_error("Expected CLI executable and test group."); }
        const auto cli = fs::absolute(argv[1]);
        const std::wstring_view group(argv[2]);
        if (group == L"roundtrip") { roundtrip(cli); }
        else if (group == L"diagnostics") { diagnostics(cli); }
        else { throw std::runtime_error("Unknown CLI test group."); }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
