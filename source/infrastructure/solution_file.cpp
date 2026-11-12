#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include "solution_codec.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace workflow;
namespace fs = std::filesystem;

Diagnostic error(ErrorCode code, std::string message, std::optional<SolutionId> solution = {})
{
    DiagnosticLocation location;
    location.solution = solution;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

Diagnostic unknown_content(const SolutionDocument& document)
{
    return error(ErrorCode::UnsupportedSolutionContent,
        "The loaded solution contains unknown manifest fields.", document.solution().id);
}

Result<std::string> read_manifest(const fs::path& manifest)
{
    if (!fs::is_regular_file(manifest)) {
        return error(ErrorCode::SolutionIoFailure, "The solution directory requires a regular solution.json file.");
    }
    const auto size = fs::file_size(manifest);
    std::string text;
    if (size > text.max_size() || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        return error(ErrorCode::SolutionIoFailure, "The manifest exceeds the supported file size.");
    }
    text.resize(static_cast<std::size_t>(size));
    std::ifstream input(manifest, std::ios::binary);
    if (!input || !input.read(text.data(), static_cast<std::streamsize>(size))
        || input.peek() != std::char_traits<char>::eof() || input.bad()) {
        return error(ErrorCode::SolutionIoFailure, "Could not read the complete solution manifest.");
    }
    return text;
}

void diagnose_tool_support(const Solution& solution, const ToolRegistry& registry, std::vector<Diagnostic>& diagnostics)
{
    for (const auto& flow : solution.workflows) {
        std::set<ToolTypeId> checked;
        for (const auto& node : flow.nodes) {
            const auto* configured = std::get_if<ToolConfiguration>(&node.configuration);
            if (!configured || !checked.insert(configured->type).second) { continue; }
            const auto* descriptor = registry.find_descriptor(configured->type);
            if (!descriptor) { continue; } // diagnose_solution reports missing tools.
            auto supported = registry.validate(*descriptor);
            if (auto* failure = std::get_if<Diagnostic>(&supported)) {
                failure->location.solution = solution.id;
                failure->location.workflow = flow.id;
                failure->location.node = node.id;
                diagnostics.push_back(std::move(*failure));
            }
        }
    }
}

struct TemporaryManifest {
    fs::path directory;
    fs::path file;
    ~TemporaryManifest()
    {
        std::error_code ignored;
        fs::remove(file, ignored);
        fs::remove(directory, ignored);
    }
};

bool manifest_exists(const fs::path& manifest)
{
    std::error_code ignored;
    return fs::exists(fs::symlink_status(manifest, ignored));
}

Result<std::monostate> publish_manifest(const fs::path& directory, const std::string& text)
{
    fs::create_directories(directory);
    const auto manifest = directory / solution_manifest_name;
    const auto exists = [] {
        return error(ErrorCode::SolutionAlreadyExists, "The destination already contains a solution manifest.");
    };
    if (manifest_exists(manifest)) { return exists(); }
    auto issued = IdFactory::generate<MessageId>();
    if (auto* failure = std::get_if<Diagnostic>(&issued)) { return std::move(*failure); }
    auto staging = directory / (".visionpro-manifest-" + std::get<MessageId>(issued).to_string());
    auto file = staging / solution_manifest_name;
    if (!fs::create_directory(staging)) {
        return error(ErrorCode::SolutionIoFailure, "Could not reserve the temporary manifest directory.");
    }
    const TemporaryManifest temporary{std::move(staging), std::move(file)};
    {
        std::ofstream output(temporary.file, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) { return error(ErrorCode::SolutionIoFailure, "Could not write the complete temporary manifest."); }
    }
    // The same-volume rename must fail if another writer created the target.
    if (!MoveFileExW(temporary.file.c_str(), manifest.c_str(), 0)) {
        const auto native_error = GetLastError();
        if (native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS || manifest_exists(manifest)) { return exists(); }
        return error(ErrorCode::SolutionIoFailure, "Could not publish a new solution manifest. Win32 error "
            + std::to_string(native_error) + ".");
    }
    return std::monostate{};
}

} // namespace

