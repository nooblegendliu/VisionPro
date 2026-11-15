#include "simulation.hpp"
#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr std::size_t width = 12;
constexpr std::size_t height = 10;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr auto pixels = MeasurementUnit::Pixel;
using Samples = std::array<std::uint8_t, width * height>;

void check(bool passed, std::string_view message)
{
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

std::string error_text(const Diagnostic& diagnostic)
{ return std::string(error_code_name(diagnostic.code)) + ": " + diagnostic.message; }

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error_text(*failure)); }
    return std::get<T>(std::move(result));
}

template<UuidIdentity T> T id(unsigned suffix = 1)
{
    constexpr std::string_view hex = "0123456789abcdef";
    std::string text = "00000000-0000-4000-8000-000000000000";
    text[text.size() - 2] = hex.at(suffix >> 4);
    text.back() = hex.at(suffix & 0x0f);
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view name)
{ return take(SchemaIdFactory::parse<T>(name)); }

Samples rectangles(std::uint8_t background, std::uint8_t foreground)
{
    Samples plane;
    plane.fill(background);
    for (std::size_t y = 1; y <= 2; ++y) { std::fill_n(plane.data() + y * width + 1, 3, foreground); }
    for (std::size_t y = 5; y <= 7; ++y) { std::fill_n(plane.data() + y * width + 7, 3, foreground); }
    return plane;
}

class ImageFile final {
public:
    explicit ImageFile(const Samples& samples)
        : directory(fs::current_path() / ("vision-integration-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        check(fs::create_directory(directory), "The image fixture directory can be reserved.");
        try {
            write(samples);
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~ImageFile() { cleanup(); }

    const fs::path directory;
    const fs::path name{u8"固定 图像.bmp"};

private:
    void cleanup() const
    {
        std::error_code ignored;
        fs::remove(directory / name, ignored);
        fs::remove(directory, ignored);
    }

    void write(const Samples& samples) const
    {
        constexpr std::size_t row_bytes = width * 3;
        static_assert(row_bytes % 4 == 0);
        std::array<std::uint8_t, 54> header{};
        const auto write_le = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                header[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
            }
        };
        header[0] = 'B';
        header[1] = 'M';
        write_le(2, static_cast<std::uint32_t>(header.size() + row_bytes * height));
        write_le(10, static_cast<std::uint32_t>(header.size()));
        write_le(14, 40); // BITMAPINFOHEADER
        write_le(18, width);
        write_le(22, height);
        header[26] = 1;   // One plane, 24-bit BGR.
        header[28] = 24;
        write_le(34, row_bytes * height);

        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(header.data()), header.size());
        for (std::size_t y = height; y != 0; --y) {
            std::array<std::uint8_t, row_bytes> row{};
            for (std::size_t x = 0; x < width; ++x) {
                std::fill_n(row.data() + x * 3, 3, samples[(y - 1) * width + x]);
            }
            file.write(reinterpret_cast<const char*>(row.data()), row.size());
        }
        file.close();
        check(file.good(), "The fixed V02 BMP file is written.");
    }
};

ToolRegistry registry()
{
    auto registrations = visionpro::tools::vision::registrations();
    for (auto& registration : visionpro::tools::simulation_registrations()) {
        registrations.push_back(std::move(registration));
    }
    return take(ToolRegistry::create(registrations));
}

Node node(NodeId id, std::string_view tool, std::vector<Binding> bindings)
{ return {id, std::string(tool), ToolConfiguration{key<ToolTypeId>(tool), 1}, std::move(bindings), {}}; }

Binding parameter(std::string_view name, ConfigValue value)
{ return {key<ParameterId>(name), {std::move(value)}}; }

OutputReference output(NodeId producer, std::string_view port)
{ return {id<WorkflowId>(), producer, key<OutputId>(port), {}}; }

Binding input(std::string_view name, OutputReference value)
{ return {key<InputId>(name), {std::move(value)}}; }

ConfigValue enumeration(const ToolRegistry& tools, std::string_view name, std::string_view parameter, std::string_view member)
{
    const auto* tool = tools.find_descriptor(key<ToolTypeId>(name));
    check(tool, "The real tool is registered.");
    const auto declared = std::ranges::find(tool->parameters, key<ParameterId>(parameter), &ParameterDescriptor::id);
    check(declared != tool->parameters.end(), "The enum is defined by the registered tool.");
    return take(ConfigValue::enumeration(declared->type, key<EnumMemberId>(member)));
}

void connect(Workflow& flow, NodeId source, NodeId target)
{
    const auto link = id<ControlLinkId>(static_cast<unsigned>(flow.control_links.size() + 1));
    flow.control_links.push_back({link, source, {}, target});
}

struct LaneSpec {
    std::int64_t cutoff;
    std::int64_t components;
    std::int64_t required_count;
    bool passed;
};

struct Lane {
    LaneSpec expected;
    NodeId threshold;
    NodeId blob;
    NodeId judge;
};

Lane add_lane(Workflow& flow, const ToolRegistry& tools, NodeId reader, const CoordinateSystemId& frame,
    const LaneSpec& spec, unsigned first_id)
{
    const Lane lane{spec, id<NodeId>(first_id), id<NodeId>(first_id + 1), id<NodeId>(first_id + 2)};
    constexpr std::string_view threshold = "visionpro.image.threshold";
    const RegionDefinition full{id<RegionId>(first_id), frame, pixels, RegionPrimitive{frame, pixels, FullImageRegion{}}, {}};
    flow.nodes.push_back(node(lane.threshold, threshold,
        {input("image", output(reader, "image")), parameter("roi", take(ConfigValue::region2d(full))),
            parameter("threshold", ConfigValue::integer(spec.cutoff)),
            parameter("mode", enumeration(tools, threshold, "mode", "Binary"))}));
    flow.nodes.push_back(node(lane.blob, "visionpro.image.blob",
        {input("binary", output(lane.threshold, "binary")), parameter("connectivity", ConfigValue::integer(8))}));
    flow.nodes.push_back(node(lane.judge, "visionpro.judge.integer_range",
        {input("value", output(lane.blob, "count")), parameter("minimum", ConfigValue::integer(spec.required_count)),
            parameter("maximum", ConfigValue::integer(spec.required_count))}));
    connect(flow, reader, lane.threshold);
    connect(flow, lane.threshold, lane.blob);
    connect(flow, lane.blob, lane.judge);
    return lane;
}

struct Delivery {
    NodeId collect{id<NodeId>(30)};
    NodeId deliver{id<NodeId>(31)};
};

void add_delivery(Workflow& flow, const std::vector<Lane>& lanes, const Delivery& delivery)
{
    Binding collected{key<InputId>("values"), {}, BindingCardinality::MultipleSources};
    for (const auto& lane : lanes) {
        collected.sources.emplace_back(output(lane.blob, "count"));
        connect(flow, lane.judge, delivery.collect);
    }
    auto collect = node(delivery.collect, "visionpro.sim.collect", {std::move(collected)});
    collect.join = JoinPolicy::AllActivated;
    flow.nodes.push_back(std::move(collect));
    flow.nodes.push_back(node(delivery.deliver, "visionpro.sim.deliver", {input("values", output(delivery.collect, "values"))}));
    connect(flow, delivery.collect, delivery.deliver);
}

RunHandle execute(const ToolRegistry& tools, const Solution& model, const ImageFile& fixture)
{
    const auto provider = take(FileResourceProvider::create(model, fixture.directory));
    const auto engine = take(WorkflowEngine::create(tools, provider));
    const auto snapshot_id = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
    const auto snapshot = take(ConfigSnapshot::capture(model, tools.descriptors(), {engine->session(), snapshot_id}));
    const auto request = take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>()}, {},
        {TriggerKind::Manual, key<SchemaId>("s46.vision_integration")}));
    auto submission = engine->submit(request);
    if (const auto* rejection = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(error_text(rejection->diagnostic)); }
    check(std::holds_alternative<RunHandle>(submission), "The workflow is admitted.");
    auto run = std::get<RunHandle>(std::move(submission));
    const auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 10s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent, "The workflow reaches quiescence.");
    return run;
}

