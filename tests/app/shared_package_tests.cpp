#include "tool_catalog.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/subgraph_package.hpp>
#include <visionpro/workflow/copy.hpp>
#include <visionpro/workflow/validation.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::infrastructure;
namespace fs = std::filesystem;

template<class T> T take(Result<T> result)
{
    if (const auto* bad = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(bad->message); }
    return std::get<T>(std::move(result));
}
void check(bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } }
template<SchemaIdentity T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / ("visionpro-s64-package-" + take(IdFactory::generate<MessageId>()).to_string());
    TemporaryDirectory() { check(fs::create_directory(path), "Cannot create an isolated package directory."); }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

std::vector<SharedTargetId> targets(const Node& node)
{
    const auto* tool = std::get_if<ToolConfiguration>(&node.configuration);
    return tool ? tool->shared_targets : std::vector<SharedTargetId>{};
}

void independent_copy(const Solution& source, const ToolRegistry& registry)
{
    const auto cloned = take(clone_solution(source, ExternalReferencePolicy::Reject));
    check(source.id != cloned.solution.id, "An independent copy must have a new solution identity.");
    take(validate_solution(cloned.solution, registry.descriptors()));
    std::set<QueueId> queues;
    std::set<VariableId> variables;
    for (const auto& workflow : source.workflows) {
        const auto* copy = find_workflow(cloned.solution, cloned.identities.workflows.at(workflow.id));
        check(copy && copy->id != workflow.id, "Workflow remap must identify a fresh copied workflow.");
        for (const auto& node : workflow.nodes) {
            const auto* copied = find_node(*copy, cloned.identities.nodes.at(node.id));
            check(copied && copied->id != node.id, "Each copied node has a fresh identity.");
            const auto original = targets(node), remapped = targets(*copied);
            check(original.size() == remapped.size(), "Copy must retain every typed shared target.");
            for (std::size_t i = 0; i < original.size(); ++i) {
                if (const auto* id = std::get_if<VariableId>(&original[i])) {
                    variables.insert(*id);
                    check(remapped[i] == SharedTargetId{cloned.identities.variables.at(*id)} && remapped[i] != original[i],
                        "Variable targets must map to the copied variable in their original order.");
                } else {
                    const auto queue = std::get<QueueId>(original[i]);
                    queues.insert(queue);
                    check(remapped[i] == SharedTargetId{cloned.identities.queues.at(queue)} && remapped[i] != original[i],
                        "Queue targets must map to the copied queue in their original order.");
                }
            }
        }
    }
    check(variables.size() >= 2 && queues.size() >= 2, "The representative copy must cover multiple distinct variables and queues.");
}

