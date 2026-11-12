#include "solution_save.hpp"

#include "managed_cleanup.hpp"
#include "solution_codec.hpp"
#include "solution_storage.hpp"

#include <visionpro/core/runtime_id_factory.hpp>

#include <windows.h>

#include <algorithm>
#include <limits>
#include <set>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace workflow;
using Check = Result<std::monostate>;
namespace fs = std::filesystem;

Diagnostic error(ErrorCode code, const Solution& solution, std::string message, std::optional<ResourceId> resource = {})
{
    DiagnosticLocation location;
    location.solution = solution.id;
    location.resource = resource;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

bool same_version(const ManagedResource& first, const ManagedResource& second)
{
    return first.revision == second.revision && first.sha256 == second.sha256 && first.path.base == second.path.base
        && first.path.path == second.path.path && first.path.named_root == second.path.named_root;
}

const ResourceDefinition* find_resource(const Solution& solution, ResourceId id)
{
    const auto found = std::ranges::find(solution.resources, id, &ResourceDefinition::id);
    return found == solution.resources.end() ? nullptr : &*found;
}

struct ResourceFile {
    ResourceId id;
    fs::path path;
    std::span<const std::byte> bytes;
};

struct SavePreparation {
    Solution solution;
    fs::path temporary_manifest;
    fs::path resource_directory;
    std::vector<ResourceFile> files;
    std::string manifest;
};

Result<SavePreparation> prepare_save(const SolutionDocument& source, const Solution& edited,
    std::span<const ManagedResourceWrite> writes)
{
    const auto& previous = source.solution();
    if (!source.can_save()) {
        return error(ErrorCode::UnsupportedSolutionContent, previous, "The loaded source contains unknown manifest fields.");
    }
    if (previous.id != edited.id || previous.revision != edited.revision) {
        return error(ErrorCode::SolutionSaveConflict, previous, "The edit must be based on the loaded solution identity and revision.");
    }
    if (edited.format_version != solution_format_version) {
        return error(ErrorCode::UnsupportedSolutionFormat, previous, "The edited manifest format version is unsupported.");
    }
    if (previous.revision == std::numeric_limits<std::uint64_t>::max()) {
        return error(ErrorCode::IdentityExhausted, previous, "The solution revision cannot be incremented.");
    }
    auto issued = IdFactory::generate<MessageId>();
    if (auto* failure = std::get_if<Diagnostic>(&issued)) { return std::move(*failure); }
    const auto transaction = std::get<MessageId>(issued).to_string();
    SavePreparation prepared{edited, source.directory() / (".visionpro-manifest-" + transaction + ".tmp"),
        source.directory() / "resources", {}, {}};
    auto& next = prepared.solution;
    next.revision = previous.revision + 1;
    prepared.files.reserve(writes.size());
    std::set<ResourceId> supplied;
    for (const auto& write : writes) {
        auto configured = std::ranges::find(next.resources, write.resource, &ResourceDefinition::id);
        auto* managed = configured == next.resources.end() ? nullptr : std::get_if<ManagedResource>(&configured->location);
        if (!managed || !supplied.insert(write.resource).second) {
            return error(ErrorCode::InvalidResource, next, "Each file write requires one distinct managed resource in the edited model.", write.resource);
        }
        const auto* existing = find_resource(previous, write.resource);
        const auto* old = existing ? std::get_if<ManagedResource>(&existing->location) : nullptr;
        if (old && old->revision == std::numeric_limits<std::uint64_t>::max()) {
            return error(ErrorCode::IdentityExhausted, next, "The managed resource revision cannot be incremented.", write.resource);
        }
        const std::uint64_t revision = old ? old->revision + 1 : 1;
        auto digest = detail::sha256(write.bytes);
        if (auto* failure = std::get_if<Diagnostic>(&digest)) { return std::move(*failure); }
        const auto filename = write.resource.to_string() + "-" + std::to_string(revision) + "-" + transaction + ".bin";
        *managed = {{PathBase::SolutionDirectory, "resources/" + filename, {}}, revision,
            std::get<std::string>(std::move(digest))};
        prepared.files.push_back({write.resource, prepared.resource_directory / filename, write.bytes});
    }
    for (const auto& resource : next.resources) {
        const auto* managed = std::get_if<ManagedResource>(&resource.location);
        if (!managed || supplied.contains(resource.id)) { continue; }
        const auto* existing = find_resource(previous, resource.id);
        const auto* old = existing ? std::get_if<ManagedResource>(&existing->location) : nullptr;
        if (!old || existing->kind != resource.kind || !same_version(*old, *managed)) {
            return error(ErrorCode::InvalidResource, next, "A changed managed version requires an explicit file write.", resource.id);
        }
    }
    auto validated = validate_model_shape(next);
    if (auto* failure = std::get_if<Diagnostic>(&validated)) { return std::move(*failure); }
    prepared.manifest = detail::serialize_manifest(detail::encode_solution(next));
    return prepared;
}

Check compare_manifest(const SolutionDocument& source, const fs::path& manifest)
{
    auto bytes = detail::read_manifest(manifest);
    if (auto* failure = std::get_if<Diagnostic>(&bytes)) {
        std::error_code io;
        const auto status = fs::symlink_status(manifest, io);
        if (status.type() == fs::file_type::not_found) {
            return error(ErrorCode::SolutionSaveConflict, source.solution(), "The loaded manifest no longer exists.");
        }
        return std::move(*failure);
    }
    const auto& text = std::get<std::string>(bytes);
    auto digest = detail::sha256(std::as_bytes(std::span{text.data(), text.size()}));
    if (auto* failure = std::get_if<Diagnostic>(&digest)) { return std::move(*failure); }
    if (std::get<std::string>(digest) != source.manifest_sha256()) {
        return error(ErrorCode::SolutionSaveConflict, source.solution(), "The on-disk manifest changed after this revision was loaded.");
    }
    return std::monostate{};
}

Check verify_retained_versions(const SolutionDocument& source, const SavePreparation& prepared)
{
    for (const auto& resource : prepared.solution.resources) {
        const auto* managed = std::get_if<ManagedResource>(&resource.location);
        if (!managed || std::ranges::find(prepared.files, resource.id, &ResourceFile::id) != prepared.files.end()) { continue; }
        auto checked = detail::verify_managed_resource(*managed, source.directory());
        if (auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.solution = prepared.solution.id;
            failure->location.resource = resource.id;
            return std::move(*failure);
        }
    }
    return std::monostate{};
}

struct StagedFiles {
    const SavePreparation& preparation;
    std::size_t written{};
    bool resource_directory_created{false};
    bool manifest_written{false};
    bool committed{false};

    ~StagedFiles()
    {
        if (committed) { return; }
        std::error_code ignored;
        if (manifest_written) { fs::remove(preparation.temporary_manifest, ignored); }
        for (const auto& file : std::span{preparation.files}.first(written)) { fs::remove(file.path, ignored); }
        if (resource_directory_created) { fs::remove(preparation.resource_directory, ignored); }
    }
};

std::optional<Diagnostic> at(const detail::SaveCheckpoint& checkpoint, detail::SaveStage stage)
{
    return checkpoint ? checkpoint(stage) : std::nullopt;
}

Check stage_files(StagedFiles& staged, const detail::SaveCheckpoint& checkpoint)
{
    const auto& prepared = staged.preparation;
    if (!prepared.files.empty()) {
        staged.resource_directory_created = fs::create_directory(prepared.resource_directory);
        const auto attributes = GetFileAttributesW(prepared.resource_directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
            || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return error(ErrorCode::InvalidPath, prepared.solution, "The managed resources directory must be a local directory in the package.");
        }
        for (const auto& file : prepared.files) {
            if (auto failure = at(checkpoint, detail::SaveStage::ResourceWrite)) { return std::move(*failure); }
            auto written = detail::write_staged_file(file.path, file.bytes);
            if (auto* failure = std::get_if<Diagnostic>(&written)) {
                failure->location.solution = prepared.solution.id;
                failure->location.resource = file.id;
                return std::move(*failure);
            }
            ++staged.written;
        }
    }
    if (auto failure = at(checkpoint, detail::SaveStage::ManifestWrite)) { return std::move(*failure); }
    const auto& text = prepared.manifest;
    auto written = detail::write_staged_file(prepared.temporary_manifest, std::as_bytes(std::span{text.data(), text.size()}));
    if (auto* failure = std::get_if<Diagnostic>(&written)) { return std::move(*failure); }
    staged.manifest_written = true;
    return std::monostate{};
}

void cleanup_after_commit(SolutionSaveResult& saved, const SolutionDocument& source, const SavePreparation& prepared,
    const fs::path& manifest, const detail::SaveCheckpoint& checkpoint, Diagnostic fallback)
{
    try {
        if (auto failure = at(checkpoint, detail::SaveStage::Cleanup)) {
            saved.cleanup.diagnostics.push_back(std::move(*failure));
            return;
        }
        auto actual = detail::read_manifest(manifest);
        const auto* current = std::get_if<std::string>(&actual);
        if (!current || *current != prepared.manifest) {
            saved.cleanup.diagnostics.push_back(error(ErrorCode::SolutionCleanupFailure, saved.solution,
                "The committed manifest could not be confirmed before resource cleanup."));
            return;
        }
        saved.cleanup = detail::cleanup_managed_versions(source.solution(), saved.solution, source.directory(), source.resource_roots());
    } catch (...) {
        saved.cleanup.diagnostics.push_back(std::move(fallback));
    }
}

} // namespace

