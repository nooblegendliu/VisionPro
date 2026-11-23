#include "run_summary.hpp"
#include "tool_catalog.hpp"
#include "trigger_command.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/text.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/infrastructure/mvs_camera.hpp>

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace visionpro::app {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
using nlohmann::json;
namespace fs = std::filesystem;

enum class Command { Run, RunSolution, Save, Trigger, Replay };
struct InputArgument { InputId id; std::string text; };
struct ManualRun {
    WorkflowId workflow;
    std::optional<NodeId> group;
    std::vector<InputArgument> inputs;
};
struct Arguments {
    Command command;
    fs::path manifest{};
    std::optional<WorkflowId> workflow{};
    std::optional<NodeId> group{};
    std::vector<InputArgument> inputs{};
    std::vector<NamedResourceRoot> roots{};
    TriggerSequence sequence{};
    fs::path replay_sequence{};
    std::vector<ManualRun> entry_inputs{};
};

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr auto usage =
    "visionpro_cli cameras\n"
    "visionpro_cli run --solution <solution.json> --workflow <uuid> [--group <uuid>] [--input <id>=<value>]... [--root <id>=<directory>]...\n"
    "visionpro_cli run-solution --solution <solution.json> [--entry-input <workflow-uuid>/<id>=<value>]... [--root <id>=<directory>]...\n"
    "visionpro_cli replay --solution <solution.json> --sequence <runs.json> [--root <id>=<directory>]...\n"
    "Replay runs in order in one session. The sequence is a JSON array of {\"workflow\":\"uuid\",\"inputs\":{\"id\":\"value\"}}.\n"
    "visionpro_cli save --solution <solution.json> [--root <id>=<directory>]...\n"
    "visionpro_cli trigger --solution <solution.json> --workflow <uuid> [--frame <int64>]... [--stop drain|cancel] [--root <id>=<directory>]...\n"
    "For a continuous entry, use --advance-ms <uint32> repeatedly to advance its controlled clock. Default stop mode is drain.\n"
    "For a camera entry, use --camera-frames <1..10000> to acquire and submit real frames from the configured device.\n"
    "Use --group to debug one composite Group in isolation. Inputs follow the selected public interface.\n"
    "Built-in tools include simulation, list selection, integer range judgement, calculation, logic and formatting.\n"
    "Shared tools read and write global values, send and receive queued records, and join workpiece measurements.\n"
#ifdef VISIONPRO_WITH_OPENCV
    "Image tools include visionpro.image.read_file, visionpro.image.threshold and visionpro.image.blob.\n"
#endif
    "Explicit inputs support Bool, Int64, Float64, String and Binary (two hex digits per byte).\n";

std::string utf8(std::wstring_view text)
{
    if (text.empty()) { return {}; }
    const auto length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) { throw UsageError("An argument contains invalid Unicode."); }
    std::string value(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, value.data(), size, nullptr, nullptr) != size) {
        throw UsageError("Could not convert an argument to UTF-8.");
    }
    return value;
}

template<class T> T argument_value(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw UsageError(failure->message); }
    return std::get<T>(std::move(value));
}

std::pair<std::wstring_view, std::wstring_view> assignment(std::wstring_view text)
{
    const auto separator = text.find(L'=');
    if (separator == std::wstring_view::npos || separator == 0) { throw UsageError("Expected an ID and value separated by '='."); }
    return {text.substr(0, separator), text.substr(separator + 1)};
}

template<class T> T number(std::string_view text)
{
    T value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) { throw UsageError("An input does not match its numeric type."); }
    return value;
}

