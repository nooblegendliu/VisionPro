#include "vision/image_output.hpp"
#include "vision/vision_tools.hpp"
#include <algorithm>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/image_archive_writer.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/tool_execution.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

namespace
{
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;
template <class T> T take(Result<T> result)
{
    if (auto* failure = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(failure->message);
    }
    return std::get<T>(std::move(result));
}
template <SchemaIdentity T> T key(std::string_view name)
{
    return take(SchemaIdFactory::parse<T>(name));
}
template <UuidIdentity T> T id()
{
    return take(IdFactory::generate<T>());
}
void check(bool valid, std::string_view message)
{
    if (!valid) {
        throw std::runtime_error(std::string(message));
    }
}

struct Capture : ImageArchiveWriter
{
    ImageSaveOptions options;
    std::vector<ArchiveImage> files;
    bool fail{};
    ImageSaveReport save(const ToolInvocationIdentity&, const ImageSaveOptions& settings,
                         std::vector<ArchiveImage> images, const std::function<bool()>&) override
    {
        options = settings;
        files = std::move(images);
        ImageSaveReport report;
        for (const auto& image : files) {
            const auto name = image.prefix + "-" + options.verdict + "." + image.extension;
            report.images.push_back({image.kind, name, std::filesystem::path(L"D:/capture") / name, {}});
            if (fail) {
                report.failure = Diagnostic{ErrorCode::InvalidPath, Severity::Error, {}, "Injected I/O failure."};
                break;
            }
        }
        return report;
    }
};
struct Provider : InvocationResourceProvider
{
    std::shared_ptr<Capture> archive = std::make_shared<Capture>();
    std::span<const ResourceKind> kinds() const noexcept override
    {
        return {};
    }
    std::shared_ptr<ImageArchiveWriter> image_archive_writer() const override
    {
        return archive;
    }
    Result<std::shared_ptr<const ResourceLease>> acquire(const ToolExecutionConfiguration&,
                                                         const ToolResourceBinding&) override
    {
        throw std::logic_error("A package should not acquire node directories.");
    }
};

class MarkSource final : public ToolInstance
{
  public:
    explicit MarkSource(const ToolExecutionConfiguration& config) : ToolInstance(config) {}
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> call, InvocationCompletion) override
    {
        const auto code = *call->data().parameters.at(0).value.as_int64();
        const auto judgement = code == 1   ? ToolJudgement::OK
                               : code == 0 ? ToolJudgement::NG
                                           : ToolJudgement::NotEvaluated;
        const auto mark = take(
            DataValue::point2d({key<CoordinateSystemId>("camera"), MeasurementUnit::Pixel, 20.0 + code * 20, 14.0}));
        const OutputContent content =
            code < 0 ? OutputContent{NotProduced{OutputAbsenceReason::ConditionNotMet}} : OutputContent{mark};
        return InvocationStart{ToolCompletion{
            call->data().identity,
            ToolCompletionStatus::Succeeded,
            judgement,
            {{key<OutputId>("mark"), content}, {key<OutputId>("caption"), take(DataValue::string("XYZ"))}},
            {}}};
    }
};
class MarkFactory final : public ToolInstanceFactory
{
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    {
        return std::shared_ptr<ToolInstance>{std::make_shared<MarkSource>(config)};
    }
};
ToolRegistry test_registry()
{
    auto tools = visionpro::tools::vision::registrations();
    auto schema = key<SchemaId>("test.mark_source");
    ToolDescriptor marks{
        key<ToolTypeId>("test.mark_source"),
        "Mark",
        1,
        {schema, 1},
        {{key<ParameterId>("code"), "Code", DataType::integer(), false, ConfigValue::integer(1)}},
        {},
        {{key<OutputId>("mark"), "Mark", DataType::point2d(), {}, false, {OutputAbsenceReason::ConditionNotMet}},
         {key<OutputId>("caption"), "Caption", DataType::string()}}};
    tools.push_back({marks, {schema, 1, 1}, std::make_shared<MarkFactory>()});
    return take(ToolRegistry::create(tools));
}

