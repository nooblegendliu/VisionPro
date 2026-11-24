#include "run_image.hpp"

#include "image_panel.hpp"
#include "class_palette.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run_handle.hpp>

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

Diagnostic issue(ErrorCode code, std::string text) { return {code, Severity::Error, {}, std::move(text)}; }

const ToolConfiguration* run_tool(const RunHandle& run, const NodeId& node)
{
    const auto* flow = find_workflow(run.request().snapshot().solution(), run.identity().workflow);
    const auto* configured = flow ? find_node(*flow, node) : nullptr;
    return configured ? std::get_if<ToolConfiguration>(&configured->configuration) : nullptr;
}

bool version_one_tool(const ToolConfiguration* tool, std::string_view type)
{
    return tool && tool->config_version == 1 && tool->type.text() == type;
}

Result<DataValue> available_output(const OutputSlot* slot)
{
    if (!slot) { return issue(ErrorCode::UnknownTarget, "The selected result has no such output."); }
    if (const auto* invalid = std::get_if<InvalidOutput>(&slot->content)) { return invalid->diagnostic; }
    if (const auto* value = std::get_if<DataValue>(&slot->content)) { return *value; }
    return issue(ErrorCode::InputUnavailable, "The selected output was not produced by this invocation.");
}

struct TracedImage {
    ImageHandle image;
    IndexSource source;
};

Result<TracedImage> selected_image(const DataValue& input, IndexSource source)
{
    // The resolver applies these steps without implicitly unwrapping Optional values.
    const auto& selector = std::visit([](const auto& ref) -> const FixedSelector& { return ref.selector; }, source);
    const auto* selected = &input;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) { selected = selected->field(field->field); }
        else {
            const auto index = std::get<ElementSelection>(step).index;
            const auto* list = selected->elements();
            selected = list && index < list->size() ? &(*list)[index] : nullptr;
        }
        if (!selected) { return issue(ErrorCode::InvalidSelector, "The recorded image selector no longer resolves."); }
    }
    const auto* frame = selected->as_image();
    if (!frame) { return issue(ErrorCode::TypeMismatch, "The recorded input does not select an Image."); }
    return TracedImage{frame->image, std::move(source)};
}

Result<TracedImage> produced_image(const RunHandle& run, const ResultTrace& trace, const FixedSelector& path)
{
    if (trace.run != run.identity() || trace.reference.workflow != run.identity().workflow) {
        return issue(ErrorCode::ResultIdentityMismatch, "The recorded image comes from another run.");
    }
    const auto* output = std::get_if<OutputId>(&trace.reference.output);
    if (!output || !trace.invocation || !trace.attempt) {
        return issue(ErrorCode::InputUnavailable, "The recorded image has no exact output, invocation and attempt.");
    }
    const InvocationKey producer{trace.run.run, trace.reference.node, trace.scope, *trace.invocation};
    const auto queried = run.lookup(producer);
    if (const auto* failure = std::get_if<Diagnostic>(&queried)) { return *failure; }
    const auto& source = std::get<std::optional<NodeResult>>(queried);
    if (!source) { return issue(ErrorCode::InputUnavailable, "The recorded image source has no published result."); }
    // RunHandle::lookup checks the run, node, scope and invocation key.
    if (source->data().identity.attempt != *trace.attempt) {
        return issue(ErrorCode::ResultIdentityMismatch, "The published image does not match the recorded attempt.");
    }
    const auto value = available_output(source->output(*output));
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
    return selected_image(std::get<DataValue>(value), IndexOutputReference{trace.reference.workflow, trace.reference.node, *output, path});
}

Result<TracedImage> fixed_image(const RunHandle& run, IndexInputReference source, InputOrigin origin)
{
    const auto* fixed = run.request().input(source.input);
    if (!fixed || fixed->origin != origin) {
        return issue(ErrorCode::InputUnavailable, "The recorded image does not match a fixed run input.");
    }
    return selected_image(fixed->value, std::move(source));
}

Result<FixedSelector> traced_selector(const Selector& selector, const RunBindingTrace& binding, std::size_t source_index)
{
    FixedSelector result;
    for (std::size_t i = 0; i < selector.size(); ++i) {
        const auto& step = selector[i];
        if (const auto* field = std::get_if<FieldSelection>(&step)) { result.emplace_back(*field); }
        else if (const auto* fixed = std::get_if<ElementSelection>(&step)) { result.emplace_back(*fixed); }
        else {
            const auto trace = std::ranges::find_if(binding.sources, [&](const SourceTrace& entry) {
                return entry.source_index == source_index && entry.selector_step == i;
            });
            if (trace == binding.sources.end() || !trace->index_value || !std::in_range<std::size_t>(*trace->index_value)) {
                return issue(ErrorCode::InvalidSelector, "The input has no recorded index for this image selector.");
            }
            result.emplace_back(ElementSelection{static_cast<std::size_t>(*trace->index_value)});
        }
    }
    return result;
}

