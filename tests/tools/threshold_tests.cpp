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
#include <filesystem>
#include <fstream>
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

template<SchemaIdentity T> T key(std::string_view spelling) { return take(SchemaIdFactory::parse<T>(spelling)); }

template<UuidIdentity T> T id(char suffix = '1')
{
    std::string value{"00000000-0000-4000-8000-000000000000"};
    value.back() = suffix;
    return take(IdFactory::parse<T>(value));
}

CoordinateSystemId camera() { return key<CoordinateSystemId>("s44.camera"); }

CoordinateFrameDefinition coordinates(const CoordinateSystemId& frame)
{ return {frame, CoordinateKind::Image, pixels, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}; }

ImageFrame image(ImageLayout layout, std::span<const unsigned char> bytes)
{ return {take(ImageHandle::copy(layout, std::as_bytes(bytes), camera())), {}, {}}; }

RegionDefinition region(const CoordinateSystemId& frame, RegionShape shape, char suffix = '1')
{ return {id<RegionId>(suffix), frame, pixels, RegionPrimitive{frame, pixels, std::move(shape)}, {}}; }

struct Fixture {
    const ToolRegistry registry = take(ToolRegistry::create(visionpro::tools::vision::registrations()));
    const ToolTypeId type = key<ToolTypeId>("visionpro.image.threshold");

    Node node(const RegionDefinition& roi, std::int64_t level, std::string_view mode, char suffix = '1') const
    {
        const auto* descriptor = registry.find_descriptor(type);
        check(descriptor, "The production threshold tool is registered.");
        const auto mode_id = key<ParameterId>("mode");
        const auto declaration = std::ranges::find(descriptor->parameters, mode_id, &ParameterDescriptor::id);
        check(declaration != descriptor->parameters.end(), "Threshold declares its mode enum.");
        const auto image_id = key<InputId>("image");
        return {id<NodeId>(suffix), "Threshold", ToolConfiguration{type, 1},
            {{image_id, {ScopedInputReference{id<WorkflowId>(), {}, image_id, {}}}},
                {key<ParameterId>("roi"), {take(ConfigValue::region2d(roi))}},
                {key<ParameterId>("threshold"), {ConfigValue::integer(level)}},
                {mode_id, {take(ConfigValue::enumeration(declaration->type, key<EnumMemberId>(mode)))}}}, {}};
    }

    Solution solution(const ImageFrame& input, std::vector<Node> nodes, std::vector<ControlLink> links = {}) const
    {
        InputDescriptor source{key<InputId>("image"), "Image", DataType::image()};
        source.binding.allowed_sources = {BindingSourceKind::PublicInput};
        const auto entry = nodes.front().id;
        Workflow flow{id<WorkflowId>(), "Real threshold", {key<SchemaId>("s44.test"), 1, {std::move(source)}, {}, {}},
            std::move(nodes), std::move(links), {}, {entry}};
        Solution model{id<SolutionId>(), "Threshold fixture", 1, 1, {std::move(flow)}, {}, {}, {},
            {coordinates(input.image.coordinate_frame())}, {}};
        if (input.image.root_frame() != input.image.coordinate_frame()) { model.coordinates.push_back(coordinates(input.image.root_frame())); }
        return model;
    }

    RunSubmission execute(const Solution& model, const ImageFrame& input,
        std::shared_ptr<InvocationResourceProvider> files = {}) const
    {
        const auto engine = take(WorkflowEngine::create(registry, std::move(files)));
        const auto snapshot_id = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
        const auto snapshot = take(ConfigSnapshot::capture(model, registry.descriptors(), {engine->session(), snapshot_id}));
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>()},
            {{key<InputId>("image"), take(DataValue::image(input))}}, {TriggerKind::Manual, key<SchemaId>("s44.test")}));
        auto submission = engine->submit(request);
        if (const auto* run = std::get_if<RunHandle>(&submission)) {
            const auto waited = take(run->wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
            check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent, "Threshold workflow reaches quiescence.");
        }
        return submission;
    }
};

RunHandle accepted(RunSubmission submission)
{
    if (const auto* rejection = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(error_text(rejection->diagnostic)); }
    check(std::holds_alternative<RunHandle>(submission), "Threshold workflow is admitted.");
    return std::get<RunHandle>(std::move(submission));
}

