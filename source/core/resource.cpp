#include <visionpro/core/resource.hpp>
#include <visionpro/core/text.hpp>

#include <algorithm>
#include <set>
#include <string_view>
#include <type_traits>

namespace visionpro::core {
namespace {

using Check = Result<std::monostate>;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
template <class E> bool known(E value, E last)
{
    using Number = std::underlying_type_t<E>;
    return static_cast<Number>(value) >= 0 && static_cast<Number>(value) <= static_cast<Number>(last);
}
bool text_ok(std::string_view text)
{ return !text.empty() && text.find('\0') == std::string_view::npos && is_valid_utf8(text); }
bool digest_ok(std::string_view text)
{
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool leaf_ok(std::string_view text)
{
    return text_ok(text) && text != "." && text != ".."
        && text.find_first_of("/\\:*?\"<>|") == std::string_view::npos
        && std::ranges::none_of(text, [](unsigned char c) { return c < 0x20; })
        && text.back() != '.' && text.back() != ' ';
}
Check file_path(const ResourcePath& path, bool confined = false)
{
    if (auto checked = validate_resource_path(path, confined); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    const auto position = path.path.find_last_of("/\\");
    const auto leaf = std::string_view(path.path).substr(position == std::string::npos ? 0 : position + 1);
    if (!leaf_ok(leaf)) { return error(ErrorCode::InvalidPath, "A file resource requires a nonempty filename, not a directory."); }
    return std::monostate{};
}
bool file_kind(ResourceKind kind)
{
    return kind == ResourceKind::Image || kind == ResourceKind::Mask || kind == ResourceKind::Template
        || kind == ResourceKind::Calibration || kind == ResourceKind::Binary;
}

Check location_check(ResourceKind kind, const ManagedResource& source)
{
    if (!file_kind(kind) || source.path.base != PathBase::SolutionDirectory || source.revision == 0 || !digest_ok(source.sha256)) {
        return error(ErrorCode::InvalidResource, "Managed file resources require a file kind, solution-relative path, positive revision and SHA-256 digest.");
    }
    return file_path(source.path, true);
}
Check location_check(ResourceKind kind, const ExternalFileResource& source)
{
    if (!file_kind(kind) || !known(source.read_policy, ResourceReadPolicy::PerInvocation)
        || (source.expected_sha256 && !digest_ok(*source.expected_sha256)) || (source.revision && *source.revision == 0)) {
        return error(ErrorCode::InvalidResource, "Invalid external resource kind, read policy, checksum or revision.");
    }
    if ((kind == ResourceKind::Template || kind == ResourceKind::Calibration)
        && source.read_policy != ResourceReadPolicy::SnapshotLocked) {
        return error(ErrorCode::InvalidResource, "Templates and calibration resources must be locked to the execution snapshot.");
    }
    if (source.read_policy == ResourceReadPolicy::SnapshotLocked && !source.expected_sha256) {
        return error(ErrorCode::InvalidResource, "Snapshot-locked external files require an expected content digest.");
    }
    if (source.read_policy == ResourceReadPolicy::PerInvocation && source.revision) {
        return error(ErrorCode::InvalidResource, "Per-invocation dynamic input cannot advertise a pinned resource revision.");
    }
    return file_path(source.path);
}
Check location_check(ResourceKind kind, const ImageSequenceResource& source)
{
    if (kind != ResourceKind::DirectorySequence || !known(source.sort, SequenceSort::ExplicitOrder)
        || !known(source.advance, SequenceAdvance::ExplicitCommand) || !known(source.repeat, SequenceRepeat::Loop)
        || !known(source.missing, SequenceMissing::Skip) || !known(source.trigger, SequenceTrigger::External)) {
        return error(ErrorCode::InvalidResource, "Image sequences require explicit, recognized ordering, advance, repeat, missing and trigger policies.");
    }
    if (const auto* directory = std::get_if<DirectorySelection>(&source.selection)) {
        if (source.sort == SequenceSort::ExplicitOrder || !text_ok(directory->filename_pattern)
            || directory->filename_pattern.find_first_of("/\\:\"<>|") != std::string::npos
            || std::ranges::any_of(directory->filename_pattern, [](unsigned char c) { return c < 0x20; })) {
            return error(ErrorCode::InvalidResource, "Directory sequences require named sorting and a filename-only filter.");
        }
        return validate_resource_path(directory->directory);
    }
    const auto& files = std::get<std::vector<ResourcePath>>(source.selection);
    if (files.empty() || source.initial_index >= files.size() || source.sort != SequenceSort::ExplicitOrder) {
        return error(ErrorCode::InvalidResource, "Explicit sequences require nonempty ordered files and an in-range initial cursor.");
    }
    for (const auto& file : files) {
        if (auto checked = file_path(file); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    return std::monostate{};
}
Check location_check(ResourceKind kind, const DeviceResource& source)
{
    if (kind != ResourceKind::Device || source.configuration.type().kind() != TypeKind::Record) {
        return error(ErrorCode::InvalidResource, "Devices require a declared adapter and a typed Record configuration, never a device handle.");
    }
    return std::monostate{};
}
Check location_check(ResourceKind kind, const OutputPathResource& source)
{
    if (kind != ResourceKind::OutputPath || !known(source.naming, OutputNaming::CorrelationKey)
        || !known(source.overwrite, OutputOverwrite::UniqueSuffix) || source.capacity_bytes == 0
        || !leaf_ok(source.stem) || !leaf_ok(source.extension) || source.extension.front() == '.') {
        return error(ErrorCode::InvalidResource, "Outputs require valid naming/overwrite policies, filename parts and positive capacity.");
    }
    return validate_resource_path(source.directory);
}

} // namespace

Result<std::monostate> validate_resource_path(const ResourcePath& path, bool confined)
{
    if (!known(path.base, PathBase::Absolute) || !text_ok(path.path)
        || ((path.base == PathBase::NamedRoot) != path.named_root.has_value())) {
        return error(ErrorCode::InvalidPath, "Resource paths need valid UTF-8 and an explicit, consistent path base.");
    }
    std::string normalized = path.path;
    std::ranges::replace(normalized, '\\', '/');
    const bool drive = normalized.size() >= 3
        && ((normalized[0] >= 'a' && normalized[0] <= 'z') || (normalized[0] >= 'A' && normalized[0] <= 'Z'))
        && normalized[1] == ':' && normalized[2] == '/';
    const bool unc = normalized.starts_with("//");
    // A single leading separator is relative to the current drive on Windows.
    // Only a drive-rooted path or a complete UNC path supplies an explicit root.
    if (normalized.front() == '/' && !unc) {
        return error(ErrorCode::InvalidPath, "Root-relative paths require an explicit drive or UNC share.");
    }
    const bool absolute = drive || unc;
    if ((path.base == PathBase::Absolute) != absolute || (confined && path.base != PathBase::SolutionDirectory)) {
        return error(ErrorCode::InvalidPath, "Absolute and relative paths must match their declared base; managed paths stay under the solution.");
    }
    for (std::size_t index = 0; index < normalized.size(); ++index) {
        const auto c = static_cast<unsigned char>(normalized[index]);
        if (c < 0x20 || normalized[index] == '*' || normalized[index] == '?' || normalized[index] == '"'
            || normalized[index] == '<' || normalized[index] == '>' || normalized[index] == '|'
            || (normalized[index] == ':' && !(drive && index == 1))) {
            return error(ErrorCode::InvalidPath, "Paths cannot contain control characters, drive-relative syntax or filename wildcard syntax.");
        }
    }
    std::size_t start = drive ? 3 : (unc ? 2 : 0);
    std::size_t component_count = 0;
    while (start < normalized.size()) {
        const auto end = normalized.find('/', start);
        const auto component = std::string_view(normalized).substr(start,
            end == std::string::npos ? normalized.size() - start : end - start);
        if (component.empty() || ((confined || (unc && component_count < 2)) && (component == "." || component == ".."))
            || (component != "." && component != ".." && (component.back() == '.' || component.back() == ' '))) {
            return error(ErrorCode::InvalidPath, "Managed paths cannot traverse parents; path components must be unambiguous.");
        }
        ++component_count;
        if (end == std::string::npos) { break; }
        start = end + 1;
    }
    if (unc && component_count < 2) {
        return error(ErrorCode::InvalidPath, "UNC paths require both server and share names.");
    }
    return std::monostate{};
}

Result<std::monostate> validate_resource_reference(const ResourceReference& reference)
{
    if (!known(reference.kind, ResourceKind::Binary) || (reference.revision && *reference.revision == 0)) {
        auto result = error(ErrorCode::InvalidResource, "Resource references require a known kind and a positive revision when pinned.");
        result.location.resource = reference.id;
        return result;
    }
    return std::monostate{};
}

Result<std::monostate> validate_resource_requirement(const ResourceRequirement& requirement)
{
    if (!known(requirement.kind, ResourceKind::Binary)) {
        return error(ErrorCode::InvalidResource, "Tool resource slots must declare a known resource kind.");
    }
    return std::monostate{};
}

Result<std::monostate> validate_resource_definition(const ResourceDefinition& definition)
{
    if (!known(definition.kind, ResourceKind::Binary) || !text_ok(definition.name)) {
        auto result = error(ErrorCode::InvalidResource, "Resource definitions require a known kind and a nonempty UTF-8 display name.");
        result.location.resource = definition.id;
        return result;
    }
    auto result = std::visit([&](const auto& source) { return location_check(definition.kind, source); }, definition.location);
    if (auto* failure = std::get_if<Diagnostic>(&result)) { failure->location.resource = definition.id; }
    return result;
}

Result<std::monostate> validate_resource_catalog(std::span<const ResourceDefinition> definitions)
{
    std::set<ResourceId> identities;
    for (const auto& definition : definitions) {
        if (!identities.insert(definition.id).second) {
            auto result = error(ErrorCode::InvalidResource, "Resource catalog contains a duplicate identity.");
            result.location.resource = definition.id;
            return result;
        }
        if (auto checked = validate_resource_definition(definition); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    return std::monostate{};
}

Result<std::monostate> validate_resource_reference(const ResourceReference& reference,
    std::span<const ResourceDefinition> definitions)
{
    if (auto checked = validate_resource_reference(reference); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (auto checked = validate_resource_catalog(definitions); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    const auto fail = [&](ErrorCode code, std::string message) -> Check {
        auto result = error(code, std::move(message));
        result.location.resource = reference.id;
        return result;
    };
    const auto definition = std::ranges::find(definitions, reference.id, &ResourceDefinition::id);
    if (definition == definitions.end()) { return fail(ErrorCode::ResourceNotFound, "Referenced resource is absent from the catalog."); }
    if (definition->kind != reference.kind) { return fail(ErrorCode::ResourceKindMismatch, "Referenced resource kind differs from its definition."); }
    if (reference.revision) {
        std::optional<std::uint64_t> actual;
        if (const auto* managed = std::get_if<ManagedResource>(&definition->location)) { actual = managed->revision; }
        if (const auto* external = std::get_if<ExternalFileResource>(&definition->location)) { actual = external->revision; }
        if (actual != reference.revision) { return fail(ErrorCode::ResourceVersionMismatch, "Pinned revision is unavailable in the resource definition."); }
    }
    return std::monostate{};
}

} // namespace visionpro::core
