#include "node_state_service.hpp"
#include "runtime_values.hpp"
#include "tool_configuration.hpp"
#include <visionpro/workflow/execution_plan.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace visionpro::workflow::detail {
namespace {
using namespace core;

Diagnostic error(ErrorCode code, WorkflowId workflow, NodeId node, std::string message)
{
    DiagnosticLocation location;
    location.workflow = workflow;
    location.node = node;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

StateDomain domain(StateScope scope, const ToolInvocationIdentity& identity)
{
    switch (scope) {
    case StateScope::Invocation: return InvocationStateDomain{{identity.session, identity.invocation}};
    case StateScope::WorkflowRun: return WorkflowRunStateDomain{identity.session, identity.invocation.workflow_run};
    case StateScope::SolutionSession: return SessionStateDomain{identity.session};
    }
    throw std::logic_error("A fixed state scope must be valid.");
}

bool same_specification(const ToolExecutionConfiguration& previous, const ToolExecutionConfiguration& next)
{
    const auto& a = previous.data().tool;
    const auto& b = next.data().tool;
    const auto& left = *previous.effective_state();
    const auto& right = *next.effective_state();
    return a.id == b.id && a.config_version == b.config_version && a.schema.id == b.schema.id
        && a.schema.version == b.schema.version && left.schema == right.schema
        && left.initial_value == right.initial_value && left.scope == right.scope && left.reset == right.reset
        && left.compatibility == right.compatibility && left.mutation == right.mutation
        && left.concurrency == right.concurrency;
}

bool same_configuration(const ToolExecutionConfiguration& previous, std::span<const Binding> previous_bindings,
    const ToolExecutionConfiguration& next, std::span<const Binding> next_bindings)
{
    const auto same_parameter = [](const FixedToolParameter& a, const FixedToolParameter& b) {
        return a.id == b.id && a.value == b.value;
    };
    const auto same_resource = [](const ToolResourceBinding& a, const ToolResourceBinding& b) {
        return a.slot == b.slot && a.resource == b.resource;
    };
    const auto same_binding = [](const Binding& a, const Binding& b) {
        return a.target == b.target && a.sources == b.sources && a.cardinality == b.cardinality
            && a.conversion == b.conversion
            && std::ranges::is_permutation(a.availability.accepted_absence, b.availability.accepted_absence);
    };
    return std::ranges::is_permutation(previous.data().parameters, next.data().parameters, same_parameter)
        && std::ranges::is_permutation(previous.data().resources, next.data().resources, same_resource)
        && previous.data().shared_targets == next.data().shared_targets
        && std::ranges::is_permutation(previous_bindings, next_bindings, same_binding);
}
} // namespace

struct StateCell {
    StateCell(const NodeStateDefinition& spec, const DataValue& start, NodeStateKey key, WorkflowId flow)
        : definition(spec), initial(start), record{std::move(key), StateInitialization::NotInitialized, {}, {}}, workflow(flow) {}
    const NodeStateDefinition definition;
    const DataValue initial;
    NodeState record;
    const WorkflowId workflow;
    std::once_flag initialize_once;
    std::mutex mutex;
};

struct StateGeneration {
    ToolExecutionConfiguration configuration;
    std::vector<Binding> bindings;
    std::map<ResourceId, ResourceLocation> resources;
    std::uint64_t generation;
    DataValue initial;
    std::shared_ptr<StateCell> session;
};

std::optional<std::uint64_t> serialized_state_generation(const RunStateBindings& bindings, NodeId node) noexcept
{
    const auto found = bindings.find(node);
    if (found == bindings.end()) { return {}; }
    const auto& definition = *found->second->configuration.effective_state();
    if (definition.concurrency != StateConcurrencyPolicy::Serialized || definition.scope == StateScope::Invocation) { return {}; }
    return found->second->generation;
}

InvocationStateLease::InvocationStateLease(ToolInvocationIdentity identity, std::shared_ptr<StateCell> cell)
    : identity_(identity), key_(cell->record.key), cell_(std::move(cell))
{}

Result<DataValue> InvocationStateLease::read() const
{
    const std::lock_guard lock(mutex_);
    if (access_ == Access::Closed) {
        return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Tool state access has expired."};
    }
    const std::lock_guard cell_lock(cell_->mutex);
    return staged_ ? *staged_ : *cell_->record.current_value;
}

Result<std::monostate> InvocationStateLease::write(const DataValue& value)
{
    const std::lock_guard lock(mutex_);
    if (access_ != Access::Open) {
        return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Tool state writes are sealed."};
    }
    if (value.type() != cell_->definition.schema) {
        return error(ErrorCode::TypeMismatch, cell_->workflow, key_.node, "State value must match the declared Schema.");
    }
    if (cell_->definition.mutation == StateMutationTiming::DuringComputation) {
        const std::lock_guard cell_lock(cell_->mutex);
        cell_->record.current_value = value;
    } else {
        staged_ = value;
    }
    return std::monostate{};
}

void InvocationStateLease::seal() noexcept
{
    const std::lock_guard lock(mutex_);
    if (access_ == Access::Open) { access_ = Access::Sealed; }
}

void InvocationStateLease::finish() noexcept
{
    const std::lock_guard lock(mutex_);
    access_ = Access::Closed;
    if (settled_) { cell_.reset(); }
}

void InvocationStateLease::settle(bool succeeded) noexcept
{
    const std::lock_guard lock(mutex_);
    if (settled_) { return; }
    if (succeeded && staged_) {
        const std::lock_guard cell_lock(cell_->mutex);
        cell_->record.current_value = *staged_;
    }
    staged_.reset();
    settled_ = true;
    if (access_ == Access::Closed) { cell_.reset(); }
    else { access_ = Access::Sealed; }
}

StateCatalog NodeStateService::capture()
{
    const std::lock_guard lock(mutex_);
    return versions_;
}

namespace {
Result<std::shared_ptr<StateGeneration>> bind_generation(const ToolExecutionConfiguration& configuration,
    const Node& node, const Solution& solution, StateCatalog& catalog, std::uint64_t& last)
{
    const auto& data = configuration.data();
    const auto& spec = configuration.effective_state();
    const auto key = std::pair{data.workflow, data.node};
    const bool strict = spec->compatibility == StateCompatibilityPolicy::AlwaysNewGeneration;
    std::map<ResourceId, ResourceLocation> resources;
    if (strict) {
        for (const auto& binding : data.resources) {
            const auto it = std::ranges::find(solution.resources, binding.resource.id, &ResourceDefinition::id);
            if (it != solution.resources.end()) { resources.emplace(it->id, it->location); }
        }
    }
    const auto existing = catalog.find(key);
    if (existing != catalog.end() && same_specification(existing->second->configuration, configuration)
        && (!strict || (existing->second->resources == resources
            && same_configuration(existing->second->configuration, existing->second->bindings, configuration, node.bindings)))) {
        return existing->second;
    }
    auto initial = materialize_config_value(spec->initial_value);
    if (const auto* failure = std::get_if<Diagnostic>(&initial)) { return *failure; }
    const auto next = last ? next_state_generation(last) : Result<std::uint64_t>{std::uint64_t{1}};
    if (const auto* failure = std::get_if<Diagnostic>(&next)) { return *failure; }
    last = std::get<std::uint64_t>(next);
    auto version = std::make_shared<StateGeneration>(StateGeneration{configuration,
        strict ? node.bindings : std::vector<Binding>{}, std::move(resources), last, std::get<DataValue>(std::move(initial)), {}});
    if (spec->scope == StateScope::SolutionSession) {
        version->session = std::make_shared<StateCell>(*spec, version->initial,
            NodeStateKey{data.node, last, SessionStateDomain{data.session}}, data.workflow);
    }
    catalog.insert_or_assign(key, version);
    return version;
}

Result<std::monostate> prepare_workflow_state(const ConfigSnapshot& snapshot, const Workflow& flow,
    StateCatalog& catalog, std::uint64_t& last)
{
    std::map<NodeId, const Node*> nodes;
    std::map<NodeId, std::vector<NodeId>> successors;
    for (const auto& node : flow.nodes) { nodes.emplace(node.id, &node); }
    for (const auto& link : flow.control_links) { successors[link.source].push_back(link.target); }
    std::set<NodeId> visited;
    auto pending = flow.entries;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!visited.insert(id).second) { continue; }
        const auto found = nodes.find(id);
        if (found == nodes.end()) { return error(ErrorCode::UnknownTarget, flow.id, id, "Unknown node in the solution execution scope."); }
        const auto& node = *found->second;
        if (!node.enabled) { continue; }
        if (const auto it = successors.find(id); it != successors.end()) {
            pending.insert(pending.end(), it->second.begin(), it->second.end());
        }
        if (const auto* group = std::get_if<GroupConfiguration>(&node.configuration)) {
            pending.insert(pending.end(), group->entries.begin(), group->entries.end());
            continue;
        }
        const auto* tool = snapshot.find_tool(node);
        if (!tool) { return error(ErrorCode::UnknownTool, flow.id, id, "Unknown tool in the solution execution scope."); }
        if (!tool->state) { continue; }
        auto configured = project_tool_configuration(snapshot, flow.id, node);
        if (const auto* failure = std::get_if<Diagnostic>(&configured)) { return *failure; }
        auto generation = bind_generation(std::get<ToolExecutionConfiguration>(configured), node, snapshot.solution(), catalog, last);
        if (const auto* failure = std::get_if<Diagnostic>(&generation)) { return *failure; }
    }
    return std::monostate{};
}
} // namespace

