#include "tool_catalog.hpp"
#include "../tools/image_fixtures.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/infrastructure/image_directory_trigger.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/trigger_control.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::infrastructure;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using Clock = TriggerControl::Clock;

void check(bool pass, const char* why) { if (!pass) { throw std::runtime_error(why); } }
template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template<class Id> Id key(std::string_view id) { return take(SchemaIdFactory::parse<Id>(id)); }
template<class Id> Id fresh() { return take(IdFactory::generate<Id>()); }
Binding set(std::string_view id, ConfigValue value) { return {key<ParameterId>(id), {std::move(value)}}; }

struct Scratch {
    fs::path path = fs::temp_directory_path() / ("vp-image-entry-" + fresh<MessageId>().to_string());
    Scratch() { check(fs::create_directory(path), "Cannot create temporary image directory."); }
    ~Scratch() { std::error_code unused; fs::remove_all(path, unused); }
    void add(const char* name) const {
        const auto& bytes = visionpro::test_fixtures::png;
        std::ofstream file(path / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
        file.close();
        check(file.good(), "Cannot create test image.");
    }
};

Solution fixture()
{
    const Node node{fresh<NodeId>(), "Source", ToolConfiguration{key<ToolTypeId>("visionpro.image.source"), 1}, {}, {}};
    const Workflow flow{fresh<WorkflowId>(), "Input", {key<SchemaId>("source.test"), 1}, {node}, {}, {}, {node.id}, {}};
    return {fresh<SolutionId>(), "Image entry", 1, 1, {flow}, {}, {}, {},
        {{key<CoordinateSystemId>("sensor"), CoordinateKind::Image, MeasurementUnit::Pixel, {0,0}, {1,0}, {0,-1}, Handedness::LeftHanded}}};
}

NodeResult finish(RunSubmission submission)
{
    if (const auto* bad = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(bad->diagnostic.message); }
    const auto& run = std::get<RunHandle>(submission);
    const auto terminal = take(run.wait(RunWaitTarget::Quiescence, Clock::now() + 5s));
    if (terminal.progress.status != ExecutionStatus::Succeeded && !terminal.progress.diagnostics.empty()) {
        throw std::runtime_error(terminal.progress.diagnostics.front().message);
    }
    check(terminal.outcome == RunWaitOutcome::Satisfied && terminal.progress.status == ExecutionStatus::Succeeded
        && terminal.progress.nodes.size() == 1 && terminal.progress.nodes.front().invocation,
        "Each accepted delivery must finish its one configured node.");
    return take(run.lookup(*terminal.progress.nodes.front().invocation)).value();
}

const DataValue& output(const NodeResult& result, std::string_view id)
{
    const auto* slot = result.output(key<OutputId>(id));
    check(slot && std::holds_alternative<DataValue>(slot->content), "Expected a produced image-source output.");
    return std::get<DataValue>(slot->content);
}

void rejected(const TriggerReceipt& receipt, ErrorCode code)
{
    const auto* error = std::get_if<RunRejected>(&receipt.submission);
    check(error && error->diagnostic.code == code, "Delivery must be rejected with its specific reason.");
}

void migrate_default_image_source_name(const ToolRegistry& registry)
{
    const Scratch scratch;
    scratch.add("first.png");
    auto model = fixture();
    auto& flow = model.workflows.front();
    auto& source = flow.nodes.front();
    source.name = "图像元";
    const auto id = source.id;
    const auto sequence = fresh<ResourceId>();
    const auto format = model.format_version;
    const auto config = std::get<ToolConfiguration>(source.configuration);
    model.resources.push_back({sequence, "图像元序列 · " + id.to_string(), ResourceKind::DirectorySequence,
        ImageSequenceResource{std::vector<ResourcePath>{{PathBase::SolutionDirectory, "first.png", {}}},
            SequenceSort::ExplicitOrder, SequenceAdvance::PerInvocation, SequenceRepeat::Loop, SequenceMissing::Fail,
            SequenceTrigger::Manual, 0}});
    source.resources = {{key<SchemaId>("sequence"), {sequence, ResourceKind::DirectorySequence, {}}}};
    source.bindings = {set("frame_origin", take(ConfigValue::point2d({model.coordinates.front().id, MeasurementUnit::Pixel, 0, 0})))};
    auto custom = source;
    custom.id = fresh<NodeId>();
    custom.name = "工位 A 的图像元";
    flow.nodes.push_back(custom);
    const auto other_id = fresh<NodeId>();
    flow.nodes.push_back({other_id, "图像元", ToolConfiguration{key<ToolTypeId>("visionpro.calc.int64"),1}, {}, {}});
    take(write_new_solution(scratch.path, model));
    const auto loaded = take(load_solution(scratch.path, registry));
    const auto& migrated = loaded.solution().workflows.front();
    const auto* named = find_node(migrated, id);
    check(named && named->name == "图像源" && loaded.solution().format_version == format
        && std::get<ToolConfiguration>(named->configuration).type == config.type
        && std::get<ToolConfiguration>(named->configuration).config_version == config.config_version,
        "Loading a default source name must preserve node identity and configuration version.");
    check(find_node(migrated, custom.id)->name == custom.name && find_node(migrated, other_id)->name == "图像元",
        "Custom source names and unrelated nodes must survive a display-name upgrade.");
    check(loaded.solution().resources.front().id == sequence && loaded.solution().resources.front().name == "图像源序列 · " + id.to_string(),
        "The generated sequence label must retain the resource identity.");
}

void switch_source_with_bound_consumer(const ToolRegistry& registry)
{
    const Scratch scratch;
    scratch.add("first.png");
    auto model = fixture();
    auto& flow = model.workflows.front();
    const auto source = flow.nodes.front().id;
    const auto consumer = fresh<NodeId>();
    const auto& tool = *registry.find_descriptor(key<ToolTypeId>("visionpro.image.source"));
    const auto kind = std::ranges::find(tool.parameters, key<ParameterId>("source_type"), &ParameterDescriptor::id);
    const auto source_type = [&](std::string_view name) {
        return set("source_type", take(ConfigValue::enumeration(kind->type, key<EnumMemberId>(name))));
    };
    const auto input = key<InputId>("image");
    const auto main = key<OutputId>("image");
    const auto coords = model.coordinates.front().id;
    const auto file = fresh<ResourceId>(), device = fresh<ResourceId>();
    model.resources = {{file, "Local PNG", ResourceKind::Image,
            ExternalFileResource{{PathBase::SolutionDirectory, "first.png", {}}, ResourceReadPolicy::PerInvocation}},
        {device, "Camera", ResourceKind::Device, DeviceResource{key<SchemaId>("visionpro.camera.simulated"),
            take(camera_device_config("SIM-001"))}}};
    flow.interface.inputs = {{input, "External frame", DataType::image(), true, {}, {}, {}, {MeasurementUnit::Pixel,{}}}};
    flow.nodes.push_back({consumer, "Bound consumer", ToolConfiguration{tool.id, 1},
        {source_type("SDK"), {input, {OutputReference{flow.id, source, main, {}}}}}, {}});
    flow.control_links.push_back({fresh<ControlLinkId>(), source, {}, consumer});
    const auto renderer = fresh<NodeId>();
    const auto& image_output = *registry.find_descriptor(key<ToolTypeId>("visionpro.image.output"));
    const auto pixels = std::ranges::find(image_output.parameters, key<ParameterId>("pixel_mode"), &ParameterDescriptor::id);
    flow.nodes.push_back({renderer, "Output image", ToolConfiguration{image_output.id, 1},
        {{input, {OutputReference{flow.id, source, main, {}}}},
         set("pixel_mode", take(ConfigValue::enumeration(pixels->type, key<EnumMemberId>("Mono8"))))}, {}});
    flow.control_links.push_back({fresh<ControlLinkId>(), consumer, {}, renderer});
    auto& producer = flow.nodes.front();
    producer.resources = {{key<SchemaId>("source"), {file, ResourceKind::Image, {}}},
        {key<SchemaId>("camera"), {device, ResourceKind::Device, {}}}};
    producer.bindings = {source_type("Local"), set("frame_origin", take(ConfigValue::point2d({coords, MeasurementUnit::Pixel, 0, 0}))),
        {input, {ScopedInputReference{flow.id, {}, input, {}}}}};
    const auto provider = take(FileResourceProvider::create(model, scratch.path));
    auto session = take(WorkflowSession::create(registry));
    auto active = take(session->activate(model, provider));
    const std::array sdk_pixels{std::byte{17}, std::byte{51}, std::byte{93}};
    ImageFrame external{take(ImageHandle::copy({3, 1, 3, {PixelDepth::UInt8, ChannelOrder::Mono}}, sdk_pixels, coords)), {}, {}};
    const std::vector<RunInput> inputs{{input, take(DataValue::image(external))}};
    for (const std::string_view mode : {"Local", "Camera", "SDK"}) {
        if (mode != "Local") {
            producer.bindings.front() = source_type(mode);
            active = take(session->publish(active, model, provider));
        }
        const auto request = take(RunRequest::create(active, WorkflowTarget{flow.id}, inputs,
            {TriggerKind::Manual, key<SchemaId>("test.source.switch")}));
        auto submitted = session->submit(request);
        if (const auto* error = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(error->diagnostic.message); }
        const auto& run = std::get<RunHandle>(submitted);
        const auto progress = take(run.wait(RunWaitTarget::Quiescence, Clock::now() + 5s)).progress;
        check(progress.status == ExecutionStatus::Succeeded && progress.nodes.size() == 3
            && progress.nodes[0].node == source && progress.nodes[1].node == consumer
            && progress.nodes[2].node == renderer && progress.nodes[0].invocation && progress.nodes[1].invocation
            && progress.nodes[2].invocation, "All three bound nodes must execute after a source switch.");
        const auto produced = take(run.lookup(*progress.nodes[0].invocation)).value();
        const auto consumed = take(run.lookup(*progress.nodes[1].invocation)).value();
        const auto& frame = *output(produced, "image").as_image();
        check(frame.image.shares_storage_with(output(consumed, "image").as_image()->image),
            "The original downstream binding must receive the producer's current image.");
        const auto rendered = take(run.lookup(*progress.nodes[2].invocation)).value();
        const auto& result = *output(rendered, "image").as_image();
        check(result.image.copy_pixels() == frame.image.copy_pixels() && result.frame == frame.frame
            && *output(rendered, "pixel_format").as_int64() == 17301505,
            "Image output publishes the selected local, simulated camera or SDK frame with its original provenance.");
        if (mode == "Local") {
            check(*output(produced, "file_name").as_string() == "first.png"
                && frame.image.copy_pixels() == std::vector{std::byte{0}, std::byte{128}, std::byte{129}, std::byte{255}},
                "Local must deliver the selected file.");
        } else if (mode == "Camera") {
            check(frame.frame && frame.frame->source == device && *output(produced, "serial").as_string() == "SIM-001",
                "Camera must deliver a physical acquisition from the selected simulator.");
        } else {
            check(frame.image.shares_storage_with(external.image), "SDK must deliver the frame supplied for this run.");
        }
    }
}

void sdk_delivery(const ToolRegistry& registry)
{
    auto model = fixture();
    auto& flow = model.workflows.front();
    auto& node = flow.nodes.front();
    const auto* tool = registry.find_descriptor(std::get<ToolConfiguration>(node.configuration).type);
    check(tool, "The production catalog must register the unified source.");
    const auto kind = std::ranges::find(tool->parameters, key<ParameterId>("source_type"), &ParameterDescriptor::id);
    node.bindings.push_back(set("source_type", take(ConfigValue::enumeration(kind->type, key<EnumMemberId>("SDK")))));
    const auto port = key<InputId>("pixels"), ticket = key<InputId>("ticket"), message = key<InputId>("command");
    flow.interface.inputs = {{port, "Pixels", DataType::image(), true, {}, {}, {}, {MeasurementUnit::Pixel,{}}},
        {ticket, "Ticket", DataType::integer()}, {message, "Command", DataType::string()}};
    node.bindings.push_back({key<InputId>("image"), {ScopedInputReference{flow.id, {}, port, {}}}});
    node.bindings.push_back(set("filter_enabled", ConfigValue::boolean(true)));
    node.bindings.push_back(set("trigger_text", take(ConfigValue::string("START"))));
    node.bindings.push_back({key<ParameterId>("message"), {ScopedInputReference{flow.id, {}, message, {}}},
        BindingCardinality::SingleSource, ConversionKind::WrapOptional});
    std::shared_ptr<WorkflowSession> session = take(WorkflowSession::create(registry));
    const auto active = take(session->activate(model));
    const auto sender = key<SchemaId>("sdk.sender");
    auto sdk = take(TriggerControl::start_sdk(session, active.identity(), flow.id, port, sender));
    const auto duplicate = TriggerControl::start_sdk(session, active.identity(), flow.id, port, sender);
    const auto* repeated = std::get_if<Diagnostic>(&duplicate);
    check(repeated && repeated->code == ErrorCode::DuplicateIdentity, "A sender cannot arm twice for the same live input.");
    auto observation = take(session->observe(10));
    std::array<std::byte,2> bytes{std::byte{7}, std::byte{11}};
    const auto capture = [&] {
        return ImageFrame{take(ImageHandle::copy({2,1,2,{PixelDepth::UInt8,ChannelOrder::Mono}}, bytes, model.coordinates.front().id)), {}, {}};
    };
    const auto first = capture();
    const std::array inputs{RunInput{ticket, DataValue::integer(65)}, RunInput{message, take(DataValue::string("START"))}};
    const std::array mismatch{inputs.front(), RunInput{message, take(DataValue::string("STOP"))}};
    rejected(sdk->sdk_frame(4, first), ErrorCode::MissingRequiredValue);
    rejected(sdk->sdk_frame(4, first, {}, mismatch), ErrorCode::InputUnavailable);
    check(!observation.wait(Clock::now()).event, "A rejected SDK command cannot publish a run or a frame.");
    const auto coordinates = model.coordinates.front().id;
    rejected(sdk->sdk_frame(4, {2,1,1,{PixelDepth::UInt8,ChannelOrder::Mono}}, bytes, coordinates, {}, {}, {}, inputs),
        ErrorCode::InvalidImage);
    std::array borrowed{std::byte{7}, std::byte{255}, std::byte{11}};
    const auto pending = sdk->sdk_frame(4, {1,2,2,{PixelDepth::UInt8,ChannelOrder::Mono}}, borrowed,
        coordinates, {}, {}, {}, inputs);
    borrowed.fill(std::byte{29});
    bytes.fill(std::byte{29});
    const auto result = finish(pending.submission);
    check(*output(result, "sn").as_int64() == 1, "Rejected inputs and filtered commands cannot consume the initial SN.");
    const auto& held = output(result, "image").as_image()->image;
    check(held.copy_pixels() == std::vector{std::byte{7}, std::byte{11}}, "Caller buffer reuse cannot change an accepted SDK frame.");
    rejected(sdk->sdk_frame(4, capture(), {}, inputs), ErrorCode::DuplicateIdentity);
    const auto second = finish(sdk->sdk_frame(8, capture(), {}, inputs).submission);
    check(output(second,"image").as_image()->image.copy_pixels() == std::vector{std::byte{29},std::byte{29}}
        && *output(second,"sn").as_int64() == *output(result,"sn").as_int64() + 1, "Only accepted deliveries advance the SDK node.");
    model.name = "Republished source";
    const auto published = take(session->publish(active, model, {}));
    rejected(sdk->sdk_frame(9, capture(), {}, inputs), ErrorCode::InvalidRunRequest);
    check(std::holds_alternative<Diagnostic>(TriggerControl::start_sdk(session, active.identity(), flow.id, port, sender)),
        "A new callback cannot be armed with a stale snapshot identity.");
    auto replacement = take(TriggerControl::start_sdk(session, published.identity(), flow.id, port, sender));
    finish(replacement->sdk_frame(1, capture(), {}, inputs).submission);
    check(take(replacement->stop(TriggerStopMode::Drain, Clock::now() + 5s)) == RunWaitOutcome::Satisfied,
        "Draining SDK callbacks must wait for physical completion.");
    rejected(replacement->sdk_frame(2, capture(), {}, inputs), ErrorCode::InvalidState);
    check(held.copy_pixels().front() == std::byte{7}, "Republishing and stopping keeps the first result readable.");
}

void continuous_sequence(const ToolRegistry& registry)
{
    const Scratch scratch;
    std::vector<ResourcePath> files;
    for (const char* name : {"A.png", "B.png", "C.png"}) {
        scratch.add(name);
        files.push_back({PathBase::SolutionDirectory, name, {}});
    }
    auto model = fixture();
    auto& flow = model.workflows.front();
    auto& node = flow.nodes.front();
    model.resources.push_back({fresh<ResourceId>(), "Sequence", ResourceKind::DirectorySequence,
        ImageSequenceResource{files, SequenceSort::ExplicitOrder, SequenceAdvance::PerInvocation,
            SequenceRepeat::Stop, SequenceMissing::Fail, SequenceTrigger::Manual, 0}});
    node.resources = {{key<SchemaId>("sequence"), {model.resources.front().id, ResourceKind::DirectorySequence, {}}}};
    node.bindings = {set("frame_origin", take(ConfigValue::point2d({model.coordinates.front().id, MeasurementUnit::Pixel,0,0}))),
        set("auto_next", ConfigValue::boolean(true)), set("stop_at_end", ConfigValue::boolean(true)),
        set("selected_only", ConfigValue::boolean(true)), set("selected_indices", take(ConfigValue::list(DataType::integer(),
            {ConfigValue::integer(0), ConfigValue::integer(2)})))};
    auto provider = take(FileResourceProvider::create(model, scratch.path));
    std::shared_ptr<WorkflowSession> session = take(WorkflowSession::create(registry));
    const auto active = take(session->activate(model, provider));
    const auto epoch = Clock::time_point{};
    auto ticks = take(TriggerControl::start_local(session, active.identity(), flow.id, 10, epoch));
    check(!ticks->tick(epoch + 9ms), "Local continuous acquisition must observe its interval.");
    const auto first = finish(ticks->tick(epoch + 10ms).value().submission);
    check(*output(first,"file_name").as_string() == "A.png", "Production starts with the first selected file.");
    const auto debug = [&] {
        auto request = take(RunRequest::create(active, NodeTarget{flow.id,node.id}, {}, {TriggerKind::Debug,key<SchemaId>("source.debug")},
            {}, DebugStatePolicy::IsolatedSession));
        return finish(session->submit(request));
    };
    const auto debug_a = debug(), debug_c = debug();
    check(*output(debug_a,"file_name").as_string() == "A.png" && *output(debug_c,"file_name").as_string() == "C.png"
        && *output(debug_a,"sn").as_int64() == *output(first,"sn").as_int64(), "Independent debugging has its own cursor and SN.");
    const auto last = finish(ticks->tick(epoch + 20ms).value().submission);
    check(*output(last,"file_name").as_string() == "C.png" && *output(last,"sn").as_int64() == *output(first,"sn").as_int64()+1,
        "Debugging must not consume the last production image.");
    check(!ticks->tick(epoch + 30ms) && !ticks->accepting(), "The local subscription closes only after publishing its last selected frame.");
    take(ticks->stop(TriggerStopMode::Drain, Clock::now() + 5s));
}

void directory_events(const ToolRegistry& registry)
{
    const Scratch scratch;
    scratch.add("A.png");
    auto model = fixture();
    auto& flow = model.workflows.front();
    const auto directory = scratch.path.generic_u8string();
    flow.nodes.front().bindings = {
        set("frame_origin", take(ConfigValue::point2d({model.coordinates.front().id, MeasurementUnit::Pixel, 0, 0}))),
        set("subscribe_folder", ConfigValue::boolean(true)), set("trigger_flow", ConfigValue::boolean(true)),
        set("folder_path", take(ConfigValue::string(std::string(directory.begin(), directory.end()))))};
    std::shared_ptr<WorkflowSession> session = take(WorkflowSession::create(registry));
    take(session->activate(model, take(FileResourceProvider::create(model, scratch.path))));
    const auto watch = take(ImageDirectoryTrigger::open(session, flow.id));
    check(!take(watch->poll()), "Arming a directory retains its initial contents as a baseline.");
    scratch.add("B.png");
    check(!take(watch->poll()), "An unstable new directory listing cannot start a run.");
    const auto change = finish(take(watch->poll()).value().submission);
    check(*output(change, "file_name").as_string() == "B.png", "A directory change delivers the new file, even when an older name sorts first.");
    check(!take(watch->poll()), "Repeated scans of the same file do not retransmit its image.");
    check(fs::remove(scratch.path / "B.png"), "Cannot remove the test input.");
    check(!take(watch->poll()), "A deletion batch also waits for a stable scan.");
    const auto deletion = finish(take(watch->poll()).value().submission);
    const auto* image = deletion.output(key<OutputId>("image"));
    check(image && !std::holds_alternative<DataValue>(image->content), "A deletion event must not republish the old file or fall back to A.");
    take(watch->control()->stop(TriggerStopMode::Drain, Clock::now() + 5s));
    scratch.add("C.png");
    check(!take(watch->poll()) && !take(watch->poll()), "Changes after stop cannot trigger a new run.");
}
}

int main()
{
    try {
        const auto registry = take(visionpro::app::make_tool_registry());
        migrate_default_image_source_name(registry);
        switch_source_with_bound_consumer(registry);
        sdk_delivery(registry);
        continuous_sequence(registry);
        directory_events(registry);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
