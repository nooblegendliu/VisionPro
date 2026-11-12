#include "resource_codec.hpp"
#include "value_codec.hpp"

namespace visionpro::infrastructure::detail {
using namespace core;

template<> struct EnumNames<PathBase> {
    static constexpr auto values = std::to_array<std::string_view>({"solution_directory", "named_root", "absolute"});
};
template<> struct EnumNames<ResourceReadPolicy> {
    static constexpr auto values = std::to_array<std::string_view>({"snapshot_locked", "per_invocation"});
};
template<> struct EnumNames<SequenceSort> {
    static constexpr auto values = std::to_array<std::string_view>({"name_ascending", "name_descending", "explicit_order"});
};
template<> struct EnumNames<SequenceAdvance> {
    static constexpr auto values = std::to_array<std::string_view>({"per_invocation", "explicit_command"});
};
template<> struct EnumNames<SequenceRepeat> {
    static constexpr auto values = std::to_array<std::string_view>({"stop", "loop"});
};
template<> struct EnumNames<SequenceMissing> {
    static constexpr auto values = std::to_array<std::string_view>({"fail", "skip"});
};
template<> struct EnumNames<SequenceTrigger> {
    static constexpr auto values = std::to_array<std::string_view>({"manual", "external"});
};
template<> struct EnumNames<OutputNaming> {
    static constexpr auto values = std::to_array<std::string_view>({"fixed_name", "sequence_number", "correlation_key"});
};
template<> struct EnumNames<OutputOverwrite> {
    static constexpr auto values = std::to_array<std::string_view>({"fail", "replace", "unique_suffix"});
};

namespace {

Json encode_path(const ResourcePath& path)
{
    return {{"base", write_enum(path.base)}, {"path", path.path}, {"named_root", write_optional(path.named_root, write_id<SchemaId>)}};
}

ResourcePath decode_path(const Reader& reader)
{
    reader.object({"base", "path", "named_root"});
    return {read_enum<PathBase>(reader.field("base")), std::string(reader.field("path").text()),
        read_optional<SchemaId>(reader.field("named_root"), read_id<SchemaId>)};
}

Json encode_revision(const std::optional<std::uint64_t>& revision)
{
    return write_optional(revision, [](std::uint64_t value) { return std::to_string(value); });
}

std::optional<std::uint64_t> decode_revision(const Reader& reader)
{
    return read_optional<std::uint64_t>(reader, &Reader::u64);
}

Json encode_location(const ManagedResource& resource)
{
    return {{"kind", "managed"}, {"path", encode_path(resource.path)},
        {"revision", std::to_string(resource.revision)}, {"sha256", resource.sha256}};
}

Json encode_location(const ExternalFileResource& resource)
{
    return {{"kind", "external_file"}, {"path", encode_path(resource.path)}, {"read_policy", write_enum(resource.read_policy)},
        {"expected_sha256", write_optional(resource.expected_sha256, [](const auto& text) { return text; })},
        {"revision", encode_revision(resource.revision)}};
}

Json encode_location(const ImageSequenceResource& resource)
{
    Json selection;
    if (const auto* directory = std::get_if<DirectorySelection>(&resource.selection)) {
        selection = {{"kind", "directory"}, {"directory", encode_path(directory->directory)}, {"filename_pattern", directory->filename_pattern}};
    } else {
        selection = {{"kind", "files"}, {"files", write_array(std::get<std::vector<ResourcePath>>(resource.selection), encode_path)}};
    }
    return {{"kind", "image_sequence"}, {"selection", std::move(selection)}, {"sort", write_enum(resource.sort)},
        {"advance", write_enum(resource.advance)}, {"repeat", write_enum(resource.repeat)}, {"missing", write_enum(resource.missing)},
        {"trigger", write_enum(resource.trigger)}, {"initial_index", std::to_string(resource.initial_index)}};
}

Json encode_location(const DeviceResource& resource)
{
    return {{"kind", "device"}, {"adapter", write_id(resource.adapter)}, {"configuration", encode_value(resource.configuration)}};
}

Json encode_location(const OutputPathResource& resource)
{
    return {{"kind", "output_path"}, {"directory", encode_path(resource.directory)}, {"naming", write_enum(resource.naming)},
        {"stem", resource.stem}, {"extension", resource.extension}, {"overwrite", write_enum(resource.overwrite)},
        {"capacity_bytes", std::to_string(resource.capacity_bytes)}};
}

std::variant<DirectorySelection, std::vector<ResourcePath>> decode_selection(const Reader& reader)
{
    const auto kind = reader.field("kind");
    if (kind.text() == "directory") {
        reader.object({"kind", "directory", "filename_pattern"});
        return DirectorySelection{decode_path(reader.field("directory")), std::string(reader.field("filename_pattern").text())};
    }
    if (kind.text() == "files") {
        reader.object({"kind", "files"});
        return read_array<ResourcePath>(reader.field("files"), decode_path);
    }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown sequence selection.");
}

ResourceLocation decode_location(const Reader& reader)
{
    const auto kind = reader.field("kind");
    const auto token = kind.text();
    if (token == "managed") {
        reader.object({"kind", "path", "revision", "sha256"});
        return ManagedResource{decode_path(reader.field("path")), reader.field("revision").u64(),
            std::string(reader.field("sha256").text())};
    }
    if (token == "external_file") {
        reader.object({"kind", "path", "read_policy", "expected_sha256", "revision"});
        return ExternalFileResource{decode_path(reader.field("path")), read_enum<ResourceReadPolicy>(reader.field("read_policy")),
            read_optional<std::string>(reader.field("expected_sha256"), &Reader::text), decode_revision(reader.field("revision"))};
    }
    if (token == "image_sequence") {
        reader.object({"kind", "selection", "sort", "advance", "repeat", "missing", "trigger", "initial_index"});
        return ImageSequenceResource{decode_selection(reader.field("selection")), read_enum<SequenceSort>(reader.field("sort")),
            read_enum<SequenceAdvance>(reader.field("advance")), read_enum<SequenceRepeat>(reader.field("repeat")),
            read_enum<SequenceMissing>(reader.field("missing")), read_enum<SequenceTrigger>(reader.field("trigger")),
            reader.field("initial_index").size_value()};
    }
    if (token == "device") {
        reader.object({"kind", "adapter", "configuration"});
        return DeviceResource{read_id<SchemaId>(reader.field("adapter")), decode_value(reader.field("configuration"))};
    }
    if (token == "output_path") {
        reader.object({"kind", "directory", "naming", "stem", "extension", "overwrite", "capacity_bytes"});
        return OutputPathResource{decode_path(reader.field("directory")), read_enum<OutputNaming>(reader.field("naming")),
            std::string(reader.field("stem").text()), std::string(reader.field("extension").text()),
            read_enum<OutputOverwrite>(reader.field("overwrite")), reader.field("capacity_bytes").u64()};
    }
    kind.fail(ErrorCode::UnsupportedSolutionContent, "Unknown resource location kind.");
}

} // namespace

Json encode_resource_reference(const ResourceReference& reference)
{
    return {{"id", write_id(reference.id)}, {"kind", write_enum(reference.kind)}, {"revision", encode_revision(reference.revision)}};
}

ResourceReference decode_resource_reference(const Reader& reader)
{
    reader.object({"id", "kind", "revision"});
    ResourceReference reference{read_id<ResourceId>(reader.field("id")), read_enum<ResourceKind>(reader.field("kind")),
        decode_revision(reader.field("revision"))};
    static_cast<void>(reader.checked(validate_resource_reference(reference)));
    return reference;
}

Json encode_resource(const ResourceDefinition& definition)
{
    return {{"id", write_id(definition.id)}, {"name", definition.name}, {"kind", write_enum(definition.kind)},
        {"location", std::visit([](const auto& value) { return encode_location(value); }, definition.location)}};
}

ResourceDefinition decode_resource(const Reader& reader)
{
    reader.object({"id", "name", "kind", "location"});
    return {read_id<ResourceId>(reader.field("id")), std::string(reader.field("name").text()),
        read_enum<ResourceKind>(reader.field("kind")), decode_location(reader.field("location"))};
}

} // namespace visionpro::infrastructure::detail
