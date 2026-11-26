#include "image_source_model.hpp"

#include <algorithm>
#include <limits>

namespace visionpro::tools::image_source {
namespace {
using namespace core;

ConfigValue enumeration(std::string_view field, std::initializer_list<std::string_view> names)
{
    std::vector<EnumMember> members;
    for (const auto name : names) { members.push_back({key<EnumMemberId>(name), std::string(name)}); }
    const auto type = std::get<DataType>(DataType::enumeration(
        key<SchemaId>("visionpro.image.source." + std::string(field)), 1, members));
    return std::get<ConfigValue>(ConfigValue::enumeration(type, members.front().id));
}

ParameterDescriptor parameter(std::string_view name, const ConfigValue& initial, bool bound = false)
{
    ParameterDescriptor result{key<ParameterId>(name), std::string(name), initial.type(), false, initial, {}, {}, {}};
    if (bound) {
        result.subscribable = true;
        result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::PublicInput,
            BindingSourceKind::RunOutput, BindingSourceKind::GlobalRead};
    } else { result.binding.time = BindingTime::SnapshotBuild; }
    return result;
}

NodeStateDefinition source_state()
{
    const auto integer = DataType::integer();
    const auto image = DataType::image();
    const auto cached = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.image.source.cached"), 1,
        {{key<FieldId>("path"), DataType::string()}, {key<FieldId>("stamp"), integer},
            {key<FieldId>("size"), integer}, {key<FieldId>("frame"), image}}));
    const auto type = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.image.source.state"), 1,
        {{key<FieldId>("published"), integer}, {key<FieldId>("cursor"), integer},
         {key<FieldId>("previous"), DataType::optional(image)}, {key<FieldId>("cache"), DataType::list(cached)},
         {key<FieldId>("seen"), DataType::list(DataType::string())}}));
    const auto initial = std::get<ConfigValue>(ConfigValue::record(type, {
        {key<FieldId>("published"), ConfigValue::integer(0)}, {key<FieldId>("cursor"), ConfigValue::integer(-1)},
        {key<FieldId>("previous"), ConfigValue::none(image)},
        {key<FieldId>("cache"), std::get<ConfigValue>(ConfigValue::list(cached, {}))},
        {key<FieldId>("seen"), std::get<ConfigValue>(ConfigValue::list(DataType::string(), {}))}}));
    return {type, initial, StateScope::SolutionSession, StateResetPolicy::DomainStartAndExplicit,
        StateCompatibilityPolicy::AlwaysNewGeneration, StateMutationTiming::OnResultAccepted, StateConcurrencyPolicy::Serialized};
}

void specialize(ToolDescriptor& tool, std::span<const FixedToolParameter> configured, std::span<const ToolResourceBinding> resources)
{
    const auto setting = [&](std::string_view name) -> const ConfigValue* {
        const auto found = std::ranges::find_if(configured, [&](const auto& item){ return item.id.text() == name; });
        if (found != configured.end()) { return &found->value; }
        const auto field = std::ranges::find_if(tool.parameters, [&](const auto& item){ return item.id.text() == name; });
        return field != tool.parameters.end() && field->default_value ? &*field->default_value : nullptr;
    };
    const auto equals = [&](std::string_view name, std::string_view value) {
        const auto* item = setting(name);
        return item && item->as_enum() && item->as_enum()->text() == value;
    };
    const auto on = [&](std::string_view name) {
        const auto* item = setting(name);
        return item && item->as_bool() && *item->as_bool();
    };
    const bool local = equals("source_type", "Local");
    const bool camera = equals("source_type", "Camera");
    const bool acquire = camera && equals("camera_input", "Acquire");
    const bool delivered = camera && !acquire;
    const bool subscribed = local && on("subscribe_folder");
    const bool stitch = on("stitch");
    const bool clear = stitch && on("clear_on_trigger");
    const bool filter = on("filter_enabled");
    const bool exposure = acquire && on("control_exposure");
    const bool gain = acquire && on("control_gain");
    const bool has_sequence = std::ranges::any_of(resources, [](const auto& item){ return item.slot.text() == "sequence"; });
    const bool has_file = std::ranges::any_of(resources, [](const auto& item){ return item.slot.text() == "source"; });
    if (local || acquire) { tool.inputs.clear(); }
    std::erase_if(tool.resources, [&](const auto& slot) {
        if (slot.slot.text() == "camera") { return !acquire; }
        if (!local || subscribed) { return true; }
        if (slot.slot.text() == "sequence") { return !has_sequence; }
        return has_sequence && !has_file;
    });
    for (auto& slot : tool.resources) { slot.required = true; }
    std::erase_if(tool.parameters, [&](const auto& p) {
        const auto field = p.id.text();
        return (field == "frame_origin" && !local && !acquire) || (field == "stitch_origin" && !stitch)
            || (field == "sn_subscription" && !local) || (field == "folder_path" && !subscribed)
            || (field == "clear_value" && !clear) || (field == "message" && !filter)
            || (field == "exposure_us" && !exposure) || (field == "gain" && !gain);
    });
    tool.execution.effects = acquire ? ToolEffects::External : ToolEffects::State;
    tool.execution.retry = delivered ? ToolRetry::StagedState : ToolRetry::Never;
    tool.may_end_continuous = local && on("auto_next") && on("stop_at_end");
    if (filter) { tool.message_filter = ToolMessageFilter{key<ParameterId>("message"), key<ParameterId>("trigger_text")}; }
    else { tool.message_filter.reset(); }
}
}