struct Fixture
{
    ToolDescriptor tool = visionpro::tools::vision::image_output_descriptor();
    ToolRegistry registry = test_registry();
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    WorkflowId flow_id = id<WorkflowId>();
    CoordinateSystemId frame = key<CoordinateSystemId>("camera");
    Node node{id<NodeId>(), "Image output", ToolConfiguration{tool.id, 1}};
    Workflow flow{flow_id, "Image", {key<SchemaId>("test.image_output"), 1}, {}, {}, {}, {node.id}};
    std::vector<RunInput> values;
    std::vector<Node> upstream;
    std::vector<ResourceDefinition> directories;
    ImageFrame original;

    Fixture() : original(make_image())
    {
        bind("image", "image", take(DataValue::image(original)), false);
    }
    ImageFrame make_image() const
    {
        // Pixel (0,0) is red in the input's native BGR order.
        std::vector<std::byte> bytes(1000 * 32 * 3);
        bytes[2] = std::byte{255};
        return {
            take(ImageHandle::copy({1000, 32, 3000, {PixelDepth::UInt8, ChannelOrder::BGR}}, bytes, frame)), {}, {}};
    }
    void bind(std::string_view port, std::string_view name, DataValue value, bool multiple)
    {
        const auto input = key<InputId>(name);
        InputDescriptor source{input, std::string(name), value.type()};
        source.binding.allowed_sources = {BindingSourceKind::PublicInput};
        flow.interface.inputs.push_back(source);
        values.push_back({input, value});
        const BindingTarget target = key<InputId>(port);
        auto bound = std::ranges::find(node.bindings, target, &Binding::target);
        const BindingSource ref = ScopedInputReference{flow_id, {}, input, {}};
        if (bound != node.bindings.end()) {
            bound->sources.push_back(ref);
        }
        else {
            node.bindings.push_back(
                {target, {ref}, multiple ? BindingCardinality::MultipleSources : BindingCardinality::SingleSource});
        }
    }
    void set(std::string_view name, ConfigValue value)
    {
        const BindingTarget target = key<ParameterId>(name);
        std::erase_if(node.bindings, [&](auto& b) { return b.target == target; });
        node.bindings.push_back({target, {std::move(value)}});
    }
    void choice(std::string_view name, std::string_view value)
    {
        const auto p = std::ranges::find(tool.parameters, key<ParameterId>(name), &ParameterDescriptor::id);
        set(name, take(ConfigValue::enumeration(p->type, key<EnumMemberId>(value))));
    }
    void point(std::string_view name, int x, int y)
    {
        bind("points", name,
             take(DataValue::point2d({frame, MeasurementUnit::Pixel, static_cast<double>(x), static_cast<double>(y)})),
             true);
    }
    Solution configuration()
    {
        flow.nodes = upstream;
        flow.nodes.push_back(node);
        return Solution{
            id<SolutionId>(),
            "Result images",
            1,
            1,
            {flow},
            directories,
            {},
            {},
            {{frame, CoordinateKind::Image, MeasurementUnit::Pixel, {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}},
            {}};
    }
    NodeResult execute(std::shared_ptr<InvocationResourceProvider> resources = {}, const Solution* loaded = nullptr)
    {
        const auto solution = loaded ? *loaded : configuration();
        auto engine = take(WorkflowEngine::create(registry, resources ? resources : provider));
        auto snapshot =
            take(ConfigSnapshot::capture(solution, registry.descriptors(),
                                         {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}));
        auto submission = engine->submit(take(RunRequest::create(snapshot, WorkflowTarget{flow_id}, values,
                                                                 {TriggerKind::Manual, key<SchemaId>("test.output")})));
        if (auto* rejected = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error(rejected->diagnostic.message);
        }
        auto handle = std::get<RunHandle>(std::move(submission));
        auto progress = take(handle.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s)).progress;
        const auto output = std::ranges::find(progress.nodes, node.id, &RunNodeProgress::node);
        if (output == progress.nodes.end() || !output->invocation) {
            throw std::runtime_error(progress.diagnostics.empty() ? "Output tool was not dispatched."
                                                                  : progress.diagnostics.front().message);
        }
        return *take(handle.lookup(*output->invocation));
    }
    void storage(std::string_view verdict)
    {
        for (auto name : {"save_enabled", "trigger_enabled", "save_original", "save_rendered"}) {
            set(name, ConfigValue::boolean(true));
        }
        set("original_prefix", take(ConfigValue::string("raw")));
        set("rendered_prefix", take(ConfigValue::string("marked")));
        const auto type = take(system_field_type(SystemField::Judgement));
        bind("trigger", "inspection", take(DataValue::enumeration(type, key<EnumMemberId>(verdict))), false);
    }
};
const DataValue& value(const NodeResult& r, std::string_view field)
{
    if (r.data().status != ExecutionStatus::Succeeded) {
        throw std::runtime_error(r.data().diagnostics.front().message);
    }
    return std::get<DataValue>(r.output(key<OutputId>(field))->content);
}
bool missing(const NodeResult& r, std::string_view field)
{
    return std::holds_alternative<NotProduced>(r.output(key<OutputId>(field))->content);
}
bool marked(const ImageHandle& image, int x, int y)
{
    return take(image.row(y))[3 * x + 1] != std::byte{};
}

