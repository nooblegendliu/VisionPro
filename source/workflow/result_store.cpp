#include <visionpro/workflow/result_store.hpp>
#include "runtime_values.hpp"

#include <algorithm>

namespace visionpro::workflow {
namespace {
using namespace core;
using Check = Result<std::monostate>;

Diagnostic error(ErrorCode code, std::string message, const RunIdentity& run,
    std::optional<NodeId> node = {})
{
    DiagnosticLocation location;
    location.workflow = run.workflow;
    location.node = node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

Check validate_record(const ConfigSnapshot& snapshot, const NodeResult& result)
{
    const auto& data = result.data();
    const auto& run = data.identity.run;
    const auto node_id = data.identity.invocation.node;
    const auto fail = [&](ErrorCode code, std::string message) {
        auto diagnostic = error(code, std::move(message), run, node_id);
        diagnostic.location.solution = snapshot.solution().id;
        return diagnostic;
    };
    const auto* workflow = find_workflow(snapshot.solution(), run.workflow);
    const auto* node = workflow ? find_node(*workflow, node_id) : nullptr;
    if (!node) {
        return fail(ErrorCode::ResultIdentityMismatch, "Result workflow and node must exist in the supplied snapshot.");
    }
    const ToolDescriptor* tool = nullptr;
    std::span<const OutputDescriptor> outputs;
    std::span<const ControlOutputDescriptor> controls;
    if (const auto* group = std::get_if<GroupConfiguration>(&node->configuration)) {
        outputs = group->interface.outputs;
        controls = group->interface.control_outputs;
    } else {
        const auto& configuration = std::get<ToolConfiguration>(node->configuration);
        tool = snapshot.find_tool(configuration.type);
        if (!tool) {
            auto diagnostic = fail(ErrorCode::UnknownTool, "Result node tool is not registered in this snapshot.");
            diagnostic.location.tool = configuration.type;
            return diagnostic;
        }
        if (tool->config_version != configuration.config_version) {
            auto diagnostic = fail(ErrorCode::IncompatibleSchemaVersion, "Result node configuration does not match its registered tool version.");
            diagnostic.location.tool = configuration.type;
            return diagnostic;
        }
        outputs = tool->outputs;
        controls = tool->control_outputs;
    }
    for (const auto& slot : data.outputs) {
        const auto declaration = std::ranges::find(outputs, slot.id, &OutputDescriptor::id);
        const auto output_error = [&](ErrorCode code, std::string message) {
            auto diagnostic = fail(code, std::move(message));
            diagnostic.location.output = slot.id;
            return diagnostic;
        };
        if (declaration == outputs.end()) {
            return output_error(ErrorCode::UnknownTarget, "Result contains an undeclared output identity.");
        }
        if (slot.type != declaration->type) {
            return output_error(ErrorCode::TypeMismatch, "Result slot type differs from its declared output Schema.");
        }
        if (const auto* value = std::get_if<DataValue>(&slot.content)) {
            auto checked = detail::validate_runtime_domain(*value, snapshot.solution(), declaration->semantics, tool);
            if (auto* failure = std::get_if<Diagnostic>(&checked)) {
                failure->location.solution = snapshot.solution().id;
                failure->location.workflow = run.workflow;
                failure->location.node = node_id;
                failure->location.output = slot.id;
                return *failure;
            }
        }
    }
    if (data.status == ExecutionStatus::Succeeded) {
        for (const auto& declaration : outputs) {
            const auto* slot = result.output(declaration.id);
            if (!slot || !std::holds_alternative<DataValue>(slot->content)) {
                auto diagnostic = fail(ErrorCode::InvalidNodeResult,
                    "Successful results require an available value for every declared output; Optional None and empty collections are values.");
                diagnostic.location.output = declaration.id;
                return diagnostic;
            }
        }
    }
    for (const auto& selected : data.selected_control_outputs) {
        if (std::ranges::find(controls, selected, &ControlOutputDescriptor::id) == controls.end()) {
            return fail(ErrorCode::InvalidNodeResult, "Result selected an undeclared control outlet.");
        }
    }
    return std::monostate{};
}
} // namespace

core::Result<std::monostate> ResultStore::insert(const ConfigSnapshot& snapshot, const NodeResult& result)
{
    const auto& data = result.data();
    const auto& run = data.identity.run;
    const auto& invocation = data.identity.invocation;
    if (snapshot.identity().session != session_ || run.snapshot != snapshot.identity()
        || invocation.workflow_run != run.run) {
        return error(core::ErrorCode::ResultIdentityMismatch,
            "Store session, result run and supplied snapshot identities must agree.", run, invocation.node);
    }
    const auto registered = runs_.find(run.run);
    if (registered != runs_.end() && registered->second != run) {
        return error(core::ErrorCode::ResultIdentityMismatch,
            "A session run identity cannot be reused for another workflow or snapshot.", run, invocation.node);
    }
    if (retired_.contains(run.run)) {
        return error(core::ErrorCode::ResultRetired, "A retired run cannot accept late results.", run, invocation.node);
    }
    if (results_.contains(invocation)) {
        return error(core::ErrorCode::DuplicateResult,
            "This logical invocation already has a result; a different attempt does not replace it.", run, invocation.node);
    }
    if (auto checked = validate_record(snapshot, result); std::holds_alternative<core::Diagnostic>(checked)) { return checked; }
    runs_.emplace(run.run, run);
    results_.emplace(invocation, result);
    return std::monostate{};
}

core::Result<std::optional<NodeResult>> ResultStore::lookup(const RunIdentity& run,
    const core::InvocationKey& invocation) const
{
    if (run.snapshot.session != session_ || invocation.workflow_run != run.run) {
        return error(core::ErrorCode::ResultIdentityMismatch,
            "Lookup session and invocation run must agree with the requested run identity.", run, invocation.node);
    }
    const auto registered = runs_.find(run.run);
    if (registered != runs_.end() && registered->second != run) {
        return error(core::ErrorCode::ResultIdentityMismatch,
            "Lookup workflow and snapshot do not match the registered run.", run, invocation.node);
    }
    if (retired_.contains(run.run)) {
        return error(core::ErrorCode::ResultRetired, "Requested run results have been explicitly retired.", run, invocation.node);
    }
    const auto found = results_.find(invocation);
    if (found == results_.end()) { return std::optional<NodeResult>{}; }
    return std::optional<NodeResult>{found->second};
}

core::Result<std::monostate> ResultStore::release_run(const RunIdentity& run)
{
    if (run.snapshot.session != session_) {
        return error(core::ErrorCode::ResultIdentityMismatch, "Cannot retire another session's run.", run);
    }
    const auto registered = runs_.find(run.run);
    if (registered != runs_.end() && registered->second != run) {
        return error(core::ErrorCode::ResultIdentityMismatch,
            "Retirement identity does not match the registered workflow and snapshot.", run);
    }
    // Remember even a run with no accepted record, so a late first result cannot resurrect it.
    runs_.emplace(run.run, run);
    retired_.insert(run.run);
    std::erase_if(results_, [&](const auto& entry) { return entry.first.workflow_run == run.run; });
    return std::monostate{};
}

} // namespace visionpro::workflow
