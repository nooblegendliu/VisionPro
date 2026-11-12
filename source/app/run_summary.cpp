#include "run_summary.hpp"

#include <stdexcept>

namespace visionpro::app {
namespace {
using namespace core;
using namespace workflow;
using nlohmann::json;

template<class... F> struct Overload : F... { using F::operator()...; };

template<class Range, class Convert> json array_json(const Range& values, Convert convert)
{
    auto result = json::array();
    for (const auto& value : values) { result.push_back(convert(value)); }
    return result;
}

template<class Id> std::string id_text(const Id& id)
{
    if constexpr (UuidIdentity<Id>) { return id.to_string(); }
    else if constexpr (RuntimeIdentity<Id>) { return std::to_string(id.value()); }
    else { return std::string(id.text()); }
}

template<class Id> json optional_id(const std::optional<Id>& id)
{ return id ? json(id_text(*id)) : json(nullptr); }

std::string_view name(ExecutionStatus status)
{
    switch (status) {
    case ExecutionStatus::Pending: return "Pending";
    case ExecutionStatus::Running: return "Running";
    case ExecutionStatus::Succeeded: return "Succeeded";
    case ExecutionStatus::Failed: return "Failed";
    case ExecutionStatus::Skipped: return "Skipped";
    case ExecutionStatus::Cancelled: return "Cancelled";
    case ExecutionStatus::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

std::string_view name(NodeSkipReason reason)
{
    switch (reason) {
    case NodeSkipReason::Disabled: return "Disabled";
    case NodeSkipReason::UpstreamClosed: return "UpstreamClosed";
    case NodeSkipReason::FailFast: return "FailFast";
    case NodeSkipReason::RunCancelled: return "RunCancelled";
    case NodeSkipReason::RunTimedOut: return "RunTimedOut";
    case NodeSkipReason::NotSelected: return "NotSelected";
    }
    return "Unknown";
}

std::string_view name(Judgement judgement)
{
    switch (judgement) {
    case Judgement::NotEvaluated: return "NotEvaluated";
    case Judgement::OK: return "OK";
    case Judgement::NG: return "NG";
    }
    return "Unknown";
}

std::string_view name(ControlLinkStatus status)
{
    switch (status) {
    case ControlLinkStatus::Pending: return "Pending";
    case ControlLinkStatus::Arrived: return "Arrived";
    case ControlLinkStatus::Closed: return "Closed";
    }
    return "Unknown";
}

std::string_view name(ControlCloseReason reason)
{
    switch (reason) {
    case ControlCloseReason::NotSelected: return "NotSelected";
    case ControlCloseReason::Disabled: return "Disabled";
    case ControlCloseReason::Failed: return "Failed";
    case ControlCloseReason::Cancelled: return "Cancelled";
    case ControlCloseReason::TimedOut: return "TimedOut";
    }
    return "Unknown";
}

std::string_view name(MissingReason reason)
{
    switch (reason) {
    case MissingReason::NotSelected: return "NotSelected";
    case MissingReason::Disabled: return "Disabled";
    case MissingReason::ToolDidNotProduce: return "ToolDidNotProduce";
    case MissingReason::ConditionNotMet: return "ConditionNotMet";
    }
    return "Unknown";
}

std::string_view name(OutputAbsenceReason reason)
{
    switch (reason) {
    case OutputAbsenceReason::NotSelected: return "NotSelected";
    case OutputAbsenceReason::Disabled: return "Disabled";
    case OutputAbsenceReason::ToolDidNotProduce: return "ToolDidNotProduce";
    case OutputAbsenceReason::Failed: return "Failed";
    case OutputAbsenceReason::Cancelled: return "Cancelled";
    case OutputAbsenceReason::TimedOut: return "TimedOut";
    case OutputAbsenceReason::ConditionNotMet: return "ConditionNotMet";
    }
    return "Unknown";
}

std::string_view name(ResourceKind kind)
{
    switch (kind) {
    case ResourceKind::Image: return "Image";
    case ResourceKind::Mask: return "Mask";
    case ResourceKind::Template: return "Template";
    case ResourceKind::Calibration: return "Calibration";
    case ResourceKind::Device: return "Device";
    case ResourceKind::DirectorySequence: return "DirectorySequence";
    case ResourceKind::OutputPath: return "OutputPath";
    case ResourceKind::Binary: return "Binary";
    }
    return "Unknown";
}

std::string_view name(SystemField field)
{
    switch (field) {
    case SystemField::ExecutionStatus: return "ExecutionStatus";
    case SystemField::Judgement: return "Judgement";
    case SystemField::Diagnostics: return "Diagnostics";
    case SystemField::Elapsed: return "Elapsed";
    }
    return "Unknown";
}

std::string_view name(Severity severity)
{
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
    }
    return "Unknown";
}

template<class Enum> json optional_name(const std::optional<Enum>& value)
{ return value ? json(name(*value)) : json(nullptr); }

json diagnostic_json(const Diagnostic& diagnostic)
{
    const auto& source = diagnostic.location;
    auto location = json::object();
    const auto locate = [&](std::string_view key, const auto& id) {
        if (id) { location[std::string(key)] = id_text(*id); }
    };
    locate("solution", source.solution);
    locate("workflow", source.workflow);
    locate("node", source.node);
    locate("tool", source.tool);
    locate("schema", source.schema);
    locate("input", source.input);
    locate("output", source.output);
    locate("parameter", source.parameter);
    locate("resource", source.resource);
    locate("variable", source.variable);
    locate("queue", source.queue);
    locate("region", source.region);
    locate("control_link", source.control_link);
    if (!source.path.empty()) {
        location["path"] = array_json(source.path, [](const DiagnosticPathStep& step) {
            return std::visit(Overload{
                [](const FieldId& field) { return json{{"field", id_text(field)}}; },
                [](DiagnosticElementIndex index) { return json{{"index", index.value}}; }}, step);
        });
    }
    json result{{"code", error_code_name(diagnostic.code)}, {"severity", name(diagnostic.severity)},
        {"message", diagnostic.message}, {"location", std::move(location)}};
    if (const auto& external = diagnostic.external) {
        result["external"] = {{"provider", id_text(external->provider)}, {"operation", external->operation},
            {"domain", static_cast<int>(external->domain)}, {"raw_code", std::to_string(external->raw_code)},
            {"mapped", external->mapping == ExternalCodeMapping::Mapped}, {"provider_domain", optional_id(external->provider_domain)},
            {"description", external->description ? json(*external->description) : json(nullptr)},
            {"backend_version", external->backend_version ? json(*external->backend_version) : json(nullptr)}};
    }
    return result;
}

void add_run_identity(json& object, const RunIdentity& run)
{
    object["session"] = id_text(run.snapshot.session);
    object["snapshot"] = id_text(run.snapshot.snapshot);
    object["workflow"] = id_text(run.workflow);
    object["run"] = id_text(run.run);
}

json selector_json(const Selector& selector)
{
    return array_json(selector, [](const SelectionStep& step) {
        return std::visit(Overload{
            [](const FieldSelection& field) { return json{{"field", id_text(field.field)}}; },
            [](ElementSelection element) { return json{{"index", element.index}}; }}, step);
    });
}

json resource_json(const ResourceReference& reference)
{
    return {{"resource", id_text(reference.id)}, {"kind", name(reference.kind)},
        {"revision", reference.revision ? json(std::to_string(*reference.revision)) : json(nullptr)}};
}

json update_origin_json(const UpdateOrigin& origin)
{
    return std::visit(Overload{
        [](const SessionInvocationKey& key) {
            return json{{"session", id_text(key.session)}, {"run", id_text(key.key.workflow_run)},
                {"node", id_text(key.key.node)}, {"scope", id_text(key.key.scope)}, {"invocation", id_text(key.key.invocation)}};
        },
        [](const ExternalUpdateOrigin& update) {
            return json{{"session", id_text(update.session)}, {"source", id_text(update.source)}};
        }}, origin);
}

json source_json(const SourceTrace& source)
{
    auto value = std::visit(Overload{
        [](ConstantTrace constant) { return json{{"kind", "constant"}, {"defaulted", constant.defaulted}}; },
        [](const ResultTrace& trace) {
            const auto* port = std::get_if<OutputId>(&trace.reference.output);
            json result{{"kind", "result"}, {"node", id_text(trace.reference.node)},
                {"output", port ? id_text(*port) : std::string(name(std::get<SystemField>(trace.reference.output)))},
                {"output_kind", port ? "port" : "system"}, {"selector", selector_json(trace.reference.selector)},
                {"scope", id_text(trace.scope)}, {"invocation", optional_id(trace.invocation)}, {"attempt", optional_id(trace.attempt)}};
            add_run_identity(result, trace.run);
            return result;
        },
        [](const PublicInputTrace& trace) {
            json result{{"kind", "public_input"}, {"input", id_text(trace.reference.input)}, {"scope", id_text(trace.scope)},
                {"group", optional_id(trace.reference.group)}, {"selector", selector_json(trace.reference.selector)},
                {"origin", trace.origin == InputOrigin::Provided ? "Provided" : "Default"}};
            add_run_identity(result, trace.run);
            return result;
        },
        [](const IndependentInputTrace& trace) {
            json result{{"kind", "independent_input"}, {"input", id_text(trace.input)},
                {"origin", trace.origin == InputOrigin::Provided ? "Provided" : "Default"}};
            add_run_identity(result, trace.run);
            return result;
        },
        [](const GlobalReadTrace& trace) {
            return json{{"kind", "global"}, {"variable", id_text(trace.variable)},
                {"revision", std::to_string(trace.revision)}, {"update_origin", update_origin_json(trace.update_origin)}};
        },
        [](const ResourceReference& resource) {
            auto result = resource_json(resource);
            result["resource_kind"] = result["kind"];
            result["kind"] = "resource";
            return result;
        }}, source.origin);
    value["index"] = source.source_index;
    value["accepted_absence"] = optional_name(source.accepted_absence);
    return value;
}

json binding_json(const RunBindingTrace& binding)
{
    return {{"target", std::visit([](const auto& id) { return id_text(id); }, binding.target)},
        {"target_kind", std::holds_alternative<InputId>(binding.target) ? "input" : "parameter"},
        {"sources", array_json(binding.sources, source_json)}};
}

json data_json(const DataValue& data)
{
    if (const auto* number = data.as_int64()) { return std::to_string(*number); }
    if (const auto* list = data.elements()) { return array_json(*list, data_json); }
    throw std::logic_error("The simulation catalog emitted an unsupported output type.");
}

json output_json(const OutputSlot& slot)
{
    json value = std::visit(Overload{
        [](const DataValue& data) { return json{{"status", "Produced"}, {"value", data_json(data)}}; },
        [](NotProduced missing) { return json{{"status", "NotProduced"}, {"reason", name(missing.reason)}}; },
        [](const InvalidOutput& invalid) { return json{{"status", "Invalid"}, {"diagnostic", diagnostic_json(invalid.diagnostic)}}; }}, slot.content);
    value["id"] = id_text(slot.id);
    return value;
}

json node_json(const RunNodeProgress& node, const std::optional<NodeResult>& result)
{
    json trace{{"id", id_text(node.node)}, {"status", name(node.status)}, {"skip_reason", optional_name(node.skip_reason)},
        {"invocation", nullptr}, {"scope", nullptr}, {"attempt", nullptr}, {"judgement", nullptr},
        {"selected_outputs", json::array()}, {"outputs", json::array()}, {"diagnostics", json::array()},
        {"bindings", array_json(node.bindings, binding_json)}, {"resources", array_json(node.resources, [](const RunResourceTrace& resource) {
            auto value = resource_json(resource.reference);
            value["slot"] = id_text(resource.slot);
            return value;
        })}};
    if (const auto& call = node.invocation) {
        trace["invocation"] = id_text(call->invocation);
        trace["scope"] = id_text(call->scope);
    }
    if (result) {
        const auto& data = result->data();
        trace["attempt"] = id_text(data.identity.attempt);
        trace["judgement"] = name(data.judgement);
        trace["selected_outputs"] = array_json(data.selected_control_outputs, id_text<ControlOutputId>);
        trace["outputs"] = array_json(data.outputs, output_json);
        trace["diagnostics"] = diagnostics_json(data.diagnostics);
    }
    return trace;
}
} // namespace

nlohmann::json diagnostics_json(std::span<const core::Diagnostic> diagnostics)
{ return array_json(diagnostics, diagnostic_json); }

core::Result<nlohmann::json> summarize_run(const workflow::RunHandle& handle,
    const workflow::RunProgress& progress, std::span<const core::Diagnostic> load_diagnostics)
{
    const auto& solution = handle.request().snapshot().solution();
    auto nodes = json::array();
    for (const auto& node : progress.nodes) {
        std::optional<NodeResult> result;
        if (node.invocation) {
            auto found = handle.lookup(*node.invocation);
            if (auto* failure = std::get_if<Diagnostic>(&found)) { return std::move(*failure); }
            result = std::get<std::optional<NodeResult>>(std::move(found));
        }
        nodes.push_back(node_json(node, result));
    }
    json summary{{"solution", id_text(solution.id)}, {"revision", std::to_string(solution.revision)},
        {"status", name(progress.status)}, {"quiescent", progress.quiescent},
        {"load_diagnostics", diagnostics_json(load_diagnostics)}, {"diagnostics", diagnostics_json(progress.diagnostics)},
        {"nodes", std::move(nodes)}, {"call_order", array_json(progress.call_order, id_text<NodeId>)},
        {"control_links", array_json(progress.control_links, [](const RunControlLinkProgress& link) {
            return json{{"id", id_text(link.link)}, {"status", name(link.status)}, {"close_reason", optional_name(link.close_reason)}};
        })}};
    add_run_identity(summary, handle.identity());
    return summary;
}

} // namespace visionpro::app