core::Result<SolutionDocument> load_solution(const fs::path& directory, const ToolRegistry& registry,
    std::span<const NamedResourceRoot> roots)
{
    try {
        if (!directory.is_absolute()) {
            return error(ErrorCode::InvalidPath, "The solution directory must be absolute.");
        }
        auto base = fs::canonical(directory);
        auto bytes = read_manifest(base / solution_manifest_name);
        if (auto* failure = std::get_if<Diagnostic>(&bytes)) { return std::move(*failure); }
        const auto json = detail::parse_manifest(std::get<std::string>(bytes));
        std::vector<Diagnostic> diagnostics;
        auto solution = detail::decode_solution(detail::Reader{json, diagnostics});
        const bool complete = diagnostics.empty();
        auto diagnosed = diagnose_solution(solution, registry.descriptors());
        if (auto* failure = std::get_if<Diagnostic>(&diagnosed)) { return std::move(*failure); }
        const auto& edit = std::get<std::vector<Diagnostic>>(diagnosed);
        diagnostics.insert(diagnostics.end(), edit.begin(), edit.end());
        diagnose_tool_support(solution, registry, diagnostics);
        auto resolved = FileResourceProvider::create(solution, base, roots);
        if (auto* failure = std::get_if<Diagnostic>(&resolved)) { return std::move(*failure); }
        auto provider = std::get<std::shared_ptr<FileResourceProvider>>(std::move(resolved));
        const auto resource_diagnostics = provider->diagnostics();
        diagnostics.insert(diagnostics.end(), resource_diagnostics.begin(), resource_diagnostics.end());
        return SolutionDocument{std::move(solution), std::move(base), std::move(diagnostics), complete, std::move(provider)};
    } catch (detail::CodecError& failure) {
        return std::move(failure.diagnostic);
    } catch (const detail::Json::parse_error& failure) {
        return error(ErrorCode::InvalidSolutionManifest, "Invalid JSON near byte " + std::to_string(failure.byte) + ".");
    } catch (const detail::Json::exception&) {
        return error(ErrorCode::InvalidSolutionManifest, "The solution manifest cannot be decoded.");
    } catch (...) {
        return error(ErrorCode::SolutionIoFailure, "Could not load the solution directory.");
    }
}

core::Result<std::monostate> write_new_solution(const fs::path& directory, const Solution& solution)
{
    try {
        if (!directory.is_absolute()) {
            return error(ErrorCode::InvalidPath, "The destination solution directory must be absolute.", solution.id);
        }
        if (solution.format_version != solution_format_version) {
            return error(ErrorCode::UnsupportedSolutionFormat, "The model's solution format version is unsupported.", solution.id);
        }
        auto checked = validate_model_shape(solution);
        if (auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        const auto text = detail::serialize_manifest(detail::encode_solution(solution));
        if (text.size() > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
            return error(ErrorCode::SolutionIoFailure, "The encoded manifest exceeds the supported file size.", solution.id);
        }
        return publish_manifest(directory, text);
    } catch (detail::CodecError& failure) {
        return std::move(failure.diagnostic);
    } catch (const detail::Json::exception&) {
        return error(ErrorCode::InvalidSolutionManifest, "The model cannot be represented as a UTF-8 manifest.", solution.id);
    } catch (...) {
        return error(ErrorCode::SolutionIoFailure, "Could not write the solution directory.", solution.id);
    }
}

core::Result<std::monostate> write_new_solution(const fs::path& directory, const SolutionDocument& source,
    const Solution& edited)
{
    if (!source.can_save()) { return unknown_content(source); }
    return write_new_solution(directory, edited);
}

core::Result<LoadedSolutionExecution> open_solution_execution(const SolutionDocument& document,
    const ToolRegistry& registry, WorkflowEngineOptions options)
{
    if (!document.can_save()) { return unknown_content(document); }
    auto created = WorkflowEngine::create(registry, document.resources_, options);
    if (auto* failure = std::get_if<Diagnostic>(&created)) { return std::move(*failure); }
    auto engine = std::get<std::unique_ptr<WorkflowEngine>>(std::move(created));
    const auto version = std::get<ConfigSnapshotId>(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
    auto snapshot = ConfigSnapshot::capture(document.solution(), registry.descriptors(), {engine->session(), version});
    if (auto* failure = std::get_if<Diagnostic>(&snapshot)) { return std::move(*failure); }
    return LoadedSolutionExecution{std::get<ConfigSnapshot>(std::move(snapshot)), std::move(engine)};
}

core::Result<workflow::ConfigSnapshot> activate_solution(WorkflowSession& session, const SolutionDocument& document)
{
    if (!document.can_save()) { return unknown_content(document); }
    const auto resources = document.resources_->diagnostics();
    const auto failed = std::ranges::find(resources, Severity::Error, &Diagnostic::severity);
    if (failed != resources.end()) { return *failed; }
    return session.activate(document.solution(), document.resources_);
}

core::Result<workflow::ConfigSnapshot> load_and_activate_solution(WorkflowSession& session, const fs::path& directory,
    const ToolRegistry& registry, std::span<const NamedResourceRoot> roots)
{
    auto candidate = load_solution(directory, registry, roots);
    if (auto* failure = std::get_if<Diagnostic>(&candidate)) { return std::move(*failure); }
    return activate_solution(session, std::get<SolutionDocument>(candidate));
}

} // namespace visionpro::infrastructure
