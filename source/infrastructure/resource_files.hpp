#pragma once

#include <visionpro/infrastructure/file_resources.hpp>

#include <map>
#include <string_view>

namespace visionpro::infrastructure::detail {

struct ResourcePaths {
    std::filesystem::path directory;
    std::map<core::SchemaId, std::filesystem::path> roots;
};

[[nodiscard]] core::Result<ResourcePaths> resource_paths(const std::filesystem::path& directory,
    std::span<const NamedResourceRoot> roots, bool require_existing_directory = true);
[[nodiscard]] std::filesystem::path utf8_path(std::string_view spelling);
[[nodiscard]] core::Result<std::filesystem::path> absolute_path(
    const core::ResourcePath& declaration, const ResourcePaths& paths, bool managed);
[[nodiscard]] core::Result<std::filesystem::path> existing_path(const core::ResourcePath& declaration,
    const ResourcePaths& paths, bool managed = false, bool directory = false);
[[nodiscard]] core::Result<std::vector<std::filesystem::path>> sequence_files(
    const std::filesystem::path& directory, std::string_view pattern);
[[nodiscard]] core::Result<std::monostate> copy_resource_file(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::optional<std::string_view> digest);

} // namespace visionpro::infrastructure::detail
