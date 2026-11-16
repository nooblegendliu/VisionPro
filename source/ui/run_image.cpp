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

Result<ImageHandle> produced_image(const RunHandle& run, const ResultTrace& trace)
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
    return selected_image(std::get<DataValue>(value), trace.reference.selector);
}

Result<ImageHandle> fixed_image(const RunHandle& run, const InputId& input, InputOrigin origin, const Selector& selector)
{
    const auto* fixed = run.request().input(input);
    if (!fixed || fixed->origin != origin) {
        return issue(ErrorCode::InputUnavailable, "The recorded image does not match a fixed run input.");
    }
    return selected_image(fixed->value, selector);
}

Result<ImageHandle> blob_image(const RunHandle& run, const InvocationKey& invocation)
{
    const auto progress = run.inspect();
    if (const auto* failure = std::get_if<Diagnostic>(&progress)) { return *failure; }
    const auto& nodes = std::get<RunProgress>(progress).nodes;
    const auto node = std::ranges::find_if(nodes, [&](const RunNodeProgress& call) {
        return call.node == invocation.node && call.invocation == invocation;
    });
    if (node == nodes.end()) { return issue(ErrorCode::InputUnavailable, "The selected Blob invocation has no recorded inputs."); }
    const auto binary = std::get<InputId>(SchemaIdFactory::parse<InputId>("binary"));
    const auto binding = std::ranges::find(node->bindings, BindingTarget{binary}, &RunBindingTrace::target);
    if (binding == node->bindings.end() || binding->sources.size() != 1 || binding->sources.front().accepted_absence) {
        return issue(ErrorCode::InputUnavailable, "The selected Blob invocation has no single available binary source.");
    }
    const auto& origin = binding->sources.front().origin;
    if (const auto* result = std::get_if<ResultTrace>(&origin)) { return produced_image(run, *result); }
    if (const auto* independent = std::get_if<IndependentInputTrace>(&origin)) {
        const auto* target = std::get_if<NodeTarget>(&run.request().target());
        if (!target || target->node != invocation.node || independent->run != run.identity() || independent->input != binary) {
            return issue(ErrorCode::ResultIdentityMismatch, "The recorded debug image is outside this node invocation.");
        }
        return fixed_image(run, independent->input, independent->origin, {});
    }
    if (const auto* public_input = std::get_if<PublicInputTrace>(&origin)) {
        if (public_input->run != run.identity() || public_input->reference.workflow != run.identity().workflow
            || public_input->scope != invocation.scope) {
            return issue(ErrorCode::ResultIdentityMismatch, "The recorded public image is outside this invocation's scope.");
        }
        if (public_input->reference.group || !std::holds_alternative<WorkflowTarget>(run.request().target())) {
            return issue(ErrorCode::InputUnavailable, "The recorded public image is outside the fixed workflow inputs.");
        }
        return fixed_image(run, public_input->reference.input, public_input->origin, public_input->reference.selector);
    }
    return issue(ErrorCode::InputUnavailable, "The recorded Blob source cannot provide a display image.");
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
