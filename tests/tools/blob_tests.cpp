#include "image_fixtures.hpp"
#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr auto pixels = MeasurementUnit::Pixel;
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};

void check(bool condition, std::string_view message)
{
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

std::string error_text(const Diagnostic& diagnostic)
{ return std::string(error_code_name(diagnostic.code)) + ": " + diagnostic.message; }

template<class T> T take(Result<T> result)
{
    if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error_text(*diagnostic)); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity T> T key(std::string_view spelling)
{ return take(SchemaIdFactory::parse<T>(spelling)); }

template<UuidIdentity T> T id(char suffix = '1')
{
    std::string text{"00000000-0000-4000-8000-000000000000"};
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

CoordinateSystemId camera() { return key<CoordinateSystemId>("s45.camera"); }

CoordinateFrameDefinition coordinates(const CoordinateSystemId& frame)
{ return {frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}; }

ImageFrame image(ImageLayout layout, std::span<const unsigned char> bytes)
{ return {take(ImageHandle::copy(layout, std::as_bytes(bytes), camera())), {}, {}}; }

RegionDefinition region(RegionShape shape, const CoordinateSystemId& frame = camera())
{ return {id<RegionId>(), frame, pixels, RegionPrimitive{frame, pixels, std::move(shape)}, {}}; }

ScopedInputReference public_image() { return {id<WorkflowId>(), {}, key<InputId>("image"), {}}; }

OutputReference source_output(char node, std::string_view output)
{ return {id<WorkflowId>(), id<NodeId>(node), key<OutputId>(output), {}}; }

Node blob(char node, std::int64_t connectivity, BindingSource input = public_image(),
    std::optional<RegionDefinition> roi = {})
{
    Node result{id<NodeId>(node), "Blob", ToolConfiguration{key<ToolTypeId>("visionpro.image.blob"), 1},
        {{key<InputId>("binary"), {std::move(input)}},
            {key<ParameterId>("connectivity"), {ConfigValue::integer(connectivity)}}}, {}};
    if (roi) {
        result.bindings.push_back({key<ParameterId>("roi"), {ConfigValue::some(take(ConfigValue::region2d(*roi)))}});
    }
    return result;
}

Solution solution(const ImageFrame& input, std::vector<Node> nodes, std::vector<ControlLink> links = {},
    std::vector<NodeId> entries = {})
{
    InputDescriptor source{key<InputId>("image"), "Image", DataType::image()};
    source.binding.allowed_sources = {BindingSourceKind::PublicInput};
    if (entries.empty()) { entries.push_back(nodes.front().id); }
    Workflow flow{id<WorkflowId>(), "Real blob analysis", {key<SchemaId>("s45.blob"), 1, {std::move(source)}},
        std::move(nodes), std::move(links), {}, std::move(entries)};
    Solution result{id<SolutionId>(), "Blob fixture", 1, 1, {std::move(flow)}, {}, {}, {},
        {coordinates(input.image.coordinate_frame())}, {}};
    if (input.image.root_frame() != input.image.coordinate_frame()) {
        result.coordinates.push_back(coordinates(input.image.root_frame()));
    }
    return result;
}

struct Fixture {
    const ToolRegistry registry = take(ToolRegistry::create(visionpro::tools::vision::registrations()));

    Node threshold(char node, const RegionDefinition& roi, std::int64_t cutoff) const
    {
        const auto type = key<ToolTypeId>("visionpro.image.threshold");
        const auto* descriptor = registry.find_descriptor(type);
        check(descriptor, "The production threshold tool is registered.");
        const auto mode = key<ParameterId>("mode");
        const auto declaration = std::ranges::find(descriptor->parameters, mode, &ParameterDescriptor::id);
        check(declaration != descriptor->parameters.end(), "Threshold declares its mode enum.");
        return {id<NodeId>(node), "Threshold", ToolConfiguration{type, 1}, {{key<InputId>("image"), {public_image()}},
            {key<ParameterId>("roi"), {take(ConfigValue::region2d(roi))}},
            {key<ParameterId>("threshold"), {ConfigValue::integer(cutoff)}},
            {mode, {take(ConfigValue::enumeration(declaration->type, key<EnumMemberId>("Binary")))}}}, {}};
    }

    RunHandle run(const Solution& model, const ImageFrame& input, std::shared_ptr<InvocationResourceProvider> files = {}) const
    {
        const auto engine = take(WorkflowEngine::create(registry, std::move(files)));
        const auto snapshot_id = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
        const auto snapshot = take(ConfigSnapshot::capture(model, registry.descriptors(), {engine->session(), snapshot_id}));
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>()},
            {{key<InputId>("image"), take(DataValue::image(input))}}, {TriggerKind::Manual, key<SchemaId>("s45.blob")}));
        auto submission = engine->submit(request);
        if (const auto* rejection = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error(error_text(rejection->diagnostic));
        }
        check(std::holds_alternative<RunHandle>(submission), "The blob workflow is admitted.");
        auto run = std::get<RunHandle>(std::move(submission));
        const auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
        check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent, "The blob workflow reaches quiescence.");
        return run;
    }
};

