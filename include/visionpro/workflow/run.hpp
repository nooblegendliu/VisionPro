#pragma once

#include <visionpro/core/state.hpp>
#include <visionpro/core/timing.hpp>
#include <visionpro/core/tool_output.hpp>
#include <visionpro/workflow/snapshot.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace visionpro::workflow {

struct WorkflowTarget { core::WorkflowId workflow; bool operator==(const WorkflowTarget&) const = default; };
struct NodeTarget {
    core::WorkflowId workflow;
    core::NodeId node;
    bool operator==(const NodeTarget&) const = default;
};
using RunTarget = std::variant<WorkflowTarget, NodeTarget>;
enum class TriggerKind { Manual, Continuous, External, Debug };
struct RunTrigger { TriggerKind kind; core::SchemaId source; };
enum class DebugStatePolicy { Isolated, Active };
struct RunInput { core::InputId id; core::DataValue value; };
enum class InputOrigin { Provided, Default };
struct FixedRunInput { core::InputId id; core::DataValue value; InputOrigin origin; };

// A prepared request owns its snapshot, inputs and supplied global read values.
// The run backend assigns an identity upon admission.
class RunRequest final {
public:
    RunRequest(const RunRequest&) noexcept = default;
    RunRequest& operator=(const RunRequest&) noexcept = default;
    RunRequest(RunRequest&& other) noexcept : storage_(other.storage_) {}
    RunRequest& operator=(RunRequest&& other) noexcept { storage_ = other.storage_; return *this; }
    [[nodiscard]] static core::Result<RunRequest> create(const ConfigSnapshot& snapshot,
        RunTarget target, const std::vector<RunInput>& inputs, RunTrigger trigger,
        std::optional<core::CorrelationId> correlation = std::nullopt,
        std::optional<DebugStatePolicy> debug_state = std::nullopt,
        std::span<const core::GlobalVariableEntry> globals = {});
    [[nodiscard]] const ConfigSnapshot& snapshot() const noexcept;
    [[nodiscard]] const RunTarget& target() const noexcept;
    [[nodiscard]] std::span<const FixedRunInput> inputs() const noexcept;
    [[nodiscard]] const FixedRunInput* input(const core::InputId& id) const noexcept;
    // The read snapshot contains only the explicitly supplied current values.
    [[nodiscard]] std::span<const core::GlobalVariableEntry> globals() const noexcept;
    [[nodiscard]] const RunTrigger& trigger() const noexcept;
    [[nodiscard]] const std::optional<core::CorrelationId>& correlation() const noexcept;
    [[nodiscard]] std::optional<DebugStatePolicy> debug_state() const noexcept;
private:
    struct Storage;
    explicit RunRequest(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
};

struct RunIdentity {
    SnapshotIdentity snapshot;
    core::WorkflowId workflow;
    core::WorkflowRunId run;
    auto operator<=>(const RunIdentity&) const = default;
};
struct ResultIdentity {
    RunIdentity run;
    core::InvocationKey invocation;
    core::AttemptId attempt;
    auto operator<=>(const ResultIdentity&) const = default;
};
struct OutputAddress {
    core::SessionInvocationKey invocation;
    core::OutputId output;
    auto operator<=>(const OutputAddress&) const = default;
};
[[nodiscard]] OutputAddress output_address(const ResultIdentity& identity, core::OutputId output);

enum class ExecutionStatus { Pending, Running, Succeeded, Failed, Skipped, Cancelled, TimedOut };
enum class Judgement { NotEvaluated, OK, NG };
using core::OutputAbsenceReason;
using core::NotProduced;
using core::InvalidOutput;
using core::OutputContent;
struct OutputSlot { core::OutputId id; core::DataType type; OutputContent content; };
struct NodeResultData {
    ResultIdentity identity;
    ExecutionStatus status;
    Judgement judgement;
    std::vector<OutputSlot> outputs;
    std::vector<core::Diagnostic> diagnostics;
    std::optional<std::chrono::nanoseconds> elapsed{};
    std::vector<core::ControlOutputId> selected_control_outputs{};
    std::vector<core::TimingMeasurement> timings{};
};

// A detached terminal-call record. The result store checks declared output
// Schema, and the run backend controls publication.
class NodeResult final {
public:
    NodeResult(const NodeResult&) noexcept = default;
    NodeResult& operator=(const NodeResult&) noexcept = default;
    NodeResult(NodeResult&& other) noexcept : data_(other.data_) {}
    NodeResult& operator=(NodeResult&& other) noexcept { data_ = other.data_; return *this; }
    [[nodiscard]] static core::Result<NodeResult> create(const NodeResultData& data);
    [[nodiscard]] const NodeResultData& data() const noexcept { return *data_; }
    [[nodiscard]] const OutputSlot* output(const core::OutputId& id) const noexcept;
private:
    explicit NodeResult(std::shared_ptr<const NodeResultData> data) : data_(std::move(data)) {}
    std::shared_ptr<const NodeResultData> data_;
};

} // namespace visionpro::workflow