Result<const SourceTrace*> input_source(const RunNodeProgress& node, const Workflow& flow, const InputId& input, FixedSelector& path)
{
    const auto binding = std::ranges::find(node.bindings, BindingTarget{input}, &RunBindingTrace::target);
    if (binding == node.bindings.end() || binding->sources.empty()) {
        return issue(ErrorCode::InputUnavailable, "The selected input has no recorded source.");
    }
    const auto* configured = find_node(flow, node.node);
    if (!configured) { return issue(ErrorCode::UnknownTarget, "The image consumer is absent from the snapshot."); }
    const auto policy = std::ranges::find(configured->bindings, BindingTarget{input}, &Binding::target);
    std::size_t position = 0;
    if (!std::holds_alternative<IndependentInputTrace>(binding->sources.front().origin) && policy != configured->bindings.end()
        && policy->cardinality == BindingCardinality::MultipleSources) {
        const auto* index = path.empty() ? nullptr : std::get_if<ElementSelection>(&path.front());
        if (!index) { return issue(ErrorCode::InvalidSelector, "The image path requires a list index."); }
        position = index->index;
        path.erase(path.begin());
    }
    std::unordered_set<std::size_t> omitted;
    for (const auto& source : binding->sources) {
        if (source.accepted_absence) { omitted.insert(source.source_index); }
    }
    for (const auto& source : binding->sources) {
        if (!source.selector_step && !omitted.contains(source.source_index)) {
            if (position == 0) {
                const Selector* selector = nullptr;
                if (const auto* result = std::get_if<ResultTrace>(&source.origin)) { selector = &result->reference.selector; }
                if (const auto* capture = std::get_if<PublicInputTrace>(&source.origin)) { selector = &capture->reference.selector; }
                if (const auto* global = std::get_if<GlobalReadTrace>(&source.origin)) { selector = &global->selector; }
                if (selector) {
                    auto fixed = traced_selector(*selector, *binding, source.source_index);
                    if (const auto* failure = std::get_if<Diagnostic>(&fixed)) { return *failure; }
                    const auto& steps = std::get<FixedSelector>(fixed);
                    path.insert(path.begin(), steps.begin(), steps.end());
                }
                return &source;
            }
            --position;
        }
    }
    return issue(ErrorCode::InputUnavailable, "The selected image has no available source.");
}

Result<TracedImage> traced_input_image(const RunHandle& run, InvocationKey call, std::string_view port)
{
    const auto inspected = run.inspect();
    if (const auto* failure = std::get_if<Diagnostic>(&inspected)) { return *failure; }
    const auto& progress = std::get<RunProgress>(inspected);
    const auto* flow = find_workflow(run.request().snapshot().solution(), run.identity().workflow);
    if (!flow) { return issue(ErrorCode::UnknownTarget, "The image workflow is absent from the snapshot."); }
    auto input = std::get<InputId>(SchemaIdFactory::parse<InputId>(port));
    FixedSelector path;
    for (std::size_t depth = 0; depth <= progress.groups.size() + progress.iterations.size(); ++depth) {
        const auto node = std::ranges::find(progress.nodes, std::optional{call}, &RunNodeProgress::invocation);
        if (node == progress.nodes.end()) { return issue(ErrorCode::InputUnavailable, "The image invocation has no recorded inputs."); }
        const auto source = input_source(*node, *flow, input, path);
        if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
        const auto& origin = std::get<const SourceTrace*>(source)->origin;
        if (const auto* result = std::get_if<ResultTrace>(&origin)) { return produced_image(run, *result, path); }
        if (const auto* global = std::get_if<GlobalReadTrace>(&origin)) {
            const auto values = run.request().globals();
            const auto entry = std::ranges::find(values, global->variable, &GlobalVariableEntry::variable);
            if (entry == values.end() || entry->revision != global->revision) {
                return issue(ErrorCode::ResultIdentityMismatch, "The recorded global image does not match the run's global snapshot.");
            }
            return selected_image(entry->current_value, IndexGlobalReference{global->variable, {}, path});
        }
        if (const auto* fixed = std::get_if<IndependentInputTrace>(&origin)) {
            const auto* target = std::get_if<NodeTarget>(&run.request().target());
            if (!target || target->node != call.node || fixed->run != run.identity() || fixed->input != input) {
                return issue(ErrorCode::ResultIdentityMismatch, "The recorded debug image does not match the selected target.");
            }
            return fixed_image(run, {flow->id, call.node, input, path}, fixed->origin);
        }
        const auto* capture = std::get_if<PublicInputTrace>(&origin);
        if (!capture) { return issue(ErrorCode::InputUnavailable, "The recorded input source cannot provide a display image."); }
        if (capture->run != run.identity() || capture->reference.workflow != flow->id || capture->scope != call.scope) {
            return issue(ErrorCode::ResultIdentityMismatch, "The recorded public image is outside this invocation's scope.");
        }
        input = capture->reference.input;
        if (!capture->reference.group && std::holds_alternative<WorkflowTarget>(run.request().target())) {
            return fixed_image(run, {flow->id, {}, input, path}, capture->origin);
        }
        const auto group = std::ranges::find(progress.groups, call.scope, &RunGroupCall::scope);
        const auto loop = std::ranges::find(progress.iterations, call.scope, &RunLoopIteration::scope);
        if (group != progress.groups.end() && group->group.node == capture->reference.group) { call = group->group; }
        else if (loop != progress.iterations.end() && loop->loop.node == capture->reference.group) { call = loop->loop; }
        else { return issue(ErrorCode::ResultIdentityMismatch, "The public input has no matching enclosing invocation."); }
    }
    return issue(ErrorCode::InvalidContainment, "The recorded image scope path contains a cycle.");
}

