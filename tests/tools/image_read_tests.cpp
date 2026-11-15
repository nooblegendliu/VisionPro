#include "image_fixtures.hpp"
#include "vision/vision_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace visionpro::test_fixtures;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace vision = visionpro::tools::vision;

// 2x2 grayscale PNG with 16-bit samples [0,128;129,65535].
constexpr unsigned char mono16_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x07, 0x4d, 0x8e, 0xbb, 0x00, 0x00, 0x00,
    0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0x68,
    0x60, 0x60, 0x68, 0xfc, 0xff, 0x1f, 0x00, 0x07, 0x8a, 0x03, 0x00, 0x21,
    0x30, 0x54, 0x09, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
    0x42, 0x60, 0x82,
};
// 2x2 truecolor PNG with an alpha channel.
constexpr unsigned char bgra_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
    0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0xf8, 0xef,
    0xc0, 0xf0, 0x9f, 0xa1, 0x01, 0x88, 0x19, 0x0e, 0xfc, 0x07, 0x02, 0x00,
    0x39, 0x25, 0x08, 0x7a, 0x1b, 0xe2, 0xff, 0x68, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};
// 2x2 grayscale PNG with EXIF orientation 6 (rotate 90 degrees clockwise).
constexpr unsigned char oriented_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x57, 0xdd, 0x52, 0xf8, 0x00, 0x00, 0x00,
    0x1a, 0x65, 0x58, 0x49, 0x66, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb7, 0x48, 0x11, 0x29, 0x00,
    0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xe0, 0x14,
    0x64, 0x50, 0x74, 0x04, 0x00, 0x00, 0xfa, 0x00, 0x7d, 0xcf, 0x8d, 0xc6,
    0x8e, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82,
};
constexpr unsigned char oriented_pixels[] = {9, 17, 33, 65};