RunProgress successful(const RunHandle& run)
{
    auto progress = take(run.inspect());
    if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
    check(progress.status == ExecutionStatus::Succeeded, "The blob workflow succeeds.");
    return progress;
}

const RunNodeProgress& progress_of(const RunProgress& progress, char node)
{
    const auto found = std::ranges::find(progress.nodes, id<NodeId>(node), &RunNodeProgress::node);
    check(found != progress.nodes.end() && found->invocation.has_value(), "The expected node was called.");
    return *found;
}

NodeResult result_of(const RunHandle& run, const RunNodeProgress& node)
{
    const auto result = take(run.lookup(*node.invocation));
    check(result && result->data().identity.run == run.identity(), "The node result belongs to this run.");
    return *result;
}

const DataValue& produced(const NodeResult& result, std::string_view name)
{
    const auto* slot = result.output(key<OutputId>(name));
    const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(value, "The expected node output was produced.");
    return *value;
}

const DataValue& field(const DataValue& record, std::string_view name)
{
    const auto* value = record.field(key<FieldId>(name));
    check(value, "The expected Record field is present.");
    return *value;
}

std::int64_t integer(const DataValue& value)
{
    const auto* result = value.as_int64();
    check(result, "The result is an Int64.");
    return *result;
}

const Point2D& point(const DataValue& value, const CoordinateSystemId& frame, double x, double y)
{
    const auto* result = value.as_point2d();
    check(result && result->coordinate_frame == frame && result->unit == pixels, "The point uses the input Pixel frame.");
    check(std::abs(result->x - x) <= 1e-9 && std::abs(result->y - y) <= 1e-9, "The point has the expected coordinates.");
    return *result;
}

struct Component {
    std::int64_t area;
    double centroid_x;
    double centroid_y;
    double left;
    double top;
    std::int64_t width;
    std::int64_t height;
};

void expect_blobs(const NodeResult& result, const CoordinateSystemId& frame, std::initializer_list<Component> expected)
{
    check(result.data().status == ExecutionStatus::Succeeded && result.data().judgement == Judgement::NotEvaluated
        && result.data().outputs.size() == 4, "Blob analysis publishes four outputs without a detection judgement.");
    check(integer(produced(result, "count")) == static_cast<std::int64_t>(expected.size()), "Component count matches.");
    const auto& components = produced(result, "components");
    const auto* entries = components.elements();
    const auto* component_type = components.type().element_type();
    check(entries && entries->size() == expected.size() && component_type && component_type->kind() == TypeKind::Record
        && component_type->schema_id() == key<SchemaId>("visionpro.image.blob.component")
        && component_type->schema_version() == 1, "Components form a List of the declared Record, including when empty.");

    std::int64_t total = 0;
    std::int64_t largest_area = 0;
    auto entry = entries->begin();
    for (const auto& component : expected) {
        check(integer(field(*entry, "area_pixels")) == component.area, "Component matches the expected scan order and area.");
        point(field(*entry, "centroid"), frame, component.centroid_x, component.centroid_y);
        const auto& bounds = field(*entry, "bounds");
        check(bounds.type().schema_id() == key<SchemaId>("visionpro.image.blob.bounds") && bounds.type().schema_version() == 1,
            "Bounding boxes use the declared nested Record.");
        point(field(bounds, "origin"), frame, component.left, component.top);
        check(integer(field(bounds, "width")) == component.width && integer(field(bounds, "height")) == component.height,
            "Bounding-box extent matches the occupied pixels.");
        total += component.area;
        largest_area = std::max(largest_area, component.area);
        ++entry;
    }
    check(integer(produced(result, "total_area_pixels")) == total, "Total component area matches.");
    const auto& largest = produced(result, "largest_area_pixels");
    check(largest.type() == DataType::optional(DataType::integer()), "Largest area is an Optional Int64.");
    const auto* area = largest.optional_value();
    check(expected.size() == 0 ? area == nullptr : area && integer(*area) == largest_area, "Largest area matches the components.");
}

