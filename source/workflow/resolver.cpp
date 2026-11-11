#include <visionpro/workflow/resolver.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include "runtime_values.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <type_traits>

namespace visionpro::workflow {
namespace {
using namespace core;
using Check = Result<std::monostate>;
using Owner = std::optional<NodeId>;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
bool failed(const Check& checked) { return std::holds_alternative<Diagnostic>(checked); }
template<class T, class Id> const T* find(std::span<const T> values, const Id& id)
{
    const auto it = std::ranges::find(values, id, &T::id);
    return it == values.end() ? nullptr : &*it;
}
template<class T, class Id> const T* find(const std::vector<T>& values, const Id& id)
{ return find<T, Id>(std::span<const T>{values}, id); }
template<class T> bool contains(const std::vector<T>& values, const T& value)
{ return std::ranges::find(values, value) != values.end(); }
Owner parent(const Workflow& flow, NodeId node)
{
    const auto it = std::ranges::find(flow.containment, node, &Containment::child);
    return it == flow.containment.end() ? Owner{} : it->parent;
}
template<SchemaIdentity T> T key(std::string_view name)
{ return std::get<T>(SchemaIdFactory::parse<T>(name)); }

Result<DataValue> selected(DataValue value, const Selector& selector)
{
    DiagnosticLocation where;
    for (const auto& step : selector) {
        if (const auto* field = std::get_if<FieldSelection>(&step)) {
            where.path.emplace_back(field->field);
            if (value.type().kind() != TypeKind::Record) {
                auto failure = error(ErrorCode::InvalidSelector, "Field selection requires a Record; Optional is not implicitly unwrapped.");
                failure.location = where; return failure;
            }
            const auto* member = value.field(field->field);
            if (!member) { auto failure = error(ErrorCode::UnknownField, "Runtime value has no selected field."); failure.location = where; return failure; }
            value = *member;
        } else {
            const auto index = std::get<ElementSelection>(step).index;
            where.path.emplace_back(DiagnosticElementIndex{index});
            const auto* elements = value.elements();
            if (!elements) { auto failure = error(ErrorCode::InvalidSelector, "Element selection requires a List."); failure.location = where; return failure; }
            if (index >= elements->size()) { auto failure = error(ErrorCode::IndexOutOfRange, "Fixed index exceeds the actual current List length."); failure.location = where; return failure; }
            value = (*elements)[index];
        }
    }
    return value;
}
Result<DataValue> system_value(const NodeResultData& result, SystemField field)
{
    auto schema = system_field_type(field);
    if (const auto* failure = std::get_if<Diagnostic>(&schema)) { return *failure; }
    const auto& type = std::get<DataType>(schema);
    switch (field) {
    case SystemField::ExecutionStatus: {
        const auto status = [&]() -> std::string_view {
            switch (result.status) {
            case ExecutionStatus::Pending: return "Pending";
            case ExecutionStatus::Running: return "Running";
            case ExecutionStatus::Succeeded: return "Succeeded";
            case ExecutionStatus::Failed: return "Failed";
            case ExecutionStatus::Skipped: return "Skipped";
            case ExecutionStatus::Cancelled: return "Cancelled";
            case ExecutionStatus::TimedOut: return "TimedOut";
            }
            return {};
        }();
        return DataValue::enumeration(type, key<EnumMemberId>(status));
    }
    case SystemField::Judgement: {
        const auto judgement = result.judgement == Judgement::OK ? "OK" : result.judgement == Judgement::NG ? "NG" : "NotEvaluated";
        return DataValue::enumeration(type, key<EnumMemberId>(judgement));
    }
    case SystemField::Elapsed: return DataValue::integer(result.elapsed.count());
    case SystemField::Diagnostics: {
        std::vector<DataValue> values;
        const auto& record = *type.element_type();
        const auto& severity_type = find(record.fields(), key<FieldId>("severity"))->type;
        for (const auto& diagnostic : result.diagnostics) {
            const auto name = diagnostic.severity == Severity::Error ? "Error" : diagnostic.severity == Severity::Warning ? "Warning" : "Info";
            auto value = DataValue::record(record, {
                {key<FieldId>("code"), DataValue::integer(static_cast<std::uint32_t>(diagnostic.code))},
                {key<FieldId>("severity"), std::get<DataValue>(DataValue::enumeration(severity_type, key<EnumMemberId>(name)))},
                {key<FieldId>("message"), std::get<DataValue>(DataValue::string(diagnostic.message))}});
            if (const auto* failure = std::get_if<Diagnostic>(&value)) { return *failure; }
            values.push_back(std::get<DataValue>(value));
        }
        return DataValue::list(record, values);
    }
    }
    return error(ErrorCode::InvalidBinding, "Unknown system field.");
}

struct Absence { OutputAbsenceReason reason; std::size_t trace; };
struct Failure { Diagnostic diagnostic; std::vector<Diagnostic> causes; };
struct Evaluation {
    std::optional<DataValue> value;
    bool pending{false};
    std::vector<Absence> absences;
    std::optional<Failure> failure;
    std::optional<ResultIdentity> identity;
};
Evaluation invalid(Diagnostic diagnostic, std::vector<Diagnostic> causes = {})
{ return {{}, false, {}, Failure{std::move(diagnostic), std::move(causes)}, {}}; }
Evaluation available(Result<DataValue> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { return invalid(*failure); }
    return {std::get<DataValue>(std::move(value)), false, {}, {}, {}};
}
void merge(Evaluation& target, Evaluation source)
{
    target.pending = target.pending || source.pending;
    target.absences.insert(target.absences.end(), source.absences.begin(), source.absences.end());
    if (!target.failure && source.failure) { target.failure = std::move(source.failure); }
}

class Resolution {
public:
    Resolution(const ConfigSnapshot& snapshot, const ValidationIndex& index,
        const ResolutionContext& context, const BindingTarget* target, const ResultStore& store)
        : snapshot_(snapshot), index_(index), context_(context), target_(target), store_(store) {}
    BindingResolution run();
    Check context_only()
    {
        auto result = validate_context();
        if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) { return contextualize(*diagnostic); }
        return result;
    }
private:
    Check validate_context();
    Check validate_scope_inputs(const ResolutionScope& scope);
    template<class Target> BindingResolution resolve(const Target& descriptor);
    Evaluation source(const BindingSource& value, std::size_t index);
    Evaluation output(const OutputReference& reference, std::size_t index);
    Evaluation configured(const ConfigValue& value, std::size_t index, bool defaulted);
    Evaluation inherited(const DataValue& value, std::size_t index);
    const PublicInterface& interface(const ResolutionScope& scope) const;
    bool ancestor(ExecutionScopeId outer, ExecutionScopeId inner) const;
    Diagnostic contextualize(Diagnostic failure) const;
    BindingFailure failure(Failure failure) const;
    const ConfigSnapshot& snapshot_;
    const ValidationIndex& index_;
    const ResolutionContext& context_;
    const BindingTarget* target_;
    const ResultStore& store_;
    const Workflow* flow_{nullptr};
    const Node* node_{nullptr};
    const ToolDescriptor* tool_{nullptr};
    const ResolutionScope* scope_{nullptr};
    std::vector<SourceTrace> traces_;
    std::vector<ResolvedRegionReference> regions_;
};

Diagnostic Resolution::contextualize(Diagnostic failure) const
{
    failure.location.solution = snapshot_.solution().id;
    failure.location.workflow = context_.run.workflow;
    failure.location.node = context_.consumer.node;
    if (target_) {
        if (const auto* input = std::get_if<InputId>(target_)) { failure.location.input = *input; }
        else { failure.location.parameter = std::get<ParameterId>(*target_); }
    }
    return failure;
}
BindingFailure Resolution::failure(Failure value) const
{ return {contextualize(std::move(value.diagnostic)), std::move(value.causes), traces_}; }
const PublicInterface& Resolution::interface(const ResolutionScope& scope) const
{
    return scope.group ? std::get<GroupConfiguration>(find_node(*flow_, *scope.group)->configuration).interface : flow_->interface;
}
bool Resolution::ancestor(ExecutionScopeId outer, ExecutionScopeId inner) const
{
    const auto* current = find(context_.scopes, inner);
    for (std::size_t steps = 0; current && current->parent && steps < context_.scopes.size(); ++steps) {
        if (*current->parent == outer) { return true; }
        current = find(context_.scopes, *current->parent);
    }
    return false;
}
Check Resolution::validate_scope_inputs(const ResolutionScope& scope)
{
    std::set<InputId> supplied;
    const auto& schema = interface(scope);
    for (const auto& input : scope.inputs) {
        const auto* declared = find(schema.inputs, input.id);
        if (!declared || !supplied.insert(input.id).second) {
            return error(ErrorCode::InvalidResolutionContext, "Scope input is undeclared or duplicated.");
        }
        for (auto checked : {detail::validate_runtime_value(declared->type, declared->constraints, input.value),
            detail::validate_runtime_domain(input.value, snapshot_.solution(), declared->semantics)}) {
            if (failed(checked)) { return checked; }
        }
    }
    for (const auto& input : schema.inputs) {
        if (!supplied.contains(input.id) && input.required) {
            return error(ErrorCode::MissingRequiredValue, "Current scope is missing a required public input; previous scope inputs are not reused.");
        }
    }
    return std::monostate{};
}
Check Resolution::validate_context()
{
    if (context_.run.snapshot != snapshot_.identity() || context_.run.snapshot.session != store_.session()
        || context_.consumer.workflow_run != context_.run.run) {
        return error(ErrorCode::ResultIdentityMismatch, "Resolution requires the exact current session, snapshot and run.");
    }
    if (auto known_run = store_.lookup(context_.run, context_.consumer);
        const auto* diagnostic = std::get_if<Diagnostic>(&known_run)) { return *diagnostic; }
    flow_ = find_workflow(snapshot_.solution(), context_.run.workflow);
    node_ = flow_ ? find_node(*flow_, context_.consumer.node) : nullptr;
    if (!flow_ || !flow_->enabled || !node_ || !node_->enabled) {
        return error(ErrorCode::InvalidResolutionContext, "Resolution consumer must be an enabled node in the current workflow.");
    }
    for (auto owner = parent(*flow_, node_->id); owner; owner = parent(*flow_, *owner)) {
        if (!find_node(*flow_, *owner)->enabled) { return error(ErrorCode::InvalidResolutionContext, "Consumer is inside a disabled Group."); }
    }
    if (const auto* tool = std::get_if<ToolConfiguration>(&node_->configuration)) { tool_ = snapshot_.find_tool(tool->type); }
    scope_ = find(context_.scopes, context_.consumer.scope);
    if (!scope_ || scope_->group != parent(*flow_, node_->id)) {
        return error(ErrorCode::ScopeViolation, "Current runtime scope does not match the consumer's static owner.");
    }
    std::set<ExecutionScopeId> ids;
    for (const auto& scope : context_.scopes) {
        if (!ids.insert(scope.id).second) { return error(ErrorCode::InvalidResolutionContext, "Runtime scope identities must be unique."); }
        if (scope.group) {
            const auto* group = find_node(*flow_, *scope.group);
            if (!group || !std::holds_alternative<GroupConfiguration>(group->configuration)) {
                return error(ErrorCode::ScopeViolation, "Runtime scope owner must be a declared Group.");
            }
        }
        if (scope.id.value() == 0) {
            if (scope.parent || scope.group) { return error(ErrorCode::InvalidResolutionContext, "Root scope must be the Workflow scope without a parent."); }
        } else {
            const auto* outer = scope.parent ? find(context_.scopes, *scope.parent) : nullptr;
            if (!outer || (outer->group != scope.group && (!scope.group || parent(*flow_, *scope.group) != outer->group))) {
                return error(ErrorCode::ScopeViolation, "Runtime scope must follow the static Group boundary or nest within the same owner.");
            }
            std::set<ExecutionScopeId> chain{scope.id};
            const auto* next = &scope;
            while (next->parent) {
                next = find(context_.scopes, *next->parent);
                if (!next || !chain.insert(next->id).second) { return error(ErrorCode::InvalidResolutionContext, "Runtime scope parent chain is dangling or cyclic."); }
            }
            if (next->id.value() != 0) { return error(ErrorCode::InvalidResolutionContext, "Runtime scope must descend from root scope zero."); }
        }
    }
    for (const auto& scope : context_.scopes) {
        if (auto checked = validate_scope_inputs(scope); failed(checked)) { return checked; }
    }
    std::set<ExecutionScopeId> visible;
    for (const auto id : context_.visible_outer_scopes) {
        const auto* outer = find(context_.scopes, id);
        if (!visible.insert(id).second || !outer || outer->group != scope_->group || !ancestor(id, scope_->id)) {
            return error(ErrorCode::ScopeViolation, "Visible outer scopes must be explicit same-owner ancestors, not siblings or Group internals.");
        }
    }
    std::set<NodeId> selections;
    for (const auto& selection : context_.sources) {
        const auto* producer = find_node(*flow_, selection.node);
        const auto* source_scope = find(context_.scopes, selection.scope);
        if (!selections.insert(selection.node).second || !producer || selection.node == node_->id) {
            return error(ErrorCode::InvalidResolutionContext, "Source selection must identify one distinct producer per node.");
        }
        if (!source_scope || source_scope->group != scope_->group || parent(*flow_, producer->id) != scope_->group
            || (selection.scope != scope_->id && !visible.contains(selection.scope))) {
            return error(ErrorCode::ScopeViolation, "Source selection crosses an undeclared runtime or static scope boundary.");
        }
        const auto* absence = std::get_if<MissingReason>(&selection.state);
        if (absence && *absence != MissingReason::NotSelected && *absence != MissingReason::Disabled) {
            return error(ErrorCode::InvalidResolutionContext, "Unknown source absence reason.");
        }
        if ((!producer->enabled && (!absence || *absence != MissingReason::Disabled))
            || (producer->enabled && absence && *absence == MissingReason::Disabled)) {
            return error(ErrorCode::InvalidResolutionContext, "Disabled source tracking must agree with the fixed configuration.");
        }
    }
    const auto scope_index = std::ranges::find_if(index_.scopes, [&](const auto& entry) {
        return entry.workflow == flow_->id && entry.group == scope_->group;
    });
    if (scope_index == index_.scopes.end() || !contains(scope_index->reachable, node_->id)) {
        return error(ErrorCode::IncompleteExecutionScope, "Resolution consumer is outside the validated control entry range.");
    }
    std::set<VariableId> variables;
    for (const auto& entry : context_.globals) {
        const auto* definition = find(snapshot_.solution().variables, entry.variable);
        if (!definition || !variables.insert(entry.variable).second) { return error(ErrorCode::InvalidResolutionContext, "Global read snapshot contains an unknown or duplicate variable."); }
        const auto session = std::visit([](const auto& origin) { return origin.session; }, entry.update_origin);
        if (session != context_.run.snapshot.session) { return error(ErrorCode::ResultIdentityMismatch, "Global read snapshot belongs to another solution session."); }
        if (auto checked = validate_global_variable_entry(*definition, entry); failed(checked)) { return checked; }
    }
    return std::monostate{};
}

Evaluation Resolution::output(const OutputReference& reference, std::size_t index)
{
    if (reference.workflow != flow_->id || parent(*flow_, reference.node) != scope_->group) {
        return invalid(error(ErrorCode::ScopeViolation, "Output reference cannot cross workflow or Group boundaries."));
    }
    const auto selection = std::ranges::find(context_.sources, reference.node, &SourceSelection::node);
    if (selection == context_.sources.end()) {
        return invalid(error(ErrorCode::InvalidResolutionContext, "Referenced producer has no explicit current source selection."));
    }
    const auto* invocation = std::get_if<InvocationId>(&selection->state);
    const auto trace_index = traces_.size();
    traces_.push_back({index, ResultTrace{reference, context_.run, selection->scope,
        invocation ? std::optional{*invocation} : std::nullopt, {}}, {}});
    if (std::holds_alternative<AwaitingSource>(selection->state)) { return {{}, true, {}, {}, {}}; }
    if (const auto* missing = std::get_if<MissingReason>(&selection->state)) {
        return {{}, false, {{*missing == MissingReason::Disabled ? OutputAbsenceReason::Disabled : OutputAbsenceReason::NotSelected, trace_index}}, {}, {}};
    }
    InvocationKey key{context_.run.run, reference.node, selection->scope, *invocation};
    auto lookup = store_.lookup(context_.run, key);
    if (const auto* diagnostic = std::get_if<Diagnostic>(&lookup)) { return invalid(*diagnostic); }
    const auto& result = std::get<std::optional<NodeResult>>(lookup);
    if (!result) { return {{}, true, {}, {}, {}}; }
    const auto& data = result->data();
    std::get<ResultTrace>(traces_.at(trace_index).origin).attempt = data.identity.attempt;
    Evaluation evaluated;
    if (const auto* field = std::get_if<SystemField>(&reference.output)) {
        evaluated = available(system_value(data, *field));
    } else if (data.status != ExecutionStatus::Succeeded) {
        auto causes = data.diagnostics;
        for (const auto& slot : data.outputs) {
            if (const auto* invalid = std::get_if<InvalidOutput>(&slot.content)) { causes.push_back(invalid->diagnostic); }
        }
        const auto code = data.status == ExecutionStatus::Cancelled ? ErrorCode::SourceCancelled
            : data.status == ExecutionStatus::TimedOut ? ErrorCode::SourceTimedOut : ErrorCode::SourceFailed;
        return invalid(error(code, "Current source call did not succeed; algorithm outputs cannot be used."), std::move(causes));
    } else {
        const auto* slot = result->output(std::get<OutputId>(reference.output));
        if (!slot) { return invalid(error(ErrorCode::InvalidNodeResult, "Current result lacks the declared output slot.")); }
        if (const auto* value = std::get_if<DataValue>(&slot->content)) { evaluated.value = *value; }
        else if (const auto* absence = std::get_if<NotProduced>(&slot->content)) {
            return {{}, false, {{absence->reason, trace_index}}, {}, {}};
        } else { return invalid(std::get<InvalidOutput>(slot->content).diagnostic); }
    }
    if (evaluated.value) { evaluated = available(selected(*evaluated.value, reference.selector)); }
    evaluated.identity = data.identity;
    return evaluated;
}

Evaluation Resolution::inherited(const DataValue& value, std::size_t index)
{
    Evaluation status;
    const auto visit = [&](const DataValue& child, DiagnosticPathStep step) {
        const auto first_region = regions_.size();
        auto checked = inherited(child, index);
        for (auto region = first_region; region < regions_.size(); ++region) {
            regions_[region].path.insert(regions_[region].path.begin(), step);
        }
        if (checked.failure) { checked.failure->diagnostic.location.path.insert(checked.failure->diagnostic.location.path.begin(), step); }
        merge(status, std::move(checked));
    };
    if (const auto* child = value.optional_value()) { return inherited(*child, index); }
    if (const auto* elements = value.elements()) {
        for (std::size_t item = 0; item < elements->size(); ++item) { visit((*elements)[item], DiagnosticElementIndex{item}); }
    }
    if (const auto* fields = value.fields()) { for (const auto& field : *fields) { visit(field.value, field.id); } }
    if (const auto* region = value.as_region2d()) {
        bool masks = false;
        bool geometric = false;
        const auto classify = [&](const RegionSource& source) {
            if (const auto* primitive = std::get_if<RegionPrimitive>(&source)) {
                if (std::holds_alternative<MaskConfig>(primitive->geometry)) { masks = true; }
                else if (!std::holds_alternative<EmptyRegionShape>(primitive->geometry)) { geometric = true; }
            }
        };
        classify(region->source);
        for (const auto& exclusion : region->exclusions) { classify(exclusion); }
        const auto read_region = [&](const RegionSource& source, std::vector<DiagnosticPathStep> path) {
            const auto* inherited_region = std::get_if<InheritedRegion>(&source);
            if (!inherited_region) { return; }
            auto checked = output({inherited_region->workflow, inherited_region->node, inherited_region->output, {}}, index);
            if (checked.value) {
                const auto* resolved = checked.value->as_region2d();
                if (!resolved || !checked.identity) { checked = invalid(error(ErrorCode::TypeMismatch, "Inherited region source must provide Region2D with an exact result identity.")); }
                else if (resolved->coordinate_frame != inherited_region->coordinate_frame || resolved->unit != inherited_region->unit) {
                    checked = invalid(error(ErrorCode::CoordinateMismatch, "Inherited runtime region has different coordinates before correction."));
                } else {
                    if (tool_ && tool_->region_capabilities) {
                        auto valid = validate_region(*resolved, *tool_->region_capabilities);
                        if (const auto* diagnostic = std::get_if<Diagnostic>(&valid)) { checked = invalid(*diagnostic); }
                    }
                    if (!checked.failure) {
                        classify(resolved->source);
                        for (const auto& exclusion : resolved->exclusions) { classify(exclusion); }
                        regions_.push_back({*inherited_region, *resolved, *checked.identity, index, path});
                    }
                }
            }
            if (checked.failure) {
                auto& location = checked.failure->diagnostic.location;
                location.region = region->id;
                location.path.insert(location.path.begin(), path.begin(), path.end());
            }
            merge(status, std::move(checked));
        };
        read_region(region->source, {key<FieldId>("source")});
        for (std::size_t item = 0; item < region->exclusions.size(); ++item) {
            read_region(region->exclusions[item], {key<FieldId>("exclusions"), DiagnosticElementIndex{item}});
        }
        if (masks && geometric && tool_ && tool_->region_capabilities
            && tool_->region_capabilities->geometry_mask_policy == GeometryMaskPolicy::Exclusive) {
            auto diagnostic = error(ErrorCode::UnsupportedRegion, "Resolved ROI composition mixes geometry and masks without declared composition support.");
            diagnostic.location.region = region->id;
            merge(status, invalid(std::move(diagnostic)));
        }
    }
    return status;
}
Evaluation Resolution::configured(const ConfigValue& value, std::size_t index, bool defaulted)
{
    traces_.push_back({index, ConstantTrace{defaulted}, {}});
    auto result = available(detail::materialize_config_value(value));
    if (result.value) { merge(result, inherited(*result.value, index)); }
    return result;
}
Evaluation Resolution::source(const BindingSource& source, std::size_t index)
{
    return std::visit([&](const auto& value) -> Evaluation {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ConfigValue>) { return configured(value, index, false); }
        else if constexpr (std::is_same_v<T, OutputReference>) { return output(value, index); }
        else if constexpr (std::is_same_v<T, ScopedInputReference>) {
            if (value.workflow != flow_->id || value.group != scope_->group) { return invalid(error(ErrorCode::ScopeViolation, "Public input reference must address this exact scope owner.")); }
            const auto* declared = find(interface(*scope_).inputs, value.input);
            if (!declared) { return invalid(error(ErrorCode::UnknownTarget, "Public input is not declared.")); }
            const auto* provided = find(scope_->inputs, value.input);
            traces_.push_back({index, PublicInputTrace{value, context_.run, scope_->id, provided ? InputOrigin::Provided : InputOrigin::Default}, {}});
            auto read = provided ? available(provided->value)
                : declared->default_value ? available(detail::materialize_config_value(*declared->default_value))
                : invalid(error(ErrorCode::MissingRequiredValue, "Current scope public input has no value."));
            if (read.value) { return available(selected(*read.value, value.selector)); }
            return read;
        } else if constexpr (std::is_same_v<T, ResourceReference>) {
            auto checked = validate_resource_reference(value, snapshot_.solution().resources);
            if (const auto* diagnostic = std::get_if<Diagnostic>(&checked)) { return invalid(*diagnostic); }
            auto fixed = value;
            const auto* definition = find(snapshot_.solution().resources, value.id);
            if (const auto* resource = std::get_if<ManagedResource>(&definition->location)) { fixed.revision = resource->revision; }
            else if (const auto* external = std::get_if<ExternalFileResource>(&definition->location)) { fixed.revision = external->revision; }
            traces_.push_back({index, fixed, {}});
            return available(DataValue::resource_reference(fixed));
        } else {
            const auto entry = std::ranges::find(context_.globals, value.variable, &GlobalVariableEntry::variable);
            if (entry == context_.globals.end()) { auto diagnostic = error(ErrorCode::GlobalUnavailable, "No current global read snapshot was supplied; definition initial value is not a fallback."); diagnostic.location.variable = value.variable; return invalid(diagnostic); }
            traces_.push_back({index, GlobalReadTrace{value.variable, entry->revision, entry->update_origin}, {}});
            if (value.required_revision && *value.required_revision != entry->revision) {
                auto diagnostic = error(ErrorCode::GlobalRevisionMismatch, "Global snapshot revision differs from the requested revision."); diagnostic.location.variable = value.variable; return invalid(diagnostic);
            }
            if (auto checked = detail::validate_runtime_domain(entry->current_value, snapshot_.solution(), {});
                const auto* diagnostic = std::get_if<Diagnostic>(&checked)) { return invalid(*diagnostic); }
            return available(selected(entry->current_value, value.selector));
        }
    }, source);
}

