#pragma once

#include <visionpro/core/resource_reference.hpp>
#include <visionpro/core/value.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace visionpro::core {

struct ResourceRequirement {
    SchemaId slot;
    ResourceKind kind;
    bool required{true};
};

enum class PathBase { SolutionDirectory, NamedRoot, Absolute };
struct ResourcePath {
    PathBase base;
    std::string path; // UTF-8; no process working-directory interpretation.
    std::optional<SchemaId> named_root;
};
enum class ResourceReadPolicy { SnapshotLocked, PerInvocation };
struct ManagedResource {
    ResourcePath path;
    std::uint64_t revision;
    std::string sha256;
};
struct ExternalFileResource {
    ResourcePath path;
    ResourceReadPolicy read_policy;
    std::optional<std::string> expected_sha256;
    std::optional<std::uint64_t> revision;
};
struct DirectorySelection { ResourcePath directory; std::string filename_pattern; };
enum class SequenceSort { NameAscending, NameDescending, ExplicitOrder };
enum class SequenceAdvance { PerInvocation, ExplicitCommand };
enum class SequenceRepeat { Stop, Loop };
enum class SequenceMissing { Fail, Skip };
enum class SequenceTrigger { Manual, External };
struct ImageSequenceResource {
    std::variant<DirectorySelection, std::vector<ResourcePath>> selection;
    SequenceSort sort;
    SequenceAdvance advance;
    SequenceRepeat repeat;
    SequenceMissing missing;
    SequenceTrigger trigger;
    std::size_t initial_index;
    // The current cursor belongs to runtime state, not this definition.
};
struct DeviceResource { SchemaId adapter; ConfigValue configuration; };
enum class OutputNaming { FixedName, SequenceNumber, CorrelationKey };
enum class OutputOverwrite { Fail, Replace, UniqueSuffix };
struct OutputPathResource {
    ResourcePath directory;
    OutputNaming naming;
    std::string stem;
    std::string extension;
    OutputOverwrite overwrite;
    std::uint64_t capacity_bytes;
};
using ResourceLocation = std::variant<ManagedResource, ExternalFileResource,
    ImageSequenceResource, DeviceResource, OutputPathResource>;
struct ResourceDefinition {
    ResourceId id;
    std::string name;
    ResourceKind kind;
    ResourceLocation location;
};

[[nodiscard]] Result<std::monostate> validate_resource_path(const ResourcePath& path, bool confined = false);
[[nodiscard]] Result<std::monostate> validate_resource_requirement(const ResourceRequirement& requirement);
[[nodiscard]] Result<std::monostate> validate_resource_definition(const ResourceDefinition& definition);
[[nodiscard]] Result<std::monostate> validate_resource_catalog(std::span<const ResourceDefinition> definitions);
[[nodiscard]] Result<std::monostate> validate_resource_reference(
    const ResourceReference& reference, std::span<const ResourceDefinition> definitions);

} // namespace visionpro::core
