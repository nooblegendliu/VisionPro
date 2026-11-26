#include <visionpro/infrastructure/solution_file.hpp>

#include <visionpro/core/id_factory.hpp>
#include <visionpro/workflow/copy.hpp>

#include "managed_cleanup.hpp"
#include "package_assets.hpp"
#include "resource_files.hpp"
#include "solution_image_source.hpp"
#include "solution_images.hpp"

#include <algorithm>
#include <set>
#include <type_traits>
#include <utility>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace workflow;
using Check = Result<std::monostate>;
namespace fs = std::filesystem;

Diagnostic error(ErrorCode code, SolutionId solution, std::string message)
{
    DiagnosticLocation location;
    location.solution = solution;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

std::string utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {value.begin(), value.end()};
}

bool is_within(const fs::path& path, const fs::path& directory)
{
    return std::mismatch(directory.begin(), directory.end(), path.begin(), path.end()).first == directory.end();
}

ResourcePath package_path(const fs::path& path)
{ return {PathBase::SolutionDirectory, utf8(path), {}}; }

struct AssetFile {
    fs::path source;
    fs::path target;
    std::optional<std::string> digest;
    ResourceId resource;
};

using detail::PackageAssets;

std::optional<std::string> normalized_digest(std::optional<std::string_view> digest)
{
    if (!digest) { return std::nullopt; }
    auto value = std::string(*digest);
    for (auto& ch : value) {
        if (ch >= 'A' && ch <= 'F') { ch = static_cast<char>(ch + ('a' - 'A')); }
    }
    return value;
}

struct SequenceFilter { ResourceId resource; std::string_view pattern; };

class TransferPlan {
public:
    TransferPlan(const SolutionId& solution, detail::ResourcePaths paths, bool has_source, fs::path assets)
        : solution_(solution), paths_(std::move(paths)), has_source_(has_source), assets_(std::move(assets)) {}

    Check add(ResourceDefinition& resource)
    {
        auto result = std::visit([&](auto& location) -> Check {
            using Location = std::decay_t<decltype(location)>;
            if constexpr (std::is_same_v<Location, ManagedResource>) {
                if (!has_source_) { return detail::verify_managed_resource(location, paths_.directory); }
                return add_file(location.path, resource.id, true, location.sha256);
            } else if constexpr (std::is_same_v<Location, ExternalFileResource>) {
                return add_file(location.path, resource.id, false, location.expected_sha256);
            } else if constexpr (std::is_same_v<Location, ImageSequenceResource>) {
                if (auto* selection = std::get_if<DirectorySelection>(&location.selection)) {
                    return add_directory(selection->directory, SequenceFilter{resource.id, selection->filename_pattern});
                }
                auto& paths = std::get<std::vector<ResourcePath>>(location.selection);
                for (std::size_t index = 0; index < paths.size(); ++index) {
                    const auto digest = location.embedded_sha256.empty() ? std::optional<std::string_view>{} : location.embedded_sha256[index];
                    auto copied = add_file(paths[index], resource.id, digest.has_value(), digest);
                    if (std::holds_alternative<Diagnostic>(copied)) { return copied; }
                }
            } else if constexpr (std::is_same_v<Location, OutputPathResource>) {
                return add_directory(location.directory);
            }
            return std::monostate{};
        }, resource.location);
        if (auto* failure = std::get_if<Diagnostic>(&result)) {
            failure->location.solution = solution_;
            failure->location.resource = resource.id;
        }
        return result;
    }

    Check add_archive(ImageArchivePolicy& policy) { return add_directory(policy.directory); }

    Check stage(const fs::path& destination, PackageAssets& transaction) const
    {
        auto prepared = transaction.create_directory(destination, false);
        if (std::holds_alternative<Diagnostic>(prepared) || directories_.empty()) { return prepared; }
        auto created = transaction.create_directory(destination / assets_, true);
        if (std::holds_alternative<Diagnostic>(created)) { return created; }
        for (const auto& relative : directories_) {
            created = transaction.create_directory(destination / relative, true);
            if (std::holds_alternative<Diagnostic>(created)) { return created; }
        }
        for (const auto& file : files_) {
            auto copied = transaction.copy(file.source, destination / file.target, file.digest);
            if (auto* failure = std::get_if<Diagnostic>(&copied)) {
                failure->location.solution = solution_;
                failure->location.resource = file.resource;
                return copied;
            }
        }
        return std::monostate{};
    }