void check(bool passed, std::string_view message)
{
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> T id()
{ return take(IdFactory::parse<T>("00000000-0000-4000-8000-000000000001")); }

template<class T> T key(std::string_view name)
{ return take(SchemaIdFactory::parse<T>(name)); }

struct ScratchDirectory {
    ScratchDirectory() : path(fs::current_path() / ("s42-image-" + take(IdFactory::generate<MessageId>()).to_string()))
    {
        check(fs::create_directory(path), "Could not create the image test directory.");
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    const fs::path path;
};

void write_file(const fs::path& path, std::span<const unsigned char> bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    check(static_cast<bool>(output), "Could not write the image fixture.");
}

// Replace the on-disk file before delivering the first real per-invocation lease.
class ReplacingFileProvider final : public InvocationResourceProvider {
public:
    ReplacingFileProvider(std::shared_ptr<FileResourceProvider> provider, fs::path path)
        : provider_(std::move(provider)), path_(std::move(path)) {}

    std::span<const ResourceKind> kinds() const noexcept override { return provider_->kinds(); }
    Result<std::monostate> preflight(const ExecutionPlan& plan) const override { return provider_->preflight(plan); }
    Result<std::shared_ptr<const ResourceLease>> acquire(
        const ToolExecutionConfiguration& configuration, const ToolResourceBinding& binding) override
    {
        auto captured = provider_->acquire(configuration, binding);
        if (replace_ && std::holds_alternative<std::shared_ptr<const ResourceLease>>(captured)) {
            write_file(path_, oriented_png);
            replace_ = false;
        }
        return captured;
    }
private:
    std::shared_ptr<FileResourceProvider> provider_;
    fs::path path_;
    bool replace_{true};
};

RunHandle accepted(RunSubmission submission)
{
    if (const auto* failure = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(failure->diagnostic.message); }
    check(std::holds_alternative<RunHandle>(submission), "Image workflow must be admitted.");
    return std::get<RunHandle>(std::move(submission));
}

RunProgress finished(const RunHandle& run)
{
    auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent && waited.progress.nodes.size() == 1,
        "The image workflow must report its one node and reach quiescence.");
    return std::move(waited.progress);
}

const Diagnostic& diagnostic(const RunProgress& progress, ErrorCode code)
{
    const auto found = std::ranges::find(progress.diagnostics, code, &Diagnostic::code);
    if (found == progress.diagnostics.end()) {
        const auto actual = progress.diagnostics.empty() ? std::string("no diagnostic") : progress.diagnostics.front().message;
        throw std::runtime_error("Expected " + std::string(error_code_name(code)) + ", got " + actual);
    }
    check(found->location.node == id<NodeId>(), "The image error identifies the read_file node.");
    return *found;
}

NodeResult result(const RunHandle& run, const RunNodeProgress& node)
{
    check(node.invocation.has_value(), "The started reader reports its invocation identity.");
    auto found = take(run.lookup(*node.invocation));
    check(found && found->data().identity.run == run.identity(), "The image result belongs to this run.");
    return *found;
}

struct Fixture {
    const ScratchDirectory scratch;
    const fs::path png_path{u8"图像 夹具/原始 灰度.png"};
    const fs::path bmp_path{u8"图像 夹具/颜色 样本.bmp"};
    const ToolRegistry registry = take(ToolRegistry::create(vision::registrations()));
    const CoordinateSystemId frame = key<CoordinateSystemId>("fixture.camera");
    const ResourceReference source{id<ResourceId>(), ResourceKind::Image, {}};

    Solution model(std::string_view mode, const fs::path& relative, double origin_x = 0) const
    {
        const auto reader = key<ToolTypeId>("visionpro.image.read_file");
        const auto decode_mode = key<ParameterId>("decode_mode");
        const auto* descriptor = registry.find_descriptor(reader);
        check(descriptor != nullptr, "The real image reader is registered.");
        const auto decode = std::ranges::find(descriptor->parameters, decode_mode, &ParameterDescriptor::id);
        check(decode != descriptor->parameters.end(), "The decoder mode is declared.");
        const Point2D origin{frame, MeasurementUnit::Pixel, origin_x, 0};
        const Node read{id<NodeId>(), "Read file", ToolConfiguration{reader, 1},
            {{decode_mode, {take(ConfigValue::enumeration(decode->type, key<EnumMemberId>(mode)))}},
                {key<ParameterId>("frame_origin"), {take(ConfigValue::point2d(origin))}}},
            {{key<SchemaId>("source"), source}}};
        const Workflow flow{id<WorkflowId>(), "Read image", {key<SchemaId>("s42.read"), 1, {}, {}, {}},
            {read}, {}, {}, {read.id}};
        const auto path = relative.generic_u8string();
        const ResourceDefinition file{source.id, "Input image", source.kind, ExternalFileResource{
            {PathBase::SolutionDirectory, {path.begin(), path.end()}, {}}, ResourceReadPolicy::PerInvocation, {}, {}}};
        const CoordinateFrameDefinition coordinate{frame, CoordinateKind::Image, MeasurementUnit::Pixel,
            {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded};
        return {id<SolutionId>(), "Image reader fixture", 1, 1, {flow}, {file}, {}, {}, {coordinate}, {}};
    }

    std::shared_ptr<FileResourceProvider> files(const Solution& model) const
    { return take(FileResourceProvider::create(model, scratch.path)); }

    RunRequest request(const WorkflowEngine& engine, const Solution& model) const
    {
        const auto snapshot_id = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
        const auto snapshot = take(ConfigSnapshot::capture(model, registry.descriptors(), {engine.session(), snapshot_id}));
        return take(RunRequest::create(snapshot, WorkflowTarget{id<WorkflowId>()}, {}, {TriggerKind::Manual, key<SchemaId>("s42.test")}));
    }

    RunHandle submit(const Solution& model) const
    {
        auto engine = take(WorkflowEngine::create(registry, files(model)));
        auto run = accepted(engine->submit(request(*engine, model)));
        // The engine drains accepted work before its destruction. The handle retains its result.
        return run;
    }

    ImageFrame published_image(const RunHandle& run) const
    {
        const auto progress = finished(run);
        if (!progress.diagnostics.empty()) { throw std::runtime_error(progress.diagnostics.front().message); }
        const auto& node = progress.nodes.front();
        check(progress.status == ExecutionStatus::Succeeded && node.status == ExecutionStatus::Succeeded,
            "The real read_file invocation succeeds.");
        check(node.resources.size() == 1 && node.resources.front().slot == key<SchemaId>("source")
            && node.resources.front().reference == source, "Execution traces the exact image resource lease.");
        const auto published = result(run, node);
        const auto* slot = published.output(key<OutputId>("image"));
        const auto* data = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
        const auto* image = data ? data->as_image() : nullptr;
        check(image && slot->type == DataType::image(), "The reader publishes a typed Image output.");
        check(image->image.coordinate_frame() == frame && image->image.root_frame() == frame
            && image->image.origin_x() == 0 && image->image.origin_y() == 0, "The image keeps its declared pixel frame and origin.");
        check(!image->frame && !image->captured_at, "A file image does not invent acquisition metadata.");
        return *image;
    }
};

void expect_pixels(const ImageFrame& image, ChannelOrder order, std::span<const unsigned char> pixels)
{
    check(image.image.width() == width && image.image.height() == height
        && image.image.format() == PixelFormat{PixelDepth::UInt8, order}, "Output dimensions and channel order match the fixture.");
    check(std::ranges::equal(image.image.copy_pixels(), std::as_bytes(pixels)), "The image contains the exact expected pixels.");
}

void codecs(const Fixture& fixture)
{
    write_file(fixture.scratch.path / fixture.png_path, png);
    write_file(fixture.scratch.path / fixture.bmp_path, bmp);
    for (const auto mode : {"Gray8", "Native8"}) {
        expect_pixels(fixture.published_image(fixture.submit(fixture.model(mode, fixture.png_path))), ChannelOrder::Mono, mono_pixels);
    }
    expect_pixels(fixture.published_image(fixture.submit(fixture.model("Gray8", fixture.bmp_path))), ChannelOrder::Mono, gray_pixels);
    expect_pixels(fixture.published_image(fixture.submit(fixture.model("Native8", fixture.bmp_path))), ChannelOrder::BGR, bgr_pixels);
}

void frozen_bytes(const Fixture& fixture)
{
    const auto path = fixture.scratch.path / fixture.png_path;
    write_file(path, png);
    const auto model = fixture.model("Native8", fixture.png_path);
    auto provider = std::make_shared<ReplacingFileProvider>(fixture.files(model), path);
    auto engine = take(WorkflowEngine::create(fixture.registry, std::move(provider)));
    const auto request = fixture.request(*engine, model);
    const auto first = fixture.published_image(accepted(engine->submit(request)));
    expect_pixels(first, ChannelOrder::Mono, mono_pixels);

    const auto second = fixture.published_image(accepted(engine->submit(request)));
    expect_pixels(second, ChannelOrder::Mono, oriented_pixels);
    expect_pixels(first, ChannelOrder::Mono, mono_pixels);
}

void image_failures(const Fixture& fixture)
{
    constexpr unsigned char corrupt[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    struct InvalidImage { std::span<const unsigned char> bytes; ErrorCode error; };
    const InvalidImage invalid[] = {{{}, ErrorCode::InvalidImage}, {corrupt, ErrorCode::InvalidImage},
        {mono16_png, ErrorCode::UnsupportedPixelFormat}, {bgra_png, ErrorCode::UnsupportedPixelFormat}};
    for (const auto& file : invalid) {
        write_file(fixture.scratch.path / fixture.png_path, file.bytes);
        for (const auto mode : {"Gray8", "Native8"}) {
            const auto run = fixture.submit(fixture.model(mode, fixture.png_path));
            const auto progress = finished(run);
            check(progress.status == ExecutionStatus::Failed, "Invalid image bytes fail the accepted workflow.");
            check(diagnostic(progress, file.error).location.resource == fixture.source.id, "The image error identifies the originating resource.");
            const auto published = result(run, progress.nodes.front());
            const auto* output = published.output(key<OutputId>("image"));
            check(output && !std::holds_alternative<DataValue>(output->content), "A failed decode does not produce image data.");
        }
    }
}

void origin_failures(const Fixture& fixture)
{
    write_file(fixture.scratch.path / fixture.png_path, png);
    auto invalid_origin = fixture.model("Native8", fixture.png_path, 1);
    const auto failed = finished(fixture.submit(invalid_origin));
    const auto origin = key<ParameterId>("frame_origin");
    check(failed.status == ExecutionStatus::Failed && failed.call_order.empty()
        && diagnostic(failed, ErrorCode::ConstraintViolation).location.parameter == origin,
        "A nonzero frame_origin is located and refused before decoding.");

    auto undeclared = fixture.model("Native8", fixture.png_path);
    undeclared.coordinates.clear();
    const auto engine = take(WorkflowEngine::create(fixture.registry, fixture.files(undeclared)));
    const auto submission = engine->submit(fixture.request(*engine, undeclared));
    const auto* rejected = std::get_if<RunRejected>(&submission);
    check(rejected && rejected->diagnostic.code == ErrorCode::InvalidCoordinate
        && rejected->diagnostic.location.parameter == origin, "An undeclared coordinate frame is rejected before admission.");
}

} // namespace

int main()
{
    try {
        const Fixture fixture;
        codecs(fixture);
        frozen_bytes(fixture);
        image_failures(fixture);
        origin_failures(fixture);
        std::cout << "Real PNG/BMP read_file workflows, fixed lease bytes and diagnostics passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
