#include <visionpro/core/tool_execution.hpp>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <set>
#include <type_traits>

namespace visionpro::core {
namespace {
using Check = Result<std::monostate>;
Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }
bool failed(const Check& result) { return std::holds_alternative<Diagnostic>(result); }
template<class T, class Id> const T* find(const std::vector<T>& values, const Id& id)
{
    const auto found = std::ranges::find(values, id, &T::id);
    return found == values.end() ? nullptr : &*found;
}
Diagnostic contextualize(Diagnostic diagnostic, const ToolExecutionConfigurationData& configuration)
{
    diagnostic.location.workflow = configuration.workflow;
    diagnostic.location.node = configuration.node;
    diagnostic.location.tool = configuration.tool.id;
    return diagnostic;
}
bool same_constant(const DataValue& value, const ConfigValue& configured)
{
    if (value.type() != configured.type()) { return false; }
    switch (value.type().kind()) {
    case TypeKind::Bool: return *value.as_bool() == *configured.as_bool();
    case TypeKind::Int64: return *value.as_int64() == *configured.as_int64();
    case TypeKind::Float64: return *value.as_float64() == *configured.as_float64();
    case TypeKind::String: return *value.as_string() == *configured.as_string();
    case TypeKind::Binary: return *value.as_binary() == *configured.as_binary();
    case TypeKind::Enum: return *value.as_enum() == *configured.as_enum();
    case TypeKind::Point2D: return *value.as_point2d() == *configured.as_point2d();
    case TypeKind::Line2D: return *value.as_line2d() == *configured.as_line2d();
    case TypeKind::Circle2D: return *value.as_circle2d() == *configured.as_circle2d();
    case TypeKind::Pose2D: return *value.as_pose2d() == *configured.as_pose2d();
    case TypeKind::Transform2D: return *value.as_transform2d() == *configured.as_transform2d();
    case TypeKind::Region2D: return *value.as_region2d() == *configured.as_region2d();
    case TypeKind::ResourceReference: return *value.as_resource_reference() == *configured.as_resource_reference();
    case TypeKind::Optional:
        if (const auto* payload = value.optional_value()) {
            return configured.optional_value() && same_constant(*payload, *configured.optional_value());
        }
        return !configured.optional_value();
    case TypeKind::List:
        if (value.elements()->size() != configured.elements()->size()) { return false; }
        for (std::size_t i = 0; i < value.elements()->size(); ++i) {
            if (!same_constant((*value.elements())[i], (*configured.elements())[i])) { return false; }
        }
        return true;
    case TypeKind::Record:
        for (const auto& field : *value.fields()) {
            const auto* other = configured.field(field.id);
            if (!other || !same_constant(field.value, *other)) { return false; }
        }
        return true;
    case TypeKind::Image: case TypeKind::RasterMask: return false;
    }
    return false;
}
template<class Value> bool unresolved_region(const Value& value)
{
    if (const auto* region = value.as_region2d()) {
        if (std::holds_alternative<InheritedRegion>(region->source)) { return true; }
        for (const auto& source : region->exclusions) { if (std::holds_alternative<InheritedRegion>(source)) { return true; } }
    }
    if (const auto* payload = value.optional_value()) { return unresolved_region(*payload); }
    if (const auto* elements = value.elements()) {
        for (const auto& item : *elements) { if (unresolved_region(item)) { return true; } }
    }
    if (const auto* fields = value.fields()) {
        for (const auto& field : *fields) { if (unresolved_region(field.value)) { return true; } }
    }
    return false;
}
Check validate_state_access(const InvocationContextData& context)
{
    const auto& definition = context.configuration.effective_state();
    if (!definition) { return error(ErrorCode::InvalidState, "State access requires a fixed state definition."); }
    const auto& access = *context.state;
    const auto& key = access.key();
    const auto& identity = context.identity;
    if (access.identity() != identity || key.node != identity.invocation.node) {
        return error(ErrorCode::ResultIdentityMismatch, "State access must match the exact invocation identity and node.");
    }
    if (auto checked = validate_node_state(*definition, {key, StateInitialization::NotInitialized, {}, {}});
        failed(checked)) { return checked; }
    const bool matches = std::visit([&](const auto& domain) {
        using Domain = std::decay_t<decltype(domain)>;
        if constexpr (std::is_same_v<Domain, InvocationStateDomain>) {
            return domain.invocation == SessionInvocationKey{identity.session, identity.invocation};
        } else if constexpr (std::is_same_v<Domain, WorkflowRunStateDomain>) {
            return domain.session == identity.session && domain.run == identity.invocation.workflow_run;
        } else {
            return domain.session == identity.session;
        }
    }, key.domain);
    if (!matches) { return error(ErrorCode::ResultIdentityMismatch, "State domain must belong to this session and invocation."); }
    return std::monostate{};
}
Check completion_check(const InvocationContext& context, const ToolCompletion& result)
{
    const auto& invocation = context.data();
    const auto& descriptor = invocation.configuration.data().tool;
    if (result.identity != invocation.identity) {
        return error(ErrorCode::InvalidValue, "Completion identity must match the invocation.");
    }
    if (!valid_execution_timing(result.elapsed, result.timings)) {
        return error(ErrorCode::InvalidValue, "Completion timing must have recognized scope, source, unit and nonnegative values.");
    }
    if ((result.status != ToolCompletionStatus::Succeeded && result.status != ToolCompletionStatus::Failed
            && result.status != ToolCompletionStatus::Cancelled && result.status != ToolCompletionStatus::TimedOut)
        || (result.judgement != ToolJudgement::NotEvaluated && result.judgement != ToolJudgement::OK && result.judgement != ToolJudgement::NG)) {
        return error(ErrorCode::InvalidValue, "Completion status and judgement must be recognized terminal values.");
    }
    const bool success = result.status == ToolCompletionStatus::Succeeded;
    bool has_error = false;
    for (const auto& diagnostic : result.diagnostics) {
        if (!is_valid_diagnostic(diagnostic)) {
            return error(ErrorCode::InvalidValue, "Completion contains an invalid diagnostic.");
        }
        has_error = has_error || diagnostic.severity == Severity::Error;
    }
    if ((success && has_error) || (!success
            && (result.judgement != ToolJudgement::NotEvaluated || !result.selected_control_outputs.empty()))) {
        return error(ErrorCode::InvalidValue, "Completion diagnostics, judgement and control selections must agree with its status.");
    }
    std::set<OutputId> outputs;
    const auto output_error = [](OutputId output, ErrorCode code, std::string message) {
        auto diagnostic = error(code, std::move(message));
        diagnostic.location.output = output;
        return diagnostic;
    };
    for (const auto& output : result.outputs) {
        const auto* declaration = find(descriptor.outputs, output.id);
        if (!declaration || !outputs.insert(output.id).second) {
            return output_error(output.id, ErrorCode::TypeMismatch, "Completion outputs must uniquely match declared output identities.");
        }
        if (const auto* value = std::get_if<DataValue>(&output.content)) {
            if (!success || unresolved_region(*value)) {
                return output_error(output.id, ErrorCode::InvalidValue,
                    "Only successful completions publish available values, with no unresolved ROI inheritance.");
            }
            if (declaration->type != value->type()) {
                return output_error(output.id, ErrorCode::TypeMismatch, "Completion output value has the wrong type.");
            }
        } else if (const auto* absent = std::get_if<NotProduced>(&output.content)) {
            if (!success || !allows_output_absence(*declaration, absent->reason)) {
                return output_error(output.id, ErrorCode::InvalidValue, "Output omission must match a declared reason on a non-required output.");
            }
        } else {
            auto diagnostic = std::get<InvalidOutput>(output.content).diagnostic;
            if (!is_valid_diagnostic(diagnostic) || diagnostic.severity != Severity::Error) {
                return output_error(output.id, ErrorCode::InvalidValue, "Invalid output requires a valid error diagnostic.");
            }
            if (result.status != ToolCompletionStatus::Failed) {
                diagnostic.location.output = output.id;
                return diagnostic;
            }
            has_error = true;
        }
    }
    if (result.status == ToolCompletionStatus::Failed && !has_error) {
        return error(ErrorCode::InvalidValue, "Failed completion requires an error diagnostic.");
    }
    if (success && outputs.size() != descriptor.outputs.size()) {
        for (const auto& declaration : descriptor.outputs) {
            if (!outputs.contains(declaration.id)) {
                return output_error(declaration.id, ErrorCode::MissingRequiredValue,
                    "Successful completion must report each output as available or explicitly not produced.");
            }
        }
    }
    std::set<ControlOutputId> controls;
    for (const auto& control : result.selected_control_outputs) {
        if (!find(descriptor.control_outputs, control) || !controls.insert(control).second) {
            return error(ErrorCode::InvalidValue, "Completion control selections must be unique declared outlets.");
        }
    }
    return std::monostate{};
}
} // namespace