struct TestDirectory
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("visionpro-marks-" + id<NodeId>().to_string());
    TestDirectory()
    {
        std::filesystem::create_directory(path);
    }
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void source_style_roundtrip()
{
    namespace infra = visionpro::infrastructure;
    Fixture f;
    const auto marks = key<InputId>("points");
    const auto code = key<ParameterId>("code");
    const auto source_type = key<ToolTypeId>("test.mark_source");
    const auto mark = key<OutputId>("mark");
    std::vector<BindingSource> subscribed;
    for (int judgement : {-1, 1, 0, 2}) {
        Node producer{
            id<NodeId>(), "Source", ToolConfiguration{source_type, 1}, {{code, {ConfigValue::integer(judgement)}}}};
        if (f.upstream.empty()) {
            f.flow.entries = {producer.id};
        }
        else {
            f.flow.control_links.push_back({id<ControlLinkId>(), f.upstream.back().id, {}, producer.id});
        }
        const OutputReference source{f.flow_id, producer.id, mark, {}};
        subscribed.push_back(source);
        f.node.display_styles.push_back(
            {marks, source, {"#FF0000", "#0000FF", judgement == 0 ? 50 : 100, judgement == 0 ? 3 : 1, "#FFFF00"}});
        f.upstream.push_back(std::move(producer));
    }
    f.flow.control_links.push_back({id<ControlLinkId>(), f.upstream.back().id, {}, f.node.id});
    f.node.bindings.push_back({marks,
                               subscribed,
                               BindingCardinality::MultipleSources,
                               ConversionKind::Exact,
                               {{MissingReason::ConditionNotMet}}});
    f.node.bindings.push_back(
        {key<ParameterId>("text"), {OutputReference{f.flow_id, f.upstream[2].id, key<OutputId>("caption"), {}}}});
    f.set("text_color", take(ConfigValue::string("#00FF00")));
    f.set("text_ng_color", take(ConfigValue::string("#00FFFF")));
    f.set("text_x", ConfigValue::integer(100));
    f.set("text_opacity", ConfigValue::integer(50));
    f.set("text_thickness", ConfigValue::integer(3));

    TestDirectory temp;
    take(infra::write_new_solution(temp.path, f.configuration()));
    const auto doc = take(infra::load_solution(temp.path, f.registry));
    const auto& restored = doc.solution().workflows.front().nodes.back();
    check(restored.display_styles == f.node.display_styles,
          "Source references and their exact independent styles round-trip.");
    const auto result = f.execute({}, &doc.solution());
    const auto& image = value(result, "image").as_image()->image;
    const auto row = take(image.row(14));
    const auto rgb = [&](int x, int r, int g, int b) {
        return row[x * 3] == std::byte(r) && row[x * 3 + 1] == std::byte(g) && row[x * 3 + 2] == std::byte(b);
    };
    check(rgb(40, 255, 0, 0) && rgb(20, 0, 0, 128) && rgb(25, 0, 0, 128) && rgb(60, 255, 255, 0),
          "OK, NG and unclassified geometry use their own source judgement, width, opacity and RGB.");
    const auto text_pixels = [&](const ImageHandle& image) {
        std::size_t count{};
        for (std::size_t y = 0; y < image.height(); ++y) {
            const auto bytes = take(image.row(y));
            for (int x = 100; x < 180; ++x) {
                const auto green = bytes[x * 3 + 1];
                if (green != std::byte{}) {
                    check(bytes[x * 3] == std::byte{} && green == bytes[x * 3 + 2] && green <= std::byte{128},
                          "Bound text uses its own NG cyan and opacity.");
                    ++count;
                }
            }
        }
        return count;
    };
    const auto thick = text_pixels(image);
    f.set("text_thickness", ConfigValue::integer(1));
    const auto thin = f.execute();
    check(thick > text_pixels(value(thin, "image").as_image()->image) && thick > 0,
          "Text thickness expands glyph pixels.");
}

