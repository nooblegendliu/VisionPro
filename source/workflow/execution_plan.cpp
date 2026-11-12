#include <visionpro/workflow/execution_plan.hpp>
#include "validation_scope.hpp"

#include <algorithm>
#include <set>

namespace visionpro::workflow {
namespace {
using namespace core;

DiagnosticLocation location(const RunRequest& request, std::optional<NodeId> node = {})
{
    DiagnosticLocation where;
    where.solution = request.snapshot().solution().id;
    where.workflow = std::visit([](const auto& target) { return target.workflow; }, request.target());
    where.node = node;
    return where;
}

Diagnostic unsupported(DiagnosticLocation where, std::string message)
{ return {ErrorCode::MissingCapability, Severity::Error, std::move(where), std::move(message)}; }

Result<std::monostate> check_control(std::span<const NodeId> entries, const ScopeDependencies& scope,
    DiagnosticLocation where)
{
    if (entries.size() > 1) { return unsupported(where, "This execution profile requires a single entry."); }
    std::set<NodeId> outgoing, incoming;
    for (const auto& link : scope.control_links) {
        if (link.output || !outgoing.insert(link.source).second || !incoming.insert(link.target).second) {
            where.node = link.target; where.control_link = link.id;
            return unsupported(where, "Named control outlets, branching and joins require control graph support.");
        }
    }
    return std::monostate{};
}

Result<std::monostate> check_tool(const ToolExecutionConfiguration& configuration,
    const ExecutionCapabilities& capabilities, DiagnosticLocation where)
{
    const auto& data = configuration.data();
    where.tool = data.tool.id;
    if (data.tool.execution.lifetime != ToolInstanceLifetime::Invocation) {
        return unsupported(where, "This planner supports invocation-lifetime tool instances.");
    }
    if (data.tool.execution.mode == ToolExecutionMode::Asynchronous && !capabilities.asynchronous) {
        return unsupported(where, "The execution profile does not support asynchronous tools.");
    }
    if ((configuration.effective_state() || data.tool.execution.effects == ToolEffects::State)
        && !capabilities.node_state) {
        return unsupported(where, "The execution profile does not provide a node state service.");
    }
    for (const auto& resource : data.resources) {
        if (std::ranges::find(capabilities.resource_leases, resource.resource.kind) == capabilities.resource_leases.end()) {
            where.resource = resource.resource.id;
            return unsupported(where, "The execution profile cannot provide a lease for this resource kind.");
        }
    }
    return std::monostate{};
}
} // namespace

struct ExecutionPlan::Storage {
    RunRequest request;
    ToolRegistry registry;
    ExecutionCapabilities capabilities;
    std::shared_ptr<const ValidationIndex> index;
    std::vector<NodeId> entries;
    std::vector<PreparedTool> tools;
};

core::Result<ExecutionPlan> ExecutionPlan::create(const RunRequest& request, const ToolRegistry& registry,
    const ExecutionCapabilities& capabilities)
{
    auto where = location(request);
    const auto* target = std::get_if<NodeTarget>(&request.target());
    if (target) { where.node = target->node; }
    if (request.trigger().kind == TriggerKind::Continuous || request.trigger().kind == TriggerKind::External) {
        return unsupported(where, "Only manual and debug triggers can be planned.");
    }
    if (request.debug_state() == DebugStatePolicy::Active) {
        return unsupported(where, "Independent access to active state requires state service arbitration.");
    }
    auto validated = detail::validate_execution_scope(request);
    if (const auto* failure = std::get_if<Diagnostic>(&validated)) { return *failure; }
    auto index = std::make_shared<const ValidationIndex>(std::get<ValidationIndex>(std::move(validated)));
    const auto& scope = index->scopes.front();
    const auto& snapshot = request.snapshot();
    const auto& flow = *find_workflow(snapshot.solution(), scope.workflow);
    auto entries = target ? std::vector<NodeId>{target->node} : flow.entries;
    if (!capabilities.control_graph) {
        if (auto checked = check_control(entries, scope, where);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    }
    const BindingResolver resolver{snapshot, index, request};
    std::vector<PreparedTool> tools;
    tools.reserve(scope.reachable.size());
    for (const auto& id : scope.reachable) {
        const auto& configured = std::get<ToolConfiguration>(find_node(flow, id)->configuration);
        if (auto checked = registry.validate(*snapshot.find_tool(configured.type));
            auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.solution = where.solution; failure->location.workflow = flow.id; failure->location.node = id;
            return *failure;
        }
        auto prepared = PreparedTool::create(resolver, flow.id, id);
        if (const auto* failure = std::get_if<Diagnostic>(&prepared)) { return *failure; }
        auto& tool = std::get<PreparedTool>(prepared);
        if (auto checked = check_tool(tool.configuration(), capabilities, location(request, id));
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        tools.push_back(std::move(tool));
    }
    return ExecutionPlan{std::make_shared<const Storage>(
        Storage{request, registry, capabilities, std::move(index), std::move(entries), std::move(tools)})};
}

const RunRequest& ExecutionPlan::request() const noexcept { return storage_->request; }
const ExecutionCapabilities& ExecutionPlan::capabilities() const noexcept { return storage_->capabilities; }
std::span<const core::NodeId> ExecutionPlan::entries() const noexcept { return storage_->entries; }
const ValidationIndex& ExecutionPlan::dependencies() const noexcept { return *storage_->index; }

const PreparedTool* ExecutionPlan::tool(const core::NodeId& node) const noexcept
{
    const auto found = std::ranges::find_if(storage_->tools,
        [&](const auto& tool) { return tool.configuration().data().node == node; });
    return found == storage_->tools.end() ? nullptr : &*found;
}

core::Result<std::shared_ptr<core::ToolInstance>> ExecutionPlan::create_instance(const core::NodeId& node) const
{
    const auto* prepared = tool(node);
    if (prepared) {
        const auto& configured = prepared->configuration().data();
        const auto* flow = find_workflow(request().snapshot().solution(), configured.workflow);
        if (find_node(*flow, node)->enabled) { return storage_->registry.create_instance(prepared->configuration()); }
    }
    return Diagnostic{ErrorCode::IncompleteExecutionScope, Severity::Error, location(request(), node),
        "Instance creation requires an enabled tool in the execution plan."};
}

} // namespace visionpro::workflow
