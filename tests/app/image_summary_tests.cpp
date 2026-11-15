#include "run_summary.hpp"
#include "vision/vision_tools.hpp"

#include "../tools/image_fixtures.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using nlohmann::json;
namespace fs = std::filesystem;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity Id> Id key(std::string_view value) { return take(SchemaIdFactory::parse<Id>(value)); }
template<UuidIdentity Id> Id id() { return take(IdFactory::generate<Id>()); }

class ScratchDirectory final {
public:
    ScratchDirectory() : path(fs::current_path() / ("image-summary-" + id<MessageId>().to_string()))
    {
        if (!fs::create_directory(path)) { throw std::runtime_error("Could not reserve the image fixture directory."); }
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    const fs::path path;
};

ConfigValue enum_value(const ToolRegistry& registry, const ToolTypeId& tool, const ParameterId& parameter,
    std::string_view member)
{
    const auto* descriptor = registry.find_descriptor(tool);
    if (!descriptor) { throw std::runtime_error("Missing registered tool: " + std::string(tool.text())); }
    const auto declaration = std::ranges::find(descriptor->parameters, parameter, &ParameterDescriptor::id);
    if (declaration == descriptor->parameters.end()) {
        throw std::runtime_error("Missing enumerated tool parameter: " + std::string(parameter.text()));
    }
    return take(ConfigValue::enumeration(declaration->type, key<EnumMemberId>(member)));
}

const json& find_entry(const json& entries, const char* field, std::string_view expected)
{
    const auto found = std::ranges::find_if(entries, [&](const json& entry) {
        return entry.at(field).get_ref<const std::string&>() == expected;
    });
    if (found == entries.end()) { throw std::runtime_error("Missing summary entry: " + std::string(expected)); }
    return *found;
}

const json& produced_value(const json& node, std::string_view output)
{
    const auto& summary = find_entry(node.at("outputs"), "id", output);
    if (summary.at("status") != "Produced") { throw std::runtime_error("Unexpected output summary: " + summary.dump()); }
    return summary.at("value");
}

json execute_summary(WorkflowEngine& engine, const RunRequest& request)
{
    const auto started = engine.submit(request);
    if (const auto* rejected = std::get_if<RunRejected>(&started)) { throw std::runtime_error(rejected->diagnostic.message); }
    const auto& run = std::get<RunHandle>(started);
    const auto finished = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    if (finished.outcome != RunWaitOutcome::Satisfied || !finished.progress.quiescent) {
        throw std::runtime_error("The workflow did not reach quiescence.");
    }
    return take(visionpro::app::summarize_run(run, finished.progress, {}));
}

void image_summary()
{
    const ScratchDirectory fixture;
    const auto& bmp = visionpro::test_fixtures::bmp;
    std::ofstream image_file(fixture.path / "input.bmp", std::ios::binary);
    image_file.write(reinterpret_cast<const char*>(bmp), sizeof(bmp));
    image_file.close();
    if (!image_file) { throw std::runtime_error("Could not write the BMP fixture."); }

    const auto registry = take(ToolRegistry::create(visionpro::tools::vision::registrations()));
    const auto reader_tool = key<ToolTypeId>("visionpro.image.read_file");
    const auto threshold_tool = key<ToolTypeId>("visionpro.image.threshold");
    const auto decode_mode = key<ParameterId>("decode_mode");
    const auto threshold_mode = key<ParameterId>("mode");
    const auto frame = key<CoordinateSystemId>("camera");
    constexpr auto pixels = MeasurementUnit::Pixel;
    const auto origin = take(ConfigValue::point2d({frame, pixels, 0, 0}));
    const auto input = id<ResourceId>();
    const auto flow_id = id<WorkflowId>();
    const auto read = [&](std::string_view mode) {
        return Node{id<NodeId>(), std::string(mode), ToolConfiguration{reader_tool, 1},
            {{decode_mode, {enum_value(registry, reader_tool, decode_mode, mode)}}, {key<ParameterId>("frame_origin"), {origin}}},
            {{key<SchemaId>("source"), {input, ResourceKind::Image, {}}}}};
    };
    const auto native = read("Native8");
    const auto gray = read("Gray8");
    const auto threshold = [&](std::string name, const RegionDefinition& region) {
        return Node{id<NodeId>(), std::move(name), ToolConfiguration{threshold_tool, 1},
            {{key<InputId>("image"), {OutputReference{flow_id, gray.id, key<OutputId>("image"), {}}}},
                {key<ParameterId>("roi"), {take(ConfigValue::region2d(region))}},
                {key<ParameterId>("threshold"), {ConfigValue::integer(128)}},
                {threshold_mode, {enum_value(registry, threshold_tool, threshold_mode, "Binary")}}}, {}};
    };
    const RegionDefinition original{id<RegionId>(), frame, pixels,
        RegionPrimitive{frame, pixels, FullImageRegion{}}, {RegionPrimitive{frame, pixels, AxisAlignedRect{0, 0, 1, 1}}},
        RegionBoundaryPolicy::Intersect};
    const auto first = threshold("Threshold", original);
    const Transform2D shift{frame, frame, pixels, pixels, TransformModel::Affine, {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    const RegionDefinition inherited{id<RegionId>(), frame, pixels,
        InheritedRegion{flow_id, first.id, key<OutputId>("effective_roi"), frame, pixels, 0.25, shift}, {},
        RegionBoundaryPolicy::Intersect};
    const auto second = threshold("Inherited ROI threshold", inherited);
    const Workflow flow{flow_id, "Image and threshold summary", {key<SchemaId>("test.image_summary"), 1, {}, {}, {}},
        {native, gray, first, second}, {{id<ControlLinkId>(), gray.id, {}, first.id}, {id<ControlLinkId>(), first.id, {}, second.id}},
        {}, {native.id, gray.id}, {}};
    const ResourceDefinition resource{input, "BMP", ResourceKind::Image,
        ExternalFileResource{{PathBase::SolutionDirectory, "input.bmp", {}}, ResourceReadPolicy::PerInvocation, {}, {}}};
    const CoordinateFrameDefinition coordinates{frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded};
    const Solution solution{id<SolutionId>(), "Image summary", 1, 1, {flow}, {resource}, {}, {}, {coordinates}, {}};

    const auto provider = take(FileResourceProvider::create(solution, fixture.path));
    const auto engine = take(WorkflowEngine::create(registry, provider));
    const auto revision = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), revision}));
    const auto request = take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, {},
        {TriggerKind::Manual, key<SchemaId>("test.image_summary")}));
    const auto report = execute_summary(*engine, request);
    if (report.at("status") != "Succeeded") {
        throw std::runtime_error("The image and threshold workflow failed: " + report.at("diagnostics").dump());
    }
    const auto node = [&](const NodeId& id) -> const json& { return find_entry(report.at("nodes"), "id", id.to_string()); };
    const json expected_native{{"width", 2}, {"height", 2}, {"depth", "UInt8"}, {"channel_order", "BGR"},
        {"current_frame", "camera"}, {"root_frame", "camera"}, {"origin", {{"x", 0}, {"y", 0}}},
        {"frame", nullptr}, {"captured_at_unix_ns", nullptr}};
    if (produced_value(node(native.id), "image") != expected_native) {
        throw std::runtime_error("Unexpected Native8 image summary.");
    }
    auto expected_binary = expected_native;
    expected_binary["channel_order"] = "Mono";
    if (produced_value(node(gray.id), "image") != expected_binary
        || produced_value(node(first.id), "binary") != expected_binary || produced_value(node(second.id), "binary") != expected_binary) {
        throw std::runtime_error("Unexpected Gray8 or binary image summary.");
    }

    auto expected_pixels = expected_binary;
    expected_pixels.erase("frame");
    expected_pixels.erase("captured_at_unix_ns");
    json expected_mask{{"pixels", expected_pixels}, {"mapping", {{"target_frame", "camera"}, {"target_width", 2},
        {"target_height", 2}, {"sampling", "None"}, {"transform", nullptr}}}, {"valid_pixels", {{"mode", "NonZero"}, {"threshold", 0}}},
        {"member_count", 3}};
    if (produced_value(node(first.id), "roi_mask") != expected_mask) { throw std::runtime_error("Unexpected ROI mask summary."); }
    expected_mask["member_count"] = 1;
    if (produced_value(node(second.id), "roi_mask") != expected_mask) {
        throw std::runtime_error("The inherited mask summary must reflect the translated pixels and exclusion.");
    }

    const auto& effective = produced_value(node(second.id), "effective_roi");
    const auto& source = effective.at("source");
    const auto& exclusions = effective.at("exclusions");
    if (effective.at("id") != inherited.id.to_string() || effective.at("coordinate_frame") != "camera"
        || effective.at("unit") != "Pixel" || effective.at("boundary_policy") != "Intersect" || effective.at("empty_policy") != "Error"
        || source.at("kind") != "primitive" || source.at("geometry").at("kind") != "FullImage"
        || source.at("local_direction_radians") != 0.25 || source.at("transform").at("matrix") != json(shift.matrix)
        || exclusions.size() != 1 || exclusions.at(0).at("geometry").at("kind") != "AxisAlignedRect"
        || exclusions.at(0).at("transform").at("matrix") != json(shift.matrix)) {
        throw std::runtime_error("Unexpected effective ROI summary: " + effective.dump());
    }

    const auto& binding = find_entry(node(second.id).at("bindings"), "target", "roi");
    const auto& frozen = binding.at("regions").at(0);
    const auto& producer = frozen.at("producer");
    if (binding.at("regions").size() != 1 || frozen.at("source_index") != 0
        || frozen.at("path") != json::array({{{"field", "source"}}}) || frozen.at("region") != original.id.to_string()
        || frozen.at("reference").at("transform").at("matrix") != json(shift.matrix)
        || producer.at("node") != first.id.to_string() || producer.at("invocation") != node(first.id).at("invocation")
        || producer.at("run") != report.at("run") || producer.at("output") != "effective_roi") {
        throw std::runtime_error("Unexpected frozen ROI source summary: " + frozen.dump());
    }
}