void typed_trigger_directories()
{
    namespace infra = visionpro::infrastructure;
    TestDirectory temp;
    const auto exercise = [&](std::string_view type, std::optional<DataValue> trigger, std::string_view condition,
                              std::string_view expected, bool invalid = false) {
        Fixture f;
        for (auto id : {"save_enabled", "trigger_enabled", "save_original"}) {
            f.set(id, ConfigValue::boolean(true));
        }
        f.set("original_prefix", take(ConfigValue::string("camera")));
        f.choice("trigger_source_type", type);
        f.choice("save_condition", condition);
        if (trigger) {
            f.bind("trigger", "verdict", *trigger, false);
        }
        for (const auto [slot, directory] :
             {std::pair{"original_path", "plain"}, {"original_ok_path", "ok"}, {"original_ng_path", "ng"}}) {
            const auto resource = id<ResourceId>();
            f.directories.push_back({resource, directory, ResourceKind::OutputPath,
                                     OutputPathResource{{PathBase::SolutionDirectory, directory, {}},
                                                        OutputNaming::SequenceNumber,
                                                        "camera",
                                                        "png",
                                                        OutputOverwrite::UniqueSuffix,
                                                        1024 * 1024}});
            f.node.resources.push_back({key<SchemaId>(slot), {resource, ResourceKind::OutputPath, {}}});
            std::filesystem::create_directories(temp.path / directory);
        }
        const auto config = f.configuration();
        const auto resources = take(infra::FileResourceProvider::create(config, temp.path));
        const auto result = f.execute(resources, &config);
        if (invalid) {
            check(result.data().status == ExecutionStatus::Failed &&
                      result.data().diagnostics.front().message.find("1/0") != std::string::npos,
                  "Restrictive conditions diagnose unrecognized trigger values.");
        }
        else if (expected.empty()) {
            check(missing(result, "original_name"), "A valid but nonmatching trigger skips saving.");
        }
        else {
            const auto path_value = [&](std::string_view field) {
                const auto& text = *value(result, field).as_string();
                return std::filesystem::path(std::u8string(text.begin(), text.end()));
            };
            const auto path = path_value("original_path");
            const auto name = path_value("original_name");
            check(std::filesystem::equivalent(path, temp.path / expected) &&
                      std::filesystem::is_regular_file(path / name),
                  "Image committed to the directory selected by its resolved trigger value.");
        }
    };
    exercise("Judgement", {}, "All", "plain");
    const auto type = take(system_field_type(SystemField::Judgement));
    exercise("Judgement", take(DataValue::enumeration(type, key<EnumMemberId>("NotEvaluated"))), "All", "plain");
    exercise("Integer", DataValue::integer(0), "All", "ng");
    exercise("Integer", DataValue::integer(1), "OK", "ok");
    exercise("Integer", DataValue::integer(2), "NG", "", true);
    exercise("String", take(DataValue::string("OK")), "NG", "");
    exercise("String", take(DataValue::string("NG")), "NG", "ng");
    exercise("String", take(DataValue::string("unknown")), "All", "plain");
}