Arguments parse_arguments(std::span<wchar_t*> args)
{
    if (args.empty()) { throw UsageError("Choose run, run-solution, replay, save, trigger or cameras. See --help."); }
    const std::wstring_view command{args.front()};
    Arguments parsed{Command::Run};
    if (command == L"save") { parsed.command = Command::Save; }
    else if (command == L"trigger") { parsed.command = Command::Trigger; }
    else if (command == L"replay") { parsed.command = Command::Replay; }
    else if (command == L"run-solution") { parsed.command = Command::RunSolution; }
    else if (command != L"run") { throw UsageError("Unknown command. See --help."); }
    for (std::size_t index = 1; index < args.size(); index += 2) {
        const std::wstring_view option{args[index]};
        if (index + 1 == args.size()) { throw UsageError("An option is missing its value. See --help."); }
        const std::wstring_view value{args[index + 1]};
        if (option == L"--solution") {
            if (!parsed.manifest.empty() || value.empty()) { throw UsageError("Specify one solution.json path."); }
            parsed.manifest = fs::absolute(fs::path(value));
            if (CompareStringOrdinal(parsed.manifest.filename().c_str(), -1, L"solution.json", -1, TRUE) != CSTR_EQUAL) {
                throw UsageError("The --solution path must name solution.json.");
            }
        } else if (option == L"--root") {
            const auto [name, path] = assignment(value);
            const auto id = argument_value(SchemaIdFactory::parse<SchemaId>(utf8(name)));
            if (path.empty() || std::ranges::find(parsed.roots, id, &NamedResourceRoot::id) != parsed.roots.end()) {
                throw UsageError("Each named root requires one directory.");
            }
            parsed.roots.push_back({id, fs::absolute(fs::path(path))});
        } else if (parsed.command == Command::Replay && option == L"--sequence") {
            if (!parsed.replay_sequence.empty() || value.empty()) { throw UsageError("Specify one replay sequence file."); }
            parsed.replay_sequence = fs::absolute(fs::path(value));
        } else if ((parsed.command == Command::Run || parsed.command == Command::Trigger) && option == L"--workflow") {
            if (parsed.workflow) { throw UsageError("Specify one workflow ID."); }
            parsed.workflow = argument_value(IdFactory::parse<WorkflowId>(utf8(value)));
        } else if (parsed.command == Command::Trigger && option == L"--frame") {
            parsed.sequence.frames.push_back(number<std::int64_t>(utf8(value)));
        } else if (parsed.command == Command::Trigger && option == L"--advance-ms") {
            parsed.sequence.advances.push_back(std::chrono::milliseconds(number<std::uint32_t>(utf8(value))));
        } else if (parsed.command == Command::Trigger && option == L"--camera-frames") {
            const auto count = number<std::uint32_t>(utf8(value));
            if (parsed.sequence.camera_frames || !count || count > 10000) { throw UsageError("Specify --camera-frames once with a count of 1..10000."); }
            parsed.sequence.camera_frames = count;
        } else if (parsed.command == Command::Trigger && option == L"--stop") {
            if (parsed.sequence.stop) { throw UsageError("Specify --stop once."); }
            if (value == L"drain") { parsed.sequence.stop = TriggerStopMode::Drain; }
            else if (value == L"cancel") { parsed.sequence.stop = TriggerStopMode::Cancel; }
            else { throw UsageError("Choose drain or cancel for --stop."); }
        } else if (parsed.command == Command::Run && option == L"--group") {
            if (parsed.group) { throw UsageError("Specify one Group ID."); }
            parsed.group = argument_value(IdFactory::parse<NodeId>(utf8(value)));
        } else if (parsed.command == Command::RunSolution && option == L"--entry-input") {
            const auto [qualified, text] = assignment(value);
            const auto slash = qualified.find(L'/');
            if (slash == std::wstring_view::npos) { throw UsageError("An entry input requires <workflow-uuid>/<input-id>=<value>."); }
            const auto workflow = argument_value(IdFactory::parse<WorkflowId>(utf8(qualified.substr(0, slash))));
            const auto id = argument_value(SchemaIdFactory::parse<InputId>(utf8(qualified.substr(slash + 1))));
            auto flow = std::ranges::find(parsed.entry_inputs, workflow, &ManualRun::workflow);
            if (flow == parsed.entry_inputs.end()) {
                parsed.entry_inputs.push_back({workflow, {}, {{id, utf8(text)}}});
            } else {
                if (std::ranges::find(flow->inputs, id, &InputArgument::id) != flow->inputs.end()) {
                    throw UsageError("Specify each workflow entry input once.");
                }
                flow->inputs.push_back({id, utf8(text)});
            }
        } else if (parsed.command == Command::Run && option == L"--input") {
            const auto [name, text] = assignment(value);
            const auto id = argument_value(SchemaIdFactory::parse<InputId>(utf8(name)));
            if (std::ranges::find(parsed.inputs, id, &InputArgument::id) != parsed.inputs.end()) {
                throw UsageError("Specify each input ID once.");
            }
            parsed.inputs.push_back({id, utf8(text)});
        } else {
            throw UsageError("Unknown option for this command. See --help.");
        }
    }
    if (parsed.manifest.empty()) { throw UsageError("Specify --solution <solution.json>."); }
    if (parsed.command == Command::Replay && parsed.replay_sequence.empty()) { throw UsageError("Specify --sequence <runs.json>."); }
    if ((parsed.command == Command::Run || parsed.command == Command::Trigger) && !parsed.workflow) {
        throw UsageError("Specify --workflow <uuid>.");
    }
    return parsed;
}