const RunNodeProgress& called(const RunProgress& progress, const NodeId& id)
{
    const auto call = std::ranges::find(progress.nodes, id, &RunNodeProgress::node);
    check(call != progress.nodes.end() && call->invocation && call->status == ExecutionStatus::Succeeded
        && std::ranges::count(progress.call_order, id) == 1, "Each expected node completes exactly one call.");
    return *call;
}

NodeResult result(const RunHandle& run, const RunNodeProgress& call)
{
    const auto published = take(run.lookup(*call.invocation));
    check(published && published->data().identity.run == run.identity() && published->data().status == ExecutionStatus::Succeeded,
        "The exact invocation publishes a result in this run.");
    return *published;
}

const DataValue& produced(const NodeResult& call, std::string_view port)
{
    const auto* slot = call.output(key<OutputId>(port));
    const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(value, "The declared output is available.");
    return *value;
}

const std::vector<SourceTrace>& sources(const RunNodeProgress& call, std::string_view name)
{
    const auto binding = std::ranges::find(call.bindings, BindingTarget{key<InputId>(name)}, &RunBindingTrace::target);
    check(binding != call.bindings.end(), "The input binding is traced.");
    return binding->sources;
}

void expect_source(const SourceTrace& source, std::size_t index, const NodeResult& producer, std::string_view port)
{
    const auto& identity = producer.data().identity;
    const auto* trace = std::get_if<ResultTrace>(&source.origin);
    check(trace && source.source_index == index && !source.accepted_absence
        && trace->reference == output(identity.invocation.node, port) && trace->run == identity.run
        && trace->scope == identity.invocation.scope && trace->invocation == identity.invocation.invocation
        && trace->attempt == identity.attempt, "The binding traces the ordered source to the exact producer and attempt.");
}