void render_and_publish()
{
    Fixture f;
    f.point("a", 61, 13);
    f.point("b", 775, 17);
    const auto input_pixels = f.original.image.copy_pixels();
    auto result = f.execute();
    const auto& image = value(result, "image").as_image()->image;
    check(image.format().order == ChannelOrder::RGB && take(image.row(0))[0] == std::byte{255},
          "BGR converted to RGB without swapping displayed red.");
    check(marked(image, 61, 13) && marked(image, 775, 17) && !marked(image, 62, 14),
          "Both selected typed point sources render at their positions.");
    check(*value(result, "image_width").as_int64() == 1000 && *value(result, "image_height").as_int64() == 32 &&
              *value(result, "pixel_format").as_int64() == 35127316 && *value(result, "module_status").as_int64() == 1,
          "Result dimensions, format and own status.");
    check(missing(result, "original_path") && f.provider->archive->files.empty(),
          "Pure render makes no archive request.");
    check(f.original.image.copy_pixels() == input_pixels, "Rendering preserves source bytes.");
    f.choice("scale_mode", "Display");
    auto scaled = f.execute();
    check(marked(value(scaled, "image").as_image()->image, 62, 14), "1000 px display mode doubles stroke width.");
    f.choice("pixel_mode", "Mono8");
    auto mono = f.execute();
    check(*value(mono, "pixel_format").as_int64() == 17301505 &&
              take(value(mono, "image").as_image()->image.row(0))[0] == std::byte{76},
          "Mono8 uses defined red luminance.");
    f.choice("scale_mode", "Custom");
    f.set("line_scale", take(ConfigValue::real(3)));
    f.choice("line_type", "Antialias");
    f.set("text", take(ConfigValue::string("检测 OK")));
    f.set("text_x", ConfigValue::integer(180));
    f.set("text_y", ConfigValue::integer(3));
    auto annotated = f.execute();
    bool text_drawn = false;
    auto text_image = value(annotated, "image").as_image()->image;
    for (std::size_t y = 3; y < 32; ++y) {
        const auto row = take(text_image.row(y));
        text_drawn = text_drawn || std::ranges::any_of(row.subspan(180, 100), [](auto b) { return b != std::byte{}; });
    }
    check(text_drawn, "UTF-8 annotation draws into the actual result image on the headless runtime.");
}
void line_styles_and_placement()
{
    Fixture f;
    const Line2D diagonal{{f.frame, MeasurementUnit::Pixel, 30, 2}, {f.frame, MeasurementUnit::Pixel, 40, 12}};
    f.bind("lines", "diagonal", take(DataValue::line2d(diagonal)), true);
    const auto eight = f.execute();
    check(!marked(value(eight, "image").as_image()->image, 31, 2),
          "Eight-connected diagonal keeps corner neighbors apart.");
    f.choice("line_type", "Four");
    const auto four = f.execute();
    const auto& pixels = value(four, "image").as_image()->image;
    check(marked(pixels, 31, 2) || marked(pixels, 30, 3), "Four-connected diagonal bridges the corner.");
    f.choice("line_type", "Antialias");
    const auto smooth = f.execute();
    const auto row = take(value(smooth, "image").as_image()->image.row(6));
    check(std::ranges::any_of(row.subspan(29 * 3, 15 * 3),
                              [](auto b) { return b != std::byte{} && b != std::byte{255}; }),
          "Antialiased diagonal creates partial coverage pixels.");
}

