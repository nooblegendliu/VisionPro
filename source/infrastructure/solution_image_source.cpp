#include "solution_image_source.hpp"
#include "json_codec.hpp"

namespace visionpro::infrastructure::detail {
namespace {
using namespace core;
using namespace workflow;

template<class Id> Id key(std::string_view text)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(text)); }

DataType enum_type(std::string_view name, std::initializer_list<std::string_view> names)
{
    std::vector<EnumMember> members;
    for (const auto member : names) { members.push_back({key<EnumMemberId>(member), std::string(member)}); }
    return std::get<DataType>(DataType::enumeration(key<SchemaId>(name), 1, members));
}

void add_constant(Node& node, std::string_view name, ConfigValue value)
{
    const auto id = key<ParameterId>(name);
    if (std::ranges::any_of(node.bindings, [&](const Binding& b) { return b.target == BindingTarget{id}; })) {
        DiagnosticLocation where;
        where.node = node.id;
        where.parameter = id;
        throw CodecError{{ErrorCode::UnsupportedSolutionContent, Severity::Error, std::move(where),
            "Legacy image source contains a conflicting parameter: " + std::string(name)}};
    }
    node.bindings.push_back({id, {std::move(value)}});
}

void add_selection(Node& node, std::string_view name, const DataType& type, std::string_view selected)
{
    add_constant(node, name, std::get<ConfigValue>(ConfigValue::enumeration(type, key<EnumMemberId>(selected))));
}

void migrate_enum(Node& node, std::string_view name, const DataType& old_type, const DataType& new_type)
{
    const BindingTarget target{key<ParameterId>(name)};
    for (auto& binding : node.bindings) {
        if (binding.target != target) { continue; }
        for (auto& source : binding.sources) {
            auto* value = std::get_if<ConfigValue>(&source);
            if (value && value->type() == old_type) {
                *value = std::get<ConfigValue>(ConfigValue::enumeration(new_type, *value->as_enum()));
            }
        }
    }
}
} // namespace

std::optional<LegacyImageSource> legacy_image_source(const workflow::Node& node)
{
    const auto* config = std::get_if<workflow::ToolConfiguration>(&node.configuration);
    if (!config || config->config_version != 1) { return {}; }
    if (config->type.text() == "visionpro.image.read_file") { return LegacyImageSource::Local; }
    if (config->type.text() == "visionpro.camera.acquire") { return LegacyImageSource::Acquire; }
    if (config->type.text() == "visionpro.camera.frame") { return LegacyImageSource::Delivered; }
    return {};
}

void migrate_image_source(workflow::Node& node, LegacyImageSource source)
{
    static const auto sources = enum_type("visionpro.image.source.source_type", {"Local", "Camera", "SDK"});
    static const auto cameras = enum_type("visionpro.image.source.camera_input", {"Acquire", "Delivered"});
    static const auto decode = enum_type("visionpro.image.source.decode_mode", {"Gray8", "Native8", "RGB24"});
    static const auto old_decode = enum_type("visionpro.image.read_file.decode_mode", {"Gray8", "Native8"});
    static const auto mode = enum_type("visionpro.image.source.mode", {"Single", "Continuous", "Software", "Hardware"});
    static const auto old_mode = enum_type("visionpro.camera.acquire.mode", {"Single", "Continuous", "Software", "Hardware"});
    std::get<workflow::ToolConfiguration>(node.configuration).type = key<core::ToolTypeId>("visionpro.image.source");
    const bool local = source == LegacyImageSource::Local;
    add_selection(node, "source_type", sources, local ? "Local" : "Camera");
    if (local) {
        migrate_enum(node, "decode_mode", old_decode, decode);
        add_constant(node, "auto_next", ConfigValue::boolean(false));
        add_constant(node, "stop_at_end", ConfigValue::boolean(false));
    } else {
        add_selection(node, "camera_input", cameras, source == LegacyImageSource::Acquire ? "Acquire" : "Delivered");
        add_selection(node, "decode_mode", decode, "Native8");
        if (source == LegacyImageSource::Acquire) { migrate_enum(node, "mode", old_mode, mode); }
    }
}

void migrate_image_sources(workflow::Solution& solution)
{
    for (auto& flow : solution.workflows) {
        for (auto& node : flow.nodes) {
            if (const auto kind = legacy_image_source(node)) {
                migrate_image_source(node, *kind);
                continue;
            }
            const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
            if (!tool || tool->config_version != 1 || tool->type.text() != "visionpro.image.source") { continue; }
            if (node.name == "图像元") { node.name = "图像源"; }
            const auto sequence = std::ranges::find_if(node.resources, [](const auto& bound) { return bound.slot.text() == "sequence"; });
            if (sequence == node.resources.end()) { continue; }
            const auto resource = std::ranges::find(solution.resources, sequence->resource.id, &ResourceDefinition::id);
            if (resource != solution.resources.end() && resource->kind == ResourceKind::DirectorySequence
                && resource->name == "图像元序列 · " + node.id.to_string()) {
                resource->name = "图像源序列 · " + node.id.to_string();
            }
        }
    }
}

} // namespace visionpro::infrastructure::detail
