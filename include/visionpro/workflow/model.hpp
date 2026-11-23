#pragma once

#include <visionpro/workflow/binding.hpp>
#include <visionpro/core/camera_mode.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace visionpro::workflow {

struct PublicInterface {
    core::SchemaId id;
    std::uint32_t version;
    std::vector<core::InputDescriptor> inputs;
    std::vector<core::OutputDescriptor> outputs;
    std::vector<core::ControlOutputDescriptor> control_outputs;
};
struct OutputMapping { core::OutputId output; OutputReference source; };
struct ControlOutputMapping {
    core::ControlOutputId output;
    core::NodeId source;
    std::optional<core::ControlOutputId> source_output;
};
struct ToolConfiguration {
    core::ToolTypeId type;
    std::uint32_t config_version;
    std::vector<core::SharedTargetId> shared_targets{};
};
enum class LoopExportMode { First, Last, Collect, Sum };
struct LoopExport {
    core::OutputId output;
    LoopExportMode mode;
    std::optional<core::ConfigValue> initial{};
};
struct LoopConfiguration {
    std::int64_t step{1};
    bool inclusive_end{false};
    std::int64_t interval_ms{0};
    std::vector<LoopExport> exports;
    std::optional<core::NodeId> break_source{};
};
struct GroupConfiguration {
    PublicInterface interface;
    std::vector<core::NodeId> entries;
    std::vector<OutputMapping> outputs;
    std::vector<ControlOutputMapping> control_exports;
    std::optional<LoopConfiguration> loop{};
};
struct NodeResourceBinding { core::SchemaId slot; core::ResourceReference resource; };
enum class JoinPolicy { AllActivated };
struct NodeExecutionPolicy {
    std::uint32_t timeout_ms{0}; // Per attempt. Zero disables the deadline.
    std::uint32_t max_retries{0}; // Additional attempts after the initial call.
    std::optional<core::ControlOutputId> error_output{};
    bool operator==(const NodeExecutionPolicy&) const = default;
};
struct Node {
    core::NodeId id;
    std::string name;
    std::variant<ToolConfiguration, GroupConfiguration> configuration;
    std::vector<Binding> bindings;
    std::vector<NodeResourceBinding> resources;
    bool enabled{true};
    JoinPolicy join{JoinPolicy::AllActivated};
    NodeExecutionPolicy execution_policy{};
};
struct ControlLink {
    core::ControlLinkId id;
    core::NodeId source;
    // nullopt means unconditional success; a named outlet is selected explicitly.
    std::optional<core::ControlOutputId> output;
    core::NodeId target;
};
struct Containment { core::NodeId parent; core::NodeId child; };
enum class AdmissionOverflow { Busy, Reject };
struct WorkflowAdmission {
    std::uint32_t max_in_flight{1}; // A started run occupies a slot through physical cleanup.
    std::uint32_t queue_capacity{0}; // Accepted FIFO waiters, separate from started runs.
    AdmissionOverflow overflow{AdmissionOverflow::Busy};
    bool operator==(const WorkflowAdmission&) const = default;
};
enum class TriggerMode { Continuous, External };
struct CameraTriggerBinding {
    core::ResourceId device;
    core::InputId image_input;
    core::CoordinateSystemId coordinate_frame;
    core::CameraMode mode;
    std::uint32_t timeout_ms;
    bool operator==(const CameraTriggerBinding&) const = default;
};
struct WorkflowTrigger {
    TriggerMode mode;
    core::SchemaId source;
    core::InputId frame_input; // The controlled source supplies this Int64 public input for each run.
    std::uint32_t interval_ms{0}; // Positive for continuous ticks, zero for external events.
    std::optional<CameraTriggerBinding> camera{};
    bool operator==(const WorkflowTrigger&) const = default;
};
struct Workflow {
    core::WorkflowId id;
    std::string name;
    PublicInterface interface;
    std::vector<Node> nodes; // Editable order, never an identity or execution schedule.
    std::vector<ControlLink> control_links;
    std::vector<Containment> containment; // Group ownership, independent of both graphs.
    std::vector<core::NodeId> entries;
    std::vector<OutputMapping> outputs;
    bool enabled{true};
    std::vector<ControlOutputMapping> control_exports;
    WorkflowAdmission admission{};
    std::optional<WorkflowTrigger> trigger{};
};
struct DisplayColor {
    std::uint8_t red, green, blue;
    bool operator==(const DisplayColor&) const = default;
};
using ClassPalette = std::map<std::uint8_t, DisplayColor>;
struct NodePlacement {
    core::NodeId node;
    double x;
    double y;
    ClassPalette class_palette{};
};
struct WorkflowLayout { core::WorkflowId workflow; std::vector<NodePlacement> nodes; };
inline constexpr std::uint32_t solution_graph_manifest_version = 2;
struct WorkflowControlLink {
    core::WorkflowId source;
    core::WorkflowId target;
    std::optional<core::ControlOutputId> output{};
    bool operator==(const WorkflowControlLink&) const = default;
};
struct WorkflowPortBinding {
    core::WorkflowId source;
    core::OutputId output;
    core::WorkflowId target;
    core::InputId input;
    bool operator==(const WorkflowPortBinding&) const = default;
};
struct SolutionGraph {
    std::vector<core::WorkflowId> entries;
    std::vector<WorkflowControlLink> links;
    std::vector<WorkflowPortBinding> bindings;
    bool operator==(const SolutionGraph&) const = default;
};
struct Solution {
    core::SolutionId id;
    std::string name;
    std::uint32_t format_version;
    std::uint64_t revision;
    std::vector<Workflow> workflows;
    std::vector<core::ResourceDefinition> resources;
    std::vector<core::GlobalVariableDefinition> variables;
    std::vector<core::DataQueueDefinition> queues;
    std::vector<core::CoordinateFrameDefinition> coordinates;
    std::vector<WorkflowLayout> layouts;
    std::optional<SolutionGraph> graph{};
};

// Borrowed pointers remain valid only while their editable owner is unchanged/alive.
[[nodiscard]] const Workflow* find_workflow(const Solution& solution, const core::WorkflowId& id) noexcept;
[[nodiscard]] const Node* find_node(const Workflow& workflow, const core::NodeId& id) noexcept;
[[nodiscard]] const NodePlacement* find_placement(const Solution& solution, const core::WorkflowId& workflow,
    const core::NodeId& node) noexcept;
[[nodiscard]] core::Result<std::vector<core::NodeId>> children(const Workflow& workflow,
    std::optional<core::NodeId> parent = std::nullopt);
[[nodiscard]] core::Result<std::vector<core::NodeId>> descendants(const Workflow& workflow, const core::NodeId& group);
[[nodiscard]] core::Result<std::monostate> validate_public_interface(const PublicInterface& interface);
[[nodiscard]] core::Result<std::monostate> validate_solution_graph(const Solution& solution);
[[nodiscard]] core::Result<std::monostate> validate_workflow_admission(const WorkflowAdmission& admission);
[[nodiscard]] core::Result<std::monostate> validate_workflow_trigger(const Workflow& workflow);
[[nodiscard]] core::Result<std::monostate> validate_node_execution_policy(const NodeExecutionPolicy& policy,
    const core::ToolDescriptor& tool);
// Solution envelope, stable identities and the shared definition catalogs.
[[nodiscard]] core::Result<std::monostate> validate_global_invariants(const Solution& solution);
// Local shape, identity and containment checks only. Not proof of executability:
// references, boundary/type compatibility and control/data cycles are checked in S2.5.
[[nodiscard]] core::Result<std::monostate> validate_model_shape(const Solution& solution);

} // namespace visionpro::workflow