void selectable_blob_items()
{
    Fixture f;
    const auto component_type = visionpro::tools::vision::blob_component_type();
    const auto bounds_type = std::ranges::find(component_type.fields(), key<FieldId>("bounds"), &FieldSchema::id)->type;
    const auto make_point = [&](double x, double y) {
        return take(DataValue::point2d({f.frame, MeasurementUnit::Pixel, x, y}));
    };
    const auto box = take(DataValue::record(bounds_type, {{key<FieldId>("origin"), make_point(20, 6)},
                                                          {key<FieldId>("width"), DataValue::integer(30)},
                                                          {key<FieldId>("height"), DataValue::integer(20)}}));
    const auto component =
        take(DataValue::record(component_type, {{key<FieldId>("bounds"), box},
                                                {key<FieldId>("centroid"), make_point(35, 16)},
                                                {key<FieldId>("area_pixels"), DataValue::integer(600)}}));
    const auto components = take(DataValue::list(component_type, {component}));
    f.bind("bounds", "region_features", components, true);
    f.bind("centroids", "center_features", components, true);
    auto both = f.execute();
    const auto& combined = value(both, "image").as_image()->image;
    check(marked(combined, 20, 6) && marked(combined, 35, 16), "Box and centroid can be selected together.");
    std::erase_if(f.node.bindings,
                  [](const auto& binding) { return binding.target == BindingTarget{key<InputId>("bounds")}; });
    auto centers = f.execute();
    const auto& center_only = value(centers, "image").as_image()->image;
    check(!marked(center_only, 20, 6) && marked(center_only, 35, 16), "Disabling bounds preserves the centroid.");
    f.node.bindings.erase(
        std::ranges::find(f.node.bindings, BindingTarget{key<InputId>("centroids")}, &Binding::target));
    f.node.bindings.push_back({key<InputId>("bounds"),
                               {ScopedInputReference{f.flow_id, {}, key<InputId>("region_features"), {}}},
                               BindingCardinality::MultipleSources});
    auto bounds = f.execute();
    const auto& box_only = value(bounds, "image").as_image()->image;
    check(marked(box_only, 20, 6) && !marked(box_only, 35, 16), "Disabling centroid preserves the box.");
}

