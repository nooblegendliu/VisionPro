#include "run_image.hpp"

#include "image_panel.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run_handle.hpp>

#include <algorithm>
#include <ranges>

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

Result<ImageHandle> selected_image(const DataValue& input, const Selector& selector)
{
    // The resolver applies these steps without implicitly unwrapping Optional values.
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
    return frame->image;
}

Result<ImageHandle> produced_image(const RunHandle& run, const ResultTrace& trace, Selector path)
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
    path.insert(path.begin(), trace.reference.selector.begin(), trace.reference.selector.end());
    return selected_image(std::get<DataValue>(value), path);
}

Result<ImageHandle> fixed_image(const RunHandle& run, const InputId& input, InputOrigin origin, const Selector& selector)
{
    const auto* fixed = run.request().input(input);
    if (!fixed || fixed->origin != origin) {
        return issue(ErrorCode::InputUnavailable, "The recorded image does not match a fixed run input.");
    }
    return selected_image(fixed->value, selector);
}

Result<const SourceTrace*> input_source(const RunNodeProgress& node, const Workflow& flow, const InputId& input, Selector& path)
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
    for (const auto& source : binding->sources) {
        if (!source.accepted_absence) {
            if (position == 0) { return &source; }
            --position;
        }
    }
    return issue(ErrorCode::InputUnavailable, "The selected image has no available source.");
}

Result<ImageHandle> blob_image(const RunHandle& run, InvocationKey call)
{
    const auto inspected = run.inspect();
    if (const auto* failure = std::get_if<Diagnostic>(&inspected)) { return *failure; }
    const auto& progress = std::get<RunProgress>(inspected);
    const auto* flow = find_workflow(run.request().snapshot().solution(), run.identity().workflow);
    if (!flow) { return issue(ErrorCode::UnknownTarget, "The image workflow is absent from the snapshot."); }
    auto input = std::get<InputId>(SchemaIdFactory::parse<InputId>("binary"));
    Selector path;
    for (std::size_t depth = 0; depth <= progress.groups.size() + progress.iterations.size(); ++depth) {
        const auto node = std::ranges::find(progress.nodes, std::optional{call}, &RunNodeProgress::invocation);
        if (node == progress.nodes.end()) { return issue(ErrorCode::InputUnavailable, "The image invocation has no recorded inputs."); }
        const auto source = input_source(*node, *flow, input, path);
        if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
        const auto& origin = std::get<const SourceTrace*>(source)->origin;
        if (const auto* result = std::get_if<ResultTrace>(&origin)) { return produced_image(run, *result, std::move(path)); }
        if (const auto* fixed = std::get_if<IndependentInputTrace>(&origin)) {
            const auto* target = std::get_if<NodeTarget>(&run.request().target());
            if (!target || target->node != call.node || fixed->run != run.identity() || fixed->input != input) {
                return issue(ErrorCode::ResultIdentityMismatch, "The recorded debug image does not match the selected target.");
            }
            return fixed_image(run, input, fixed->origin, path);
        }
        const auto* capture = std::get_if<PublicInputTrace>(&origin);
        if (!capture) { return issue(ErrorCode::InputUnavailable, "The recorded Blob source cannot provide a display image."); }
        if (capture->run != run.identity() || capture->reference.workflow != flow->id || capture->scope != call.scope) {
            return issue(ErrorCode::ResultIdentityMismatch, "The recorded public image is outside this invocation's scope.");
        }
        input = capture->reference.input;
        path.insert(path.begin(), capture->reference.selector.begin(), capture->reference.selector.end());
        if (!capture->reference.group && std::holds_alternative<WorkflowTarget>(run.request().target())) {
            return fixed_image(run, input, capture->origin, path);
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
        return value && (value->as_image() || value->as_raster_mask());
    });
    return found != outputs.end() ? &*found : nullptr;
}
} // namespace

Result<std::monostate> show_run_image(ImagePanel& panel, const RunHandle& run, const InvocationKey& invocation,
    const std::optional<OutputId>& output)
{
    const auto queried = run.lookup(invocation);
    if (const auto* failure = std::get_if<Diagnostic>(&queried)) { return *failure; }
    const auto& selected = std::get<std::optional<NodeResult>>(queried);
    if (!selected) { return issue(ErrorCode::InputUnavailable, "The selected invocation has no published result."); }
    const auto* tool = run_tool(run, invocation.node);
    if (version_one_tool(tool, "visionpro.image.blob")) {
        if (output) {
            const auto value = available_output(selected->output(*output));
            if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
        }
        const auto input = blob_image(run, invocation);
        if (const auto* failure = std::get_if<Diagnostic>(&input)) { return *failure; }
        return panel.present_blob(std::get<ImageHandle>(input), *selected);
    }

    const auto* slot = output ? selected->output(*output) : display_output(*selected);
    if (!slot && !output) { return issue(ErrorCode::InputUnavailable, "The selected result has no published image or raster mask."); }
    const auto value = available_output(slot);
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
    const auto& data = std::get<DataValue>(value);
    if (data.as_image()) {
        const bool threshold = version_one_tool(tool, "visionpro.image.threshold") && slot->id.text() == "binary";
        return panel.present_result(*selected, slot->id, threshold ? &*selected : nullptr, nullptr);
    }
    if (const auto* mask = data.as_raster_mask()) { return panel.set_frame(mask->pixels); }
    return issue(ErrorCode::TypeMismatch, "The selected output is not an Image or RasterMask.");
}

} // namespace visionpro::ui
