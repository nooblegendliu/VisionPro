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
using FixedSelectionStep = std::variant<FieldSelection, ElementSelection>;
using FixedSelector = std::vector<FixedSelectionStep>;
enum class SystemField { ExecutionStatus, Judgement, Diagnostics, Elapsed };

// References are values with stable IDs. Index sources admit only a fixed path.
template<class Path> struct OutputReferenceWith {
    core::WorkflowId workflow;
    core::NodeId node;
    std::variant<core::OutputId, SystemField> output;
    Path selector;
    bool operator==(const OutputReferenceWith&) const = default;
};
template<class Path> struct ScopedInputReferenceWith {
    core::WorkflowId workflow;
    std::optional<core::NodeId> group;
    core::InputId input;
    Path selector;
    bool operator==(const ScopedInputReferenceWith&) const = default;
};
template<class Path> struct GlobalReferenceWith {
    core::VariableId variable;
    std::optional<std::uint64_t> required_revision;
    Path selector;
    bool operator==(const GlobalReferenceWith&) const = default;
};
using IndexOutputReference = OutputReferenceWith<FixedSelector>;
using IndexInputReference = ScopedInputReferenceWith<FixedSelector>;
using IndexGlobalReference = GlobalReferenceWith<FixedSelector>;
using IndexSource = std::variant<IndexOutputReference, IndexInputReference, IndexGlobalReference>;
struct DynamicElementSelection { IndexSource source; bool operator==(const DynamicElementSelection&) const = default; };
using SelectionStep = std::variant<FieldSelection, ElementSelection, DynamicElementSelection>;
using Selector = std::vector<SelectionStep>;
using OutputReference = OutputReferenceWith<Selector>;
using ScopedInputReference = ScopedInputReferenceWith<Selector>;
using GlobalReference = GlobalReferenceWith<Selector>;
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

[[nodiscard]] core::Result<IndexSource> make_index_source(const BindingSource& source);
[[nodiscard]] BindingSource index_binding_source(const IndexSource& index);
[[nodiscard]] const Selector* binding_selector(const BindingSource& source) noexcept;
[[nodiscard]] core::BindingSourceKind source_kind(const BindingSource& source) noexcept;
// Shape only: metadata, source existence, selectors and dependency graphs are S2.5/S2.6.
[[nodiscard]] core::Result<std::monostate> validate_binding_shape(const Binding& binding);

} // namespace visionpro::workflow