NodeResult measure(const Fixture& fixture, const ImageFrame& input, std::int64_t connectivity,
    std::optional<RegionDefinition> roi = {})
{
    const auto run = fixture.run(solution(input, {blob('1', connectivity, public_image(), std::move(roi))}), input);
    return result_of(run, progress_of(successful(run), '1'));
}

void connectivity_and_area(const Fixture& fixture)
{
    constexpr unsigned char diagonal[]{1, 0, 0, 0, 17, 0, 0, 0, 0};
    const auto input = image({3, 3, 3, mono8}, diagonal);
    expect_blobs(measure(fixture, input, 4), camera(), {{1, 0, 0, 0, 0, 1, 1}, {1, 1, 1, 1, 1, 1, 1}});
    expect_blobs(measure(fixture, input, 8), camera(), {{2, 0.5, 0.5, 0, 0, 2, 2}});

    constexpr unsigned char single[]{7};
    expect_blobs(measure(fixture, image({1, 1, 1, mono8}, single), 4), camera(), {{1, 0, 0, 0, 0, 1, 1}});
    constexpr unsigned char ring[]{1, 1, 1, 1, 0, 1, 1, 1, 1};
    expect_blobs(measure(fixture, image({3, 3, 3, mono8}, ring), 8), camera(), {{8, 1, 1, 0, 0, 3, 3}});
    constexpr unsigned char background[]{0, 0, 0, 0};
    expect_blobs(measure(fixture, image({2, 2, 2, mono8}, background), 8), camera(), {});
}

void component_order(const Fixture& fixture)
{
    // Both touch the top row. The right component reaches farther left on the last row.
    constexpr unsigned char samples[]{0, 10, 0, 0, 20, 0,
        0, 10, 0, 0, 20, 0,
        0, 0, 0, 0, 20, 0,
        20, 20, 20, 20, 20, 0};
    const auto input = image({6, 4, 6, mono8}, samples);
    expect_blobs(measure(fixture, input, 4), camera(), {{2, 1, 0.5, 1, 0, 1, 2}, {8, 2.75, 2.25, 0, 0, 5, 4}});
}

void cropped_components(const Fixture& fixture)
{
    constexpr unsigned char padded[]{1, 2, 3, 4, 99, 98, 11, 12, 13, 14, 97, 96, 21, 22, 23, 24};
    constexpr unsigned char root_pixels[]{1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24};
    constexpr unsigned char cropped_pixels[]{12, 13, 14, 22, 23, 24};
    const auto root = image({4, 3, 6, mono8}, padded);
    const auto frame = key<CoordinateSystemId>("s45.crop");
    const ImageFrame crop{take(root.image.crop({1, 1, 3, 2}, frame)), {}, {}};
    check(crop.image.row_stride() == 6, "The cropped view retains the padded input step.");
    const auto run = fixture.run(solution(crop,
        {fixture.threshold('1', region(FullImageRegion{}, frame), 13), blob('2', 8, source_output('1', "binary"))},
        {{id<ControlLinkId>(), id<NodeId>(), {}, id<NodeId>('2')}}), crop);
    const auto progress = successful(run);
    const auto threshold = result_of(run, progress_of(progress, '1'));
    const auto* binary = produced(threshold, "binary").as_image();
    constexpr unsigned char binary_pixels[]{0, 0, 255, 255, 255, 255};
    check(binary && binary->image.root_frame() == camera() && binary->image.origin_x() == 1 && binary->image.origin_y() == 1
        && std::ranges::equal(binary->image.copy_pixels(), std::as_bytes(std::span{binary_pixels})),
        "Threshold publishes the expected image and its crop origin in the root frame.");
    const auto result = result_of(run, progress_of(progress, '2'));
    expect_blobs(result, frame, {{4, 1.25, 0.75, 0, 0, 3, 2}});
    const auto& center = point(field(produced(result, "components").elements()->front(), "centroid"), frame, 1.25, 0.75);
    check(std::abs(center.x + static_cast<double>(binary->image.origin_x()) - 2.25) <= 1e-9
        && std::abs(center.y + static_cast<double>(binary->image.origin_y()) - 1.75) <= 1e-9,
        "The local centroid projects to the expected root position.");
    check(std::ranges::equal(root.image.copy_pixels(), std::as_bytes(std::span{root_pixels}))
        && std::ranges::equal(crop.image.copy_pixels(), std::as_bytes(std::span{cropped_pixels})),
        "The processing chain keeps the source image and crop pixels unchanged.");
}

