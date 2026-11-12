#include "node_state_service.hpp"
#include "runtime_values.hpp"

#include <algorithm>
#include <stdexcept>

namespace visionpro::workflow::detail {
namespace {
using namespace core;

Diagnostic error(ErrorCode code, WorkflowId workflow, NodeId node, std::string message)
{
    DiagnosticLocation location;
    location.workflow = workflow;
    location.node = node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

StateDomain domain(StateScope scope, const ToolInvocationIdentity& identity)
{
    switch (scope) {
    case StateScope::Invocation: return InvocationStateDomain{{identity.session, identity.invocation}};
    case StateScope::WorkflowRun: return WorkflowRunStateDomain{identity.session, identity.invocation.workflow_run};
    case StateScope::SolutionSession: return SessionStateDomain{identity.session};
    }
    throw std::logic_error("A fixed state scope must be valid.");
}

bool same_specification(const ToolExecutionConfiguration& previous, const ToolExecutionConfiguration& next)
{
    const auto& a = previous.data().tool;
    const auto& b = next.data().tool;
    const auto& left = *previous.effective_state();
    const auto& right = *next.effective_state();
    return a.id == b.id && a.config_version == b.config_version && a.schema.id == b.schema.id
        && a.schema.version == b.schema.version && left.schema == right.schema
        && left.initial_value == right.initial_value && left.scope == right.scope && left.reset == right.reset
        && left.compatibility == right.compatibility && left.mutation == right.mutation
        && left.concurrency == right.concurrency;
}

bool same_configuration(const ToolExecutionConfiguration& previous, std::span<const Binding> previous_bindings,
    const ToolExecutionConfiguration& next, std::span<const Binding> next_bindings)
{
    const auto same_parameter = [](const FixedToolParameter& a, const FixedToolParameter& b) {
        return a.id == b.id && a.value == b.value;
    };
    const auto same_resource = [](const ToolResourceBinding& a, const ToolResourceBinding& b) {
        return a.slot == b.slot && a.resource == b.resource;
    };
    const auto same_binding = [](const Binding& a, const Binding& b) {
        return a.target == b.target && a.sources == b.sources && a.cardinality == b.cardinality
            && a.conversion == b.conversion
            && std::ranges::is_permutation(a.availability.accepted_absence, b.availability.accepted_absence);
    };
    return std::ranges::is_permutation(previous.data().parameters, next.data().parameters, same_parameter)
        && std::ranges::is_permutation(previous.data().resources, next.data().resources, same_resource)
        && std::ranges::is_permutation(previous_bindings, next_bindings, same_binding);
}
} // namespace

struct StateCell {
    const NodeStateDefinition definition;
    const DataValue initial;
    NodeState record;
    const WorkflowId workflow;
};

InvocationStateLease::InvocationStateLease(ToolInvocationIdentity identity, std::shared_ptr<StateCell> cell)
    : identity_(identity), key_(cell->record.key), cell_(std::move(cell))
{}

Result<DataValue> InvocationStateLease::read() const
{
    const std::lock_guard lock(mutex_);
    if (access_ == Access::Closed) {
        return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Tool state access has expired."};
    }
    return staged_ ? *staged_ : *cell_->record.current_value;
}

Result<std::monostate> InvocationStateLease::write(const DataValue& value)
{
    const std::lock_guard lock(mutex_);
    if (access_ != Access::Open) {
        return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Tool state writes are sealed."};
    }
    if (value.type() != cell_->definition.schema) {
        return error(ErrorCode::TypeMismatch, cell_->workflow, key_.node, "State value must match the declared Schema.");
    }
    if (cell_->definition.mutation == StateMutationTiming::DuringComputation) {
        cell_->record.current_value = value;
    } else {
        staged_ = value;
    }
    return std::monostate{};
}

void InvocationStateLease::seal() noexcept
{
    const std::lock_guard lock(mutex_);
    if (access_ == Access::Open) { access_ = Access::Sealed; }
}

void InvocationStateLease::finish() noexcept
{
    const std::lock_guard lock(mutex_);
    access_ = Access::Closed;
    if (settled_) { cell_.reset(); }
}

void InvocationStateLease::settle(bool succeeded) noexcept
{
    const std::lock_guard lock(mutex_);
    if (settled_) { return; }
    if (succeeded && staged_) { cell_->record.current_value = *staged_; }
    staged_.reset();
    settled_ = true;
    if (access_ == Access::Closed) { cell_.reset(); }
    else { access_ = Access::Sealed; }
}

Result<std::shared_ptr<StateCell>> NodeStateService::select(
    const InvocationContext& context, const Node& node, bool isolated)
{
    const auto& configuration = context.data().configuration;
    const auto& data = configuration.data();
    const auto& definition = configuration.effective_state();
    const auto key = std::pair{data.workflow, data.node};
    const std::lock_guard lock(mutex_);
    if (!definition) {
        if (!isolated) { versions_.erase(key); }
        return std::shared_ptr<StateCell>{};
    }
    const bool compare_bindings = definition->compatibility == StateCompatibilityPolicy::AlwaysNewGeneration;
    const auto found = isolated ? versions_.end() : versions_.find(key);
    const bool compatible = found != versions_.end() && same_specification(found->second.configuration, configuration)
        && (!compare_bindings || same_configuration(found->second.configuration, found->second.bindings,
            configuration, node.bindings));
    if (compatible && found->second.session) { return found->second.session; }

    auto materialized = materialize_config_value(definition->initial_value);
    if (const auto* failure = std::get_if<Diagnostic>(&materialized)) { return *failure; }
    if (!compatible) {
        const auto next = last_generation_ == 0 ? Result<std::uint64_t>{std::uint64_t{1}} : next_state_generation(last_generation_);
        if (const auto* failure = std::get_if<Diagnostic>(&next)) { return *failure; }
        last_generation_ = std::get<std::uint64_t>(next);
    }
    const auto generation = compatible ? found->second.generation : last_generation_;
    auto cell = std::make_shared<StateCell>(StateCell{*definition, std::get<DataValue>(std::move(materialized)),
        {{data.node, generation, domain(definition->scope, context.data().identity)},
            StateInitialization::NotInitialized, {}, {}}, data.workflow});
    if (!isolated && !compatible) {
        versions_.insert_or_assign(key, StateVersion{configuration, compare_bindings ? node.bindings : std::vector<Binding>{},
            generation, definition->scope == StateScope::SolutionSession ? cell : nullptr});
    }
    return cell;
}

Result<std::shared_ptr<InvocationStateLease>> NodeStateService::acquire(const InvocationContext& context,
    const Node& node, bool isolated, ToolInstance& instance)
{
    auto selected = select(context, node, isolated);
    if (const auto* failure = std::get_if<Diagnostic>(&selected)) { return *failure; }
    auto cell = std::get<std::shared_ptr<StateCell>>(std::move(selected));
    if (!cell) { return std::shared_ptr<InvocationStateLease>{}; }
    auto& record = cell->record;
    if (record.initialization == StateInitialization::NotInitialized) {
        // The workflow slot prevents another invocation or reset from using
        // this cell while Init runs. No repository lock crosses tool code.
        auto initialized = initialize_tool_state(instance, context, cell->initial);
        if (auto* failure = std::get_if<Diagnostic>(&initialized)) {
            record.initialization_error = std::move(*failure);
            record.initialization = StateInitialization::Failed;
        } else {
            record.current_value = std::get<DataValue>(std::move(initialized));
            record.initialization = StateInitialization::Ready;
        }
    }
    if (record.initialization_error) { return *record.initialization_error; }
    return std::make_shared<InvocationStateLease>(context.data().identity, std::move(cell));
}

Result<std::monostate> NodeStateService::reset(WorkflowId workflow, NodeId node)
{
    const std::lock_guard lock(mutex_);
    const auto found = versions_.find({workflow, node});
    if (found == versions_.end() || !found->second.session) {
        return error(ErrorCode::InvalidState, workflow, node, "The node has no initialized session state.");
    }
    auto& cell = *found->second.session;
    if (cell.definition.reset != StateResetPolicy::DomainStartAndExplicit) {
        return error(ErrorCode::MissingCapability, workflow, node, "The node does not allow explicit state reset.");
    }
    if (cell.record.initialization_error) { return *cell.record.initialization_error; }
    if (cell.record.initialization != StateInitialization::Ready) {
        return error(ErrorCode::InvalidState, workflow, node, "The node has no initialized session state.");
    }
    cell.record.current_value = cell.initial;
    return std::monostate{};
}

void NodeStateService::clear() noexcept
{
    const std::lock_guard lock(mutex_);
    versions_.clear();
}

} // namespace visionpro::workflow::detail
