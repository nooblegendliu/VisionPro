#pragma once

#include <visionpro/core/tool_descriptor.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace visionpro::workflow {

struct FieldSelection { core::FieldId field; bool operator==(const FieldSelection&) const = default; };
struct ElementSelection { std::size_t index; bool operator==(const ElementSelection&) const = default; };
using SelectionStep = std::variant<FieldSelection, ElementSelection>;
using Selector = std::vector<SelectionStep>;
enum class SystemField { ExecutionStatus, Judgement, Diagnostics, Elapsed };

// Configuration references never contain run/attempt IDs or editable names.
struct OutputReference {
    core::WorkflowId workflow;
    core::NodeId node;
    std::variant<core::OutputId, SystemField> output;
    Selector selector;
    bool operator==(const OutputReference&) const = default;
};
struct ScopedInputReference {
    core::WorkflowId workflow;
    std::optional<core::NodeId> group; // nullopt addresses this workflow's public inputs.
    core::InputId input;
    Selector selector;
    bool operator==(const ScopedInputReference&) const = default;
};
struct GlobalReference {
    core::VariableId variable;
    std::optional<std::uint64_t> required_revision; // nullopt: read and trace the current revision.
    Selector selector;
    bool operator==(const GlobalReference&) const = default;
};
using BindingTarget = std::variant<core::InputId, core::ParameterId>;
using BindingSource = std::variant<core::ConfigValue, OutputReference, ScopedInputReference,
    core::ResourceReference, GlobalReference>;
enum class MissingReason { NotSelected, Disabled, ToolDidNotProduce, ConditionNotMet };
struct AvailabilityPolicy {
    // Empty means require an available value. Pending/errors never become absence.
    std::vector<MissingReason> accepted_absence;
};
struct Binding {
    BindingTarget target;
    std::vector<BindingSource> sources;
    core::BindingCardinality cardinality{core::BindingCardinality::SingleSource};
    core::ConversionKind conversion{core::ConversionKind::Exact};
    AvailabilityPolicy availability;
};

[[nodiscard]] core::BindingSourceKind source_kind(const BindingSource& source) noexcept;
// Shape only: metadata, source existence, selectors and dependency graphs are S2.5/S2.6.
[[nodiscard]] core::Result<std::monostate> validate_binding_shape(const Binding& binding);

} // namespace visionpro::workflow