Result<std::monostate> NodeStateService::prepare_solution(const ConfigSnapshot& snapshot)
{
    const std::lock_guard lock(mutex_);
    if (prepared_solution_ == snapshot.identity().snapshot) { return std::monostate{}; }
    const auto& solution = snapshot.solution();
    // Seed configurations before publication can fork this catalog. A compatible cold
    // session node then has one cell, whichever round calls it first. Init stays lazy.
    auto staged = versions_;
    auto last = last_generation_;
    std::map<WorkflowId, std::vector<WorkflowId>> successors;
    for (const auto& link : solution.graph->links) { successors[link.source].push_back(link.target); }
    auto pending = solution.graph->entries;
    std::set<WorkflowId> visited;
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (!visited.insert(id).second) { continue; }
        const auto* flow = find_workflow(solution, id);
        if (!flow || !flow->enabled) { continue; } // Graph endpoints were checked before pinning.
        auto result = prepare_workflow_state(snapshot, *flow, staged, last);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
        if (const auto it = successors.find(id); it != successors.end()) {
            pending.insert(pending.end(), it->second.begin(), it->second.end());
        }
    }
    versions_.swap(staged);
    last_generation_ = last;
    prepared_solution_ = snapshot.identity().snapshot;
    return std::monostate{};
}