const RunNodeProgress& progress_of(const RunProgress& progress, char suffix)
{
    const auto found = std::ranges::find(progress.nodes, id<NodeId>(suffix), &RunNodeProgress::node);
    check(found != progress.nodes.end(), "The requested threshold node is in the run.");
    return *found;
}

NodeResult result_of(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "A dispatched threshold has an invocation identity.");
    const auto found = take(run.lookup(*node.invocation));
    check(found.has_value() && found->data().identity.run == run.identity(), "The result belongs to this invocation and run.");
    return *found;
}

void expect_code(const RunProgress& progress, ErrorCode code, NodeId node, const auto& identity)
{
    const auto found = std::ranges::find(progress.diagnostics, code, &Diagnostic::code);
    if (found == progress.diagnostics.end()) {
        throw std::runtime_error("Expected " + std::string(error_code_name(code)) + ", got "
            + (progress.diagnostics.empty() ? "no diagnostic" : error_text(progress.diagnostics.front())));
    }
    check(found->location.node == node && identity(found->location), "The error identifies the node and the invalid input.");
}

const DataValue& produced(const NodeResult& result, std::string_view name)
{
    const auto* slot = result.output(key<OutputId>(name));
    const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
    check(value, "All three threshold outputs are available on success.");
    return *value;
}

void expect_pixels(const ImageHandle& image, std::span<const unsigned char> expected)
{ check(std::ranges::equal(image.copy_pixels(), std::as_bytes(expected)), "Image pixels match the fixed expectation."); }

const RegionDefinition& verify_outputs(const NodeResult& result, const ImageFrame& input, RegionId region_id,
    std::span<const unsigned char> binary_pixels, std::span<const unsigned char> mask_pixels)
{
    if (!result.data().diagnostics.empty()) { throw std::runtime_error(error_text(result.data().diagnostics.front())); }
    check(result.data().status == ExecutionStatus::Succeeded && result.data().outputs.size() == 3,
        "Threshold publishes its three required outputs as one successful result.");
    const auto* binary = produced(result, "binary").as_image();
    const auto* effective = produced(result, "effective_roi").as_region2d();
    const auto* mask = produced(result, "roi_mask").as_raster_mask();
    check(binary && effective && mask, "Threshold outputs are typed as Image, Region2D and RasterMask.");
    const auto& source = input.image;
    for (const auto* derived : {&binary->image, &mask->pixels}) {
        check(derived->width() == source.width() && derived->height() == source.height() && derived->format() == mono8,
            "Both output images keep the input dimensions and have Mono8 format.");
        check(derived->coordinate_frame() == source.coordinate_frame() && derived->root_frame() == source.root_frame()
            && derived->origin_x() == source.origin_x() && derived->origin_y() == source.origin_y(),
            "Both output images keep the full input coordinate mapping.");
        check(!derived->shares_storage_with(source), "Derived pixels have independent storage.");
    }
    check(binary->frame == input.frame && binary->captured_at == input.captured_at, "The binary frame retains acquisition metadata.");
    take(validate_raster_mask(*mask, source));
    check(mask->mapping.sampling == MaskSampling::None && !mask->mapping.transform && mask->valid_pixels == MaskValueRule{},
        "The output mask directly addresses target pixels using NonZero.");
    check(effective->id == region_id && effective->coordinate_frame == source.coordinate_frame() && effective->unit == pixels
        && std::holds_alternative<RegionPrimitive>(effective->source)
        && std::ranges::all_of(effective->exclusions, [](const RegionSource& item) { return std::holds_alternative<RegionPrimitive>(item); }),
        "The effective ROI owns the configured identity and a resolved symbolic expression.");
    expect_pixels(binary->image, binary_pixels);
    expect_pixels(mask->pixels, mask_pixels);
    return *effective;
}

