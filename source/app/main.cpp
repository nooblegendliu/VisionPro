#include "run_summary.hpp"
#include "simulation.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace visionpro::app {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;
using nlohmann::json;
namespace fs = std::filesystem;

enum class Command { Run, Save };
struct InputArgument { InputId id; std::string text; };
struct Arguments {
    Command command;
    fs::path manifest{};
    std::optional<WorkflowId> workflow{};
    std::vector<InputArgument> inputs{};
    std::vector<NamedResourceRoot> roots{};
};

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr auto usage =
    "visionpro_cli run --solution <solution.json> --workflow <uuid> [--input <id>=<value>]... [--root <id>=<directory>]...\n"
    "visionpro_cli save --solution <solution.json> [--root <id>=<directory>]...\n"
    "Inputs follow the workflow's public Bool, Int64, Float64 and String schemas.\n"
    "The installed catalog contains the VisionPro simulation tools.\n";

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

Arguments parse_arguments(std::span<wchar_t*> args)
{
    if (args.empty()) { throw UsageError("Choose run or save. See --help."); }
    const std::wstring_view command{args.front()};
    if (command != L"run" && command != L"save") { throw UsageError("Unknown command. See --help."); }
    Arguments parsed{command == L"run" ? Command::Run : Command::Save};
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
        } else if (parsed.command == Command::Run && option == L"--workflow") {
            if (parsed.workflow) { throw UsageError("Specify one workflow ID."); }
            parsed.workflow = argument_value(IdFactory::parse<WorkflowId>(utf8(value)));
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
    if (parsed.command == Command::Run && !parsed.workflow) { throw UsageError("Specify --workflow <uuid>."); }
    return parsed;
}

template<class T> T number(std::string_view text)
{
    T value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) { throw UsageError("An input does not match its numeric type."); }
    return value;
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
    default: throw UsageError("Explicit CLI inputs support Bool, Int64, Float64 and String.");
    }
}

std::vector<RunInput> fixed_inputs(const Arguments& args, const Workflow& flow)
{
    std::vector<RunInput> inputs;
    inputs.reserve(args.inputs.size());
    for (const auto& supplied : args.inputs) {
        const auto found = std::ranges::find(flow.interface.inputs, supplied.id, &InputDescriptor::id);
        if (found == flow.interface.inputs.end()) {
            throw UsageError("Unknown workflow input '" + std::string(supplied.id.text()) + "'.");
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

int save(const SolutionDocument& document)
{
    auto saved = save_solution(document, document.solution());
    if (const auto* failure = std::get_if<Diagnostic>(&saved)) { return report_failure("save", *failure); }
    const auto& result = std::get<SolutionSaveResult>(saved);
    const json cleanup{{"removed", result.cleanup.removed}, {"deferred", result.cleanup.deferred},
        {"diagnostics", diagnostics_json(result.cleanup.diagnostics)}};
    std::cout << json{{"solution", result.solution.id.to_string()}, {"revision", std::to_string(result.solution.revision)},
        {"status", "Committed"}, {"cleanup", cleanup}, {"load_diagnostics", diagnostics_json(document.diagnostics())}}.dump(2) << '\n';
    return 0;
}

int run(const Arguments& args, const SolutionDocument& document, const ToolRegistry& registry)
{
    const auto* flow = find_workflow(document.solution(), *args.workflow);
    if (!flow) {
        DiagnosticLocation location;
        location.solution = document.solution().id;
        location.workflow = args.workflow;
        return report_failure("request", {ErrorCode::InvalidRunRequest, Severity::Error, std::move(location),
            "The selected workflow ID is absent from the solution."});
    }
    const auto inputs = fixed_inputs(args, *flow);
    auto opened = open_solution_execution(document, registry);
    if (const auto* failure = std::get_if<Diagnostic>(&opened)) { return report_failure("load", *failure); }
    const auto& execution = std::get<LoadedSolutionExecution>(opened);
    const auto trigger = argument_value(SchemaIdFactory::parse<SchemaId>("visionpro.cli"));
    auto request = RunRequest::create(execution.snapshot, WorkflowTarget{flow->id}, inputs, {TriggerKind::Manual, trigger});
    if (const auto* failure = std::get_if<Diagnostic>(&request)) { return report_failure("request", *failure); }
    auto submitted = execution.engine->submit(std::get<RunRequest>(request));
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
    auto summary = summarize_run(*handle, wait.progress, document.diagnostics());
    if (const auto* failure = std::get_if<Diagnostic>(&summary)) { return report_failure("report", *failure); }
    std::cout << std::get<json>(summary).dump(2) << '\n';
    if (wait.progress.status == ExecutionStatus::Succeeded) { return 0; }
    std::cerr << json{{"stage", "execute"}, {"diagnostics", diagnostics_json(wait.progress.diagnostics)}}.dump(2) << '\n';
    return 1;
}

int execute(std::span<wchar_t*> args)
{
    if (args.size() == 1 && std::wstring_view(args.front()) == L"--help") {
        std::cout << usage;
        return 0;
    }
    try {
        const auto options = parse_arguments(args);
        auto registered = tools::make_simulation_registry();
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return report_failure("registry", *failure); }
        const auto& registry = std::get<ToolRegistry>(registered);
        auto loaded = load_solution(options.manifest.parent_path(), registry, options.roots);
        if (const auto* failure = std::get_if<Diagnostic>(&loaded)) { return report_failure("load", *failure); }
        const auto& document = std::get<SolutionDocument>(loaded);
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