void expect_input(const RunNodeProgress& consumer, std::string_view name, const NodeResult& producer, std::string_view port)
{
    const auto& binding = sources(consumer, name);
    check(binding.size() == 1, "The input has one source.");
    expect_source(binding.front(), 0, producer, port);
}

ImageHandle expect_image(const NodeResult& call, std::string_view port, const CoordinateSystemId& frame, const Samples& samples)
{
    const auto* image = produced(call, port).as_image();
    check(image && image->image.format() == mono8 && image->image.width() == width && image->image.height() == height
        && image->image.coordinate_frame() == frame && image->image.root_frame() == frame
        && image->image.origin_x() == 0 && image->image.origin_y() == 0 && !image->frame && !image->captured_at,
        "The image uses the declared Mono8 layout, camera frame and origin.");
    check(std::ranges::equal(image->image.copy_pixels(), std::as_bytes(std::span(samples))), "All effective image pixels match.");
    return image->image;
}

const DataValue& field(const DataValue& record, std::string_view name)
{
    const auto* value = record.field(key<FieldId>(name));
    check(value, "The component record includes the declared field.");
    return *value;
}

void expect_component(const DataValue& component, const CoordinateSystemId& frame,
    std::int64_t area, double x, double y, double left, double top, std::int64_t rows)
{
    check(field(component, "area_pixels") == DataValue::integer(area)
        && field(component, "centroid") == take(DataValue::point2d({frame, pixels, x, y})), "Component area and centroid match.");
    const auto& bounds = field(component, "bounds");
    check(field(bounds, "origin") == take(DataValue::point2d({frame, pixels, left, top}))
        && field(bounds, "width") == DataValue::integer(3) && field(bounds, "height") == DataValue::integer(rows),
        "Component bounds match.");
}

void expect_blobs(const NodeResult& blob, const CoordinateSystemId& frame, std::int64_t count)
{
    check(produced(blob, "count") == DataValue::integer(count), "The lane has the expected component count.");
    const auto* components = produced(blob, "components").elements();
    check(components && components->size() == static_cast<std::size_t>(count), "The component list has the expected size.");
    const bool empty = count == 0;
    check(produced(blob, "total_area_pixels") == DataValue::integer(empty ? 0 : 15), "The lane has the expected total area.");
    const auto expected_largest = empty ? DataValue::none(DataType::integer()) : DataValue::some(DataValue::integer(9));
    check(produced(blob, "largest_area_pixels") == expected_largest, "The lane has the expected optional maximum area.");
    if (!empty) {
        expect_component(components->at(0), frame, 6, 2, 1.5, 1, 1, 2);
        expect_component(components->at(1), frame, 9, 8, 6, 7, 5, 3);
    }
}

void expect_delivery(const RunHandle& run, const RunProgress& progress, const std::vector<Lane>& lanes,
    const Delivery& delivery)
{
    const auto& collect = called(progress, delivery.collect);
    const auto& deliver = called(progress, delivery.deliver);
    const auto collect_result = result(run, collect);
    const auto deliver_result = result(run, deliver);
    const auto& collected = sources(collect, "values");
    check(collected.size() == lanes.size(), "AllActivated records every Blob producer.");
    const auto collect_position = std::ranges::find(progress.call_order, delivery.collect);
    std::vector<DataValue> values;
    for (std::size_t index = 0; index < lanes.size(); ++index) {
        const auto& lane = lanes[index];
        check(std::ranges::find(progress.call_order, lane.judge) < collect_position, "Collection waits for every judgement.");
        expect_source(collected[index], index, result(run, called(progress, lane.blob)), "count");
        values.push_back(DataValue::integer(lane.expected.components));
    }
    const auto expected = take(DataValue::list(DataType::integer(), values));
    check(produced(collect_result, "values") == expected && produced(deliver_result, "values") == expected
        && progress.call_order.back() == delivery.deliver, "Collection and delivery preserve [2, 2, 2, 0, 0] in binding order.");
    expect_input(deliver, "values", collect_result, "values");
}