void single_result(const Fixture& fixture, const ImageFrame& input, const RegionDefinition& roi, std::int64_t level,
    std::string_view mode, std::span<const unsigned char> binary, std::span<const unsigned char> mask)
{
    const auto run = accepted(fixture.execute(fixture.solution(input, {fixture.node(roi, level, mode)}), input));
    const auto progress = take(run.inspect());
    if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
    const auto result = result_of(run, progress_of(progress, '1'));
    check(progress.status == ExecutionStatus::Succeeded && verify_outputs(result, input, roi.id, binary, mask) == roi,
        "Local ROI geometry and policies remain explicit in the successful output.");
}

void threshold_edges(const Fixture& fixture)
{
    constexpr unsigned char source[]{0, 127, 128, 129, 255};
    const auto input = image({5, 1, 5, mono8}, source);
    const auto roi = region(camera(), FullImageRegion{});
    constexpr unsigned char full[]{255, 255, 255, 255, 255};
    struct Case { std::int64_t threshold; std::array<unsigned char, 5> normal, inverse; };
    constexpr Case cases[]{
        {0, {0, 255, 255, 255, 255}, {255, 0, 0, 0, 0}},
        {128, {0, 0, 0, 255, 255}, {255, 255, 255, 0, 0}},
        {255, {0, 0, 0, 0, 0}, {255, 255, 255, 255, 255}},
    };
    for (const auto& test : cases) {
        single_result(fixture, input, roi, test.threshold, "Binary", test.normal, full);
        single_result(fixture, input, roi, test.threshold, "BinaryInverse", test.inverse, full);
    }

    const auto clipped = region(camera(), AxisAlignedRect{1, 0, 3, 1});
    constexpr unsigned char normal[]{0, 0, 0, 255, 0};
    constexpr unsigned char inverse[]{0, 255, 255, 0, 0};
    constexpr unsigned char inside[]{0, 255, 255, 255, 0};
    single_result(fixture, input, clipped, 128, "Binary", normal, inside);
    single_result(fixture, input, clipped, 128, "BinaryInverse", inverse, inside);
    single_result(fixture, input, clipped, 255, "BinaryInverse", inside, inside);
    expect_pixels(input.image, source);
}

void cropped_source(const Fixture& fixture)
{
    constexpr unsigned char padded[]{1, 2, 3, 4, 99, 98, 11, 12, 13, 14, 97, 96, 21, 22, 23, 24};
    const auto source = image({4, 3, 6, mono8}, padded);
    const auto crop_frame = key<CoordinateSystemId>("s44.crop");
    const ImageFrame cropped{take(source.image.crop({1, 1, 3, 2}, crop_frame)), FrameId{id<ResourceId>(), 3},
        std::chrono::sys_time<std::chrono::nanoseconds>{20ns}};
    check(cropped.image.row_stride() == 6, "The input crop preserves its parent stride.");
    constexpr unsigned char binary[]{0, 0, 255, 255, 255, 255};
    constexpr unsigned char mask[]{255, 255, 255, 255, 255, 255};
    single_result(fixture, cropped, region(crop_frame, FullImageRegion{}), 13, "Binary", binary, mask);
    constexpr unsigned char intact_crop[]{12, 13, 14, 22, 23, 24};
    constexpr unsigned char intact_root[]{1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24};
    expect_pixels(cropped.image, intact_crop);
    expect_pixels(source.image, intact_root);
}

