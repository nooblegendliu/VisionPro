#include <visionpro/workflow/tool_execution.hpp>

#include <algorithm>
#include <set>

namespace visionpro::workflow {
namespace {
using namespace core;
using Check = Result<std::monostate>;
Diagnostic error(ErrorCode code, std::string message, const ConfigSnapshot& snapshot,
    WorkflowId workflow, NodeId node)
{
    DiagnosticLocation location;
    location.solution = snapshot.solution().id; location.workflow = workflow; location.node = node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}
Result<ConfigValue> fixed_parameter(const ParameterDescriptor& parameter, const Node& node)
{
    const auto binding = std::ranges::find(node.bindings, BindingTarget{parameter.id}, &Binding::target);
    if (binding == node.bindings.end()) { return validate_parameter_value(parameter, {}); }
    std::vector<ConfigValue> values;
    for (const auto& source : binding->sources) {
        const auto* value = std::get_if<ConfigValue>(&source);
        if (!value) {
            DiagnosticLocation location; location.parameter = parameter.id;
            return Diagnostic{ErrorCode::DisallowedBindingSource, Severity::Error, location,
                "SnapshotBuild parameter cannot be projected from a runtime source."};
        }
        switch (binding->conversion) {
        case ConversionKind::Exact: values.push_back(*value); break;
        case ConversionKind::WrapOptional: values.push_back(ConfigValue::some(*value)); break;
        case ConversionKind::Int64ToFloat64Explicit: {
            if (!value->as_int64()) {
                return Diagnostic{ErrorCode::TypeMismatch, Severity::Error, {}, "Explicit numeric conversion requires Int64."};
            }
            auto converted = ConfigValue::real(static_cast<double>(*value->as_int64()));
            if (const auto* failure = std::get_if<Diagnostic>(&converted)) { return *failure; }
            values.push_back(std::get<ConfigValue>(converted));
            break;
        }
        default: return Diagnostic{ErrorCode::UnsupportedConversion, Severity::Error, {}, "Unknown fixed-parameter conversion."};
        }
    }
    if (values.empty()) { return Diagnostic{ErrorCode::MissingRequiredValue, Severity::Error, {}, "Fixed parameter has no constant source."}; }
    if (binding->cardinality == BindingCardinality::MultipleSources) {
        if (parameter.type.kind() != TypeKind::List) {
            return Diagnostic{ErrorCode::TypeMismatch, Severity::Error, {}, "Multiple fixed sources require a List target."};
        }
        auto combined = ConfigValue::list(*parameter.type.element_type(), values);
        if (const auto* failure = std::get_if<Diagnostic>(&combined)) { return *failure; }
        return validate_parameter_value(parameter, std::get<ConfigValue>(combined));
    }
    return validate_parameter_value(parameter, values.front());
}
ToolInvocationIdentity projected_identity(const ResultIdentity& result)
{ return {result.run.snapshot.session, result.run.snapshot.snapshot, result.invocation, result.attempt}; }

Check validate_lease(const ToolResourceBinding& binding, const std::shared_ptr<const ResourceLease>& lease)
{
    DiagnosticLocation location;
    location.resource = binding.resource.id;
    if (!lease) {
        return Diagnostic{ErrorCode::InvalidResource, Severity::Error, location,
            "An invocation lease requires non-null ownership."};
    }
    if (lease->reference() != binding.resource) {
        return Diagnostic{ErrorCode::ResourceVersionMismatch, Severity::Error, location,
            "Invocation lease does not match the fixed resource identity, kind and revision."};
    }
    return std::monostate{};
}

Check validate_leases(const ToolExecutionConfiguration& configuration,
    std::span<const InvocationResourceLease> leases)
{
    std::set<SchemaId> slots;
    for (const auto& bound : leases) {
        const auto configured = std::ranges::find(configuration.data().resources, bound.slot, &ToolResourceBinding::slot);
        if (!slots.insert(bound.slot).second || configured == configuration.data().resources.end()) {
            return Diagnostic{ErrorCode::InvalidResource, Severity::Error, {}, "Invocation leases require unique configured slots."};
        }
        auto checked = validate_lease(*configured, bound.lease);
        if (auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    }
    if (slots.size() != configuration.data().resources.size()) {
        return Diagnostic{ErrorCode::MissingRequiredValue, Severity::Error, {}, "Each configured resource slot requires its explicit owning lease."};
    }
    return std::monostate{};
}

Result<std::shared_ptr<const ResourceLease>> acquire_lease(InvocationResourceProvider& provider,
    const ToolExecutionConfiguration& configuration, const ToolResourceBinding& binding)
{
    try {
        return provider.acquire(configuration, binding);
    } catch (...) {
        return Diagnostic{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {},
            "Resource provider threw across its execution boundary."};
    }
}

Result<std::vector<InvocationResourceLease>> acquire_leases(InvocationResourceProvider& provider,
    const ToolExecutionConfiguration& configuration)
{
    std::vector<InvocationResourceLease> leases;
    leases.reserve(configuration.data().resources.size());
    for (const auto& binding : configuration.data().resources) {
        auto acquired = acquire_lease(provider, configuration, binding);
        if (auto* failure = std::get_if<Diagnostic>(&acquired)) {
            if (!failure->location.resource) { failure->location.resource = binding.resource.id; }
            return std::move(*failure);
        }
        auto lease = std::get<std::shared_ptr<const ResourceLease>>(std::move(acquired));
        auto checked = validate_lease(binding, lease);
        if (auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
        leases.push_back({binding.slot, std::move(lease)});
    }
    return leases;
}
} // namespace

core::Result<PreparedTool> PreparedTool::create(const ConfigSnapshot& snapshot, core::WorkflowId workflow,
    core::NodeId node)
{
    auto resolver = BindingResolver::create(snapshot);
    if (const auto* failure = std::get_if<core::Diagnostic>(&resolver)) { return *failure; }
    return create(std::get<BindingResolver>(std::move(resolver)), workflow, node);
}

core::Result<PreparedTool> PreparedTool::create(BindingResolver resolver, core::WorkflowId workflow,
    core::NodeId node)
{
    const auto& snapshot = resolver.snapshot();
    const auto* flow = find_workflow(snapshot.solution(), workflow);
    const auto* configured = flow ? find_node(*flow, node) : nullptr;
    const auto* tool_config = configured ? std::get_if<ToolConfiguration>(&configured->configuration) : nullptr;
    if (!flow || !configured || !tool_config) {
        return error(core::ErrorCode::UnknownTarget, "Tool execution projection requires a declared tool node, not a Group.", snapshot, workflow, node);
    }
    const auto* tool = snapshot.find_tool(tool_config->type);
    if (!tool) { return error(core::ErrorCode::UnknownTool, "Tool execution metadata is absent from the snapshot.", snapshot, workflow, node); }
    core::ToolExecutionConfigurationData data{snapshot.identity().session, snapshot.identity().snapshot,
        workflow, node, *tool, {}, {}};
    for (const auto& parameter : tool->parameters) {
        if (parameter.binding.time != core::BindingTime::SnapshotBuild) { continue; }
        auto value = fixed_parameter(parameter, *configured);
        if (const auto* failure = std::get_if<core::Diagnostic>(&value)) {
            auto diagnostic = *failure;
            diagnostic.location.solution = snapshot.solution().id; diagnostic.location.workflow = workflow;
            diagnostic.location.node = node; diagnostic.location.parameter = parameter.id;
            return diagnostic;
        }
        data.parameters.push_back({parameter.id, std::get<core::ConfigValue>(std::move(value))});
    }
    for (const auto& resource : configured->resources) {
        if (auto checked = core::validate_resource_reference(resource.resource, snapshot.solution().resources);
            const auto* failure = std::get_if<core::Diagnostic>(&checked)) { return *failure; }
        auto fixed = resource.resource;
        const auto definition = std::ranges::find(snapshot.solution().resources, fixed.id, &core::ResourceDefinition::id);
        if (const auto* managed = std::get_if<core::ManagedResource>(&definition->location)) { fixed.revision = managed->revision; }
        else if (const auto* external = std::get_if<core::ExternalFileResource>(&definition->location)) { fixed.revision = external->revision; }
        data.resources.push_back({resource.slot, fixed});
    }
    auto projected = core::ToolExecutionConfiguration::create(data);
    if (const auto* failure = std::get_if<core::Diagnostic>(&projected)) { return *failure; }
    return PreparedTool{snapshot, std::move(resolver), std::get<core::ToolExecutionConfiguration>(std::move(projected))};
}

InvocationPreparation PreparedTool::prepare(const ResolutionContext& context, core::AttemptId attempt,
    const ResultStore& results, std::span<const core::InvocationResourceLease> leases) const
{
    return prepare(context, attempt, results, leases, nullptr);
}

InvocationPreparation PreparedTool::prepare(const ResolutionContext& context, core::AttemptId attempt,
    const ResultStore& results, InvocationResourceProvider& provider) const
{
    return prepare(context, attempt, results, {}, &provider);
}

InvocationPreparation PreparedTool::prepare(const ResolutionContext& context, core::AttemptId attempt,
    const ResultStore& results, std::span<const core::InvocationResourceLease> leases,
    InvocationResourceProvider* provider) const
{
    const auto& fixed = configuration_.data();
    const auto fail = [&](core::ErrorCode code, std::string message) {
        return InvocationFailure{error(code, std::move(message), snapshot_, fixed.workflow, fixed.node), {}};
    };
    if (context.run.snapshot != snapshot_.identity() || context.run.workflow != fixed.workflow
        || context.consumer.node != fixed.node || context.consumer.workflow_run != context.run.run) {
        return fail(core::ErrorCode::ResultIdentityMismatch, "Invocation projection must use the prepared node, snapshot and run.");
    }
    if (auto checked = resolver_.validate_context(context, results); const auto* failure = std::get_if<core::Diagnostic>(&checked)) {
        return InvocationFailure{*failure, {}};
    }
    std::vector<TargetResolution> bindings;
    for (const auto& input : fixed.tool.inputs) { bindings.push_back({input.id, resolver_.resolve(context, input.id, results)}); }
    for (const auto& parameter : fixed.tool.parameters) { bindings.push_back({parameter.id, resolver_.resolve(context, parameter.id, results)}); }
    std::optional<core::Diagnostic> failure;
    bool unavailable = false, pending = false;
    for (const auto& target : bindings) {
        if (const auto* invalid = std::get_if<BindingFailure>(&target.resolution)) {
            if (!failure) { failure = invalid->diagnostic; }
        } else if (std::holds_alternative<UnavailableBinding>(target.resolution)) { unavailable = true; }
        else if (std::holds_alternative<PendingBinding>(target.resolution)) { pending = true; }
    }
    if (failure) { return InvocationFailure{*failure, std::move(bindings)}; }
    if (unavailable) { return UnavailableInvocation{std::move(bindings)}; }
    if (pending) { return PendingInvocation{std::move(bindings)}; }
    const auto resource_failure = [&](core::Diagnostic diagnostic) {
        diagnostic.location.solution = snapshot_.solution().id;
        diagnostic.location.workflow = fixed.workflow;
        diagnostic.location.node = fixed.node;
        return InvocationFailure{std::move(diagnostic), std::move(bindings)};
    };
    std::vector<core::InvocationResourceLease> resources;
    if (provider) {
        auto acquired = acquire_leases(*provider, configuration_);
        if (auto* invalid = std::get_if<core::Diagnostic>(&acquired)) { return resource_failure(std::move(*invalid)); }
        resources = std::get<std::vector<core::InvocationResourceLease>>(std::move(acquired));
    } else {
        auto checked = validate_leases(configuration_, leases);
        if (auto* invalid = std::get_if<core::Diagnostic>(&checked)) { return resource_failure(std::move(*invalid)); }
        resources.assign(leases.begin(), leases.end());
    }
    core::InvocationContextData data{configuration_,
        {fixed.session, fixed.snapshot, context.consumer, attempt}, {}, {}, {}, std::move(resources)};
    for (const auto& target : bindings) {
        const auto& resolved = std::get<ResolvedBinding>(target.resolution);
        if (const auto* input = std::get_if<core::InputId>(&target.target)) { data.inputs.push_back({*input, resolved.value}); }
        else { data.parameters.push_back({std::get<core::ParameterId>(target.target), resolved.value}); }
        for (const auto& region : resolved.regions) {
            data.regions.push_back({target.target, region.reference, region.value, projected_identity(region.identity),
                region.source_index, region.path});
        }
    }
    auto invocation = core::InvocationContext::create(data);
    if (const auto* invalid = std::get_if<core::Diagnostic>(&invocation)) { return InvocationFailure{*invalid, std::move(bindings)}; }
    return PreparedInvocation{snapshot_, std::get<core::InvocationContext>(std::move(invocation)), std::move(bindings)};
}

core::Result<NodeResult> checked_node_result(const PreparedInvocation& invocation, const core::ToolCompletion& completion)
{
    const auto& context = invocation.context.data();
    const auto& configuration = context.configuration.data();
    const auto fail = [&](std::string message) {
        return error(core::ErrorCode::ResultIdentityMismatch, std::move(message), invocation.snapshot,
            configuration.workflow, configuration.node);
    };
    if (completion.identity != context.identity || invocation.snapshot.identity().session != configuration.session
        || invocation.snapshot.identity().snapshot != configuration.snapshot) {
        return fail("Completion must match the prepared snapshot, node, invocation and attempt exactly.");
    }
    if (auto checked = core::validate_tool_completion(invocation.context, completion);
        auto* invalid = std::get_if<core::Diagnostic>(&checked)) {
        invalid->location.solution = invocation.snapshot.solution().id;
        return std::move(*invalid);
    }
    const RunIdentity run{invocation.snapshot.identity(), configuration.workflow, completion.identity.invocation.workflow_run};
    NodeResultData data{{run, completion.identity.invocation, completion.identity.attempt}, ExecutionStatus::Succeeded,
        Judgement::NotEvaluated, {}, completion.diagnostics, completion.elapsed, completion.selected_control_outputs,
        completion.timings};
    switch (completion.status) {
    case core::ToolCompletionStatus::Succeeded: data.status = ExecutionStatus::Succeeded; break;
    case core::ToolCompletionStatus::Failed: data.status = ExecutionStatus::Failed; break;
    case core::ToolCompletionStatus::Cancelled: data.status = ExecutionStatus::Cancelled; break;
    case core::ToolCompletionStatus::TimedOut: data.status = ExecutionStatus::TimedOut; break;
    default: return error(core::ErrorCode::InvalidNodeResult, "Unknown tool completion status.", invocation.snapshot, configuration.workflow, configuration.node);
    }
    switch (completion.judgement) {
    case core::ToolJudgement::NotEvaluated: data.judgement = Judgement::NotEvaluated; break;
    case core::ToolJudgement::OK: data.judgement = Judgement::OK; break;
    case core::ToolJudgement::NG: data.judgement = Judgement::NG; break;
    default: return error(core::ErrorCode::InvalidNodeResult, "Unknown tool judgement.", invocation.snapshot, configuration.workflow, configuration.node);
    }
    for (const auto& output : completion.outputs) {
        const auto declaration = std::ranges::find(configuration.tool.outputs, output.id, &core::OutputDescriptor::id);
        auto& slot = data.outputs.emplace_back(OutputSlot{output.id, declaration->type, output.content});
        if (auto* invalid = std::get_if<InvalidOutput>(&slot.content)) {
            invalid->diagnostic.location.solution = invocation.snapshot.solution().id;
            invalid->diagnostic.location.workflow = configuration.workflow;
            invalid->diagnostic.location.node = configuration.node;
            invalid->diagnostic.location.tool = configuration.tool.id;
            invalid->diagnostic.location.output = output.id;
        }
    }
    if (data.status != ExecutionStatus::Succeeded) {
        const auto reason = data.status == ExecutionStatus::Failed ? OutputAbsenceReason::Failed
            : data.status == ExecutionStatus::Cancelled ? OutputAbsenceReason::Cancelled : OutputAbsenceReason::TimedOut;
        for (const auto& output : configuration.tool.outputs) {
            if (std::ranges::find(completion.outputs, output.id, &core::ToolOutput::id) == completion.outputs.end()) {
                data.outputs.push_back({output.id, output.type, NotProduced{reason}});
            }
        }
    }
    auto result = NodeResult::create(data);
    if (const auto* invalid = std::get_if<core::Diagnostic>(&result)) { return *invalid; }
    ResultStore checked(invocation.snapshot.identity().session);
    if (auto inserted = checked.insert(invocation.snapshot, std::get<NodeResult>(result));
        const auto* invalid = std::get_if<core::Diagnostic>(&inserted)) { return *invalid; }
    return result;
}

} // namespace visionpro::workflow
