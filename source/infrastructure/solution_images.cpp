#include "solution_images.hpp"
#include "json_codec.hpp"

namespace visionpro::infrastructure::detail {
namespace {
using namespace core;
using namespace workflow;
using Check = Result<std::monostate>;

template<class T> T checked(Result<T> result)
{
    if (auto* failure = std::get_if<Diagnostic>(&result)) { throw CodecError{std::move(*failure)}; }
    return std::get<T>(std::move(result));
}

const ConfigValue* constant(const Node& node, std::string_view field)
{
    for (const auto& binding : node.bindings) {
        const auto* parameter = std::get_if<ParameterId>(&binding.target);
        if (parameter && parameter->text() == field && binding.sources.size() == 1) {
            return std::get_if<ConfigValue>(&binding.sources.front());
        }
    }
    return nullptr;
}

ResourcePath relative_path(const fs::path& file)
{
    const auto utf8 = file.generic_u8string();
    return {PathBase::SolutionDirectory, {utf8.begin(), utf8.end()}, {}};
}
} // namespace

std::set<core::ResourceId> embedded_image_resources(const workflow::Solution& model)
{
    std::set<ResourceId> resources;
    for (const auto& flow : model.workflows) {
        for (const auto& node : flow.nodes) {
            const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
            if (!tool || tool->type.text() != "visionpro.image.source" || tool->config_version != 1) { continue; }
            const auto* embed = constant(node, "embed_images");
            if (!embed || !embed->as_bool() || !*embed->as_bool()) { continue; }
            const auto* type = constant(node, "source_type");
            if (type && (!type->as_enum() || type->as_enum()->text() != "Local")) { continue; }
            for (const auto& binding : node.resources) {
                if (binding.slot.text() == "sequence" || binding.slot.text() == "source") { resources.insert(binding.resource.id); }
            }
        }
    }
    return resources;
}

Check stage_image_resource(ResourceDefinition& resource, const ResourcePaths& from, const fs::path& destination,
    PackageAssets& transaction, bool relocate)
{
    try {
        checked(validate_resource_definition(resource));
        const auto failure = [&](ErrorCode code, std::string message) -> Check {
            DiagnosticLocation where; where.resource = resource.id;
            return Diagnostic{code, Severity::Error, std::move(where), std::move(message)};
        };
        auto* sequence = std::get_if<ImageSequenceResource>(&resource.location);
        auto* external = std::get_if<ExternalFileResource>(&resource.location);
        const auto* managed = std::get_if<ManagedResource>(&resource.location);
        const bool immutable = managed || (sequence && !sequence->embedded_sha256.empty());
        std::vector<fs::path> files;
        if (sequence) {
            if (const auto* selection = std::get_if<DirectorySelection>(&sequence->selection)) {
                const auto directory = checked(existing_path(selection->directory, from, false, true));
                files = checked(sequence_files(directory, selection->filename_pattern));
                std::ranges::sort(files);
                if (sequence->sort == SequenceSort::NameDescending) { std::ranges::reverse(files); }
            } else {
                const auto& paths = std::get<std::vector<ResourcePath>>(sequence->selection);
                if (paths.size() > 120) { return failure(ErrorCode::InvalidResource, "At most 120 images can be embedded per image source."); }
                for (const auto& path : paths) { files.push_back(checked(existing_path(path, from, immutable))); }
            }
            if (files.empty() || files.size() > 120 || sequence->initial_index >= files.size()) {
                return failure(ErrorCode::InvalidResource, "An embedded image sequence needs 1 to 120 files and a valid initial selection.");
            }
        } else if ((external || managed) && resource.kind == ResourceKind::Image) {
            files.push_back(checked(existing_path(managed ? managed->path : external->path, from, immutable)));
        } else { return failure(ErrorCode::ResourceKindMismatch, "Only image files and image sequences can be embedded by an image source."); }

        const bool copy = relocate || !immutable;
        const auto directory = fs::path("images-" + checked(IdFactory::generate<MessageId>()).to_string());
        if (copy) {
            checked(transaction.create_directory(destination, false));
            checked(transaction.create_directory(destination / directory, true));
        }
        std::vector<ResourcePath> paths;
        std::vector<std::string> digests;
        for (std::size_t index = 0; index < files.size(); ++index) {
            const auto bytes = checked(read_file_bytes(files[index]));
            const auto digest = checked(sha256(*bytes));
            const auto* expected = managed ? &managed->sha256 : (sequence && immutable ? &sequence->embedded_sha256[index]
                : (external && external->expected_sha256 ? &*external->expected_sha256 : nullptr));
            if (expected && !std::ranges::equal(digest, *expected, {}, std::identity{}, [](char c) {
                return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
            })) { return failure(ErrorCode::ResourceVersionMismatch, "The image differs from its saved SHA-256 digest."); }
            if (copy) {
                const auto parent = directory / std::to_string(index + 1);
                checked(transaction.create_directory(destination / parent, true));
                const auto relative = parent / files[index].filename();
                checked(transaction.write(destination / relative, *bytes));
                paths.push_back(relative_path(relative));
                digests.push_back(digest);
            }
        }
        if (copy && sequence) {
            sequence->selection = std::move(paths);
            sequence->sort = SequenceSort::ExplicitOrder;
            sequence->embedded_sha256 = std::move(digests);
        } else if (copy) {
            const auto revision = managed ? managed->revision : external->revision.value_or(1);
            resource.location = ManagedResource{std::move(paths.front()), revision, std::move(digests.front())};
        }
        return std::monostate{};
    } catch (CodecError& error) {
        error.diagnostic.location.resource = resource.id;
        return std::move(error.diagnostic);
    }
}
} // namespace visionpro::infrastructure::detail
