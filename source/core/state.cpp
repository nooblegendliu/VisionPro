#include <visionpro/core/state.hpp>
#include <visionpro/core/text.hpp>

#include <limits>
#include <set>
#include <type_traits>

namespace visionpro::core {
namespace {
using Check = Result<std::monostate>;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
template <class E> bool known(E value, E last)
{
    using Number = std::underlying_type_t<E>;
    return static_cast<Number>(value) >= 0 && static_cast<Number>(value) <= static_cast<Number>(last);
}
bool valid_name(const std::string& name)
{ return !name.empty() && name.find('\0') == std::string::npos && is_valid_utf8(name); }
} // namespace

Result<std::monostate> validate_global_variable_definition(const GlobalVariableDefinition& definition)
{
    if (!valid_name(definition.name) || definition.type != definition.initial_value.type()) {
        auto result = error(ErrorCode::InvalidGlobalVariable, "Global variables require a UTF-8 name and an initial value of their declared type.");
        result.location.variable = definition.id;
        return result;
    }
    return std::monostate{};
}

Result<std::monostate> validate_global_variable_definitions(std::span<const GlobalVariableDefinition> definitions)
{
    std::set<VariableId> identities;
    for (const auto& definition : definitions) {
        if (!identities.insert(definition.id).second) {
            auto result = error(ErrorCode::InvalidGlobalVariable, "Global variable identities must be unique in a solution.");
            result.location.variable = definition.id;
            return result;
        }
        if (auto checked = validate_global_variable_definition(definition); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    return std::monostate{};
}

Result<std::monostate> validate_global_variable_entry(const GlobalVariableDefinition& definition, const GlobalVariableEntry& entry)
{
    if (auto checked = validate_global_variable_definition(definition); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (entry.variable != definition.id || entry.current_value.type() != definition.type) {
        auto result = error(ErrorCode::InvalidGlobalVariable, "Runtime shared value identity/type does not match its definition.");
        result.location.variable = entry.variable;
        return result;
    }
    return std::monostate{};
}

Result<std::monostate> validate_queue_definition(const DataQueueDefinition& definition)
{
    if (!valid_name(definition.name) || definition.message_schema.kind() != TypeKind::Record || definition.capacity == 0
        || !known(definition.overflow, QueueOverflowPolicy::DropOldest) || !known(definition.receive.empty, EmptyQueuePolicy::Fail)
        || !known(definition.receive.consumers, QueueConsumerPolicy::CompetingConsumers)
        || !known(definition.receive.batch, QueueBatchPolicy::RequireExact) || definition.receive.maximum_batch_size == 0
        || definition.receive.maximum_batch_size > definition.capacity) {
        auto result = error(ErrorCode::InvalidQueue, "Queues require a complete Record Schema, positive capacity and explicit valid receive/overflow policies.");
        result.location.queue = definition.id;
        return result;
    }
    return std::monostate{};
}

Result<std::monostate> validate_queue_definitions(std::span<const DataQueueDefinition> definitions)
{
    std::set<QueueId> identities;
    for (const auto& definition : definitions) {
        if (!identities.insert(definition.id).second) {
            auto result = error(ErrorCode::InvalidQueue, "Queue identities must be unique in a solution.");
            result.location.queue = definition.id;
            return result;
        }
        if (auto checked = validate_queue_definition(definition); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    }
    return std::monostate{};
}

Result<std::monostate> validate_queue_message(const DataQueueDefinition& definition, const QueueMessage& message)
{
    if (auto checked = validate_queue_definition(definition); std::holds_alternative<Diagnostic>(checked)) { return checked; }
    if (message.queue != definition.id || message.sequence == 0 || message.payload.type() != definition.message_schema) {
        auto result = error(ErrorCode::InvalidQueue, "Queued messages require matching queue/schema and positive enqueue sequence; correlation is a separate business identity.");
        result.location.queue = message.queue;
        return result;
    }
    return std::monostate{};
}

Result<std::monostate> validate_node_state_definition(const NodeStateDefinition& definition)
{
    if (definition.schema != definition.initial_value.type() || !known(definition.scope, StateScope::SolutionSession)
        || !known(definition.reset, StateResetPolicy::DomainStartAndExplicit)
        || !known(definition.compatibility, StateCompatibilityPolicy::AlwaysNewGeneration)
        || !known(definition.mutation, StateMutationTiming::OnResultAccepted)
        || !known(definition.concurrency, StateConcurrencyPolicy::ToolManaged)
        || (definition.concurrency == StateConcurrencyPolicy::IsolatedPerInvocation && definition.scope != StateScope::Invocation)) {
        return error(ErrorCode::InvalidState, "State definition has incompatible initial data, scope, reset, compatibility, mutation or concurrency policies.");
    }
    return std::monostate{};
}

Result<std::monostate> validate_node_state(const NodeStateDefinition& definition, const NodeState& state)
{
    if (auto checked = validate_node_state_definition(definition); auto* failure = std::get_if<Diagnostic>(&checked)) {
        failure->location.node = state.key.node;
        return checked;
    }
    const auto fail = [&](std::string message) -> Check {
        auto result = error(ErrorCode::InvalidState, std::move(message));
        result.location.node = state.key.node;
        return result;
    };
    if (state.key.generation == 0 || !known(state.initialization, StateInitialization::Failed)) {
        return fail("State generation must be positive and initialization state must be recognized.");
    }
    const bool scope_matches = (definition.scope == StateScope::Invocation && std::holds_alternative<InvocationStateDomain>(state.key.domain))
        || (definition.scope == StateScope::WorkflowRun && std::holds_alternative<WorkflowRunStateDomain>(state.key.domain))
        || (definition.scope == StateScope::SolutionSession && std::holds_alternative<SessionStateDomain>(state.key.domain));
    if (!scope_matches) { return fail("State key domain does not match the declared lifetime."); }
    if (const auto* invocation = std::get_if<InvocationStateDomain>(&state.key.domain)) {
        if (invocation->invocation.key.node != state.key.node) { return fail("Invocation state key must belong to the same node as its invocation."); }
    }
    if (state.initialization == StateInitialization::Ready) {
        if (!state.current_value || state.current_value->type() != definition.schema || state.initialization_error) {
            return fail("Ready state requires matching current data and no initialization failure.");
        }
    } else {
        if (state.current_value) { return fail("Uninitialized, initializing or failed state cannot expose valid current data."); }
        if ((state.initialization == StateInitialization::Failed) != state.initialization_error.has_value()) {
            return fail("Only failed initialization carries an initialization diagnostic.");
        }
        if (state.initialization_error && state.initialization_error->severity != Severity::Error) {
            return fail("Failed initialization requires error severity.");
        }
    }
    return std::monostate{};
}

Result<std::uint64_t> next_state_generation(std::uint64_t generation)
{
    if (generation == 0 || generation == std::numeric_limits<std::uint64_t>::max()) {
        return error(ErrorCode::InvalidState, "State generation must be positive and cannot wrap on increment.");
    }
    return generation + 1;
}

} // namespace visionpro::core