const OutputSlot* display_output(const NodeResult& result)
{
    const auto& outputs = result.data().outputs;
    const auto found = std::ranges::find_if(outputs, [](const OutputSlot& output) {
        const auto* value = std::get_if<DataValue>(&output.content);
        return value && (value->as_image() || value->as_raster_mask() || value->type() == class_map_type());
    });
    return found != outputs.end() ? &*found : nullptr;
}

Result<QImage> sample_thumbnail(const ImageHandle& image, const RasterMask* mask, const ClassPalette* palette)
{
    const bool mono = image.format() == PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono};
    if (!mono && image.format() != PixelFormat{PixelDepth::UInt8, ChannelOrder::BGR}) {
        return issue(ErrorCode::UnsupportedPixelFormat, "Preview supports Mono8 and BGR8 images.");
    }
    if (image.width() > (std::numeric_limits<int>::max)() || image.height() > (std::numeric_limits<int>::max)()) {
        return issue(ErrorCode::SizeOverflow, "Preview dimensions exceed the display range.");
    }
    if (mask) {
        auto checked = validate_raster_mask(*mask, image);
        if (const auto* error = std::get_if<Diagnostic>(&checked)) { return *error; }
        if (mask->mapping.transform || mask->pixels.format() != PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) {
            return issue(ErrorCode::InvalidMask, "Preview requires a directly mapped Mono8 member mask.");
        }
    }
    const QSize source_size(static_cast<int>(image.width()), static_cast<int>(image.height()));
    auto size = source_size.scaled({80, 48}, Qt::KeepAspectRatio).expandedTo({1, 1});
    QImage preview(size, QImage::Format_RGB32);
    if (preview.isNull()) { return issue(ErrorCode::InvalidImage, "Could not allocate the preview image."); }
    for (int y = 0; y < size.height(); ++y) {
        const auto sy = static_cast<std::size_t>(y) * image.height() / size.height();
        const auto row = image.row(sy);
        if (const auto* error = std::get_if<Diagnostic>(&row)) { return *error; }
        const auto pixels = std::get<std::span<const std::byte>>(row);
        std::span<const std::byte> members;
        if (mask) {
            const auto member_row = mask->pixels.row(sy);
            if (const auto* error = std::get_if<Diagnostic>(&member_row)) { return *error; }
            members = std::get<std::span<const std::byte>>(member_row);
        }
        auto* target = reinterpret_cast<QRgb*>(preview.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const auto sx = static_cast<std::size_t>(x) * image.width() / size.width();
            QRgb color;
            if (mono) {
                const auto gray = std::to_integer<std::uint8_t>(pixels[sx]);
                color = palette ? class_color(*palette, gray).rgb() : qRgb(gray, gray, gray);
            } else {
                const auto* bgr = pixels.data() + sx * 3;
                color = qRgb(std::to_integer<int>(bgr[2]), std::to_integer<int>(bgr[1]), std::to_integer<int>(bgr[0]));
            }
            if (mask) {
                const auto value = std::to_integer<std::uint8_t>(members[sx]);
                if (mask->valid_pixels.mode == MaskValueMode::NonZero ? value != 0 : value >= mask->valid_pixels.threshold) {
                    color = qRgb((qRed(color) * 155 + 16 * 100) / 255, (qGreen(color) * 155 + 211 * 100) / 255,
                        (qBlue(color) * 155 + 166 * 100) / 255);
                }
            }
            target[x] = color;
        }
    }
    return preview;
}
} // namespace