template<class Target> BindingResolution Resolution::resolve(const Target& descriptor)
{
    const auto binding = std::ranges::find(node_->bindings, *target_, &Binding::target);
    const bool defaulted = binding == node_->bindings.end();
    if (defaulted && !descriptor.default_value) { return failure({error(ErrorCode::MissingRequiredValue, "Target has neither binding nor declared default."), {}}); }
    const auto cardinality = defaulted ? BindingCardinality::SingleSource : binding->cardinality;
    const auto conversion = defaulted ? ConversionKind::Exact : binding->conversion;
    const auto count = defaulted ? std::size_t{1} : binding->sources.size();
    const auto& expected = cardinality == BindingCardinality::MultipleSources ? *descriptor.type.element_type() : descriptor.type;
    Evaluation aggregate;
    std::vector<DataValue> values;
    for (std::size_t index = 0; index < count; ++index) {
        const auto first_region = regions_.size();
        auto read = defaulted ? configured(*descriptor.default_value, index, true) : source(binding->sources[index], index);
        bool omitted = false;
        std::vector<Absence> rejected;
        for (const auto& absence : read.absences) {
            const auto normal = absence.reason == OutputAbsenceReason::Disabled || absence.reason == OutputAbsenceReason::NotSelected;
            const auto reason = absence.reason == OutputAbsenceReason::Disabled ? MissingReason::Disabled : MissingReason::NotSelected;
            if (!defaulted && normal && contains(binding->availability.accepted_absence, reason)) {
                traces_.at(absence.trace).accepted_absence = reason; omitted = true;
            } else { rejected.push_back(absence); }
        }
        read.absences = std::move(rejected);
        if (omitted && read.absences.empty() && !read.pending && !read.failure) {
            regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(first_region), regions_.end());
            read.value.reset();
            if (cardinality != BindingCardinality::MultipleSources) { read.value = DataValue::none(*expected.element_type()); }
        } else if (read.value && !read.pending && read.absences.empty() && !read.failure) {
            // Recheck runtime type before executing a conversion, including defaults.
            auto compatible = defaulted ? Check{std::monostate{}}
                : core::validate_binding(descriptor, source_kind(binding->sources[index]), read.value->type(), conversion);
            if (const auto* diagnostic = std::get_if<Diagnostic>(&compatible)) { read = invalid(*diagnostic); }
            else {
                switch (conversion) {
                case ConversionKind::Exact: break;
                case ConversionKind::WrapOptional: read.value = DataValue::some(*read.value); break;
                case ConversionKind::Int64ToFloat64Explicit: read = available(DataValue::real(static_cast<double>(*read.value->as_int64()))); break;
                }
            }
        }
        if (read.value && !read.pending && read.absences.empty() && !read.failure) {
            auto checked = detail::validate_runtime_domain(*read.value, snapshot_.solution(), descriptor.semantics, tool_, true);
            if (const auto* diagnostic = std::get_if<Diagnostic>(&checked)) { read = invalid(*diagnostic); }
            else { values.push_back(*read.value); }
        }
        merge(aggregate, std::move(read));
    }
    if (aggregate.failure) { return failure(std::move(*aggregate.failure)); }
    if (!aggregate.absences.empty()) {
        return UnavailableBinding{aggregate.absences.front().reason,
            contextualize(error(ErrorCode::InputUnavailable, "Current source is unavailable and its absence was not accepted.")), traces_};
    }
    if (aggregate.pending) { return PendingBinding{traces_}; }
    auto result = cardinality == BindingCardinality::MultipleSources
        ? DataValue::list(*descriptor.type.element_type(), values) : Result<DataValue>{values.front()};
    if (const auto* diagnostic = std::get_if<Diagnostic>(&result)) { return failure({*diagnostic, {}}); }
    const auto& value = std::get<DataValue>(result);
    for (auto checked : {detail::validate_runtime_value(descriptor.type, descriptor.constraints, value),
        detail::validate_runtime_domain(value, snapshot_.solution(), descriptor.semantics, tool_, true)}) {
        if (const auto* diagnostic = std::get_if<Diagnostic>(&checked)) { return failure({*diagnostic, {}}); }
    }
    return ResolvedBinding{value, traces_, regions_};
}

