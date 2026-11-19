#pragma once

#include <visionpro/core/state.hpp>

#include <stop_token>
#include <vector>

namespace visionpro::core {

struct GlobalWrite { VariableId variable; DataValue value; };
struct QueueRead { QueueId queue; std::size_t count; DataType expected_schema; };
struct QueuePreview {
    QueueId queue;
    std::vector<QueueMessage> messages; // Oldest first, immutable payload handles.
    std::uint64_t enqueued{};
    std::uint64_t received{};
    std::uint64_t dropped_oldest{};
    std::uint64_t rejected{};
};

// A session's typed shared-data view. Definitions remain valid for this view's lifetime.
class SharedData {
public:
    virtual ~SharedData() = default;
    [[nodiscard]] virtual const GlobalVariableDefinition* variable_definition(VariableId id) const noexcept = 0;
    [[nodiscard]] virtual const DataQueueDefinition* queue_definition(QueueId id) const noexcept = 0;
    [[nodiscard]] virtual Result<GlobalVariableEntry> read(VariableId id) const = 0;
    [[nodiscard]] virtual Result<std::vector<GlobalVariableEntry>> read_many(std::span<const VariableId> ids) const = 0;
    [[nodiscard]] virtual Result<std::vector<GlobalVariableEntry>> read_all() const = 0;
    [[nodiscard]] virtual Result<GlobalVariableEntry> write(VariableId id, const DataValue& value, const UpdateOrigin& origin) = 0;
    [[nodiscard]] virtual Result<std::vector<GlobalVariableEntry>> write_many(std::span<const GlobalWrite> updates,
        const UpdateOrigin& origin) = 0;
    [[nodiscard]] virtual Result<QueueMessage> send(QueueId id, const DataValue& payload, const CorrelationId& correlation,
        const UpdateOrigin& producer, std::stop_token stop = {}) = 0;
    // A batch preserves request order, then FIFO order within each requested queue.
    // In Wait mode the passed token can interrupt the wait without consuming anything.
    [[nodiscard]] virtual Result<std::vector<QueueMessage>> receive_many(std::span<const QueueRead> requests,
        const UpdateOrigin& consumer, std::stop_token stop = {}) = 0;
    [[nodiscard]] Result<std::vector<QueueMessage>> receive(const QueueRead& request, const UpdateOrigin& consumer,
        std::stop_token stop = {})
    { return receive_many(std::span{&request, 1}, consumer, stop); }
    [[nodiscard]] virtual Result<QueuePreview> preview(QueueId id) const = 0;
};

} // namespace visionpro::core