core::Result<SolutionSaveResult> detail::save_solution(const SolutionDocument& source, const workflow::Solution& edited,
    std::span<const ManagedResourceWrite> writes, const SaveCheckpoint& checkpoint)
{
    try {
        auto planned = prepare_save(source, edited, writes);
        if (auto* failure = std::get_if<Diagnostic>(&planned)) { return std::move(*failure); }
        auto& prepared = std::get<SavePreparation>(planned);
        if (auto failure = at(checkpoint, SaveStage::BeforeLock)) { return std::move(*failure); }
        auto locked = PackageWriteLock::acquire(source.directory());
        if (auto* failure = std::get_if<Diagnostic>(&locked)) { return std::move(*failure); }
        const auto manifest = source.directory() / solution_manifest_name;
        auto compared = compare_manifest(source, manifest);
        if (auto* failure = std::get_if<Diagnostic>(&compared)) { return std::move(*failure); }
        auto verified = verify_retained_versions(source, prepared);
        if (auto* failure = std::get_if<Diagnostic>(&verified)) { return std::move(*failure); }
        StagedFiles staged{prepared};
        auto written = stage_files(staged, checkpoint);
        if (auto* failure = std::get_if<Diagnostic>(&written)) { return std::move(*failure); }

        SolutionSaveResult saved{std::move(prepared.solution), {}};
        saved.cleanup.diagnostics.reserve(1);
        auto cleanup_failure = error(ErrorCode::SolutionCleanupFailure, saved.solution, "Unexpected failure while cleaning obsolete managed resources.");
        if (auto failure = at(checkpoint, SaveStage::Commit)) { return std::move(*failure); }
        // The cooperative lock covers both comparisons and the replacement.
        // Rechecking also detects edits by another writer during file staging.
        compared = compare_manifest(source, manifest);
        if (auto* failure = std::get_if<Diagnostic>(&compared)) { return std::move(*failure); }
        auto committed = publish_file(prepared.temporary_manifest, manifest, true);
        if (auto* failure = std::get_if<Diagnostic>(&committed)) { return std::move(*failure); }
        staged.committed = true;
        cleanup_after_commit(saved, source, prepared, manifest, checkpoint, std::move(cleanup_failure));
        return Result<SolutionSaveResult>{std::move(saved)};
    } catch (CodecError& failure) {
        return std::move(failure.diagnostic);
    } catch (...) {
        return error(ErrorCode::SolutionIoFailure, source.solution(), "Could not prepare or commit the solution save.");
    }
}