std::vector<WorkspaceImageSource> workspace_image_sources(const RunHandle& run, const NodeResult& selected)
{
    std::vector<WorkspaceImageSource> sources;
    const auto& identity = selected.data().identity;
    const auto* flow = find_workflow(run.request().snapshot().solution(), run.identity().workflow);
    if (!flow || selected.data().status != ExecutionStatus::Succeeded) { return sources; }
    const auto append = [&](const NodeResult& record, const OutputSlot* referenced, bool upstream) {
        const auto& result = record.data();
        const auto* node = find_node(*flow, result.identity.invocation.node);
        if (!node || result.status != ExecutionStatus::Succeeded) { return; }
        auto name = QString::fromStdString(node->name);
        if (upstream) { name.prepend(QStringLiteral("来源图 · ")); }
        const auto* tool = std::get_if<ToolConfiguration>(&node->configuration);
        if (version_one_tool(tool, "visionpro.image.blob") || version_one_tool(tool, "visionpro.measure.color_mean")) {
            sources.push_back({result.identity, {}, name + QStringLiteral(" · 检测图"), upstream});
            return;
        }
        for (const auto& slot : result.outputs) {
            if (referenced && &slot != referenced) { continue; }
            const auto* value = std::get_if<DataValue>(&slot.content);
            if (value && (value->as_image() || value->as_raster_mask() || value->type() == class_map_type())) {
                sources.push_back({result.identity, slot.id, name + QStringLiteral(" · ") + QString::fromUtf8(slot.id.text()), upstream});
            }
        }
    };
    append(selected, nullptr, false);
    if (!sources.empty()) { return sources; }
    const auto inspected = run.inspect();
    const auto* progress = std::get_if<RunProgress>(&inspected);
    if (!progress) { return sources; }
    const auto call = std::ranges::find(progress->nodes, std::optional{identity.invocation}, &RunNodeProgress::invocation);
    if (call == progress->nodes.end()) { return sources; }
    for (const auto& binding : call->bindings) {
        for (const auto& source : binding.sources) {
            const auto* trace = std::get_if<ResultTrace>(&source.origin);
            if (source.selector_step || source.accepted_absence || !trace || trace->run != run.identity()
                || trace->reference.workflow != flow->id || !trace->attempt || !trace->invocation) { continue; }
            const auto* output = std::get_if<OutputId>(&trace->reference.output);
            if (!output) { continue; }
            const auto producer = run.lookup({run.identity().run, trace->reference.node, trace->scope, *trace->invocation});
            const auto* found = std::get_if<std::optional<NodeResult>>(&producer);
            if (!found || !*found || (*found)->data().identity.attempt != *trace->attempt) { continue; }
            const auto* slot = (*found)->output(*output);
            if (slot && std::holds_alternative<DataValue>(slot->content)) { append(**found, slot, true); }
        }
    }
    // A producer can supply several measured scalars to the same judgement.
    std::vector<WorkspaceImageSource> unique;
    for (auto& source : sources) {
        if (std::ranges::none_of(unique, [&](const auto& prior) {
                return prior.result == source.result && prior.output == source.output;
            })) { unique.push_back(std::move(source)); }
    }
    return unique;
}