core::ToolDescriptor descriptor()
{
    using namespace core;
    const auto id = "visionpro.image.source";
    ToolDescriptor tool{key<ToolTypeId>(id), "图像源", 1, {key<SchemaId>(id), 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    const ValueSemantics pixels{MeasurementUnit::Pixel, {}};
    tool.parameters = {parameter("source_type", enumeration("source_type", {"Local","Camera","SDK"})),
        parameter("camera_input", enumeration("camera_input", {"Acquire","Delivered"})),
        parameter("decode_mode", enumeration("decode_mode", {"Gray8","Native8","RGB24"})),
        parameter("mode", enumeration("mode", {"Single","Continuous","Software","Hardware"}))};
    for (const auto name : {"frame_origin", "stitch_origin"}) {
        ParameterDescriptor p{key<ParameterId>(name), name, DataType::point2d(), true, {}, {}, {}, pixels};
        p.binding.time = BindingTime::SnapshotBuild;
        tool.parameters.push_back(std::move(p));
    }
    for (const auto name : {"show_name", "auto_next", "stop_at_end", "embed_images", "output_mono8", "stitch",
        "clear_on_trigger", "filter_enabled", "control_exposure", "control_gain", "subscribe_folder", "refresh_thumbnails",
        "trigger_flow", "selected_only"}) {
        const std::string_view field{name};
        tool.parameters.push_back(parameter(field, ConfigValue::boolean(field == "show_name" || field == "auto_next" || field == "stop_at_end")));
    }
    const auto number = [&](std::string_view name, std::int64_t initial, std::int64_t low, std::int64_t high) {
        auto p = parameter(name, ConfigValue::integer(initial));
        p.constraints.integer_range = IntegerRange{low, high};
        tool.parameters.push_back(std::move(p));
    };
    number("sn_initial", 1, 0, std::numeric_limits<std::int64_t>::max());
    number("timeout_ms", 1000, 1, 60000);
    number("cache_size", 0, 0, 5);
    number("stitch_start", 0, 0, 20000);
    number("stitch_height", 0, 0, 20000);
    const auto empty_text = std::get<ConfigValue>(ConfigValue::string(""));
    tool.parameters.push_back(parameter("trigger_text", empty_text));
    tool.parameters.push_back(parameter("selected_indices", std::get<ConfigValue>(ConfigValue::list(DataType::integer(), {}))));
    tool.parameters.push_back(parameter("selected_files", std::get<ConfigValue>(ConfigValue::list(DataType::string(), {}))));
    tool.parameters.push_back(parameter("source_names", std::get<ConfigValue>(ConfigValue::list(DataType::string(), {}))));
    tool.parameters.push_back(parameter("message", ConfigValue::none(DataType::string()), true));
    tool.parameters.push_back(parameter("clear_value", ConfigValue::integer(0), true));
    tool.parameters.push_back(parameter("exposure_us", std::get<ConfigValue>(ConfigValue::real(1000)), true));
    tool.parameters.push_back(parameter("gain", std::get<ConfigValue>(ConfigValue::real(0)), true));
    tool.parameters.push_back(parameter("sn_subscription", ConfigValue::none(DataType::integer()), true));
    tool.parameters.push_back(parameter("folder_path", empty_text, true));
    InputDescriptor input{key<InputId>("image"), "本轮输入图像", DataType::image(), true, {}, {}, {}, pixels};
    input.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::RunOutput};
    tool.inputs.push_back(std::move(input));
    tool.resources = {{key<SchemaId>("source"), ResourceKind::Image, false}, {key<SchemaId>("sequence"), ResourceKind::DirectorySequence, false},
        {key<SchemaId>("camera"), ResourceKind::Device, false, true}};
    const auto output = [&](std::string_view name, const DataType& type, ValueSemantics semantics = {}) {
        tool.outputs.push_back({key<OutputId>(name), std::string(name), type, semantics, false,
            {OutputAbsenceReason::ConditionNotMet, OutputAbsenceReason::ToolDidNotProduce}});
    };
    for (const std::string name : {"image","gray_image","image_1","image_2","image_3"}) {
        output(name, DataType::image(), pixels);
        output(name + "_width", DataType::integer()); output(name + "_height", DataType::integer());
        output(name == "image" ? "pixel_format" : name + "_pixel_format", DataType::integer());
    }
    for (const auto name : {"frame_number","camera_frame_number","lost_frames",
        "lost_packets","acquire_failures","sn"}) { output(name, DataType::integer()); }
    for (const auto name : {"file_path","file_name","serial","timestamp_source"}) { output(name, DataType::string()); }
    for (const auto name : {"device_ticks","tick_frequency_hz","device_frame_number"}) { output(name, DataType::optional(DataType::string())); }
    output("elapsed_ms", DataType::real());
    tool.state = source_state();
    tool.state_in_admission_order = true;
    tool.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono},{PixelDepth::UInt8, ChannelOrder::BGR},{PixelDepth::UInt8, ChannelOrder::RGB}};
    tool.execution.mode = ToolExecutionMode::Asynchronous;
    tool.execution.effects = ToolEffects::External;
    tool.execution.cancellation = ToolCancellation::Cooperative;
    tool.specialize = specialize;
    return tool;
}
}
