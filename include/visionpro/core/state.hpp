#pragma once

#include <visionpro/core/runtime_id.hpp>
#include <visionpro/core/value.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace visionpro::core {

struct ExternalUpdateOrigin { SolutionSessionId session; SchemaId source; };
using UpdateOrigin = std::variant<SessionInvocationKey, ExternalUpdateOrigin>;
struct GlobalVariableDefinition {
    VariableId id;
    std::string name;
    DataType type;
    ConfigValue initial_value;
};
struct GlobalVariableEntry {
    VariableId variable;
    DataValue current_value;
    std::uint64_t revision; // Zero is the initialized value; writes advance it in the later service.
    UpdateOrigin update_origin;
};

enum class QueueOverflowPolicy { Wait, Reject, DropOldest };
enum class EmptyQueuePolicy { Wait, ReturnEmpty, Fail };
enum class QueueConsumerPolicy { SingleConsumer, CompetingConsumers };
enum class QueueBatchPolicy { UpToLimit, RequireExact };
struct QueueReceivePolicy {
    EmptyQueuePolicy empty;
    std::size_t maximum_batch_size;
    QueueConsumerPolicy consumers;
    QueueBatchPolicy batch;
};
struct DataQueueDefinition {
    QueueId id;
    std::string name;
    DataType message_schema;
    std::size_t capacity;
    QueueOverflowPolicy overflow;
    QueueReceivePolicy receive;
};
struct QueueMessage {
    MessageId id;
    QueueId queue;
    std::uint64_t sequence; // Successful enqueue order starts at 1, independent of MessageId.
    CorrelationId correlation;
    UpdateOrigin producer_origin;
    std::chrono::sys_time<std::chrono::nanoseconds> created_at;
    DataValue payload;
};

enum class StateScope { Invocation, WorkflowRun, SolutionSession };
enum class StateResetPolicy { DomainStart, DomainStartAndExplicit };
enum class StateCompatibilityPolicy { PreserveCompatible, AlwaysNewGeneration };
enum class StateMutationTiming { DuringComputation, OnResultAccepted };
enum class StateConcurrencyPolicy { Serialized, IsolatedPerInvocation, ToolManaged };
struct NodeStateDefinition {
    DataType schema;
    ConfigValue initial_value;
    StateScope scope;
    StateResetPolicy reset;
    StateCompatibilityPolicy compatibility;
    StateMutationTiming mutation;
    StateConcurrencyPolicy concurrency;
};
struct InvocationStateDomain {
    SessionInvocationKey invocation;
    bool operator==(const InvocationStateDomain&) const = default;
};
struct WorkflowRunStateDomain {
    SolutionSessionId session;
    WorkflowRunId run;
    bool operator==(const WorkflowRunStateDomain&) const = default;
};
struct SessionStateDomain {
    SolutionSessionId session;
    bool operator==(const SessionStateDomain&) const = default;
};
using StateDomain = std::variant<InvocationStateDomain, WorkflowRunStateDomain, SessionStateDomain>;
struct NodeStateKey {
    NodeId node;
    std::uint64_t generation;
    StateDomain domain;
    bool operator==(const NodeStateKey&) const = default;
};
enum class StateInitialization { NotInitialized, Initializing, Ready, Failed };
struct NodeState {
    NodeStateKey key;
    StateInitialization initialization;
    std::optional<DataValue> current_value;
    std::optional<Diagnostic> initialization_error;
};

[[nodiscard]] Result<std::monostate> validate_global_variable_definition(const GlobalVariableDefinition& definition);
[[nodiscard]] Result<std::monostate> validate_global_variable_definitions(std::span<const GlobalVariableDefinition> definitions);
[[nodiscard]] Result<std::monostate> validate_global_variable_entry(const GlobalVariableDefinition& definition, const GlobalVariableEntry& entry);
[[nodiscard]] Result<std::monostate> validate_queue_definition(const DataQueueDefinition& definition);
[[nodiscard]] Result<std::monostate> validate_queue_definitions(std::span<const DataQueueDefinition> definitions);
[[nodiscard]] Result<std::monostate> validate_queue_message(const DataQueueDefinition& definition, const QueueMessage& message);
[[nodiscard]] Result<std::monostate> validate_node_state_definition(const NodeStateDefinition& definition);
[[nodiscard]] Result<std::monostate> validate_node_state(const NodeStateDefinition& definition, const NodeState& state);
[[nodiscard]] Result<std::uint64_t> next_state_generation(std::uint64_t generation);

} // namespace visionpro::core