void portable_workflows(const SolutionDocument& document, const ToolRegistry& registry)
{
    const auto& source = document.solution();
    TemporaryDirectory temp;
    auto host = source;
    host.id = take(IdFactory::generate<SolutionId>());
    const auto workflow_id = take(IdFactory::generate<WorkflowId>());
    const auto node_id = take(IdFactory::generate<NodeId>());
    host.workflows = {{workflow_id, "Existing workflow", {key<SchemaId>("s64.test.host"), 1},
        {{node_id, "Add", ToolConfiguration{key<ToolTypeId>("visionpro.sim.add"), 1},
            {{key<InputId>("left"), {ConfigValue::integer(2)}}, {key<InputId>("right"), {ConfigValue::integer(3)}}}}},
        {}, {}, {node_id}}};
    host.layouts.clear();
    for (auto& def : host.queues) { def.name = "Renamed host queue"; }
    for (auto& def : host.variables) { def.name = "Renamed host variable"; }
    const auto wrong_record = take(DataType::record(take(SchemaIdFactory::parse<SchemaId>("s64.test.wrong")), 1,
        {{take(SchemaIdFactory::parse<FieldId>("other")), DataType::integer()}}));
    for (const auto& workflow : source.workflows) {
        const auto package = temp.path / workflow.id.to_string();
        take(export_subgraph_package(package, source, workflow.id, {}, registry, &document));
        const auto imported = take(import_subgraph_package(package, host, {}, {}, registry));
        const auto* added = find_workflow(imported.solution, imported.workflow);
        check(added && added->id != workflow.id && added->nodes.size() == workflow.nodes.size()
            && imported.solution.workflows.size() == 2 && !imported.group, "A workflow package adds one fresh workflow.");
        check(imported.solution.variables.size() == host.variables.size() && imported.solution.queues.size() == host.queues.size(),
            "Import must bind the existing host catalog without adding duplicate shared definitions.");
        take(validate_solution(imported.solution, registry.descriptors()));
        std::set<SharedTargetId> needed;
        for (std::size_t i = 0; i < workflow.nodes.size(); ++i) {
            const auto original = targets(workflow.nodes[i]);
            check(added->nodes[i].id != workflow.nodes[i].id && targets(added->nodes[i]) == original,
                "Fresh imported nodes retain exact host target kinds, UUIDs and order.");
            needed.insert(original.begin(), original.end());
        }
        const auto denied = [&](const Solution& candidate, SubgraphHostCapabilities capabilities = {}) {
            auto result = import_subgraph_package(package, candidate, {}, {}, registry, nullptr, capabilities);
            const auto* bad = std::get_if<Diagnostic>(&result);
            check(bad && bad->code == ErrorCode::MissingCapability && candidate.workflows.size() == 1
                && candidate.workflows.front().id == workflow_id && candidate.workflows.front().nodes.front().id == node_id,
                "Failed import must leave the existing workflow and node intact.");
            take(validate_solution(candidate, registry.descriptors()));
            return *bad;
        };
        denied(host, {true, true, false});
        for (const auto& target : needed) {
            auto missing = host, wrong = host;
            if (const auto* id = std::get_if<VariableId>(&target)) {
                std::erase_if(missing.variables, [&](const auto& value) { return value.id == *id; });
                auto& other = *std::ranges::find(wrong.variables, *id, &GlobalVariableDefinition::id);
                other.type = DataType::boolean(); other.initial_value = ConfigValue::boolean(false);
                check(denied(missing).location.variable == *id && denied(wrong).location.variable == *id,
                    "Missing and mismatched host variables must identify the exact required UUID.");
            } else {
                const auto queue = std::get<QueueId>(target);
                std::erase_if(missing.queues, [&](const auto& value) { return value.id == queue; });
                std::ranges::find(wrong.queues, queue, &DataQueueDefinition::id)->message_schema = wrong_record;
                check(denied(missing).location.queue == queue && denied(wrong).location.queue == queue,
                    "Missing and mismatched queue schemas must identify the exact required UUID.");
            }
        }
    }
    auto engine = take(WorkflowEngine::create(registry));
    const auto snapshot = take(ConfigSnapshot::capture(host, registry.descriptors(),
        {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}));
    auto submitted = engine->submit(take(RunRequest::create(snapshot, WorkflowTarget{workflow_id}, {},
        {TriggerKind::Manual, key<SchemaId>("s64.package.test")})));
    if (const auto* bad = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(bad->diagnostic.message); }
    const auto run = std::get<RunHandle>(std::move(submitted));
    const auto done = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + std::chrono::seconds(5)));
    check(done.outcome == RunWaitOutcome::Satisfied && done.progress.status == ExecutionStatus::Succeeded
        && done.progress.nodes.size() == 1 && done.progress.nodes.front().invocation, "The original host workflow still runs after all rejected imports.");
    const auto record = take(run.lookup(*done.progress.nodes.front().invocation));
    const auto* slot = record ? record->output(key<OutputId>("value")) : nullptr;
    check(slot && std::get<DataValue>(slot->content) == DataValue::integer(5), "The retained host workflow still computes 2 + 3 = 5.");
    take(validate_solution(source, registry.descriptors()));
}
} // namespace

int main()
{
    try {
        const auto registry = take(visionpro::app::make_tool_registry());
        const auto source = take(load_solution(fs::path{VISIONPRO_SHARED_DIRECTORY}, registry));
        check(source.can_save() && source.diagnostics().empty(), "The S6.4 example must load with the production catalog.");
        independent_copy(source.solution(), registry);
        portable_workflows(source, registry);
        std::cout << "S6.4 shared targets passed copy, portable host binding and rejected import checks.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
