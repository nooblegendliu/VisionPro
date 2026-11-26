#include "image_output.hpp"
#include "vision_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/validation.hpp>

#include <algorithm>
#include <limits>

namespace visionpro::tools::vision
{
namespace
{
using namespace core;
template <SchemaIdentity T> T key(std::string_view name)
{
    return std::get<T>(SchemaIdFactory::parse<T>(name));
}

ConfigValue enumeration(std::string_view name, std::initializer_list<std::string_view> choices)
{
    std::vector<EnumMember> members;
    for (auto choice : choices) {
        members.push_back({key<EnumMemberId>(choice), std::string(choice)});
    }
    auto type = std::get<DataType>(
        DataType::enumeration(key<SchemaId>("visionpro.image.output." + std::string(name)), 1, members));
    return std::get<ConfigValue>(ConfigValue::enumeration(type, members.front().id));
}

ConfigValue unclassified_trigger(std::string_view type)
{
    if (type == "Integer") {
        return ConfigValue::integer(-1);
    }
    if (type == "String") {
        return std::get<ConfigValue>(ConfigValue::string(""));
    }
    const auto judgement = std::get<DataType>(workflow::system_field_type(workflow::SystemField::Judgement));
    return std::get<ConfigValue>(ConfigValue::enumeration(judgement, key<EnumMemberId>("NotEvaluated")));
}

void specialize(ToolDescriptor& tool, std::span<const FixedToolParameter> fixed, std::span<const ToolResourceBinding>)
{
    const auto setting = [&](std::string_view name) -> const ConfigValue& {
        const auto found = std::ranges::find_if(fixed, [&](auto& item) { return item.id.text() == name; });
        const auto field = std::ranges::find_if(tool.parameters, [&](auto& item) { return item.id.text() == name; });
        if (found != fixed.end() && found->value.type() == field->type) {
            return found->value;
        }
        return *field->default_value;
    };
    const bool saving = *setting("save_enabled").as_bool();
    const bool trigger = *setting("trigger_enabled").as_bool();
    const bool original = *setting("save_original").as_bool();
    const bool rendered = *setting("save_rendered").as_bool();
    const std::string condition{setting("save_condition").as_enum()->text()};
    const bool writing = saving && (!trigger || condition != "None");
    const bool drawing = *setting("output_enabled").as_bool() || (writing && rendered);
    auto trigger_input = std::ranges::find(tool.inputs, key<InputId>("trigger"), &InputDescriptor::id);
    if (trigger_input != tool.inputs.end() && writing && trigger) {
        auto unclassified = unclassified_trigger(setting("trigger_source_type").as_enum()->text());
        trigger_input->type = unclassified.type();
        trigger_input->required = condition != "All";
        trigger_input->default_value = trigger_input->required ? std::nullopt : std::optional{std::move(unclassified)};
    }
    std::erase_if(tool.inputs, [&](const InputDescriptor& port) {
        if (port.id.text() == "image") {
            return false;
        }
        if (port.id.text() == "trigger") {
            return !writing || !trigger;
        }
        return !drawing;
    });
    std::erase_if(tool.parameters, [&](const ParameterDescriptor& port) {
        const auto name = port.id.text();
        return (name == "original_prefix" && (!writing || !original)) ||
               (name == "rendered_prefix" && (!writing || !rendered)) || (name == "text" && !drawing);
    });
    std::erase_if(tool.resources, [&](const ResourceRequirement& resource) {
        const auto name = resource.slot.text();
        if (!writing || (name.starts_with("original") ? !original : !rendered)) {
            return true;
        }
        if (name.ends_with("_ok_path")) {
            return !trigger || condition == "NG";
        }
        if (name.ends_with("_ng_path")) {
            return !trigger || condition == "OK";
        }
        return trigger && condition != "All";
    });
    tool.execution.effects = writing ? ToolEffects::External : ToolEffects::None;
    tool.execution.retry = writing ? ToolRetry::Never : ToolRetry::Idempotent;
}
} // namespace

core::ToolDescriptor image_output_descriptor()
{
    using namespace core;
    constexpr std::string_view id = "visionpro.image.output";
    ToolDescriptor tool{
        key<ToolTypeId>(id), "输出图像", 1, {key<SchemaId>(id), 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    const auto add = [&](std::string_view name, ConfigValue value, bool dynamic = false) -> ParameterDescriptor& {
        ParameterDescriptor p{key<ParameterId>(name), std::string(name), value.type(), false, value, {}, {}, {}};
        p.binding.time = dynamic ? BindingTime::BeforeInvocation : BindingTime::SnapshotBuild;
        p.subscribable = dynamic;
        if (dynamic) {
            p.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput,
                                         BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
        }
        return tool.parameters.emplace_back(std::move(p));
    };
    const auto integer = [&](std::string_view name, std::int64_t initial, std::int64_t min, std::int64_t max) {
        add(name, ConfigValue::integer(initial)).constraints.integer_range = IntegerRange{min, max};
    };
    const auto str = [&](std::string_view name, std::string_view value, bool dynamic = false) {
        add(name, std::get<ConfigValue>(ConfigValue::string(value)), dynamic);
    };
    for (auto name : {"output_enabled", "save_enabled", "save_original", "save_rendered", "trigger_enabled",
                      "date_directory", "ftp_enabled", "sync_storage"}) {
        const std::string_view field{name};
        add(field, ConfigValue::boolean(field == "output_enabled" || field == "sync_storage"));
    }
    add("save_condition", enumeration("save_condition", {"All", "OK", "NG", "None"}));
    add("trigger_source_type", enumeration("trigger_source_type", {"Judgement", "Integer", "String"})).structural = true;
    add("pixel_mode", enumeration("pixel_mode", {"RGB24", "Mono8"}));
    add("file_format", enumeration("file_format", {"PNG", "JPEG", "BMP"}));
    add("storage_policy", enumeration("storage_policy", {"Stop", "Overwrite"}));
    add("space_unit", enumeration("space_unit", {"MB", "GB"}));
    add("scale_mode", enumeration("scale_mode", {"Original", "Display", "Custom"}));
    add("line_type", enumeration("line_type", {"Eight", "Four", "Antialias"}));
    for (auto id : {"original_suffix", "rendered_suffix"}) {
        add(id, enumeration("suffix", {"Sequence", "Time", "None"}));
    }
    for (auto id : {"line_scale", "text_scale"}) {
        add(id, std::get<ConfigValue>(ConfigValue::real(1))).constraints.real_range = RealRange{0.05, 100};
    }
    for (auto id : {"original_prefix", "rendered_prefix", "text"}) {
        str(id, "", true);
    }
    str("color", "#00FF00");
    for (const auto name : {"text_color", "text_ok_color", "text_ng_color"}) {
        str(name, "");
    }
    integer("text_opacity", 100, 0, 100);
    integer("text_thickness", 1, 1, 64);
    str("ftp_host", "");
    str("ftp_user", "");
    str("ftp_credential", "");
    str("ftp_path", "/");
    str("ftp_encoding", "UTF-8");
    integer("ftp_port", 21, 1, 65535);
    integer("font_size", 16, 1, 1024);
    integer("text_x", 0, 0, 1000000);
    integer("text_y", 0, 0, 1000000);
    integer("jpeg_quality", 90, 0, 100);
    integer("original_cache", 20, 1, 20);
    integer("rendered_cache", 20, 1, 20);
    integer("save_interval", 0, 0, 10);
    integer("max_files", 0, 0, 2147483647);
    integer("min_free", 0, 0, 2147483647);
    integer("retention_days", 0, 0, 500);
    const auto input = [&](std::string_view name, const DataType& type, bool multiple) {
        InputDescriptor port{
            key<InputId>(name), std::string(name), multiple ? DataType::list(type) : type, !multiple, {}, {}, {}, {}};
        port.binding.allowed_sources = {BindingSourceKind::RunOutput, BindingSourceKind::PublicInput};
        if (multiple) {
            port.binding.cardinality = BindingCardinality::MultipleSources;
            port.default_value = std::get<ConfigValue>(ConfigValue::list(type, {}));
        }
        tool.inputs.push_back(std::move(port));
    };
    input("image", DataType::image(), false);
    input("trigger", std::get<DataType>(workflow::system_field_type(workflow::SystemField::Judgement)), false);
    tool.inputs.back().binding.allowed_sources.push_back(BindingSourceKind::GlobalRead);
    tool.inputs.back().binding.allowed_sources.push_back(BindingSourceKind::Constant);
    for (auto id : {"bounds", "centroids"}) {
        input(id, DataType::list(blob_component_type()), true);
    }
    input("masks", DataType::raster_mask(), true);
    input("regions", DataType::region2d(), true);
    input("points", DataType::point2d(), true);
    input("lines", DataType::line2d(), true);
    input("circles", DataType::circle2d(), true);
    for (auto name : {"original_path", "rendered_path", "original_ok_path", "original_ng_path", "rendered_ok_path",
                      "rendered_ng_path"}) {
        tool.resources.push_back({key<SchemaId>(name), ResourceKind::OutputPath, false});
    }
    const auto output = [&](std::string_view id, DataType type, bool conditional) {
        OutputDescriptor port{key<OutputId>(id), std::string(id), std::move(type), {}, !conditional};
        if (conditional) {
            port.allowed_absence = {OutputAbsenceReason::ConditionNotMet, OutputAbsenceReason::ToolDidNotProduce};
        }
        tool.outputs.push_back(std::move(port));
    };
    output("image", DataType::image(), true);
    tool.outputs.back().semantics.unit = MeasurementUnit::Pixel;
    for (auto id : {"image_width", "image_height", "pixel_format"}) {
        output(id, DataType::integer(), true);
    }
    for (auto id : {"original_path", "original_name", "rendered_path", "rendered_name", "big_file_path",
                    "original_big_file_path", "rendered_big_file_path"}) {
        output(id, DataType::string(), true);
    }
    output("module_status", DataType::integer(), false);
    tool.module_status_output = key<OutputId>("module_status");
    output("elapsed_ms", DataType::real(), false);
    tool.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono},
                          {PixelDepth::UInt8, ChannelOrder::BGR},
                          {PixelDepth::UInt8, ChannelOrder::RGB}};
    tool.execution.effects = ToolEffects::External;
    tool.execution.cancellation = ToolCancellation::Cooperative;
    tool.specialize = specialize;
    tool.uses_display_item_styles = true;
    return tool;
}

namespace image_output
{
const core::DataValue& parameter(const core::InvocationContextData& call, std::string_view name)
{
    return std::ranges::find_if(call.parameters, [&](const auto& p) { return p.id.text() == name; })->value;
}
const core::DataValue* input(const core::InvocationContextData& call, std::string_view name)
{
    const auto it = std::ranges::find_if(call.inputs, [&](const auto& p) { return p.id.text() == name; });
    return it == call.inputs.end() ? nullptr : &it->value;
}
} // namespace image_output
} // namespace visionpro::tools::vision