cv::Rect text_bounds(const ImageHandle& image)
{
    int left = static_cast<int>(image.width()), right = -1, top = static_cast<int>(image.height()), bottom = -1;
    for (int y = 0; y < static_cast<int>(image.height()); ++y) {
        const auto row = take(image.row(y));
        for (int x = 0; x < static_cast<int>(image.width()); ++x) {
            if (row[3 * x + 1] == std::byte{}) {
                continue;
            }
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    check(right >= left && bottom >= top, "Text must render nonempty glyphs.");
    return {left, top, right - left + 1, bottom - top + 1};
}

void font_scale_and_position()
{
    Fixture f;
    f.set("font_size", ConfigValue::integer(9));
    f.set("text", take(ConfigValue::string("AB")));
    f.set("text_x", ConfigValue::integer(180));
    const auto original = text_bounds(value(f.execute(), "image").as_image()->image);
    check(original.x >= 180 && original.y >= 0, "Text starts at the configured origin.");
    f.set("text_x", ConfigValue::integer(217));
    f.set("text_y", ConfigValue::integer(4));
    const auto placed = text_bounds(value(f.execute(), "image").as_image()->image);
    check(placed.x - original.x == 37 && placed.y - original.y == 4 && placed.size() == original.size(),
          "Changing text X/Y translates the glyphs exactly without resizing.");
    f.choice("scale_mode", "Display");
    const auto display = text_bounds(value(f.execute(), "image").as_image()->image);
    check(display.width > placed.width && display.height > placed.height,
          "1000 px image increases the stored font size with the display multiplier.");
    f.choice("scale_mode", "Custom");
    f.set("text_scale", take(ConfigValue::real(2)));
    const auto custom = text_bounds(value(f.execute(), "image").as_image()->image);
    check(custom.width == 2 * placed.width && custom.height == placed.height,
          "The custom font width multiplier stretches the glyphs horizontally and preserves their original height.");
    f.set("text", take(ConfigValue::string("")));
    f.point("center", 40, 16);
    f.set("line_scale", take(ConfigValue::real(3)));
    const auto enlarged = f.execute();
    check(marked(value(enlarged, "image").as_image()->image, 42, 17),
          "Custom stroke multiplier increases actual pixel coverage.");
}

void region_and_mask()
{
    Fixture f;
    const auto shift = Transform2D{f.frame,
                                   f.frame,
                                   MeasurementUnit::Pixel,
                                   MeasurementUnit::Pixel,
                                   TransformModel::Affine,
                                   {1, 0, 8, 0, 1, 0, 0, 0, 1}};
    RegionDefinition region{id<RegionId>(),
                            f.frame,
                            MeasurementUnit::Pixel,
                            RegionPrimitive{f.frame, MeasurementUnit::Pixel, AxisAlignedRect{8, 6, 22, 20}, 0, shift},
                            {RegionPrimitive{f.frame, MeasurementUnit::Pixel, AxisAlignedRect{20, 10, 10, 8}}}};
    f.bind("regions", "effective_roi", take(DataValue::region2d(region)), true);
    const auto outline = f.execute();
    const auto& pixels = value(outline, "image").as_image()->image;
    check(marked(pixels, 16, 6) && marked(pixels, 37, 25) && !marked(pixels, 8, 6),
          "ROI outlines use the upstream affine placement and exact included pixel footprint.");
    check(marked(pixels, 20, 10) && marked(pixels, 29, 17) && !marked(pixels, 24, 13) && !marked(pixels, 18, 12),
          "ROI exclusion contour is visible while exclusion and inclusion interiors remain unchanged.");
    std::vector<std::byte> bytes(1000 * 32);
    bytes[12 * 1000 + 18] = std::byte{255};
    RasterMask mask{take(ImageHandle::copy({1000, 32, 1000, {PixelDepth::UInt8, ChannelOrder::Mono}}, bytes, f.frame)),
                    {f.frame, 1000, 32, {}, MaskSampling::None},
                    {}};
    f.bind("masks", "roi_mask", take(DataValue::raster_mask(mask)), true);
    auto combined = f.execute();
    const auto mask_row = take(value(combined, "image").as_image()->image.row(12));
    check(mask_row[18 * 3 + 1] == std::byte{89} && mask_row[17 * 3 + 1] == std::byte{},
          "An unstyled mask blends exactly 35 percent and leaves the adjacent pixel unchanged.");
}

void jpeg_and_naming()
{
    Fixture f;
    f.storage("OK");
    f.choice("file_format", "JPEG");
    std::vector<std::byte> texture(1000 * 32 * 3);
    for (std::size_t i = 0; i < texture.size(); ++i) {
        texture[i] = static_cast<std::byte>((i * 113 + (i / 19) * 71) % 256);
    }
    f.original = {
        take(ImageHandle::copy({1000, 32, 3000, {PixelDepth::UInt8, ChannelOrder::BGR}}, texture, f.frame)), {}, {}};
    f.values.front().value = take(DataValue::image(f.original));
    f.set("save_rendered", ConfigValue::boolean(false));
    f.set("jpeg_quality", ConfigValue::integer(15));
    f.choice("original_suffix", "Time");
    auto first = f.execute();
    const auto low = f.provider->archive->files.at(0).bytes;
    check(!missing(first, "original_name") && f.provider->archive->files.at(0).suffix == ArchiveSuffix::Time,
          "Time suffix propagates with the JPEG image.");
    f.set("jpeg_quality", ConfigValue::integer(95));
    f.choice("original_suffix", "None");
    f.execute();
    const auto& high = f.provider->archive->files.at(0).bytes;
    check(high.size() > low.size() && !cv::imdecode(high, cv::IMREAD_COLOR).empty() &&
              !cv::imdecode(low, cv::IMREAD_COLOR).empty(),
          "JPEG quality 95 preserves more encoded detail than quality 15 for the fixed textured image.");
    check(f.provider->archive->files.at(0).suffix == ArchiveSuffix::None, "No suffix reaches the writer explicitly.");
    f.choice("original_suffix", "Sequence");
    f.execute();
    check(f.provider->archive->files.at(0).suffix == ArchiveSuffix::Sequence,
          "Sequence suffix reaches the writer explicitly.");
}

void storage_modes()
{
    for (auto condition : {"All", "OK", "NG", "None"}) {
        for (auto verdict : {"OK", "NG"}) {
            Fixture f;
            f.storage(verdict);
            f.choice("save_condition", condition);
            f.point("a", 10, 10);
            const auto result = f.execute();
            check(*value(result, "module_status").as_int64() == 1, "Own success independent of inspection judgement.");
            const bool saved = std::string_view(condition) == "All" || std::string_view(condition) == verdict;
            check(f.provider->archive->files.size() == (saved ? 2u : 0u), "Filtering counts.");
            check(missing(result, "original_name") == !saved && missing(result, "rendered_path") == !saved,
                  "Conditional archive output slots.");
            if (saved) {
                const auto& files = f.provider->archive->files;
                check(f.provider->archive->options.verdict == verdict, "Archive receives correct OK/NG.");
                const auto raw = cv::imdecode(files[0].bytes, cv::IMREAD_COLOR);
                const auto marked = cv::imdecode(files[1].bytes, cv::IMREAD_COLOR);
                check(raw.at<cv::Vec3b>(10, 10)[1] == 0 && marked.at<cv::Vec3b>(10, 10)[1] == 255,
                      "Separately encoded original/rendered pixels.");
            }
        }
    }
    for (auto format : {"BMP", "JPEG", "PNG"}) {
        Fixture f;
        f.storage("OK");
        f.choice("file_format", format);
        f.set("jpeg_quality", ConfigValue::integer(50));
        f.set("output_enabled", ConfigValue::boolean(false));
        f.set("save_rendered", ConfigValue::boolean(false));
        const auto result = f.execute();
        check(missing(result, "image") && !missing(result, "original_path") && missing(result, "rendered_path"),
              "Save without publishing image.");
        check(std::get<NotProduced>(result.output(key<OutputId>("rendered_path"))->content).reason ==
                  OutputAbsenceReason::ConditionNotMet,
              "Disabled rendered storage has a conditional absence.");
        check(!cv::imdecode(f.provider->archive->files.at(0).bytes, cv::IMREAD_COLOR).empty(),
              "Encoded format can be decoded.");
    }
    Fixture capacity;
    capacity.storage("OK");
    capacity.set("min_free", ConfigValue::integer(2));
    capacity.set("retention_days", ConfigValue::integer(500));
    for (const auto& [unit, expected] :
         {std::pair{"MB", std::uint64_t{2} << 20}, std::pair{"GB", std::uint64_t{2} << 30}}) {
        capacity.choice("space_unit", unit);
        const auto stored = capacity.execute();
        const auto& options = capacity.provider->archive->options;
        check(!missing(stored, "original_name") && options.minimum_free_bytes == expected &&
                  options.retention_days == 500,
              "The 500-day setting and minimum free space in MB or GB reach the writer in the correct units.");
    }
    Fixture failure;
    failure.storage("NG");
    failure.provider->archive->fail = true;
    const auto result = failure.execute();
    check(result.data().status == ExecutionStatus::Failed && !result.data().diagnostics.empty() &&
              result.data().diagnostics.front().message.find("Committed:") != std::string::npos,
          "Partial external effects are reported with the committed artifact.");
}
} // namespace
int main()
{
    try {
        render_and_publish();
        line_styles_and_placement();
        selectable_blob_items();
        font_scale_and_position();
        region_and_mask();
        jpeg_and_naming();
        storage_modes();
        source_style_roundtrip();
        typed_trigger_directories();
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