void rejected_inputs_and_empty_roi(const Fixture& fixture)
{
    constexpr unsigned char source[]{127, 128};
    const auto input = image({2, 1, 2, mono8}, source);
    auto roi = region(camera(), FullImageRegion{});
    const auto level = key<ParameterId>("threshold");
    for (std::int64_t invalid : {-1, 256}) {
        const auto rejected = fixture.execute(fixture.solution(input, {fixture.node(roi, invalid, "Binary")}), input);
        const auto* refusal = std::get_if<RunRejected>(&rejected);
        check(refusal && refusal->diagnostic.code == ErrorCode::ConstraintViolation
            && refusal->diagnostic.location.parameter == level, "Out-of-range thresholds are rejected at admission with parameter identity.");
    }

    constexpr unsigned char color[]{10, 20, 30};
    constexpr unsigned char depth[]{0, 1};
    const auto bgr = image({1, 1, 3, {PixelDepth::UInt8, ChannelOrder::BGR}}, color);
    const auto mono16 = image({1, 1, 2, {PixelDepth::UInt16, ChannelOrder::Mono}}, depth);
    for (const auto* unsupported : {&bgr, &mono16}) {
        const auto run = accepted(fixture.execute(fixture.solution(*unsupported, {fixture.node(roi, 128, "Binary")}), *unsupported));
        const auto progress = take(run.inspect());
        check(progress.status == ExecutionStatus::Failed && progress.call_order.empty(), "Unsupported input formats fail before tool dispatch.");
        expect_code(progress, ErrorCode::UnsupportedPixelFormat, id<NodeId>(), [](const DiagnosticLocation& location) {
            return location.input == key<InputId>("image");
        });
    }

    roi.exclusions.push_back(RegionPrimitive{camera(), pixels, FullImageRegion{}});
    const auto run = accepted(fixture.execute(fixture.solution(input, {fixture.node(roi, 128, "BinaryInverse")}), input));
    const auto progress = take(run.inspect());
    check(progress.status == ExecutionStatus::Failed && progress.call_order.size() == 1, "An empty required ROI fails during evaluation.");
    expect_code(progress, ErrorCode::EmptyRegion, id<NodeId>(), [&](const DiagnosticLocation& location) { return location.region == roi.id; });
    const auto failed = result_of(run, progress_of(progress, '1'));
    check(std::ranges::none_of(failed.data().outputs, [](const OutputSlot& slot) { return std::holds_alternative<DataValue>(slot.content); }),
        "A failed threshold does not publish partial produced outputs.");

    roi.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    constexpr unsigned char empty[]{0, 0};
    single_result(fixture, input, roi, 128, "BinaryInverse", empty, empty);
}

using Site = std::array<std::size_t, 2>;

std::array<unsigned char, 81> selected_pixels(std::initializer_list<Site> selected)
{
    std::array<unsigned char, 81> output{};
    for (const auto& [x, y] : selected) { output[y * 9 + x] = 255; }
    return output;
}

void frozen_inheritance(const Fixture& fixture)
{
    std::array<unsigned char, 81> all{};
    all.fill(255);
    const auto input = image({9, 9, 9, mono8}, all);
    const auto source = region(camera(), RotatedRect{4, 4, 4, 2, std::numbers::pi / 4});
    const Transform2D shift{camera(), camera(), pixels, pixels, TransformModel::Affine, {1, 0, 1, 0, 1, 0, 0, 0, 1}};
    const InheritedRegion reference{id<WorkflowId>(), id<NodeId>(), key<OutputId>("effective_roi"), camera(), pixels, 0, shift};
    const RegionDefinition destination{id<RegionId>('2'), camera(), pixels, reference,
        {RegionPrimitive{camera(), pixels, AxisAlignedRect{5, 4, 1, 1}}}};
    auto model = fixture.solution(input, {fixture.node(source, 128, "Binary"), fixture.node(destination, 128, "Binary", '2')},
        {{id<ControlLinkId>(), id<NodeId>(), {}, id<NodeId>('2')}});
    const auto run = accepted(fixture.execute(model, input));
    const auto progress = take(run.inspect());
    if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
    check(progress.status == ExecutionStatus::Succeeded && progress.call_order == std::vector{id<NodeId>(), id<NodeId>('2')},
        "The real producer publishes before the real consumer evaluates its ROI.");
    const auto first = result_of(run, progress_of(progress, '1'));
    const auto& consumer = progress_of(progress, '2');
    const auto second = result_of(run, consumer);
    const auto seven = selected_pixels({{3, 3}, {4, 3}, {3, 4}, {4, 4}, {5, 4}, {4, 5}, {5, 5}});
    const auto six = selected_pixels({{4, 3}, {5, 3}, {4, 4}, {6, 4}, {5, 5}, {6, 5}});
    const auto& upstream_roi = verify_outputs(first, input, source.id, seven, seven);
    const auto& inherited_roi = verify_outputs(second, input, destination.id, six, six);
    check(inherited_roi.exclusions == destination.exclusions && std::get<RegionPrimitive>(inherited_roi.source).transform == shift,
        "The downstream Region2D preserves the inherited transform and the local exclusion.");
    const auto trace = std::ranges::find(consumer.bindings, BindingTarget{key<ParameterId>("roi")}, &RunBindingTrace::target);
    check(trace != consumer.bindings.end() && trace->regions.size() == 1 && trace->regions.front().reference == reference
        && trace->regions.front().identity == first.data().identity && trace->regions.front().value == upstream_roi,
        "The consumer uses the frozen Region2D output from the real producer invocation.");

    const auto wrong_frame = key<CoordinateSystemId>("s44.other");
    auto wrong_reference = reference;
    wrong_reference.coordinate_frame = wrong_frame;
    wrong_reference.transform->from = wrong_frame;
    auto wrong_region = destination;
    wrong_region.source = wrong_reference;
    model.coordinates.push_back(coordinates(wrong_frame));
    model.workflows.front().nodes.back() = fixture.node(wrong_region, 128, "Binary", '2');
    const auto failed = accepted(fixture.execute(model, input));
    const auto failed_progress = take(failed.inspect());
    check(failed_progress.status == ExecutionStatus::Failed && failed_progress.call_order == std::vector{id<NodeId>()}
        && result_of(failed, progress_of(failed_progress, '1')).data().status == ExecutionStatus::Succeeded,
        "The actual upstream frame mismatch fails before the consumer runs.");
    expect_code(failed_progress, ErrorCode::CoordinateMismatch, id<NodeId>('2'), [&](const DiagnosticLocation& location) {
        return location.parameter == key<ParameterId>("roi") && location.region == wrong_region.id
            && location.path == std::vector<DiagnosticPathStep>{key<FieldId>("source")};
    });
    expect_pixels(input.image, all);
}

