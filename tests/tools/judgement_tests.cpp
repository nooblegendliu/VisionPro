#include "vision/vision_tools.hpp"

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

void check(bool passed, std::string_view message)
{
    if (!passed) { throw std::runtime_error(std::string(message)); }
}

std::string error_text(const Diagnostic& diagnostic)
{ return std::string(error_code_name(diagnostic.code)) + ": " + diagnostic.message; }

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(error_text(*failure)); }
    return std::get<T>(std::move(result));
}

template<SchemaIdentity T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }

template<UuidIdentity T> T id()
{ return take(IdFactory::parse<T>("00000000-0000-4000-8000-000000000001")); }

ToolTypeId judge_type() { return key<ToolTypeId>("visionpro.judge.integer_range"); }

RunHandle execute_typed(const ToolRegistry& registry, ToolTypeId type, ConfigValue minimum, ConfigValue maximum, DataValue value)
{
    const auto input_id = key<InputId>("value");
    const auto workflow_id = id<WorkflowId>();
    const auto node_id = id<NodeId>();
    const auto schema = key<SchemaId>("s45.judgement_test");
    InputDescriptor input{input_id, "Value", value.type()};
    input.binding.allowed_sources = {BindingSourceKind::PublicInput};
    Node judge{node_id, "Judge range", ToolConfiguration{type, 1},
        {{input_id, {ScopedInputReference{workflow_id, {}, input_id, {}}}},
            {key<ParameterId>("minimum"), {std::move(minimum)}},
            {key<ParameterId>("maximum"), {std::move(maximum)}}}, {}};
    Workflow flow{workflow_id, "Integer judgement", {schema, 1, {std::move(input)}, {}, {}},
        {std::move(judge)}, {}, {}, {node_id}};
    const Solution solution{id<SolutionId>(), "Integer judgement fixture", 1, 1, {std::move(flow)}, {}, {}, {}, {}, {}};
    const auto engine = take(WorkflowEngine::create(registry));
    const auto snapshot_id = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1));
    const auto snapshot = take(ConfigSnapshot::capture(solution, registry.descriptors(), {engine->session(), snapshot_id}));
    const auto request = take(RunRequest::create(snapshot, WorkflowTarget{workflow_id},
        {{input_id, std::move(value)}}, {TriggerKind::Manual, schema}));
    auto submission = engine->submit(request);
    if (const auto* rejection = std::get_if<RunRejected>(&submission)) { throw std::runtime_error(error_text(rejection->diagnostic)); }
    auto* accepted = std::get_if<RunHandle>(&submission);
    check(accepted, "The integer judgement workflow must be admitted.");
    const auto waited = take(accepted->wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent, "The workflow must reach quiescence.");
    return std::move(*accepted);
}

RunHandle execute(const ToolRegistry& registry, IntegerRange range, std::int64_t value)
{
    return execute_typed(registry, judge_type(), ConfigValue::integer(range.minimum),
        ConfigValue::integer(range.maximum), DataValue::integer(value));
}

void floating_range(const ToolRegistry& registry)
{
    for (const auto value : {9.5, 10.0, 40.0, 40.5}) {
        const auto run = execute_typed(registry, key<ToolTypeId>("visionpro.judge.float_range"),
            take(ConfigValue::real(10)), take(ConfigValue::real(40)), take(DataValue::real(value)));
        const auto progress = take(run.inspect());
        check(progress.status == ExecutionStatus::Succeeded && progress.nodes.front().invocation.has_value(),
            "Float64 closed-range judgement succeeds for in-range and out-of-range values.");
        const auto result = take(run.lookup(*progress.nodes.front().invocation));
        check(result && result->data().judgement == (value >= 10 && value <= 40 ? Judgement::OK : Judgement::NG),
            "Float64 endpoints are inclusive and fractional values preserve their precision.");
    }
}

void closed_range(const ToolRegistry& registry)
{
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    struct Case { IntegerRange range; std::int64_t value; bool passed; };
    constexpr Case cases[]{
        {{-2, 2}, -3, false}, {{-2, 2}, -2, true}, {{-2, 2}, 0, true}, {{-2, 2}, 2, true}, {{-2, 2}, 3, false},
        {{6, 6}, 6, true}, {{6, 6}, 7, false},
        {{low, high}, low, true}, {{low, high}, high, true},
        {{low + 1, high - 1}, low, false}, {{low + 1, high - 1}, high, false},
    };
    for (const auto& test : cases) {
        const auto run = execute(registry, test.range, test.value);
        const auto progress = take(run.inspect());
        if (!progress.diagnostics.empty()) { throw std::runtime_error(error_text(progress.diagnostics.front())); }
        check(progress.status == ExecutionStatus::Succeeded && progress.nodes.size() == 1
            && progress.call_order == std::vector{id<NodeId>()} && progress.nodes.front().invocation.has_value(),
            "OK and NG both complete one technical invocation successfully.");
        const auto result = take(run.lookup(*progress.nodes.front().invocation));
        const auto expected = test.passed ? Judgement::OK : Judgement::NG;
        check(result && result->data().status == ExecutionStatus::Succeeded && result->data().judgement == expected,
            "The business judgement must reflect the closed Int64 range.");
        const auto* output = result->output(key<OutputId>("passed"));
        const auto* value = output ? std::get_if<DataValue>(&output->content) : nullptr;
        check(value && value->as_bool() && *value->as_bool() == test.passed,
            "The required Bool output agrees with the business judgement.");
    }
}

void reversed_range(const ToolRegistry& registry)
{
    const auto run = execute(registry, {3, 2}, 2);
    const auto progress = take(run.inspect());
    check(progress.status == ExecutionStatus::Failed && progress.call_order.empty() && progress.nodes.size() == 1
        && progress.nodes.front().status == ExecutionStatus::Failed && !progress.nodes.front().invocation,
        "An inverted range must fail before the node starts its call.");
    const auto invalid = std::ranges::find(progress.diagnostics, ErrorCode::ConstraintViolation, &Diagnostic::code);
    check(invalid != progress.diagnostics.end(), "An inverted range must report ConstraintViolation.");
    check(invalid->location.workflow == id<WorkflowId>() && invalid->location.node == id<NodeId>()
        && invalid->location.tool == judge_type() && invalid->location.parameter == key<ParameterId>("maximum"),
        "The range configuration diagnostic must identify the judge and its maximum parameter.");
}
} // namespace

int main()
{
    try {
        const auto registry = take(ToolRegistry::create(visionpro::tools::vision::registrations()));
        closed_range(registry);
        floating_range(registry);
        reversed_range(registry);
        std::cout << "Integer range workflow judgement and configuration checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