Result<std::monostate> validate_tool_completion(const InvocationContext& context, const ToolCompletion& result)
{
    auto checked = completion_check(context, result);
    if (auto* failure = std::get_if<Diagnostic>(&checked)) {
        *failure = contextualize(std::move(*failure), context.data().configuration.data());
    }
    return checked;
}

struct ToolExecutionConfiguration::Storage {
    ToolExecutionConfigurationData configuration;
    std::optional<NodeStateDefinition> state;
};

const ToolExecutionConfigurationData& ToolExecutionConfiguration::data() const noexcept
{ return data_->configuration; }

const std::optional<NodeStateDefinition>& ToolExecutionConfiguration::effective_state() const noexcept
{ return data_->state; }

Result<ToolExecutionConfiguration> ToolExecutionConfiguration::create(const ToolExecutionConfigurationData& data)
{
    const auto fail = [&](Diagnostic diagnostic) -> Result<ToolExecutionConfiguration> { return contextualize(std::move(diagnostic), data); };
    if (auto checked = validate_descriptor(data.tool); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    std::set<ParameterId> parameters;
    for (const auto& parameter : data.parameters) {
        const auto* declared = find(data.tool.parameters, parameter.id);
        if (!declared || declared->binding.time != BindingTime::SnapshotBuild || !parameters.insert(parameter.id).second) {
            return fail(error(ErrorCode::InvalidValue, "Fixed configuration parameters must be unique declared SnapshotBuild parameters."));
        }
        if (auto checked = validate_parameter_value(*declared, parameter.value); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
        if (unresolved_region(parameter.value)) { return fail(error(ErrorCode::InvalidValue, "SnapshotBuild parameters cannot retain runtime ROI inheritance.")); }
    }
    for (const auto& declared : data.tool.parameters) {
        if (declared.binding.time == BindingTime::SnapshotBuild && !parameters.contains(declared.id)) {
            return fail(error(ErrorCode::MissingRequiredValue, "Every SnapshotBuild parameter must already be fixed, including defaults."));
        }
    }
    std::set<SchemaId> slots;
    for (const auto& binding : data.resources) {
        const auto found = std::ranges::find(data.tool.resources, binding.slot, &ResourceRequirement::slot);
        if (found == data.tool.resources.end() || !slots.insert(binding.slot).second || found->kind != binding.resource.kind) {
            return fail(error(ErrorCode::InvalidResource, "Fixed resource slots must uniquely match declared resource kinds."));
        }
        if (auto checked = validate_resource_reference(binding.resource); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    }
    for (const auto& required : data.tool.resources) {
        if (required.required && !slots.contains(required.slot)) { return fail(error(ErrorCode::MissingRequiredValue, "Required configuration resource slot is absent.")); }
    }
    auto state = data.tool.state;
    if (state) {
        const auto& projection = data.tool.state_parameters;
        // Descriptor validation guarantees the exact types and full mapping,
        // and every SnapshotBuild parameter was fixed above.
        if (projection.initial_value) {
            state->initial_value = find(data.parameters, *projection.initial_value)->value;
        }
        if (projection.policy) {
            const auto& value = find(data.parameters, projection.policy->id)->value;
            const auto choice = std::ranges::find(projection.policy->choices, *value.as_enum(), &StatePolicyChoice::choice);
            state->scope = choice->scope;
            state->reset = choice->reset;
        }
        if (unresolved_region(state->initial_value)) {
            return fail(error(ErrorCode::InvalidState, "State initial value cannot retain runtime ROI inheritance."));
        }
    }
    return ToolExecutionConfiguration{std::make_shared<const Storage>(Storage{data, std::move(state)})};
}

Result<InvocationContext> InvocationContext::create(const InvocationContextData& data)
{
    const auto& configuration = data.configuration.data();
    const auto fail = [&](Diagnostic diagnostic) -> Result<InvocationContext> { return contextualize(std::move(diagnostic), configuration); };
    if (data.identity.session != configuration.session || data.identity.snapshot != configuration.snapshot
        || data.identity.invocation.node != configuration.node) {
        return fail(error(ErrorCode::InvalidValue, "Invocation identity must match the fixed configuration's session, snapshot and node."));
    }
    if (data.state) {
        if (auto checked = validate_state_access(data); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    }
    const auto values = [&](const auto& declarations, const auto& actual) -> Check {
        using Id = decltype(actual.front().id);
        std::set<Id> seen;
        for (const auto& item : actual) {
            const auto* declaration = find(declarations, item.id);
            if (!declaration || !seen.insert(item.id).second || declaration->type != item.value.type()) {
                return error(ErrorCode::TypeMismatch, "Invocation values must uniquely match their declared target identities and exact types.");
            }
        }
        if (seen.size() != declarations.size()) { return error(ErrorCode::MissingRequiredValue, "All invocation values must be resolved before tool invocation, including defaults."); }
        return std::monostate{};
    };
    for (auto checked : {values(configuration.tool.inputs, data.inputs), values(configuration.tool.parameters, data.parameters)}) {
        if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    }
    for (const auto& fixed : configuration.parameters) {
        if (!same_constant(find(data.parameters, fixed.id)->value, fixed.value)) {
            return fail(error(ErrorCode::InvalidValue, "Invocation cannot replace a parameter fixed in the configuration snapshot."));
        }
    }
    std::set<SchemaId> leases;
    for (const auto& binding : data.resources) {
        const auto declared = std::ranges::find(configuration.resources, binding.slot, &ToolResourceBinding::slot);
        if (!binding.lease || declared == configuration.resources.end() || !leases.insert(binding.slot).second
            || binding.lease->reference() != declared->resource) {
            return fail(error(ErrorCode::InvalidResource, "Invocation leases must uniquely match the exact fixed resource references."));
        }
    }
    if (leases.size() != configuration.resources.size()) { return fail(error(ErrorCode::MissingRequiredValue, "Every configured resource requires a typed lease for this invocation.")); }
    for (std::size_t i = 0; i < data.regions.size(); ++i) {
        const auto& region = data.regions[i];
        const bool known = std::visit([&](auto target) {
            using Id = decltype(target);
            if constexpr (std::is_same_v<Id, InputId>) { return find(data.inputs, target) != nullptr; }
            else { return find(data.parameters, target) != nullptr; }
        }, region.target);
        if (!known || region.reference.workflow != configuration.workflow
            || region.reference.node != region.producer.invocation.node
            || region.producer.session != data.identity.session || region.producer.snapshot != data.identity.snapshot
            || region.producer.invocation.workflow_run != data.identity.invocation.workflow_run
            || region.value.coordinate_frame != region.reference.coordinate_frame || region.value.unit != region.reference.unit) {
            return fail(error(ErrorCode::InvalidValue, "Frozen ROI source must retain a matching current-run identity, target and coordinates."));
        }
        if (auto checked = validate_region(region.value); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
        if (std::holds_alternative<InheritedRegion>(region.value.source)
            || std::ranges::any_of(region.value.exclusions, [](const auto& source) { return std::holds_alternative<InheritedRegion>(source); })) {
            return fail(error(ErrorCode::InvalidValue, "Frozen ROI source definition must be self-contained."));
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (data.regions[j].target == region.target && data.regions[j].source_index == region.source_index && data.regions[j].path == region.path) {
                return fail(error(ErrorCode::InvalidValue, "Frozen ROI entries require distinct target/source/path addresses."));
            }
        }
    }
    std::vector<bool> used_regions(data.regions.size(), false);
    const auto check_regions = [&](const auto& self, const DataValue& value, const ToolValueTarget& target) -> Check {
        if (const auto* region = value.as_region2d()) {
            const auto frozen = [&](const RegionSource& source) {
                const auto* reference = std::get_if<InheritedRegion>(&source);
                if (!reference) { return true; }
                for (std::size_t i = 0; i < data.regions.size(); ++i) {
                    if (!used_regions[i] && data.regions[i].target == target && data.regions[i].reference == *reference) {
                        used_regions[i] = true; return true;
                    }
                }
                return false;
            };
            if (!frozen(region->source) || !std::ranges::all_of(region->exclusions, frozen)) {
                return error(ErrorCode::MissingRequiredValue, "Every inherited ROI expression requires its frozen current-run source definition.");
            }
        }
        if (const auto* payload = value.optional_value()) { return self(self, *payload, target); }
        if (const auto* elements = value.elements()) {
            for (const auto& element : *elements) { if (auto checked = self(self, element, target); failed(checked)) { return checked; } }
        }
        if (const auto* fields = value.fields()) {
            for (const auto& field : *fields) { if (auto checked = self(self, field.value, target); failed(checked)) { return checked; } }
        }
        return std::monostate{};
    };
    for (const auto& input : data.inputs) {
        if (auto checked = check_regions(check_regions, input.value, ToolValueTarget{input.id}); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    }
    for (const auto& parameter : data.parameters) {
        if (auto checked = check_regions(check_regions, parameter.value, ToolValueTarget{parameter.id}); const auto* failure = std::get_if<Diagnostic>(&checked)) { return fail(*failure); }
    }
    if (std::ranges::any_of(used_regions, [](bool used) { return !used; })) {
        return fail(error(ErrorCode::InvalidValue, "Frozen ROI metadata cannot contain entries unrelated to the fixed invocation values."));
    }
    return InvocationContext{std::make_shared<const InvocationContextData>(data)};
}

namespace detail {
struct InvocationState {
    explicit InvocationState(ToolInvocationIdentity identity) : identity(std::move(identity)) {}

    const ToolInvocationIdentity identity;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<const InvocationContext> context;
    std::shared_ptr<ToolInstance> instance;
    std::shared_ptr<InvocationControl> control;
    std::weak_ptr<InvocationControl> registered_control;
    std::optional<ToolCompletion> completion;
    std::optional<std::weak_ptr<InvocationWakeup>> completion_wakeup;
    std::optional<std::weak_ptr<InvocationWakeup>> exit_wakeup;
    bool exited{false};
    bool exiting{false};
    bool registered{false};
    bool cancelled{false};
    bool starting{true};
    bool cancelling{false};
    ~InvocationState() { if (instance) { instance->active_.fetch_sub(1); } }
    InvocationProgress progress() const { return {completion, exited, cancelled}; }
    bool reached(InvocationWaitTarget target) const
    { return target == InvocationWaitTarget::LogicalCompletion ? completion.has_value() : exited; }
    std::optional<std::weak_ptr<InvocationWakeup>>& wakeup(InvocationWaitTarget target)
    { return target == InvocationWaitTarget::LogicalCompletion ? completion_wakeup : exit_wakeup; }
    std::shared_ptr<InvocationWakeup> take_wakeup(InvocationWaitTarget target)
    {
        auto& registration = wakeup(target);
        if (!registration) { return {}; }
        auto listener = registration->lock();
        registration->reset();
        return listener;
    }

    // The transition that last satisfies this condition owns cleanup.
    // Called under mutex by the exit report and the two backend call boundaries.
    bool ready_for_cleanup() const noexcept { return exiting && !starting && !cancelling; }
    void finish_exit()
    {
        std::shared_ptr<ToolInstance> owned_instance;
        std::shared_ptr<InvocationControl> owned_control;
        std::shared_ptr<const InvocationContext> owned_context;
        {
            std::lock_guard lock(mutex);
            owned_instance = std::move(instance);
            owned_control = std::move(control);
            owned_context = std::move(context);
        }
        // Backend destructors can reenter the operation. Keep the invocation
        // context available through destruction of the operation-owned objects.
        owned_control.reset();
        const std::weak_ptr<ToolInstance> remaining_instance = owned_instance;
        owned_instance.reset();
        if (const auto& access = owned_context->data().state) { access->finish(); }
        owned_context.reset();
        // If another owner keeps the instance, retain its serialized slot
        // through lease cleanup as well.
        if (const auto held = remaining_instance.lock()) { held->active_.fetch_sub(1); }
        std::shared_ptr<InvocationWakeup> listener;
        {
            std::lock_guard lock(mutex);
            exiting = false;
            exited = true;
            listener = take_wakeup(InvocationWaitTarget::PhysicalExit);
            changed.notify_all();
        }
        if (listener) { listener->signal(identity, InvocationWaitTarget::PhysicalExit); }
    }
};
} // namespace detail

Result<std::shared_ptr<ToolInstance>> create_tool_instance(ToolInstanceFactory& factory, const ToolExecutionConfiguration& configuration)
{
    try {
        auto result = factory.create(configuration);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) { return contextualize(*failure, configuration.data()); }
        const auto& instance = std::get<std::shared_ptr<ToolInstance>>(result);
        if (!instance || &instance->configuration().data() != &configuration.data()) {
            return contextualize(error(ErrorCode::InvalidState, "Factory must return a non-null independent instance bound to the supplied fixed configuration."), configuration.data());
        }
        bool unissued = false;
        if (!instance->issued_.compare_exchange_strong(unissued, true)) {
            return contextualize(error(ErrorCode::InvalidState, "Factory returned an instance already issued by an earlier factory call."), configuration.data());
        }
        return instance;
    } catch (...) {
        return contextualize(error(ErrorCode::ExecutionBoundaryFailure, "Tool factory threw across its execution boundary."), configuration.data());
    }
}

Result<DataValue> initialize_tool_state(ToolInstance& instance, const InvocationContext& context, const DataValue& initial)
{
    const auto& fixed = context.data().configuration;
    const auto fail = [&](Diagnostic diagnostic) -> Result<DataValue> { return contextualize(std::move(diagnostic), fixed.data()); };
    const auto& definition = fixed.effective_state();
    if (&instance.configuration().data() != &fixed.data() || !definition || context.data().state) {
        return fail(error(ErrorCode::InvalidState, "State initialization requires the exact prepared stateful configuration and no active state access."));
    }
    if (!same_constant(initial, definition->initial_value)) {
        return fail(error(ErrorCode::InvalidState, "State initialization must start from the configured initial value."));
    }
    try {
        auto result = instance.initialize_state(context, initial);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) {
            if (!is_valid_diagnostic(*failure) || failure->severity != Severity::Error) {
                return fail(error(ErrorCode::InvalidState, "Tool state initialization returned an invalid failure diagnostic."));
            }
            return fail(*failure);
        }
        const auto& value = std::get<DataValue>(result);
        if (value.type() != definition->schema) {
            return fail(error(ErrorCode::TypeMismatch, "Initialized state must match the declared state schema."));
        }
        if (unresolved_region(value)) {
            return fail(error(ErrorCode::InvalidState, "Initialized state cannot retain unresolved ROI inheritance."));
        }
        return result;
    } catch (...) {
        return fail(error(ErrorCode::ExecutionBoundaryFailure, "Tool state initialization threw across its execution boundary."));
    }
}

Result<std::monostate> InvocationCompletion::register_control(std::shared_ptr<InvocationControl> control) const
{
    std::lock_guard lock(state_->mutex);
    if (!control || state_->registered || state_->exited || state_->exiting
        || state_->context->data().configuration.data().tool.execution.mode != ToolExecutionMode::Asynchronous) {
        return error(ErrorCode::InvalidState, "Asynchronous startup requires one non-null control registration before activity begins.");
    }
    state_->control = control; state_->registered_control = control; state_->registered = true;
    return std::monostate{};
}
Result<std::monostate> InvocationCompletion::complete(const ToolCompletion& result) const
{
    const auto state = state_;
    std::shared_ptr<InvocationWakeup> listener;
    {
        std::lock_guard lock(state->mutex);
        if (state->completion || state->exited) { return error(ErrorCode::InvalidState, "Invocation completion is accepted only once, before physical exit."); }
        if (auto checked = validate_tool_completion(*state->context, result); failed(checked)) { return checked; }
        state->completion = result; // Detach caller-owned containers; DataValue owns immutable nested payloads.
        if (const auto& access = state->context->data().state) { access->seal(); }
        listener = state->take_wakeup(InvocationWaitTarget::LogicalCompletion);
        state->changed.notify_all();
    }
    if (listener) { listener->signal(state->identity, InvocationWaitTarget::LogicalCompletion); }
    return std::monostate{};
}
Result<std::monostate> InvocationCompletion::mark_exited() const
{
    const auto state = state_;
    bool clean;
    {
        std::lock_guard lock(state->mutex);
        if (!state->completion || state->exited || state->exiting) { return error(ErrorCode::InvalidState, "Physical exit follows one terminal completion and is reported once."); }
        state->exiting = true;
        clean = state->ready_for_cleanup();
    }
    if (clean) { state->finish_exit(); }
    return std::monostate{};
}
bool InvocationCompletion::cancellation_requested() const
{ std::lock_guard lock(state_->mutex); return state_->cancelled; }

Result<InvocationOperation> InvocationOperation::start(std::shared_ptr<ToolInstance> instance, const InvocationContext& context)
{
    const auto& configuration = context.data().configuration.data();
    const auto fail = [&](Diagnostic diagnostic) -> Result<InvocationOperation> { return contextualize(std::move(diagnostic), configuration); };
    if (!instance || &instance->configuration().data() != &configuration) {
        return fail(error(ErrorCode::InvalidState, "Invocation instance is not bound to this fixed configuration."));
    }
    const auto& access = context.data().state;
    if (!access && (context.data().configuration.effective_state() || configuration.tool.execution.effects == ToolEffects::State)) {
        return fail(error(ErrorCode::MissingCapability, "Stateful invocation requires a typed state service and access lease."));
    }
    auto state = std::make_shared<detail::InvocationState>(context.data().identity);
    state->context = std::make_shared<const InvocationContext>(context);
    if (configuration.tool.execution.reentrancy == ToolReentrancy::Serialized) {
        std::size_t expected = 0;
        if (!instance->active_.compare_exchange_strong(expected, 1)) { return fail(error(ErrorCode::InvalidState, "Serialized instance still has an invocation before physical exit.")); }
    } else { instance->active_.fetch_add(1); }
    state->instance = instance;
    if (access && access->started_.exchange(true)) {
        return fail(error(ErrorCode::InvalidState, "State access can be used by only one invocation operation."));
    }
    InvocationCompletion completion{state};
    const auto expose = [&] {
        instance.reset();
        bool clean;
        {
            std::lock_guard lock(state->mutex);
            state->starting = false;
            clean = state->ready_for_cleanup();
        }
        if (clean) { state->finish_exit(); }
        return InvocationOperation{state};
    };
    const auto startup_failed = [&](Diagnostic diagnostic) -> Result<InvocationOperation> {
        diagnostic = contextualize(std::move(diagnostic), configuration);
        diagnostic.severity = Severity::Error;
        if (!is_valid_diagnostic(diagnostic)) {
            diagnostic.message = "Tool startup failed with an invalid diagnostic.";
            diagnostic.external.reset();
        }
        bool registered;
        bool exited;
        {
            std::lock_guard lock(state->mutex);
            // No operation is exposed before begin returns. Callback completion
            // during startup is provisional and cannot hide a startup failure.
            const bool already_sealed = state->completion.has_value();
            state->completion = ToolCompletion{context.data().identity, ToolCompletionStatus::Failed,
                ToolJudgement::NotEvaluated, {}, {diagnostic}};
            if (!already_sealed && access) { access->seal(); }
            registered = state->registered; exited = state->exited || state->exiting;
            state->changed.notify_all();
        }
        if (!registered && !exited) { static_cast<void>(completion.mark_exited()); }
        return expose();
    };
    try {
        auto begun = instance->begin(state->context, completion);
        if (const auto* failure = std::get_if<Diagnostic>(&begun)) { return startup_failed(*failure); }
        auto& start = std::get<InvocationStart>(begun);
        if (const auto* synchronous = std::get_if<ToolCompletion>(&start)) {
            if (configuration.tool.execution.mode != ToolExecutionMode::Synchronous) { return startup_failed(error(ErrorCode::InvalidState, "Asynchronous tool must return its controlled asynchronous operation.")); }
            if (auto checked = completion.complete(*synchronous); const auto* failure = std::get_if<Diagnostic>(&checked)) { return startup_failed(*failure); }
            if (auto checked = completion.mark_exited(); const auto* failure = std::get_if<Diagnostic>(&checked)) { return startup_failed(*failure); }
        } else {
            const auto& asynchronous = std::get<AsynchronousInvocation>(start);
            bool registered;
            {
                std::lock_guard lock(state->mutex);
                registered = state->registered && state->registered_control.lock() == asynchronous.control;
            }
            if (configuration.tool.execution.mode != ToolExecutionMode::Asynchronous || !asynchronous.control || !registered) {
                return startup_failed(error(ErrorCode::InvalidState, "Asynchronous start must return its previously registered non-null control object."));
            }
        }
    } catch (...) {
        return startup_failed(error(ErrorCode::ExecutionBoundaryFailure, "Tool begin threw across its execution boundary."));
    }
    return expose();
}
InvocationProgress InvocationOperation::poll() const
{ std::lock_guard lock(state_->mutex); return state_->progress(); }
bool InvocationOperation::physically_exited() const
{ std::lock_guard lock(state_->mutex); return state_->exited; }
Result<std::monostate> InvocationOperation::notify_when(InvocationWaitTarget target,
    std::weak_ptr<InvocationWakeup> wakeup) const
{
    const auto listener = wakeup.lock();
    if (!listener || (target != InvocationWaitTarget::LogicalCompletion && target != InvocationWaitTarget::PhysicalExit)) {
        return error(ErrorCode::InvalidValue, "Notification requires a known target and a live wakeup receiver.");
    }
    const auto state = state_;
    {
        std::lock_guard lock(state->mutex);
        auto& registration = state->wakeup(target);
        if (registration) { return error(ErrorCode::InvalidState, "Invocation notification already registered for this target."); }
        if (!state->reached(target)) {
            registration.emplace(std::move(wakeup));
            return std::monostate{};
        }
        registration.emplace();
    }
    listener->signal(state->identity, target);
    return std::monostate{};
}
Result<InvocationWaitResult> InvocationOperation::wait_for(InvocationWaitTarget target, std::chrono::milliseconds timeout) const
{
    if ((target != InvocationWaitTarget::LogicalCompletion && target != InvocationWaitTarget::PhysicalExit) || timeout.count() < 0) {
        return error(ErrorCode::InvalidValue, "Wait requires a known target and a nonnegative timeout.");
    }
    std::unique_lock lock(state_->mutex);
    const auto reached = [&]() { return state_->reached(target); };
    const bool ready = state_->changed.wait_for(lock, timeout, reached);
    return InvocationWaitResult{ready ? InvocationWaitOutcome::Reached : InvocationWaitOutcome::TimedOut, state_->progress()};
}
Result<CancellationRequest> InvocationOperation::request_cancel() const
{
    const auto state = state_;
    std::shared_ptr<InvocationControl> control;
    {
        std::lock_guard lock(state->mutex);
        if (state->exited) { return CancellationRequest::AlreadyExited; }
        if (state->exiting) { return CancellationRequest::CleanupInProgress; }
        if (state->cancelled) { return CancellationRequest::AlreadyRequested; }
        if (state->context->data().configuration.data().tool.execution.cancellation != ToolCancellation::Cooperative) {
            return error(ErrorCode::MissingCapability, "Tool does not declare cooperative cancellation support.");
        }
        state->cancelled = true;
        state->cancelling = true;
        control = state->control;
    }
    bool callback_failed = false;
    try {
        if (control) { control->request_cancel(); }
    } catch (...) {
        callback_failed = true;
    }
    // Release the local reference before making cleanup eligible. Otherwise a
    // concurrent exit could publish while the control was still being released.
    control.reset();
    bool clean;
    {
        std::lock_guard lock(state->mutex);
        state->cancelling = false;
        clean = state->ready_for_cleanup();
    }
    if (clean) { state->finish_exit(); }
    if (callback_failed) {
        return error(ErrorCode::ExecutionBoundaryFailure, "Cancellation callback threw; cancellation still does not imply physical exit.");
    }
    return CancellationRequest::Requested;
}

} // namespace visionpro::core
