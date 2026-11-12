#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/tool_registry.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using namespace visionpro::core;

int failures = 0;
void check(bool condition, std::string_view description)
{
    if (condition) { return; }
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
}

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<class T>
void rejects(const Result<T>& result, ErrorCode expected, std::string_view description)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == expected, description);
}

template<UuidIdentity T> T identity(std::string_view text)
{ return take(IdFactory::parse<T>(text)); }
template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<RuntimeIdentity T> T count(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }

ToolDescriptor descriptor(std::string_view type)
{
    ParameterDescriptor offset{key<ParameterId>("offset"), "Offset", DataType::integer()};
    offset.binding.time = BindingTime::SnapshotBuild;
    return {key<ToolTypeId>(type), "Offset tool", 1, {key<SchemaId>("test.offset.schema"), 1},
        {offset}, {{key<InputId>("input"), "Input", DataType::integer()}},
        {{key<OutputId>("value"), "Value", DataType::integer()}}, {}};
}

ToolExecutionConfiguration configure(const ToolDescriptor& tool, NodeId node, std::int64_t offset)
{
    return take(ToolExecutionConfiguration::create({
        identity<SolutionSessionId>("00000000-0000-4000-8000-000000000001"),
        count<ConfigSnapshotId>(1), identity<WorkflowId>("00000000-0000-4000-8000-000000000002"),
        node, tool, {{tool.parameters[0].id, ConfigValue::integer(offset)}}, {}}));
}

InvocationContext make_context(const ToolExecutionConfiguration& configuration, std::int64_t input)
{
    const auto& fixed = configuration.data();
    return take(InvocationContext::create({configuration, {fixed.session, fixed.snapshot,
        {count<WorkflowRunId>(1), fixed.node, count<ExecutionScopeId>(0), count<InvocationId>(1)}, count<AttemptId>(1)},
        {{fixed.tool.inputs[0].id, DataValue::integer(input)}},
        {{fixed.parameters[0].id, DataValue::integer(*fixed.parameters[0].value.as_int64())}}, {}, {}}));
}

ToolCompletion offset_result(const InvocationContext& context)
{
    const auto& fixed = context.data();
    const auto value = *fixed.inputs[0].value.as_int64() + *fixed.parameters[0].value.as_int64();
    return {fixed.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
        {{fixed.configuration.data().tool.outputs[0].id, DataValue::integer(value)}}, {}};
}

class OffsetTool final : public ToolInstance {
public:
    explicit OffsetTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    { return InvocationStart{offset_result(*context)}; }
};

struct ControlledDriver {
    std::optional<InvocationCompletion> completion;
    std::weak_ptr<const InvocationContext> context;
};

class FixtureControl final : public InvocationControl {
public:
    void request_cancel() override {}
};

