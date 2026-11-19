#include "logic_tools.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template<SchemaIdentity T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }
template<UuidIdentity T> T id() { return take(IdFactory::generate<T>()); }

void check(bool condition, std::string_view message)
{ if (!condition) { throw std::runtime_error(std::string(message)); } }

RunHandle run(const ToolRegistry& registry, const ToolDescriptor& tool, std::vector<Binding> bindings)
{
    const auto schema = key<SchemaId>("s63.logic.test");
    const Node node{id<NodeId>(), "Subject", ToolConfiguration{tool.id, 1}, std::move(bindings)};
    const Workflow flow{id<WorkflowId>(), "Logic", {schema, 1}, {node}, {}, {}, {node.id}};
    const Solution solution{id<SolutionId>(), "Logic tests", 1, 1, {flow}};
    const auto engine = take(WorkflowEngine::create(registry));
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(),
        {engine->session(), take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}));
    auto submission = engine->submit(take(RunRequest::create(snapshot, WorkflowTarget{flow.id}, {}, {TriggerKind::Manual, schema})));
    if (const auto* rejected = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(rejected->diagnostic.message); }
    auto handle = std::get<RunHandle>(submission);
    const auto done = take(handle.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(done.outcome == RunWaitOutcome::Satisfied && done.progress.quiescent, "Logic operation failed to quiesce.");
    return handle;
}

void arithmetic(const ToolRegistry& registry)
{
    const auto& tool = *registry.find_descriptor(key<ToolTypeId>("visionpro.calc.int64"));
    const auto& operation = tool.parameters.front();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    struct Case { const char* operation; std::int64_t left; std::int64_t right; std::optional<std::int64_t> expected; };
    const Case cases[] = {
        {"Add", max, 0, max}, {"Add", max, 1, {}}, {"Add", min, -1, {}},
        {"Subtract", 0, -1, 1}, {"Subtract", min, 1, {}}, {"Subtract", 0, min, {}}, {"Subtract", -1, min, max},
        {"Multiply", min, 1, min}, {"Multiply", max, -1, -max}, {"Multiply", -3, -4, 12},
        {"Multiply", -1, min, {}}, {"Multiply", max, 2, {}}, {"Multiply", -3, max, {}}, {"Multiply", min, 0, 0},
        {"Divide", -7, 3, -2}, {"Divide", min, -1, {}}, {"Divide", 1, 0, {}}};
    for (const auto& item : cases) {
        const auto result = run(registry, tool, {{key<InputId>("left"), {ConfigValue::integer(item.left)}},
            {key<InputId>("right"), {ConfigValue::integer(item.right)}},
            {operation.id, {take(ConfigValue::enumeration(operation.type, key<EnumMemberId>(item.operation)))}}});
        const auto progress = take(result.inspect());
        if (item.expected) {
            check(progress.status == ExecutionStatus::Succeeded, "A representable integer calculation failed.");
            const auto record = take(result.lookup(*progress.nodes.front().invocation));
            const auto& value = std::get<DataValue>(record->output(key<OutputId>("value"))->content);
            check(*value.as_int64() == *item.expected, "Unexpected integer result.");
        } else {
            check(progress.status == ExecutionStatus::Failed && std::ranges::any_of(progress.diagnostics,
                [](const auto& d) { return d.code == ErrorCode::InvalidValue; }), "Invalid integer arithmetic must fail explicitly.");
        }
    }
}

void formatting(const ToolRegistry& registry)
{
    const auto& tool = *registry.find_descriptor(key<ToolTypeId>("visionpro.format.int64_list"));
    const auto list = [](std::initializer_list<std::int64_t> values) {
        std::vector<ConfigValue> elements;
        for (const auto value : values) { elements.push_back(ConfigValue::integer(value)); }
        return take(ConfigValue::list(DataType::integer(), elements));
    };
    const auto failed = [&](std::vector<Binding> bindings, ErrorCode code, std::string_view parameter) {
        const auto progress = take(run(registry, tool, std::move(bindings)).inspect());
        check(progress.status == ExecutionStatus::Failed && std::ranges::any_of(progress.diagnostics,
            [&](const auto& d) { return d.code == code && d.location.parameter == key<ParameterId>(parameter); }),
            "Invalid formatter configuration must identify its exact parameter.");
    };
    for (auto pattern : {"literal", "{other}", "{value}{value}", "{value}}"}) {
        failed({{key<ParameterId>("format"), {take(ConfigValue::string(pattern))}}}, ErrorCode::ConstraintViolation, "format");
    }
    failed({{key<ParameterId>("indices"), {list({0, 0})}}, {key<ParameterId>("skip_out_of_range"), {list({0})}}},
        ErrorCode::IndexOutOfRange, "indices");
    failed({{key<ParameterId>("indices"), {list({0})}}, {key<ParameterId>("skip_out_of_range"), {list({1})}}},
        ErrorCode::ConstraintViolation, "skip_out_of_range");
}
} // namespace

int main()
{
    try {
        const auto registry = take(ToolRegistry::create(visionpro::tools::logic_registrations()));
        arithmetic(registry);
        formatting(registry);
        std::cout << "Integer and formatter edge cases passed.\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