Result<QImage> workspace_thumbnail(const RunHandle& run, const WorkspaceImageSource& source, const Solution& display_config)
{
    const auto queried = run.lookup(source.result.invocation);
    if (const auto* error = std::get_if<Diagnostic>(&queried)) { return *error; }
    const auto& record = std::get<std::optional<NodeResult>>(queried);
    if (!record || record->data().identity != source.result) {
        return issue(ErrorCode::ResultIdentityMismatch, "The image preview has no matching result.");
    }
    const auto mask = [&]() -> const RasterMask* {
        const auto* slot = record->output(std::get<OutputId>(SchemaIdFactory::parse<OutputId>("roi_mask")));
        const auto* value = slot ? std::get_if<DataValue>(&slot->content) : nullptr;
        return value ? value->as_raster_mask() : nullptr;
    };
    const auto* tool = run_tool(run, source.result.invocation.node);
    if (!source.output) {
        const bool blob = version_one_tool(tool, "visionpro.image.blob");
        auto traced = traced_input_image(run, source.result.invocation, blob ? "binary" : "image");
        if (const auto* error = std::get_if<Diagnostic>(&traced)) { return *error; }
        return sample_thumbnail(std::get<TracedImage>(traced).image, blob ? nullptr : mask(), nullptr);
    }
    const auto available = available_output(record->output(*source.output));
    if (const auto* error = std::get_if<Diagnostic>(&available)) { return *error; }
    const auto& value = std::get<DataValue>(available);
    if (const auto* image = value.as_image()) {
        const bool threshold = version_one_tool(tool, "visionpro.image.threshold") && source.output->text() == "binary";
        return sample_thumbnail(image->image, threshold ? mask() : nullptr, nullptr);
    }
    if (const auto* raster = value.as_raster_mask()) { return sample_thumbnail(raster->pixels, nullptr, nullptr); }
    if (value.type() == class_map_type()) {
        const auto* pixels = value.field(std::get<FieldId>(SchemaIdFactory::parse<FieldId>("pixels")));
        if (pixels && pixels->as_image()) {
            const auto* placement = find_placement(display_config, source.result.run.workflow, source.result.invocation.node);
            static const ClassPalette defaults;
            return sample_thumbnail(pixels->as_image()->image, nullptr, placement ? &placement->class_palette : &defaults);
        }
    }
    return issue(ErrorCode::TypeMismatch, "The selected result has no preview image.");
}

Result<std::monostate> show_run_image(ImagePanel& panel, const RunHandle& run, const InvocationKey& invocation,
    const std::optional<OutputId>& output, const Solution* display_config, bool exact_port)
{
    const auto queried = run.lookup(invocation);
    if (const auto* failure = std::get_if<Diagnostic>(&queried)) { return *failure; }
    const auto& selected = std::get<std::optional<NodeResult>>(queried);
    if (!selected) { return issue(ErrorCode::InputUnavailable, "The selected invocation has no published result."); }
    const auto* tool = run_tool(run, invocation.node);
    const bool blob = version_one_tool(tool, "visionpro.image.blob");
    if (!exact_port && (blob || version_one_tool(tool, "visionpro.measure.color_mean"))) {
        if (output) {
            const auto value = available_output(selected->output(*output));
            if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
        }
        const auto input = traced_input_image(run, invocation, blob ? "binary" : "image");
        if (const auto* failure = std::get_if<Diagnostic>(&input)) { return *failure; }
        const auto& traced = std::get<TracedImage>(input);
        const RunImageSource source{run.identity().snapshot.session, run.identity().workflow, invocation.node, traced.source};
        return panel.present_image(traced.image, source, blob ? nullptr : &*selected, blob ? &*selected : nullptr);
    }

    const auto* slot = output ? selected->output(*output) : display_output(*selected);
    if (!slot && !output) { return issue(ErrorCode::InputUnavailable, "The selected result has no published image or raster mask."); }
    const auto value = available_output(slot);
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
    const auto& data = std::get<DataValue>(value);
    if (data.type() == class_map_type()) {
        auto map = read_class_map(data);
        if (const auto* failure = std::get_if<Diagnostic>(&map)) { return *failure; }
        const auto& layout = display_config ? *display_config : run.request().snapshot().solution();
        const auto* placement = find_placement(layout, run.identity().workflow, invocation.node);
        return panel.present_class_map(std::get<ClassMap>(map),
            {run.identity().snapshot.session, run.identity().workflow, invocation.node, slot->id},
            placement ? placement->class_palette : ClassPalette{});
    }
    if (data.as_image()) {
        const bool threshold = version_one_tool(tool, "visionpro.image.threshold") && slot->id.text() == "binary";
        return panel.present_result(*selected, slot->id, threshold ? &*selected : nullptr, nullptr);
    }
    if (const auto* mask = data.as_raster_mask()) {
        return panel.set_frame(mask->pixels, RunImageSource{run.identity().snapshot.session, run.identity().workflow, invocation.node, slot->id});
    }
    return issue(ErrorCode::TypeMismatch, "The selected output is not an Image or RasterMask.");
}

} // namespace visionpro::ui