DataValue input_value(const InputDescriptor& input, std::string_view text)
{
    switch (input.type.kind()) {
    case TypeKind::Bool:
        if (text == "true") { return DataValue::boolean(true); }
        if (text == "false") { return DataValue::boolean(false); }
        throw UsageError("A Bool input requires true or false.");
    case TypeKind::Int64: return DataValue::integer(number<std::int64_t>(text));
    case TypeKind::Float64: return argument_value(DataValue::real(number<double>(text)));
    case TypeKind::String: return argument_value(DataValue::string(text));
    case TypeKind::Binary: {
        auto bytes = decode_hex(text);
        if (!bytes) { throw UsageError("A Binary input requires two hex digits per byte."); }
        return DataValue::binary(*bytes);
    }
    default: throw UsageError("Explicit CLI inputs support Bool, Int64, Float64, String and Binary.");
    }
}

std::vector<ManualRun> read_sequence(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw UsageError("Could not open the replay sequence file."); }
    std::vector<std::set<std::string>> object_keys;
    const auto reject_duplicates = [&](int, json::parse_event_t event, json& parsed) {
        if (event == json::parse_event_t::object_start) { object_keys.emplace_back(); }
        if (event == json::parse_event_t::object_end) { object_keys.pop_back(); }
        if (event == json::parse_event_t::key && !object_keys.back().insert(parsed.get_ref<const std::string&>()).second) {
            throw UsageError("Duplicate replay field: " + parsed.get<std::string>());
        }
        return true;
    };
    const auto steps = json::parse(input, reject_duplicates, false);
    if (!steps.is_array() || steps.empty()) { throw UsageError("The replay sequence must be a nonempty JSON array."); }
    std::vector<ManualRun> runs;
    for (const auto& step : steps) {
        if (!step.is_object() || !step.contains("workflow") || !step.at("workflow").is_string()
            || step.size() != (step.contains("inputs") ? 2u : 1u)) {
            throw UsageError("Each replay step requires a workflow UUID and optional inputs.");
        }
        ManualRun run{argument_value(IdFactory::parse<WorkflowId>(step.at("workflow").get_ref<const std::string&>())), {}, {}};
        if (step.contains("inputs")) {
            const auto& inputs = step.at("inputs");
            if (!inputs.is_object()) { throw UsageError("Replay inputs must be an object of ID/text pairs."); }
            for (const auto& [name, value] : inputs.items()) {
                if (!value.is_string()) { throw UsageError("A replay input value must be text, as for --input."); }
                run.inputs.push_back({argument_value(SchemaIdFactory::parse<InputId>(name)), value.get<std::string>()});
            }
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

std::vector<RunInput> fixed_inputs(const ManualRun& args, const PublicInterface& interface)
{
    std::vector<RunInput> inputs;
    inputs.reserve(args.inputs.size());
    for (const auto& supplied : args.inputs) {
        const auto found = std::ranges::find(interface.inputs, supplied.id, &InputDescriptor::id);
        if (found == interface.inputs.end()) {
            const std::string prefix = args.group ? "Unknown Group input '" : "Unknown workflow input '";
            throw UsageError(prefix + std::string(supplied.id.text()) + "'.");
        }
        inputs.push_back({supplied.id, input_value(*found, supplied.text)});
    }
    return inputs;
}

int report_failure(std::string_view stage, const Diagnostic& diagnostic, int code = 1)
{
    std::cerr << json{{"stage", stage}, {"diagnostics", diagnostics_json(std::span{&diagnostic, 1})}}.dump(2) << '\n';
    return code;
}

int finish_output()
{
    // Commit the buffered tail before reporting success instead of relying on process teardown.
    std::cout.flush();
    if (!std::cout) {
        return report_failure("report", {ErrorCode::ExecutionBoundaryFailure, Severity::Error, {},
            "Could not write the complete command output to standard output."});
    }
    return 0;
}

int save(const SolutionDocument& document)
{
    auto saved = save_solution(document, document.solution());
    if (const auto* failure = std::get_if<Diagnostic>(&saved)) { return report_failure("save", *failure); }
    const auto& result = std::get<SolutionSaveResult>(saved);
    const json cleanup{{"removed", result.cleanup.removed}, {"deferred", result.cleanup.deferred},
        {"diagnostics", diagnostics_json(result.cleanup.diagnostics)}};
    std::cout << json{{"solution", result.solution.id.to_string()}, {"revision", std::to_string(result.solution.revision)},
        {"status", "Committed"}, {"cleanup", cleanup}, {"load_diagnostics", diagnostics_json(document.diagnostics())}}.dump(2) << '\n';
    return finish_output();
}

Result<RunRequest> manual_request(const ManualRun& args, const ConfigSnapshot& snapshot)
{
    const auto* flow = find_workflow(snapshot.solution(), args.workflow);
    if (!flow) {
        DiagnosticLocation location;
        location.solution = snapshot.solution().id;
        location.workflow = args.workflow;
        return Diagnostic{ErrorCode::InvalidRunRequest, Severity::Error, std::move(location),
            "The selected workflow ID is absent from the solution."};
    }
    RunTarget target = WorkflowTarget{flow->id};
    const auto* interface = &flow->interface;
    if (args.group) {
        const auto* node = find_node(*flow, *args.group);
        const auto* group = node ? std::get_if<GroupConfiguration>(&node->configuration) : nullptr;
        if (!group || group->loop) {
            DiagnosticLocation location;
            location.workflow = flow->id;
            location.node = args.group;
            return Diagnostic{ErrorCode::InvalidRunRequest, Severity::Error, std::move(location),
                "The selected node must be a composite Group in this workflow."};
        }
        target = NodeTarget{flow->id, node->id};
        interface = &group->interface;
    }
    const auto inputs = fixed_inputs(args, *interface);
    const auto trigger = argument_value(SchemaIdFactory::parse<SchemaId>("visionpro.cli"));
    return RunRequest::create(snapshot, target, inputs,
        {args.group ? TriggerKind::Debug : TriggerKind::Manual, trigger});
}

int execute_run(const RunRequest& request, WorkflowEngine& engine, std::span<const Diagnostic> diagnostics, json& report)
{
    auto submitted = engine.submit(request);
    if (const auto* failure = std::get_if<RunRejected>(&submitted)) { return report_failure("admission", failure->diagnostic); }
    const auto* handle = std::get_if<RunHandle>(&submitted);
    if (!handle) {
        return report_failure("admission", {ErrorCode::InvalidRunRequest, Severity::Error, {}, "The workflow is busy."});
    }
    const auto waited = handle->wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::time_point::max());
    if (const auto* failure = std::get_if<Diagnostic>(&waited)) { return report_failure("execute", *failure); }
    const auto& wait = std::get<RunWaitResult>(waited);
    if (wait.outcome != RunWaitOutcome::Satisfied) {
        return report_failure("execute", {ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "The run has not exited."});
    }
    auto summary = summarize_run(*handle, wait.progress, diagnostics);
    if (const auto* failure = std::get_if<Diagnostic>(&summary)) { return report_failure("report", *failure); }
    report = std::get<json>(std::move(summary));
    if (wait.progress.status == ExecutionStatus::Succeeded) { return 0; }
    std::cerr << json{{"stage", "execute"}, {"diagnostics", diagnostics_json(wait.progress.diagnostics)}}.dump(2) << '\n';
    return 1;
}

int run(const Arguments& args, const SolutionDocument& document, const ToolRegistry& registry)
{
    const bool replay = args.command == Command::Replay;
    const auto steps = replay ? read_sequence(args.replay_sequence)
        : std::vector<ManualRun>{{*args.workflow, args.group, args.inputs}};
    auto opened = open_solution_execution(document, registry);
    if (const auto* failure = std::get_if<Diagnostic>(&opened)) { return report_failure("load", *failure); }
    const auto& execution = std::get<LoadedSolutionExecution>(opened);
    std::vector<RunRequest> requests;
    for (const auto& step : steps) {
        auto request = manual_request(step, execution.snapshot);
        if (const auto* failure = std::get_if<Diagnostic>(&request)) { return report_failure("request", *failure); }
        requests.push_back(std::get<RunRequest>(std::move(request)));
    }
    json runs = json::array();
    int status = 0;
    for (const auto& request : requests) {
        json report;
        status = execute_run(request, *execution.engine, document.diagnostics(), report);
        if (!report.is_null()) { runs.push_back(std::move(report)); }
        if (status != 0) { break; }
    }
    if (replay || !runs.empty()) {
        const auto report = replay ? json{{"runs", std::move(runs)}} : std::move(runs.front());
        std::cout << report.dump(2) << '\n';
        if (const auto output_error = finish_output(); output_error != 0) { return output_error; }
    }
    return status;
}

int run_solution(const Arguments& args, const SolutionDocument& document, const ToolRegistry& registry)
{
    const auto& solution = document.solution();
    std::vector<WorkflowRunInputs> inputs;
    for (const auto& entry : args.entry_inputs) {
        const auto* flow = find_workflow(solution, entry.workflow);
        if (!flow || !flow->enabled || (solution.graph
            && std::ranges::find(solution.graph->entries, entry.workflow) == solution.graph->entries.end())) {
            throw UsageError("An entry input must identify an enabled solution entry workflow.");
        }
        inputs.push_back({entry.workflow, fixed_inputs(entry, flow->interface)});
    }
    auto created = WorkflowSession::create(registry);
    if (const auto* failure = std::get_if<Diagnostic>(&created)) { return report_failure("session", *failure); }
    auto& session = *std::get<std::unique_ptr<WorkflowSession>>(created);
    auto activated = activate_solution(session, document);
    if (const auto* failure = std::get_if<Diagnostic>(&activated)) { return report_failure("load", *failure); }
    const auto trigger = argument_value(SchemaIdFactory::parse<SchemaId>("visionpro.cli"));
    auto result = session.run_solution(std::get<ConfigSnapshot>(activated), inputs, {TriggerKind::Manual, trigger});
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { return report_failure("solution", *failure); }
    const auto& run = std::get<SolutionRunReport>(result);
    auto summary = summarize_solution(run, solution, document.diagnostics());
    if (const auto* failure = std::get_if<Diagnostic>(&summary)) { return report_failure("report", *failure); }
    std::cout << std::get<json>(summary).dump(2) << '\n';
    if (const auto output_error = finish_output(); output_error != 0) { return output_error; }
    if (run.status == ExecutionStatus::Succeeded) { return 0; }
    std::cerr << json{{"stage", "solution"}, {"diagnostics", diagnostics_json(run.diagnostics)}}.dump(2) << '\n';
    return 1;
}

int execute(std::span<wchar_t*> args)
{
    if (args.size() == 1 && std::wstring_view(args.front()) == L"--help") {
        std::cout << usage;
        return finish_output();
    }
    try {
        if (!args.empty() && std::wstring_view(args.front()) == L"cameras") {
            if (args.size() != 1) { throw UsageError("cameras takes no arguments."); }
            auto inventory = query_mvs_cameras();
            json mvs{{"available", false}, {"sdk_version", nullptr}, {"devices", json::array()}};
            if (const auto* failure = std::get_if<Diagnostic>(&inventory)) {
                if (failure->code != ErrorCode::MissingCapability || failure->external) { return report_failure("camera_inventory", *failure); }
            } else {
                const auto& result = std::get<MvsCameraInventory>(inventory);
                mvs["available"] = true;
                mvs["sdk_version"] = result.sdk_version;
                for (const auto& item : result.devices) {
                    mvs["devices"].push_back({{"serial", item.serial}, {"model", item.model}, {"firmware", item.firmware}, {"transport", item.transport}});
                }
            }
            std::cout << json{{"simulated", {"SIM-001"}}, {"mvs", std::move(mvs)}}.dump(2) << '\n';
            return finish_output();
        }
        const auto options = parse_arguments(args);
        auto registered = make_tool_registry();
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return report_failure("registry", *failure); }
        const auto& registry = std::get<ToolRegistry>(registered);
        auto loaded = load_solution(options.manifest.parent_path(), registry, options.roots);
        if (const auto* failure = std::get_if<Diagnostic>(&loaded)) { return report_failure("load", *failure); }
        const auto& document = std::get<SolutionDocument>(loaded);
        if (options.command == Command::RunSolution) { return run_solution(options, document, registry); }
        if (options.command == Command::Trigger) {
            auto result = replay_triggers(document, registry, *options.workflow, options.sequence);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) { return report_failure("trigger", *failure); }
            const auto& replay = std::get<TriggerReplay>(result);
            std::cout << replay.report.dump(2) << '\n';
            if (const auto output_error = finish_output(); output_error != 0) { return output_error; }
            if (replay.failures.empty()) { return 0; }
            std::cerr << json{{"stage", "trigger"}, {"diagnostics", diagnostics_json(replay.failures)}}.dump(2) << '\n';
            return 1;
        }
        return options.command == Command::Save ? save(document) : run(options, document, registry);
    } catch (const UsageError& error) {
        return report_failure("arguments", {ErrorCode::InvalidRunRequest, Severity::Error, {}, error.what()}, 2);
    } catch (const fs::filesystem_error&) {
        return report_failure("paths", {ErrorCode::InvalidPath, Severity::Error, {}, "Could not resolve a command path."});
    } catch (...) {
        return report_failure("command", {ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Unexpected command failure."});
    }
}
} // namespace
} // namespace visionpro::app

int wmain(int argc, wchar_t** argv)
{
    return visionpro::app::execute({argv + 1, static_cast<std::size_t>(argc - 1)});
}