    std::set<fs::path> external;
    std::set<SchemaId> unresolved_roots;

private:
    // An empty result leaves the declaration external or already relative to
    // the first package base. Only paths owned by an existing source are copied.
    Result<std::optional<fs::path>> local_path(ResourcePath& path, bool managed)
    {
        if (path.named_root && !paths_.roots.contains(*path.named_root)) {
            unresolved_roots.insert(*path.named_root);
            return std::optional<fs::path>{};
        }
        auto resolved = detail::absolute_path(path, paths_, managed);
        if (auto* failure = std::get_if<Diagnostic>(&resolved)) { return std::move(*failure); }
        const auto& absolute = std::get<fs::path>(resolved);
        std::error_code io;
        auto physical = detail::windows_display_path(fs::weakly_canonical(detail::windows_access_path(absolute), io));
        if (io) { return error(ErrorCode::InvalidPath, solution_, "Could not resolve resource path: " + path.path); }
        if (is_within(physical, paths_.directory)) {
            if (has_source_) { return std::optional<fs::path>{std::move(physical)}; }
            if (path.base == PathBase::SolutionDirectory) { return std::optional<fs::path>{}; }
        } else if (managed) {
            return error(ErrorCode::InvalidPath, solution_, "Managed resource resolves outside its package: " + path.path);
        }
        external.insert(std::move(physical));
        if (path.base == PathBase::SolutionDirectory) { path = {PathBase::Absolute, utf8(absolute), {}}; }
        return std::optional<fs::path>{};
    }

    fs::path allocate_directory()
    {
        directories_.push_back(assets_ / std::to_string(directories_.size() + 1));
        return directories_.back();
    }

    Check add_file(ResourcePath& path, ResourceId resource, bool managed, std::optional<std::string_view> digest)
    {
        auto ownership = local_path(path, managed);
        if (auto* failure = std::get_if<Diagnostic>(&ownership)) { return std::move(*failure); }
        if (!std::get<std::optional<fs::path>>(ownership)) { return std::monostate{}; }
        auto located = detail::existing_path(path, paths_, managed);
        if (auto* failure = std::get_if<Diagnostic>(&located)) { return std::move(*failure); }
        auto source = std::get<fs::path>(std::move(located));
        if (!is_within(source, paths_.directory)) {
            return error(ErrorCode::InvalidPath, solution_, "The source resource changed while preparing the package.");
        }
        auto expected = normalized_digest(digest);
        const auto [shared, inserted] = standalone_.try_emplace(source, files_.size());
        if (inserted) {
            auto target = allocate_directory() / source.filename();
            files_.push_back({std::move(source), std::move(target), std::move(expected), resource});
        } else if (expected) {
            auto& copied = files_[shared->second];
            if (copied.digest && copied.digest != expected) {
                return error(ErrorCode::ResourceVersionMismatch, solution_, "Resource aliases declare different content digests.");
            }
            copied.digest = std::move(expected);
        }
        path = package_path(files_[shared->second].target);
        return std::monostate{};
    }

    Check add_directory(ResourcePath& path, std::optional<SequenceFilter> sequence = {})
    {
        auto ownership = local_path(path, false);
        if (auto* failure = std::get_if<Diagnostic>(&ownership)) { return std::move(*failure); }
        const auto& directory = std::get<std::optional<fs::path>>(ownership);
        if (!directory) { return std::monostate{}; }
        auto target = allocate_directory();
        if (sequence) {
            auto located = detail::existing_path(path, paths_, false, true);
            if (auto* failure = std::get_if<Diagnostic>(&located)) { return std::move(*failure); }
            const auto& existing = std::get<fs::path>(located);
            if (!is_within(existing, paths_.directory)) {
                return error(ErrorCode::InvalidPath, solution_, "The source sequence moved outside its package.");
            }
            auto selected = detail::sequence_files(existing, sequence->pattern);
            if (auto* failure = std::get_if<Diagnostic>(&selected)) { return std::move(*failure); }
            for (const auto& member : std::get<std::vector<fs::path>>(selected)) {
                auto file = detail::windows_display_path(fs::canonical(detail::windows_access_path(member)));
                if (!fs::is_regular_file(detail::windows_access_path(file)) || !is_within(file, paths_.directory)) {
                    return error(ErrorCode::InvalidPath, solution_, "A package sequence contains a member outside the package: " + utf8(member));
                }
                files_.push_back({std::move(file), target / member.filename(), {}, sequence->resource});
            }
        } else if (fs::exists(detail::windows_access_path(*directory)) && !fs::is_directory(detail::windows_access_path(*directory))) {
            return error(ErrorCode::InvalidPath, solution_, "The package output location is not a directory: " + path.path);
        }
        path = package_path(target);
        return std::monostate{};
    }

    SolutionId solution_;
    detail::ResourcePaths paths_;
    bool has_source_;
    fs::path assets_;
    std::vector<fs::path> directories_;
    std::vector<AssetFile> files_;
    std::map<fs::path, std::size_t> standalone_;
};

Result<fs::path> asset_directory()
{
    auto issued = IdFactory::generate<MessageId>();
    if (auto* failure = std::get_if<Diagnostic>(&issued)) { return std::move(*failure); }
    return fs::path("resources-" + std::get<MessageId>(issued).to_string());
}

} // namespace