void pipeline(std::span<const LaneSpec> specs, bool join_lanes)
{
    const auto original = rectangles(10, 200);
    const ImageFile fixture(original);
    const auto tools = registry();
    const auto frame = key<CoordinateSystemId>("s46.camera");
    const auto reader_id = id<NodeId>();
    constexpr std::string_view read_file = "visionpro.image.read_file";
    auto reader = node(reader_id, read_file, {parameter("decode_mode", enumeration(tools, read_file, "decode_mode", "Gray8")),
        parameter("frame_origin", take(ConfigValue::point2d({frame, pixels, 0, 0})))});
    const ResourceReference reference{id<ResourceId>(), ResourceKind::Image, {}};
    const auto source_slot = key<SchemaId>("source");
    reader.resources.push_back({source_slot, reference});
    Workflow flow{id<WorkflowId>(), "Vision integration", {key<SchemaId>("s46.vision_integration"), 1},
        {std::move(reader)}, {}, {}, {reader_id}};
    std::vector<Lane> lanes;
    unsigned first_lane_id = 2;
    for (const auto& spec : specs) {
        lanes.push_back(add_lane(flow, tools, reader_id, frame, spec, first_lane_id));
        first_lane_id += 3;
    }
    const Delivery delivery;
    if (join_lanes) { add_delivery(flow, lanes, delivery); }
    std::ranges::reverse(flow.nodes); // The editable order is independent of control and source order.

    const auto name = fixture.name.generic_u8string();
    const ResourceDefinition resource{reference.id, "Fixed V02 BMP", reference.kind,
        ExternalFileResource{{PathBase::SolutionDirectory, {name.begin(), name.end()}, {}}, ResourceReadPolicy::PerInvocation, {}, {}}};
    const CoordinateFrameDefinition frame_definition{frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded};
    const Solution model{id<SolutionId>(), "Vision workflow integration", 1, 1, {flow}, {resource}, {}, {}, {frame_definition}, {}};

    const auto run = execute(tools, model, fixture);
    const auto progress = take(run.inspect());
    if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
    check(progress.status == ExecutionStatus::Succeeded && progress.call_order.size() == flow.nodes.size()
        && progress.call_order.front() == reader_id, "The full workflow succeeds with the reader dispatched first.");
    check(progress.control_links.size() == flow.control_links.size()
        && std::ranges::all_of(progress.control_links, [](const auto& link) {
            return link.status == ControlLinkStatus::Arrived && !link.close_reason;
        }), "Every unconditional control edge arrives, including the edges from NG nodes.");

    const auto& read = called(progress, reader_id);
    check(read.resources.size() == 1 && read.resources.front().slot == source_slot
        && read.resources.front().reference == reference, "The source is traced to the acquired BMP lease.");
    const auto input_result = result(run, read);
    const auto source_image = expect_image(input_result, "image", frame, original);
    std::vector<ImageHandle> sibling_images;
    for (const auto& lane : lanes) {
        const auto& threshold = called(progress, lane.threshold);
        const auto& blob = called(progress, lane.blob);
        const auto& judge = called(progress, lane.judge);
        const auto threshold_result = result(run, threshold);
        const auto blob_result = result(run, blob);
        const auto judge_result = result(run, judge);
        expect_input(threshold, "image", input_result, "image");
        expect_input(blob, "binary", threshold_result, "binary");
        expect_input(judge, "value", blob_result, "count");
        const auto expected_pixels = lane.expected.components == 2 ? rectangles(0, 255) : Samples{};
        const auto image = expect_image(threshold_result, "binary", frame, expected_pixels);
        check(!source_image.shares_storage_with(image)
            && std::ranges::none_of(sibling_images, [&](const ImageHandle& prior) { return prior.shares_storage_with(image); }),
            "Every threshold result retains its own storage after all branches have run.");
        sibling_images.push_back(image);
        expect_blobs(blob_result, frame, lane.expected.components);
        check(judge_result.data().judgement == (lane.expected.passed ? Judgement::OK : Judgement::NG)
            && produced(judge_result, "passed") == DataValue::boolean(lane.expected.passed), "The real judgement matches the expected OK or NG.");
    }
    if (join_lanes) {
        expect_delivery(run, progress, lanes, delivery);
    } else {
        check(progress.call_order == std::vector{reader_id, lanes.front().threshold, lanes.front().blob, lanes.front().judge},
            "The single chain dispatches exactly the four real tools in data order.");
    }
}
} // namespace

int main(int argc, char** argv)
try {
    const std::string_view selection = argc == 2 ? argv[1] : "";
    if (selection == "single") {
        constexpr std::array single{LaneSpec{128, 2, 3, false}};
        pipeline(single, false);
    } else if (selection == "five") {
        constexpr LaneSpec five[]{{100, 2, 2, true}, {128, 2, 2, true}, {180, 2, 2, true}, {210, 0, 2, false}, {250, 0, 2, false}};
        pipeline(five, true);
    } else {
        throw std::runtime_error("Select single or five.");
    }
    std::cout << "The real " << selection << "-lane vision workflow passed.\n";
    return 0;
} catch (const std::exception& failure) {
    std::cerr << failure.what() << '\n';
    return 1;
}
