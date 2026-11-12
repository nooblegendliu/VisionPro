#pragma once

#include <visionpro/workflow/result_store.hpp>
#include <visionpro/workflow/validation.hpp>

namespace visionpro::workflow {

// A runtime scope's owner is a static Group, or nullopt for the Workflow.
// Same-owner child scopes can model explicit outer visibility without a loop scheduler.
struct ResolutionScope {
    core::ExecutionScopeId id;
    std::optional<core::ExecutionScopeId> parent;
    std::optional<core::NodeId> group;
    std::vector<RunInput> inputs;
};
struct AwaitingSource {}; // No actual call identity yet; not a fabricated NodeResult.
struct SourceSelection {
    core::NodeId node;
    core::ExecutionScopeId scope;
    std::variant<AwaitingSource, core::InvocationId, MissingReason> state;
};
struct ResolutionContext {
    RunIdentity run;
    core::InvocationKey consumer;
    std::vector<ResolutionScope> scopes;
    std::vector<core::ExecutionScopeId> visible_outer_scopes;
    std::vector<SourceSelection> sources; // One explicit selection per source node.
    std::vector<core::GlobalVariableEntry> globals; // Caller-supplied read snapshot; never initialized implicitly.
};

struct ConstantTrace { bool defaulted{false}; };
struct ResultTrace {
    OutputReference reference;
    RunIdentity run;
    core::ExecutionScopeId scope;
    std::optional<core::InvocationId> invocation;
    std::optional<core::AttemptId> attempt;
};
struct PublicInputTrace {
    ScopedInputReference reference;
    RunIdentity run;
    core::ExecutionScopeId scope;
    InputOrigin origin;
};
struct IndependentInputTrace { core::InputId input; RunIdentity run; InputOrigin origin; };
struct GlobalReadTrace { core::VariableId variable; std::uint64_t revision; core::UpdateOrigin update_origin; };
struct SourceTrace {
    std::size_t source_index;
    std::variant<ConstantTrace, ResultTrace, PublicInputTrace, GlobalReadTrace, core::ResourceReference,
        IndependentInputTrace> origin;
    std::optional<MissingReason> accepted_absence;
};
// Preserve the complete inherited region expression, including its corrections
// and exclusions. Geometry evaluation remains an algorithm responsibility.
struct ResolvedRegionReference {
    core::InheritedRegion reference;
    core::RegionDefinition value;
    ResultIdentity identity;
    std::size_t source_index{0};
    std::vector<core::DiagnosticPathStep> path;
};
struct ResolvedBinding {
    core::DataValue value;
    std::vector<SourceTrace> sources;
    std::vector<ResolvedRegionReference> regions;
};
struct PendingBinding { std::vector<SourceTrace> sources; };
struct UnavailableBinding {
    OutputAbsenceReason reason;
    core::Diagnostic diagnostic;
    std::vector<SourceTrace> sources;
};
struct BindingFailure {
    core::Diagnostic diagnostic;
    std::vector<core::Diagnostic> causes;
    std::vector<SourceTrace> sources;
};
using BindingResolution = std::variant<ResolvedBinding, PendingBinding, UnavailableBinding, BindingFailure>;

class BindingResolver final {
public:
    BindingResolver(const BindingResolver&) noexcept = default;
    BindingResolver& operator=(const BindingResolver&) noexcept = default;
    [[nodiscard]] static core::Result<BindingResolver> create(const ConfigSnapshot& snapshot);
    // Also usable for zero-argument tools before preparing a fixed invocation.
    [[nodiscard]] core::Result<std::monostate> validate_context(const ResolutionContext& context,
        const ResultStore& results) const;
    [[nodiscard]] BindingResolution resolve(const ResolutionContext& context,
        const BindingTarget& target, const ResultStore& results) const;
    [[nodiscard]] const ConfigSnapshot& snapshot() const noexcept { return snapshot_; }
private:
    friend class ExecutionPlan;
    BindingResolver(ConfigSnapshot snapshot, std::shared_ptr<const ValidationIndex> index,
        std::optional<RunRequest> request = {})
        : snapshot_(std::move(snapshot)), index_(std::move(index)), request_(std::move(request)) {}
    ConfigSnapshot snapshot_;
    std::shared_ptr<const ValidationIndex> index_;
    std::optional<RunRequest> request_;
};

} // namespace visionpro::workflow