Result<RunStateBindings> NodeStateService::bind(const ExecutionPlan& plan, StateCatalog* retired)
{
    const bool isolated = plan.request().debug_state() == DebugStatePolicy::Isolated;
    const auto& solution = plan.request().snapshot().solution();
    const std::lock_guard lock(mutex_);
    // Stage the new catalog. A failed or rejected request cannot partially
    // replace live generations, and admitted older requests retain their cells.
    auto& catalog = plan.request().debug_state() == DebugStatePolicy::IsolatedSession ? debug_versions_
        : (retired ? *retired : versions_);
    auto updated = isolated ? StateCatalog{} : catalog;
    auto last = last_generation_;
    RunStateBindings bindings;
    for (const auto& tool : plan.tools()) {
        const auto& configuration = tool.configuration();
        const auto& data = configuration.data();
        if (!configuration.effective_state()) {
            updated.erase({data.workflow, data.node});
            continue;
        }
        const auto& node = *find_node(*find_workflow(solution, data.workflow), data.node);
        auto bound = bind_generation(configuration, node, solution, updated, last);
        if (const auto* failure = std::get_if<Diagnostic>(&bound)) { return *failure; }
        bindings.emplace(data.node, std::get<std::shared_ptr<StateGeneration>>(std::move(bound)));
    }
    if (!isolated) { catalog.swap(updated); }
    last_generation_ = last;
    return bindings;
}

Result<std::shared_ptr<InvocationStateLease>> NodeStateService::acquire(const InvocationContext& context,
    ToolInstance& instance, const RunStateBindings& bindings, RunStateCells& run_states)
{
    const auto& data = context.data();
    const auto& definition = data.configuration.effective_state();
    if (!definition) { return std::shared_ptr<InvocationStateLease>{}; }
    const auto& configured = data.configuration.data();
    const auto& version = *bindings.at(configured.node);
    auto cell = version.session;
    if (!cell) {
        const auto cached = run_states.find(configured.node);
        if (cached != run_states.end()) {
            cell = cached->second;
        } else {
            cell = std::make_shared<StateCell>(*definition, version.initial, NodeStateKey{configured.node,
                version.generation, domain(definition->scope, data.identity)}, configured.workflow);
            if (definition->scope == StateScope::WorkflowRun) { run_states.emplace(configured.node, cell); }
        }
    }
    auto& record = cell->record;
    std::call_once(cell->initialize_once, [&] {
        auto initialized = initialize_tool_state(instance, context, cell->initial);
        if (auto* failure = std::get_if<Diagnostic>(&initialized)) {
            record.initialization_error = std::move(*failure);
            record.initialization = StateInitialization::Failed;
        } else {
            record.current_value = std::get<DataValue>(std::move(initialized));
            record.initialization = StateInitialization::Ready;
        }
    });
    if (record.initialization_error) { return *record.initialization_error; }
    return std::make_shared<InvocationStateLease>(data.identity, std::move(cell));
}

Result<std::monostate> NodeStateService::reset(WorkflowId workflow, NodeId node)
{
    const std::lock_guard lock(mutex_);
    const auto found = versions_.find({workflow, node});
    if (found == versions_.end() || !found->second->session) {
        return error(ErrorCode::InvalidState, workflow, node, "The node has no initialized session state.");
    }
    auto& cell = *found->second->session;
    const std::lock_guard cell_lock(cell.mutex);
    if (cell.definition.reset != StateResetPolicy::DomainStartAndExplicit) {
        return error(ErrorCode::MissingCapability, workflow, node, "The node does not allow explicit state reset.");
    }
    if (cell.record.initialization_error) { return *cell.record.initialization_error; }
    if (cell.record.initialization != StateInitialization::Ready) {
        return error(ErrorCode::InvalidState, workflow, node, "The node has no initialized session state.");
    }
    cell.record.current_value = cell.initial;
    return std::monostate{};
}

void NodeStateService::clear() noexcept
{
    const std::lock_guard lock(mutex_);
    versions_.clear();
    debug_versions_.clear();
    prepared_solution_.reset();
}

} // namespace visionpro::workflow::detail
