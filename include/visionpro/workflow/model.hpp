#pragma once

#include <visionpro/workflow/binding.hpp>

#include <cstdint>
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
struct ToolConfiguration { core::ToolTypeId type; std::uint32_t config_version; };
struct GroupConfiguration {
    PublicInterface interface;
    std::vector<core::NodeId> entries;
    std::vector<OutputMapping> outputs;
    std::vector<ControlOutputMapping> control_exports;
};
struct NodeResourceBinding { core::SchemaId slot; core::ResourceReference resource; };
enum class JoinPolicy { AllActivated };
struct Node {
    core::NodeId id;
    std::string name;
    std::variant<ToolConfiguration, GroupConfiguration> configuration;
    std::vector<Binding> bindings;
    std::vector<NodeResourceBinding> resources;
    bool enabled{true};
    JoinPolicy join{JoinPolicy::AllActivated};
};
struct ControlLink {
    core::ControlLinkId id;
    core::NodeId source;
    // nullopt means unconditional success; a named outlet is selected explicitly.
    std::optional<core::ControlOutputId> output;
    core::NodeId target;
};
struct Containment { core::NodeId parent; core::NodeId child; };
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
};
struct NodePlacement { core::NodeId node; double x; double y; };
struct WorkflowLayout { core::WorkflowId workflow; std::vector<NodePlacement> nodes; };
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
};

// Borrowed pointers remain valid only while their editable owner is unchanged/alive.
[[nodiscard]] const Workflow* find_workflow(const Solution& solution, const core::WorkflowId& id) noexcept;
[[nodiscard]] const Node* find_node(const Workflow& workflow, const core::NodeId& id) noexcept;
[[nodiscard]] core::Result<std::vector<core::NodeId>> children(const Workflow& workflow,
    std::optional<core::NodeId> parent = std::nullopt);
[[nodiscard]] core::Result<std::vector<core::NodeId>> descendants(const Workflow& workflow, const core::NodeId& group);
[[nodiscard]] core::Result<std::monostate> validate_public_interface(const PublicInterface& interface);
// Solution envelope, stable identities and the shared definition catalogs.
[[nodiscard]] core::Result<std::monostate> validate_global_invariants(const Solution& solution);
// Local shape, identity and containment checks only. Not proof of executability:
// references, boundary/type compatibility and control/data cycles are checked in S2.5.
[[nodiscard]] core::Result<std::monostate> validate_model_shape(const Solution& solution);

} // namespace visionpro::workflow