BindingResolution Resolution::run()
{
    if (auto checked = validate_context(); const auto* diagnostic = std::get_if<Diagnostic>(&checked)) { return failure({*diagnostic, {}}); }
    if (!target_) { return failure({error(ErrorCode::UnknownTarget, "A binding target is required."), {}}); }
    if (const auto* input = std::get_if<InputId>(target_)) {
        const auto& inputs = tool_ ? tool_->inputs : std::get<GroupConfiguration>(node_->configuration).interface.inputs;
        if (const auto* descriptor = find(inputs, *input)) { return resolve(*descriptor); }
    } else if (tool_) {
        if (const auto* descriptor = find(tool_->parameters, std::get<ParameterId>(*target_))) { return resolve(*descriptor); }
    }
    return failure({error(ErrorCode::UnknownTarget, "Resolution target is not declared by the current consumer."), {}});
}
} // namespace

core::Result<BindingResolver> BindingResolver::create(const ConfigSnapshot& snapshot)
{
    auto validated = validate_solution(snapshot.solution(), snapshot.tools());
    if (const auto* failure = std::get_if<core::Diagnostic>(&validated)) { return *failure; }
    return BindingResolver{snapshot, std::get<ValidationIndex>(std::move(validated))};
}
BindingResolution BindingResolver::resolve(const ResolutionContext& context,
    const BindingTarget& target, const ResultStore& results) const
{ return Resolution{snapshot_, index_, context, &target, results}.run(); }
core::Result<std::monostate> BindingResolver::validate_context(const ResolutionContext& context,
    const ResultStore& results) const
{ return Resolution{snapshot_, index_, context, nullptr, results}.context_only(); }

} // namespace visionpro::workflow