core::Result<SolutionSaveResult> save_solution(const SolutionDocument& source, const Solution& edited,
    std::span<const ManagedResourceWrite> writes)
{
    return detail::save_solution(source, edited, writes, {});
}

core::Result<SolutionCleanupReport> cleanup_solution_resources(const SolutionDocument& previous)
{
    try {
        if (!previous.can_save()) {
            return error(ErrorCode::UnsupportedSolutionContent, previous.solution(), "The previous manifest contains unknown fields.");
        }
        auto locked = detail::PackageWriteLock::acquire(previous.directory());
        if (auto* failure = std::get_if<Diagnostic>(&locked)) { return std::move(*failure); }
        auto bytes = detail::read_manifest(previous.directory() / solution_manifest_name);
        if (auto* failure = std::get_if<Diagnostic>(&bytes)) { return std::move(*failure); }
        const auto json = detail::parse_manifest(std::get<std::string>(bytes));
        std::vector<Diagnostic> unknown;
        const auto current = detail::decode_solution(detail::Reader{json, unknown});
        if (!unknown.empty()) {
            return error(ErrorCode::UnsupportedSolutionContent, current, "The current manifest contains unknown fields.");
        }
        if (current.id != previous.solution().id || current.revision <= previous.solution().revision) {
            return error(ErrorCode::SolutionSaveConflict, previous.solution(), "Cleanup requires a newer committed revision of the same solution.");
        }
        auto checked = validate_global_invariants(current);
        if (auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        return detail::cleanup_managed_versions(previous.solution(), current, previous.directory(), previous.resource_roots());
    } catch (detail::CodecError& failure) {
        return std::move(failure.diagnostic);
    } catch (const detail::Json::exception&) {
        return error(ErrorCode::InvalidSolutionManifest, previous.solution(), "The current manifest cannot be decoded for cleanup.");
    } catch (...) {
        return error(ErrorCode::SolutionCleanupFailure, previous.solution(), "Could not recheck or clean obsolete managed resources.");
    }
}

} // namespace visionpro::infrastructure
