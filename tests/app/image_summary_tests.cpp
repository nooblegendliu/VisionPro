#include "run_summary.hpp"
#include "vision/vision_tools.hpp"

#include "../tools/image_fixtures.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
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
    const auto started = engine->submit(request);
    if (const auto* rejected = std::get_if<RunRejected>(&started)) { throw std::runtime_error(rejected->diagnostic.message); }
    const auto& run = std::get<RunHandle>(started);
    const auto finished = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    if (finished.outcome != RunWaitOutcome::Satisfied || finished.progress.status != ExecutionStatus::Succeeded) {
        const auto details = visionpro::app::diagnostics_json(finished.progress.diagnostics);
        throw std::runtime_error("The image and threshold workflow did not finish successfully: " + details.dump());
    }
    const auto report = take(visionpro::app::summarize_run(run, finished.progress, {}));
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
} // namespace

int main()
{
    try {
        image_summary();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