struct MaskFile {
    const fs::path directory = fs::current_path();
    const fs::path name = fs::path{u8"掩膜 "} += take(IdFactory::generate<MessageId>()).to_string() + ".png";
    ~MaskFile()
    {
        std::error_code ignored;
        fs::remove(directory / name, ignored);
    }
};

void fixed_mask(const Fixture& fixture)
{
    const MaskFile file;
    {
        const auto& png = visionpro::test_fixtures::png;
        std::ofstream output(file.directory / file.name, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(png), sizeof png);
        output.close();
        check(output.good(), "The fixed PNG mask file is written.");
    }
    constexpr unsigned char source[]{255, 0, 128, 129};
    const auto input = image({2, 2, 2, mono8}, source);
    const ResourceReference reference{id<ResourceId>(), ResourceKind::Mask, {}};
    const MaskConfig mask{reference, camera(), 2, 2, mono8, {camera(), 2, 2, {}, MaskSampling::None},
        {MaskValueMode::AtLeastThreshold, 129}};
    const auto roi = region(camera(), mask);
    auto node = fixture.node(roi, 128, "BinaryInverse");
    const auto slot = key<SchemaId>("mask");
    node.resources.push_back({slot, reference});
    auto model = fixture.solution(input, {std::move(node)});
    const auto path = file.name.generic_u8string();
    model.resources.push_back({reference.id, "Mask PNG", ResourceKind::Mask, ExternalFileResource{
        {PathBase::SolutionDirectory, {path.begin(), path.end()}, {}}, ResourceReadPolicy::PerInvocation, {}, {}}});

    const auto files = take(FileResourceProvider::create(model, file.directory));
    const auto run = accepted(fixture.execute(model, input, files));
    const auto progress = take(run.inspect());
    if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
    const auto& call = progress_of(progress, '1');
    check(progress.status == ExecutionStatus::Succeeded && call.resources.size() == 1
        && call.resources.front().slot == slot && call.resources.front().reference == reference,
        "Threshold uses the real file lease recorded in the run.");
    constexpr unsigned char binary[]{0, 0, 255, 0};
    constexpr unsigned char members[]{0, 0, 255, 255};
    const auto result = result_of(run, call);
    check(verify_outputs(result, input, roi.id, binary, members) == roi, "The effective ROI retains the fixed mask configuration.");
}
} // namespace

int main()
{
    try {
        const Fixture fixture;
        threshold_edges(fixture);
        cropped_source(fixture);
        rejected_inputs_and_empty_roi(fixture);
        frozen_inheritance(fixture);
        fixed_mask(fixture);
        std::cout << "Real threshold workflow pixel, ROI, ownership and failure checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