core::Result<SolutionTransferResult> transfer_solution(const fs::path& destination, const Solution& draft,
    SolutionTransfer operation, const SolutionDocument* source, std::span<const NamedResourceRoot> roots)
{
    try {
        if (source && !source->can_save()) {
            return error(ErrorCode::UnsupportedSolutionContent, draft.id, "The loaded source contains unknown manifest fields.");
        }
        if (source && source->solution().id != draft.id) {
            return error(ErrorCode::ResultIdentityMismatch, draft.id, "The draft and source document must share a solution identity.");
        }
        if (!supports_solution_format(draft.format_version)) {
            return error(ErrorCode::UnsupportedSolutionFormat, draft.id, "The edited manifest format version is unsupported.");
        }
        if (auto checked = validate_model_shape(draft); auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        if (!destination.is_absolute() || destination.native().find(L'\0') != std::wstring::npos) {
            return error(ErrorCode::InvalidPath, draft.id, "The destination must be an explicit absolute package directory.");
        }
        const auto target = detail::windows_display_path(fs::weakly_canonical(detail::windows_access_path(destination)));
        std::error_code ignored;
        if (fs::exists(fs::symlink_status(detail::windows_access_path(target / solution_manifest_name), ignored))) {
            return error(ErrorCode::SolutionAlreadyExists, draft.id, "The destination already contains a solution manifest.");
        }

        auto model = draft;
        if (operation == SolutionTransfer::IndependentCopy) {
            auto copied = clone_solution(draft, ExternalReferencePolicy::Reject);
            if (auto* failure = std::get_if<Diagnostic>(&copied)) { return std::move(*failure); }
            model = std::move(std::get<SolutionCopy>(copied).solution);
            model.revision = 1;
        } else if (operation != SolutionTransfer::SaveAs) {
            return error(ErrorCode::InvalidModel, draft.id, "The solution transfer operation is invalid.");
        }
        detail::migrate_image_sources(model);
        auto assets = asset_directory();
        if (auto* failure = std::get_if<Diagnostic>(&assets)) { return std::move(*failure); }
        auto resolved = source ? detail::resource_paths(source->directory(), source->resource_roots())
                               : detail::resource_paths(target, roots, false);
        if (auto* failure = std::get_if<Diagnostic>(&resolved)) { return std::move(*failure); }
        const auto& paths = std::get<detail::ResourcePaths>(resolved);
        const auto embed = detail::embedded_image_resources(model);
        PackageAssets transaction(model.id);
        TransferPlan plan(model.id, paths, source != nullptr,
            std::get<fs::path>(std::move(assets)));
        for (auto& resource : model.resources) {
            auto added = embed.contains(resource.id)
                ? detail::stage_image_resource(resource, paths, target, transaction, true)
                : plan.add(resource);
            if (auto* failure = std::get_if<Diagnostic>(&added)) { return std::move(*failure); }
        }

        if (model.image_archive) {
            auto added = plan.add_archive(*model.image_archive);
            if (auto* failure = std::get_if<Diagnostic>(&added)) { return std::move(*failure); }
        }
        SolutionTransferResult result{std::move(model), {plan.external.begin(), plan.external.end()},
            {plan.unresolved_roots.begin(), plan.unresolved_roots.end()}};
        auto staged = plan.stage(target, transaction);
        if (auto* failure = std::get_if<Diagnostic>(&staged)) { return std::move(*failure); }
        auto published = write_new_solution(target, result.solution);
        if (auto* failure = std::get_if<Diagnostic>(&published)) { return std::move(*failure); }
        transaction.confirm_publication();
        return Result<SolutionTransferResult>{std::move(result)};
    } catch (const fs::filesystem_error& failure) {
        return error(ErrorCode::SolutionIoFailure, draft.id, "Could not transfer the solution package. Filesystem error "
            + std::to_string(failure.code().value()) + ".");
    } catch (...) {
        return error(ErrorCode::SolutionIoFailure, draft.id, "Could not transfer the solution package.");
    }
}

core::Result<ResourcePath> copy_external_resource_into_solution(const SolutionDocument& source,
    const ExternalFileResource& resource)
{
    const auto solution = source.solution().id;
    try {
        if (!source.can_save()) {
            return error(ErrorCode::UnsupportedSolutionContent, solution, "The loaded source contains unknown manifest fields.");
        }
        auto paths = detail::resource_paths(source.directory(), source.resource_roots());
        if (auto* failure = std::get_if<Diagnostic>(&paths)) { return std::move(*failure); }
        auto file = detail::existing_path(resource.path, std::get<detail::ResourcePaths>(paths));
        if (auto* failure = std::get_if<Diagnostic>(&file)) { return std::move(*failure); }
        const auto& original = std::get<fs::path>(file);
        auto directory = asset_directory();
        if (auto* failure = std::get_if<Diagnostic>(&directory)) { return std::move(*failure); }
        const auto& relative = std::get<fs::path>(directory);
        auto result = package_path(relative / original.filename());
        PackageAssets assets(solution);
        auto prepared = assets.create_directory(source.directory() / relative, true);
        if (auto* failure = std::get_if<Diagnostic>(&prepared)) { return std::move(*failure); }
        auto copied = assets.copy(original, source.directory() / relative / original.filename(), resource.expected_sha256);
        if (auto* failure = std::get_if<Diagnostic>(&copied)) { return std::move(*failure); }
        assets.confirm_publication();
        return Result<ResourcePath>{std::move(result)};
    } catch (...) {
        return error(ErrorCode::SolutionIoFailure, solution, "Could not copy the external resource into the package.");
    }
}

} // namespace visionpro::infrastructure