void blob_judgement_summary()
{
    const auto registry = take(ToolRegistry::create(visionpro::tools::vision::registrations()));
    const auto flow_id = id<WorkflowId>();
    const auto input = key<InputId>("image");
    const auto frame = key<CoordinateSystemId>("camera");
    constexpr auto pixels = MeasurementUnit::Pixel;
    const auto threshold_tool = key<ToolTypeId>("visionpro.image.threshold");
    const auto mode = key<ParameterId>("mode");
    const RegionDefinition full{id<RegionId>(), frame, pixels, RegionPrimitive{frame, pixels, FullImageRegion{}}, {}};
    const Node threshold{id<NodeId>(), "Threshold", ToolConfiguration{threshold_tool, 1},
        {{input, {ScopedInputReference{flow_id, {}, input, {}}}},
            {key<ParameterId>("roi"), {take(ConfigValue::region2d(full))}},
            {key<ParameterId>("threshold"), {ConfigValue::integer(128)}},
            {mode, {enum_value(registry, threshold_tool, mode, "Binary")}}}, {}};
    const Node blob{id<NodeId>(), "Measure components", ToolConfiguration{key<ToolTypeId>("visionpro.image.blob"), 1},
        {{key<InputId>("binary"), {OutputReference{flow_id, threshold.id, key<OutputId>("binary"), {}}}},
            {key<ParameterId>("connectivity"), {ConfigValue::integer(8)}}}, {}};
    const auto judge = [&](std::string name, OutputReference source, std::int64_t expected) {
        return Node{id<NodeId>(), std::move(name), ToolConfiguration{key<ToolTypeId>("visionpro.judge.integer_range"), 1},
            {{key<InputId>("value"), {std::move(source)}}, {key<ParameterId>("minimum"), {ConfigValue::integer(expected)}},
                {key<ParameterId>("maximum"), {ConfigValue::integer(expected)}}}, {}};
    };
    const OutputReference count{flow_id, blob.id, key<OutputId>("count"), {}};
    const auto two = judge("Expect two components", count, 2);
    const auto three = judge("Expect three components", count, 3);
    const auto first_area = judge("Expect first area six", {flow_id, blob.id, key<OutputId>("components"),
        {ElementSelection{0}, FieldSelection{key<FieldId>("area_pixels")}}}, 6);
    InputDescriptor source{input, "Image", DataType::image()};
    source.binding.allowed_sources = {BindingSourceKind::PublicInput};
    const Workflow flow{flow_id, "Blob and judgement summary", {key<SchemaId>("test.blob_summary"), 1, {source}, {}, {}},
        {threshold, blob, two, three, first_area},
        {{id<ControlLinkId>(), threshold.id, {}, blob.id}, {id<ControlLinkId>(), blob.id, {}, two.id},
            {id<ControlLinkId>(), two.id, {}, three.id}, {id<ControlLinkId>(), three.id, {}, first_area.id}},
        {}, {threshold.id}, {}};
    const CoordinateFrameDefinition coordinates{frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded};
    const Solution solution{id<SolutionId>(), "Blob summary", 1, 1, {flow}, {}, {}, {}, {coordinates}, {}};
    const auto engine = take(WorkflowEngine::create(registry));
    const auto revision = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), revision}));

    constexpr std::size_t width = 12, height = 10;
    std::array<unsigned char, width * height> samples;
    const auto submit = [&] {
        const auto image = take(ImageHandle::copy({width, height, width, {PixelDepth::UInt8, ChannelOrder::Mono}},
            std::as_bytes(std::span(samples)), frame));
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{flow_id},
            {{input, take(DataValue::image({image, {}, {}}))}}, {TriggerKind::Manual, key<SchemaId>("test.blob_summary")}));
        return execute_summary(*engine, request);
    };
    samples.fill(10);
    for (std::size_t y = 1; y <= 2; ++y) { std::fill_n(samples.begin() + y * width + 1, 3, 200); }
    for (std::size_t y = 5; y <= 7; ++y) { std::fill_n(samples.begin() + y * width + 7, 3, 200); }
    const auto report = submit();
    const auto node = [](const json& summary, const Node& node) -> const json& {
        return find_entry(summary.at("nodes"), "id", node.id.to_string());
    };
    const auto point = [](double x, double y) {
        return json{{"coordinate_frame", "camera"}, {"unit", "Pixel"}, {"x", x}, {"y", y}};
    };
    const json expected_components = json::array({
        {{"area_pixels", "6"}, {"centroid", point(2, 1.5)}, {"bounds", {{"origin", point(1, 1)}, {"width", "3"}, {"height", "2"}}}},
        {{"area_pixels", "9"}, {"centroid", point(8, 6)}, {"bounds", {{"origin", point(7, 5)}, {"width", "3"}, {"height", "3"}}}}
    });
    const auto& measured = node(report, blob);
    if (report.at("status") != "Succeeded" || measured.at("status") != "Succeeded"
        || produced_value(measured, "count") != "2" || produced_value(measured, "components") != expected_components
        || produced_value(measured, "total_area_pixels") != "15" || produced_value(measured, "largest_area_pixels") != "9") {
        throw std::runtime_error("Unexpected two-component results: " + measured.dump() + report.at("diagnostics").dump());
    }
    const auto check_judge = [&](const Node& target, bool passed) {
        const auto& summary = node(report, target);
        if (summary.at("status") != "Succeeded" || summary.at("judgement") != (passed ? "OK" : "NG")
            || produced_value(summary, "passed") != passed) {
            throw std::runtime_error("Unexpected integer range result: " + summary.dump());
        }
    };
    check_judge(two, true);
    check_judge(three, false);
    check_judge(first_area, true);
    const json expected_order = json::array({threshold.id.to_string(), blob.id.to_string(), two.id.to_string(),
        three.id.to_string(), first_area.id.to_string()});
    const auto& binding = find_entry(node(report, first_area).at("bindings"), "target", "value").at("sources").at(0);
    if (report.at("call_order") != expected_order || binding.at("node") != blob.id.to_string()
        || binding.at("invocation") != measured.at("invocation") || binding.at("run") != report.at("run")
        || binding.at("output") != "components" || binding.at("selector") != json::array({{{"index", 0}}, {{"field", "area_pixels"}}})) {
        throw std::runtime_error("NG continuation or the selected Blob field is incorrect: " + binding.dump());
    }

    samples.fill(10);
    const auto empty = submit();
    const auto& empty_blobs = node(empty, blob);
    const auto& blocked = node(empty, first_area);
    if (empty.at("status") != "Failed" || empty_blobs.at("status") != "Succeeded"
        || produced_value(empty_blobs, "count") != "0" || produced_value(empty_blobs, "components") != json::array()
        || produced_value(empty_blobs, "total_area_pixels") != "0" || !produced_value(empty_blobs, "largest_area_pixels").is_null()
        || blocked.at("status") != "Failed" || !blocked.at("invocation").is_null()) {
        throw std::runtime_error("Unexpected empty-component results: " + empty.at("diagnostics").dump());
    }
    const auto& location = find_entry(empty.at("diagnostics"), "code", "IndexOutOfRange").at("location");
    if (location.at("node") != first_area.id.to_string() || location.at("input") != "value"
        || location.at("path") != json::array({{{"index", 0}}})) {
        throw std::runtime_error("Wrong diagnostic location for the empty Blob list: " + location.dump());
    }
}
} // namespace

int main()
{
    try {
        image_summary();
        blob_judgement_summary();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