void inherited_roi(const Fixture& fixture)
{
    std::array<unsigned char, 81> foreground;
    foreground.fill(255);
    const auto input = image({9, 9, 9, mono8}, foreground);
    const auto frame = camera();
    const auto full = region(FullImageRegion{});
    const Transform2D shift{frame, frame, pixels, pixels, TransformModel::Affine, {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    const InheritedRegion reference{id<WorkflowId>(), id<NodeId>('2'), key<OutputId>("effective_roi"), frame, pixels, 0, shift};
    const RegionDefinition roi{id<RegionId>('3'), frame, pixels, reference,
        {RegionPrimitive{frame, pixels, AxisAlignedRect{5, 4, 1, 1}}}};
    const auto binary = source_output('1', "binary");

    for (bool upstream_exclusion : {false, true}) {
        auto rotated = region(RotatedRect{4, 4, 4, 2, std::numbers::pi / 4});
        rotated.id = id<RegionId>('2');
        if (upstream_exclusion) { rotated.exclusions.push_back(RegionPrimitive{frame, pixels, AxisAlignedRect{4, 5, 1, 1}}); }
        const auto run = fixture.run(solution(input,
            {fixture.threshold('1', full, 128), fixture.threshold('2', rotated, 128), blob('3', 8, binary, roi)},
            {{id<ControlLinkId>(), id<NodeId>(), {}, id<NodeId>('3')},
                {id<ControlLinkId>('2'), id<NodeId>('2'), {}, id<NodeId>('3')}},
            {id<NodeId>(), id<NodeId>('2')}), input);
        const auto progress = successful(run);
        check(progress.call_order.size() == 3 && progress.call_order.back() == id<NodeId>('3'),
            "The Blob consumer runs after both threshold producers.");
        const auto& node = progress_of(progress, '3');
        const auto result = result_of(run, node);
        expect_blobs(result, frame, {{upstream_exclusion ? 5 : 6, 5, upstream_exclusion ? 3.8 : 4.0, 4, 3, 3, 3}});

        const auto region_call = result_of(run, progress_of(progress, '2'));
        const auto* published = produced(region_call, "effective_roi").as_region2d();
        const auto region_trace = std::ranges::find(node.bindings, BindingTarget{key<ParameterId>("roi")}, &RunBindingTrace::target);
        check(published && published->exclusions == rotated.exclusions && region_trace != node.bindings.end()
            && region_trace->regions.size() == 1, "Blob resolves the published Region2D including the producer's exclusions.");
        const auto& frozen = region_trace->regions.front();
        check(frozen.reference == reference && frozen.value == *published && frozen.identity == region_call.data().identity
            && frozen.path == std::vector<DiagnosticPathStep>{key<FieldId>("source")}, "The ROI traces the exact frozen producer call.");

        const auto binary_call = result_of(run, progress_of(progress, '1'));
        const auto image_trace = std::ranges::find(node.bindings, BindingTarget{key<InputId>("binary")}, &RunBindingTrace::target);
        check(image_trace != node.bindings.end() && image_trace->sources.size() == 1, "The binary image source is traced.");
        const auto* source = std::get_if<ResultTrace>(&image_trace->sources.front().origin);
        check(source && source->reference == binary && source->run == run.identity()
            && source->scope == binary_call.data().identity.invocation.scope
            && source->invocation == binary_call.data().identity.invocation.invocation
            && source->attempt == binary_call.data().identity.attempt, "The image comes from the distinct full-image threshold call.");
    }
}

void expect_roi_error(const Fixture& fixture, const ImageFrame& input, const RegionDefinition& roi, ErrorCode code)
{
    const auto run = fixture.run(solution(input, {blob('1', 4, public_image(), roi)}), input);
    const auto progress = take(run.inspect());
    const auto diagnostic = std::ranges::find(progress.diagnostics, code, &Diagnostic::code);
    check(progress.status == ExecutionStatus::Failed && diagnostic != progress.diagnostics.end()
        && diagnostic->location.node == id<NodeId>() && diagnostic->location.parameter == key<ParameterId>("roi")
        && diagnostic->location.region == roi.id, "ROI failures identify the Blob node, parameter and region.");
    const auto result = result_of(run, progress_of(progress, '1'));
    check(std::ranges::none_of(result.data().outputs, [](const OutputSlot& output) {
        return std::holds_alternative<DataValue>(output.content);
    }), "A failed Blob call publishes no valid outputs.");
}

void roi_boundary_and_empty(const Fixture& fixture)
{
    constexpr unsigned char foreground[]{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    const auto input = image({4, 3, 4, mono8}, foreground);
    auto clipped = region(AxisAlignedRect{-1, 0, 3, 2});
    expect_roi_error(fixture, input, clipped, ErrorCode::ImageBounds);
    clipped.boundary_policy = RegionBoundaryPolicy::Intersect;
    expect_blobs(measure(fixture, input, 4, clipped), camera(), {{4, 0.5, 0.5, 0, 0, 2, 2}});

    auto empty = region(FullImageRegion{});
    empty.exclusions.push_back(RegionPrimitive{camera(), pixels, FullImageRegion{}});
    expect_roi_error(fixture, input, empty, ErrorCode::EmptyRegion);
    empty.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    expect_blobs(measure(fixture, input, 4, empty), camera(), {});
}

struct MaskFile {
    const fs::path directory = fs::current_path();
    const fs::path name = fs::path{u8"Blob 掩膜 "} += take(IdFactory::generate<MessageId>()).to_string() + ".png";
    ~MaskFile()
    {
        std::error_code ignored;
        fs::remove(directory / name, ignored);
    }
};

void file_mask(const Fixture& fixture)
{
    const MaskFile file;
    {
        const auto& bytes = visionpro::test_fixtures::png;
        std::ofstream stream(file.directory / file.name, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes), sizeof bytes);
        stream.close();
        check(stream.good(), "The PNG mask fixture is written.");
    }
    constexpr unsigned char foreground[]{1, 2, 64, 127};
    const auto input = image({2, 2, 2, mono8}, foreground);
    const ResourceReference reference{id<ResourceId>(), ResourceKind::Mask, {}};
    const auto slot = key<SchemaId>("mask");
    const auto frame = camera();
    const MaskConfig mask{reference, frame, 2, 2, mono8, {frame, 2, 2, {}, MaskSampling::None},
        {MaskValueMode::AtLeastThreshold, 129}};
    auto node = blob('1', 4, public_image(), region(mask));
    node.resources.push_back({slot, reference});
    auto model = solution(input, {std::move(node)});
    const auto name = file.name.generic_u8string();
    model.resources.push_back({reference.id, "Blob mask", ResourceKind::Mask, ExternalFileResource{
        {PathBase::SolutionDirectory, {name.begin(), name.end()}, {}}, ResourceReadPolicy::PerInvocation, {}, {}}});
    const auto files = take(FileResourceProvider::create(model, file.directory));
    const auto run = fixture.run(model, input, files);
    const auto progress = successful(run);
    const auto& call = progress_of(progress, '1');
    check(call.resources.size() == 1 && call.resources.front().slot == slot && call.resources.front().reference == reference,
        "The Blob call uses the fixed PNG mask resource.");
    expect_blobs(result_of(run, call), frame, {{2, 0.5, 1, 0, 1, 2, 1}});
    check(std::ranges::equal(input.image.copy_pixels(), std::as_bytes(std::span{foreground})), "Masking preserves the input pixels.");
}
} // namespace

int main()
{
    try {
        const Fixture fixture;
        connectivity_and_area(fixture);
        component_order(fixture);
        cropped_components(fixture);
        inherited_roi(fixture);
        roi_boundary_and_empty(fixture);
        file_mask(fixture);
        std::cout << "Real Blob connectivity, geometry, ordering, ROI and mask checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
