#include <visionpro/workflow/run.hpp>
#include "runtime_values.hpp"

#include <algorithm>
#include <set>
#include <type_traits>

namespace visionpro::workflow {
namespace {

using core::DataValue;
using core::Diagnostic;
using core::ErrorCode;
using core::Result;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, core::Severity::Error, {}, std::move(message)}; }
template <class E> bool known(E value, E last)
{
    using N = std::underlying_type_t<E>;
    return static_cast<N>(value) >= 0 && static_cast<N>(value) <= static_cast<N>(last);
}

} // namespace

struct RunRequest::Storage {
    ConfigSnapshot snapshot;
    RunTarget target;
    std::vector<FixedRunInput> inputs;
    RunTrigger trigger;
    std::optional<core::CorrelationId> correlation;
    std::optional<DebugStatePolicy> debug_state;
    std::vector<core::GlobalVariableEntry> globals;
};

Result<RunRequest> RunRequest::create(const ConfigSnapshot& snapshot, RunTarget target,
    const std::vector<RunInput>& inputs, RunTrigger trigger,
    std::optional<core::CorrelationId> correlation, std::optional<DebugStatePolicy> debug_state,
    std::span<const core::GlobalVariableEntry> globals)
{
    const auto workflow_id = std::visit([](const auto& value) { return value.workflow; }, target);
    const auto contextualize = [&](Diagnostic diagnostic) -> Result<RunRequest> {
        diagnostic.location.solution = snapshot.solution().id;
        diagnostic.location.workflow = workflow_id;
        if (const auto* node = std::get_if<NodeTarget>(&target)) { diagnostic.location.node = node->node; }
        return diagnostic;
    };
    if (!known(trigger.kind, TriggerKind::Debug) || (debug_state && !known(*debug_state, DebugStatePolicy::Active))) {
        return contextualize(error(ErrorCode::InvalidRunRequest, "Unknown trigger or debug state policy."));
    }
    const auto* workflow = find_workflow(snapshot.solution(), workflow_id);
    if (!workflow || !workflow->enabled) {
        return contextualize(error(ErrorCode::InvalidRunRequest, "Request workflow is absent or disabled in this snapshot."));
    }
    const PublicInterface* public_interface = &workflow->interface;
    const std::vector<core::InputDescriptor>* schema = &public_interface->inputs;
    if (const auto* node_target = std::get_if<NodeTarget>(&target)) {
        const auto* node = find_node(*workflow, node_target->node);
        if (!node || !node->enabled) {
            return contextualize(error(ErrorCode::InvalidRunRequest, "Independent target node is absent or disabled."));
        }
        // Disabled containing groups cannot be bypassed by targeting a child.
        auto child = node->id;
        std::set<core::NodeId> ancestors{child};
        for (;;) {
            const auto parent = std::ranges::find(workflow->containment, child, &Containment::child);
            if (parent == workflow->containment.end()) { break; }
            const auto* owner = find_node(*workflow, parent->parent);
            if (!owner || !owner->enabled || !std::holds_alternative<GroupConfiguration>(owner->configuration)
                || !ancestors.insert(owner->id).second) {
                return contextualize(error(ErrorCode::InvalidRunRequest, "Independent target requires a valid, enabled containing Group chain."));
            }
            child = owner->id;
        }
        if (const auto* group = std::get_if<GroupConfiguration>(&node->configuration)) {
            public_interface = &group->interface;
            schema = &public_interface->inputs;
        } else {
            const auto& tool_config = std::get<ToolConfiguration>(node->configuration);
            const auto* tool = snapshot.find_tool(tool_config.type);
            if (!tool || tool->config_version != tool_config.config_version) {
                return contextualize(error(ErrorCode::InvalidRunRequest, "Independent target requires its exact tool configuration version."));
            }
            public_interface = nullptr;
            schema = &tool->inputs;
        }
        if (!debug_state) { debug_state = DebugStatePolicy::Isolated; }
    } else if (debug_state) {
        return contextualize(error(ErrorCode::InvalidRunRequest, "Debug state policy applies only to independent node/group targets."));
    }
    if (public_interface) {
        if (auto checked = validate_public_interface(*public_interface);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
    }

    std::set<core::InputId> supplied;
    for (const auto& input : inputs) {
        if (!supplied.insert(input.id).second || std::ranges::find(*schema, input.id, &core::InputDescriptor::id) == schema->end()) {
            auto failure = error(ErrorCode::InvalidRunRequest, "Request contains a duplicate or undeclared public input.");
            failure.location.input = input.id;
            return contextualize(std::move(failure));
        }
    }
    std::vector<FixedRunInput> fixed;
    fixed.reserve(schema->size());
    for (const auto& declaration : *schema) {
        const auto provided = std::ranges::find(inputs, declaration.id, &RunInput::id);
        std::optional<DataValue> value;
        auto origin = InputOrigin::Provided;
        if (provided != inputs.end()) { value = provided->value; }
        else {
            if (declaration.required || !declaration.default_value) {
                auto failure = error(ErrorCode::MissingRequiredValue, "Every new request must supply its required input, including Optional inputs.");
                failure.location.input = declaration.id;
                return contextualize(std::move(failure));
            }
            auto converted = detail::materialize_config_value(*declaration.default_value);
            if (const auto* failure = std::get_if<Diagnostic>(&converted)) { return contextualize(*failure); }
            value = std::get<DataValue>(converted);
            origin = InputOrigin::Default;
        }
        if (auto checked = detail::validate_runtime_value(declaration.type, declaration.constraints, *value);
            auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.input = declaration.id;
            return contextualize(*failure);
        }
        fixed.push_back({declaration.id, *value, origin});
    }
    if (auto checked = detail::validate_global_read_snapshot(snapshot.solution(), snapshot.identity().session, globals);
        const auto* failure = std::get_if<Diagnostic>(&checked)) { return contextualize(*failure); }
    return RunRequest{std::make_shared<const Storage>(Storage{
        snapshot, std::move(target), std::move(fixed), std::move(trigger), std::move(correlation), debug_state,
        {globals.begin(), globals.end()}})};
}

const ConfigSnapshot& RunRequest::snapshot() const noexcept { return storage_->snapshot; }
const RunTarget& RunRequest::target() const noexcept { return storage_->target; }
std::span<const FixedRunInput> RunRequest::inputs() const noexcept { return storage_->inputs; }
const FixedRunInput* RunRequest::input(const core::InputId& id) const noexcept
{
    const auto found = std::ranges::find(storage_->inputs, id, &FixedRunInput::id);
    return found == storage_->inputs.end() ? nullptr : &*found;
}
std::span<const core::GlobalVariableEntry> RunRequest::globals() const noexcept { return storage_->globals; }
const RunTrigger& RunRequest::trigger() const noexcept { return storage_->trigger; }
const std::optional<core::CorrelationId>& RunRequest::correlation() const noexcept { return storage_->correlation; }
std::optional<DebugStatePolicy> RunRequest::debug_state() const noexcept { return storage_->debug_state; }

OutputAddress output_address(const ResultIdentity& identity, core::OutputId output)
{ return {{identity.run.snapshot.session, identity.invocation}, std::move(output)}; }

Result<NodeResult> NodeResult::create(const NodeResultData& data)
{
    const auto fail = [&](std::string message) -> Result<NodeResult> {
        auto failure = error(ErrorCode::InvalidNodeResult, std::move(message));
        failure.location.workflow = data.identity.run.workflow;
        failure.location.node = data.identity.invocation.node;
        return failure;
    };
    if (data.identity.run.run != data.identity.invocation.workflow_run || !known(data.status, ExecutionStatus::TimedOut)
        || !known(data.judgement, Judgement::NG) || !core::valid_execution_timing(data.elapsed, data.timings)) {
        return fail("Result identity, status, judgement or elapsed time is inconsistent.");
    }
    if (data.status == ExecutionStatus::Pending || data.status == ExecutionStatus::Running || data.status == ExecutionStatus::Skipped) {
        return fail("A terminal call result cannot represent pending/running work or fabricate a skipped invocation.");
    }
    const bool success = data.status == ExecutionStatus::Succeeded;
    if (!success && (data.judgement != Judgement::NotEvaluated || !data.selected_control_outputs.empty())) {
        return fail("Unsuccessful calls cannot publish detection judgements or normal control selections.");
    }
    bool has_error = false;
    for (const auto& diagnostic : data.diagnostics) {
        if (!core::is_valid_diagnostic(diagnostic)) { return fail("Result contains an invalid diagnostic."); }
        has_error = has_error || diagnostic.severity == core::Severity::Error;
    }
    std::set<core::ControlOutputId> selected;
    for (const auto& output : data.selected_control_outputs) {
        if (!selected.insert(output).second) { return fail("Control outlet selections must be unique."); }
    }
    std::set<core::OutputId> outputs;
    for (const auto& slot : data.outputs) {
        if (!outputs.insert(slot.id).second) { return fail("Result output identities must be unique."); }
        if (const auto* available = std::get_if<DataValue>(&slot.content)) {
            if (!success || available->type() != slot.type) { return fail("Only successful calls publish available outputs, with exact types."); }
        } else if (const auto* absent = std::get_if<NotProduced>(&slot.content)) {
            if (!known(absent->reason, OutputAbsenceReason::ConditionNotMet)
                || (success && (absent->reason == OutputAbsenceReason::Failed || absent->reason == OutputAbsenceReason::Cancelled || absent->reason == OutputAbsenceReason::TimedOut))
                || (!success && ((data.status == ExecutionStatus::Failed && absent->reason != OutputAbsenceReason::Failed)
                    || (data.status == ExecutionStatus::Cancelled && absent->reason != OutputAbsenceReason::Cancelled)
                    || (data.status == ExecutionStatus::TimedOut && absent->reason != OutputAbsenceReason::TimedOut)))) {
                return fail("Output absence reason must agree with the call's terminal state.");
            }
        } else {
            const auto& diagnostic = std::get<InvalidOutput>(slot.content).diagnostic;
            if (success || !core::is_valid_diagnostic(diagnostic) || diagnostic.severity != core::Severity::Error) {
                return fail("Invalid outputs require error diagnostics and cannot form a successful call result.");
            }
            has_error = true;
        }
    }
    if ((success && has_error) || (data.status == ExecutionStatus::Failed && !has_error)) {
        return fail("Succeeded calls cannot contain error diagnostics; Failed calls require an error explanation.");
    }
    // Copy editable containers/strings even for rvalue callers with retained aliases.
    return NodeResult{std::make_shared<const NodeResultData>(data)};
}

const OutputSlot* NodeResult::output(const core::OutputId& id) const noexcept
{
    const auto found = std::ranges::find(data_->outputs, id, &OutputSlot::id);
    return found == data_->outputs.end() ? nullptr : &*found;
}

} // namespace visionpro::workflow