class ControlledTool final : public ToolInstance {
public:
    ControlledTool(const ToolExecutionConfiguration& configuration, const std::shared_ptr<ControlledDriver>& driver)
        : ToolInstance(configuration), driver_(driver) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context,
        InvocationCompletion completion) override
    {
        const auto driver = driver_.lock();
        if (!driver) { return Diagnostic{ErrorCode::InvalidState, Severity::Error, {}, "Fixture driver expired."}; }
        auto control = std::make_shared<FixtureControl>();
        if (auto registered = completion.register_control(control);
            const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        driver->completion = std::move(completion);
        driver->context = context;
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    std::weak_ptr<ControlledDriver> driver_;
};

class FixtureFactory final : public ToolInstanceFactory {
public:
    using Creator = std::function<std::shared_ptr<ToolInstance>(const ToolExecutionConfiguration&)>;
    explicit FixtureFactory(Creator creator) : creator_(std::move(creator)) {}
    int creations{0};
private:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { ++creations; return creator_(configuration); }
    Creator creator_;
};

struct Fixtures {
    std::shared_ptr<ControlledDriver> driver = std::make_shared<ControlledDriver>();
    std::shared_ptr<FixtureFactory> sync_factory = std::make_shared<FixtureFactory>(
        [](const auto& configuration) { return std::make_shared<OffsetTool>(configuration); });
    std::shared_ptr<FixtureFactory> async_factory = std::make_shared<FixtureFactory>(
        [driver = driver](const auto& configuration) { return std::make_shared<ControlledTool>(configuration, driver); });
    std::array<ToolRegistration, 2> registrations;

    Fixtures() : registrations{{
        {descriptor("test.offset"), {key<SchemaId>("test.offset.schema"), 1, 1, {}}, sync_factory},
        {descriptor("test.controlled"), {key<SchemaId>("test.offset.schema"), 1, 1,
            {key<CapabilityId>("test.controlled")}}, async_factory}
    }}
    {
        auto& async = registrations[1].descriptor;
        async.execution.mode = ToolExecutionMode::Asynchronous;
        async.schema.required_capabilities = registrations[1].support.capabilities;
    }
};

std::optional<std::int64_t> observed_value(const InvocationOperation& operation)
{
    const auto progress = operation.poll();
    if (!progress.completion || progress.completion->outputs.size() != 1) { return std::nullopt; }
    const auto* value = std::get_if<DataValue>(&progress.completion->outputs[0].content);
    if (!value || !value->as_int64()) { return std::nullopt; }
    return *value->as_int64();
}

void tool_registry()
{
    Fixtures fixtures;
    const auto node = identity<NodeId>("00000000-0000-4000-8000-000000000003");
    const auto other = identity<NodeId>("00000000-0000-4000-8000-000000000004");
    auto registrations = fixtures.registrations;
    const auto registry = take(ToolRegistry::create(registrations));
    const auto& sync = registry.descriptors()[0];
    const auto& async = registry.descriptors()[1];
    registrations[0].descriptor.display_name = "Edited";
    registrations[0].factory.reset();
    check(registry.find_descriptor(sync.id) == &sync && sync.display_name == "Offset tool",
        "registry detaches descriptor storage and owns the factory reference");
    check(registry.find_support(sync.id)->capabilities.empty() && registry.find_support(async.id)->capabilities.size() == 1,
        "factory capabilities remain specific to each registered tool type");
    take(registry.validate(sync));
    take(registry.validate(async));

    registrations = fixtures.registrations;
    registrations[1].descriptor.id = sync.id;
    rejects(ToolRegistry::create(registrations), ErrorCode::DuplicateIdentity, "duplicate tool types are rejected");
    registrations[1].descriptor.id = async.id;
    registrations[1].factory.reset();
    rejects(ToolRegistry::create(registrations), ErrorCode::InvalidDescriptor, "registration requires a factory");
    registrations[1].factory = fixtures.async_factory;
    registrations[1].support.minimum_version = 0;
    rejects(ToolRegistry::create(registrations), ErrorCode::InvalidDescriptor, "invalid support range is rejected");
    registrations[1].support.minimum_version = 2;
    registrations[1].support.maximum_version = 2;
    const auto newer_backend = take(ToolRegistry::create(registrations));
    rejects(newer_backend.validate(async), ErrorCode::IncompatibleSchemaVersion, "unsupported version is diagnosed for the selected tool");
    registrations[1].support.minimum_version = 1;
    registrations[1].support.capabilities.clear();
    const auto limited_backend = take(ToolRegistry::create(registrations));
    take(limited_backend.validate(sync));
    const auto unavailable = limited_backend.create_instance(configure(async, node, 4));
    rejects(unavailable, ErrorCode::MissingCapability, "unsupported capability prevents creation");
    if (const auto* failure = std::get_if<Diagnostic>(&unavailable)) {
        check(failure->location.node == node && failure->location.tool == async.id,
            "factory admission diagnostic locates the requested node and tool");
    }

    auto changed = sync;
    changed.id = key<ToolTypeId>("test.unregistered");
    rejects(registry.create_instance(configure(changed, node, 4)), ErrorCode::UnknownTool, "unregistered type cannot create instances");
    changed = sync;
    changed.config_version = 2;
    rejects(registry.create_instance(configure(changed, node, 4)), ErrorCode::IncompatibleSchemaVersion,
        "different configuration version cannot create instances");
    changed = sync;
    changed.outputs[0].required = false;
    changed.outputs[0].allowed_absence = {OutputAbsenceReason::ToolDidNotProduce};
    rejects(registry.create_instance(configure(changed, node, 4)), ErrorCode::InvalidDescriptor,
        "changing the output contract without a version change is rejected");
    check(fixtures.sync_factory->creations == 0 && fixtures.async_factory->creations == 0,
        "catalog construction and validation never call factories");

    changed = sync;
    changed.display_name = "Renamed tool";
    changed.parameters[0].display_name = "Renamed parameter";
    const auto first = configure(sync, node, 4);
    const auto second = configure(changed, other, 7);
    auto first_tool = take(registry.create_instance(first));
    auto second_tool = take(registry.create_instance(second));
    check(first_tool != second_tool && fixtures.sync_factory->creations == 2 && fixtures.async_factory->creations == 0,
        "tool IDs route factory creation and each fixed node receives its own instance");
    const auto first_run = take(InvocationOperation::start(first_tool, make_context(first, 2)));
    const auto second_run = take(InvocationOperation::start(second_tool, make_context(second, 3)));
    check(observed_value(first_run) == 6 && observed_value(second_run) == 10,
        "registered tool instances observe their own fixed parameters and inputs");
}

class WakeupProbe final : public InvocationWakeup {
public:
    WakeupProbe(InvocationOperation operation, ToolInvocationIdentity identity)
        : operation_(std::move(operation)), identity_(identity) {}

    void signal(const ToolInvocationIdentity& identity, InvocationWaitTarget target) noexcept override
    {
        try {
            const auto progress = operation_.poll(); // Also verifies delivery outside the operation lock.
            if (identity != identity_ || !progress.completion
                || (target == InvocationWaitTarget::PhysicalExit && !progress.physically_exited)) { valid = false; }
            if (target == InvocationWaitTarget::LogicalCompletion) { ++completions; }
            if (target == InvocationWaitTarget::PhysicalExit) { ++exits; }
        } catch (...) { valid = false; }
    }

    std::atomic<bool> valid{true};
    std::atomic<int> completions{0};
    std::atomic<int> exits{0};
private:
    InvocationOperation operation_;
    ToolInvocationIdentity identity_;
};

void invocation_notifications()
{
    const Fixtures fixtures;
    const auto registry = take(ToolRegistry::create(fixtures.registrations));
    const auto node = identity<NodeId>("00000000-0000-4000-8000-000000000003");
    const auto sync = configure(registry.descriptors()[0], node, 4);
    const auto sync_context = make_context(sync, 2);
    const auto sync_run = take(InvocationOperation::start(take(registry.create_instance(sync)), sync_context));
    const auto sync_wakeup = std::make_shared<WakeupProbe>(sync_run, sync_context.data().identity);
    take(sync_run.notify_when(InvocationWaitTarget::LogicalCompletion, sync_wakeup));
    take(sync_run.notify_when(InvocationWaitTarget::PhysicalExit, sync_wakeup));
    check(sync_wakeup->valid && sync_wakeup->completions == 1 && sync_wakeup->exits == 1,
        "late registration synchronously delivers both completed milestones once");
    rejects(sync_run.notify_when(InvocationWaitTarget::LogicalCompletion, sync_wakeup), ErrorCode::InvalidState,
        "a milestone cannot be registered twice");

    const auto async = configure(registry.descriptors()[1], node, 7);
    const auto async_context = make_context(async, 5);
    const auto async_run = take(InvocationOperation::start(take(registry.create_instance(async)), async_context));
    const auto async_wakeup = std::make_shared<WakeupProbe>(async_run, async_context.data().identity);
    take(async_run.notify_when(InvocationWaitTarget::PhysicalExit, async_wakeup));
    rejects(async_run.notify_when(InvocationWaitTarget::LogicalCompletion, {}), ErrorCode::InvalidValue,
        "an empty weak receiver cannot register");
    check(async_wakeup->completions == 0 && async_wakeup->exits == 0 && !fixtures.driver->context.expired(),
        "asynchronous begin retains context without announcing a milestone");

    const auto token = *fixtures.driver->completion;
    const auto finished = offset_result(async_context);
    auto mismatched = finished;
    mismatched.identity.attempt = count<AttemptId>(2);
    rejects(token.complete(mismatched), ErrorCode::InvalidValue, "a mismatched completion does not reach a milestone");
    rejects(token.mark_exited(), ErrorCode::InvalidState, "physical exit requires terminal completion first");
    std::latch release_backend{1};
    Result<std::monostate> reported;
    std::jthread backend([&] {
        release_backend.wait();
        reported = token.complete(finished);
    });
    release_backend.count_down();
    const auto registered = async_run.notify_when(InvocationWaitTarget::LogicalCompletion, async_wakeup);
    backend.join();
    take(registered);
    take(reported);
    check(async_wakeup->valid && async_wakeup->completions == 1 && async_wakeup->exits == 0
        && observed_value(async_run) == 12 && !fixtures.driver->context.expired(),
        "registration racing completion yields one wakeup, with context retained until exit");
    rejects(token.complete(finished), ErrorCode::InvalidState, "duplicate completion is rejected");
    take(token.mark_exited());
    rejects(token.mark_exited(), ErrorCode::InvalidState, "duplicate exit is rejected");
    check(async_wakeup->valid && async_wakeup->completions == 1 && async_wakeup->exits == 1
        && fixtures.driver->context.expired(),
        "physical exit separately signals after releasing operation-owned context");

    const auto abandoned_run = take(InvocationOperation::start(take(registry.create_instance(async)), async_context));
    auto abandoned = std::make_shared<WakeupProbe>(abandoned_run, async_context.data().identity);
    const std::weak_ptr<InvocationWakeup> receiver = abandoned;
    take(abandoned_run.notify_when(InvocationWaitTarget::LogicalCompletion, receiver));
    abandoned.reset();
    check(receiver.expired(), "a pending notification does not keep its receiver alive");
    take(fixtures.driver->completion->complete(finished));
    take(fixtures.driver->completion->mark_exited());
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto test = argc == 2 ? std::string_view{argv[1]} : std::string_view{};
        if (test == "tool_registry") { tool_registry(); }
        else if (test == "invocation_notifications") { invocation_notifications(); }
        else { throw std::invalid_argument("Expected tool_registry or invocation_notifications."); }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
