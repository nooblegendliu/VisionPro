#include <visionpro/core/tool_execution.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using namespace visionpro::core;
int checks = 0;
int failures = 0;
void check(bool condition, std::string_view description)
{ ++checks; if (!condition) { ++failures; std::cerr << "FAIL: " << description << '\n'; } }
template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
template<class T> void rejects(const Result<T>& result, ErrorCode expected, std::string_view description)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == expected && failure->severity == Severity::Error && !failure->message.empty(), description);
}
template<UuidIdentity T> T uuid(unsigned value = 1)
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr std::string_view hex = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) { text[text.size() - 1 - index] = hex[(value >> (index * 4)) & 15]; }
    return take(IdFactory::parse<T>(text));
}
template<SchemaIdentity T> T key(std::string_view text) { return take(SchemaIdFactory::parse<T>(text)); }
template<RuntimeIdentity T> T count(std::uint64_t value) { return take(RuntimeIdFactory::from_value<T>(value)); }
ParameterId threshold() { return key<ParameterId>("threshold"); }
ParameterId offset() { return key<ParameterId>("offset"); }
InputId input_id() { return key<InputId>("input"); }
OutputId output_id() { return key<OutputId>("value"); }
ToolExecutionConfigurationData configured(unsigned node = 1, std::int64_t fixed = 10)
{
    ParameterDescriptor fixed_parameter{threshold(), "Threshold", DataType::integer()};
    fixed_parameter.structural = true; fixed_parameter.binding.time = BindingTime::SnapshotBuild;
    ParameterDescriptor dynamic{offset(), "Offset", DataType::integer(), false, ConfigValue::integer(0)};
    ToolDescriptor tool{key<ToolTypeId>("test.tool"), "Tool", 1, {key<SchemaId>("test.tool.schema"), 1},
        {fixed_parameter, dynamic}, {{input_id(), "Input", DataType::integer()}},
        {{output_id(), "Value", DataType::integer()}}, {{key<ControlOutputId>("done"), "Done"}}};
    return {uuid<SolutionSessionId>(), count<ConfigSnapshotId>(1), uuid<WorkflowId>(), uuid<NodeId>(node), tool,
        {{threshold(), ConfigValue::integer(fixed)}}, {}};
}
InvocationContextData context_data(const ToolExecutionConfiguration& configuration, std::int64_t input = 1)
{
    return {configuration,
        {configuration.data().session, configuration.data().snapshot,
            {count<WorkflowRunId>(1), configuration.data().node, count<ExecutionScopeId>(0), count<InvocationId>(1)}, count<AttemptId>(1)},
        {{input_id(), DataValue::integer(input)}},
        {{threshold(), DataValue::integer(*configuration.data().parameters[0].value.as_int64())}, {offset(), DataValue::integer(0)}}, {}, {}};
}
ToolCompletion completed(const InvocationContext& context)
{
    const auto& values = context.data();
    const auto answer = *values.inputs[0].value.as_int64() + *values.parameters[0].value.as_int64() + *values.parameters[1].value.as_int64();
    return {values.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NG,
        {{output_id(), DataValue::integer(answer)}}, {}, std::chrono::nanoseconds{2}, {key<ControlOutputId>("done")}};
}
Diagnostic artificial_error() { return {ErrorCode::InvalidValue, Severity::Error, {}, "Artificial startup failure"}; }
struct SyncInstance final : ToolInstance {
    explicit SyncInstance(ToolExecutionConfiguration configuration, int behavior = 0)
        : ToolInstance(std::move(configuration)), behavior(behavior) {}
    int behavior;
    int calls{0};
    std::weak_ptr<const InvocationContext> last_context;
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        ++calls; last_context = context;
        if (behavior == 1) { throw std::runtime_error("backend exception"); }
        if (behavior == 2) { throw 42; }
        if (behavior == 3) { return artificial_error(); }
        auto result = completed(*context);
        if (behavior == 4) { result.outputs.clear(); }
        return InvocationStart{result};
    }
};
struct Factory final : ToolInstanceFactory {
    explicit Factory(std::function<Result<std::shared_ptr<ToolInstance>>(const ToolExecutionConfiguration&)> make)
        : make(std::move(make)) {}
    std::function<Result<std::shared_ptr<ToolInstance>>(const ToolExecutionConfiguration&)> make;
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return make(configuration); }
};
Factory sync_factory()
{ return Factory{[](const auto& configuration) -> Result<std::shared_ptr<ToolInstance>> { return std::shared_ptr<ToolInstance>{std::make_shared<SyncInstance>(configuration)}; }}; }
struct Driver {
    std::optional<InvocationCompletion> completion;
    std::weak_ptr<const InvocationContext> context;
    std::weak_ptr<InvocationControl> control;
    std::function<void()> on_cancel;
    std::function<void(const InvocationContext&)> on_inline_exit;
    int cancellation_calls{0};
    bool cancellation_throws{false};
};
struct Control final : InvocationControl {
    explicit Control(std::shared_ptr<Driver> driver) : driver(std::move(driver)) {}
    void request_cancel() override
    {
        ++driver->cancellation_calls;
        if (driver->on_cancel) { driver->on_cancel(); }
        if (driver->cancellation_throws) { throw std::runtime_error("cancel failure"); }
    }
    std::shared_ptr<Driver> driver;
};
struct AsyncInstance final : ToolInstance {
    AsyncInstance(ToolExecutionConfiguration configuration, std::shared_ptr<Driver> driver, int behavior = 0)
        : ToolInstance(std::move(configuration)), driver(std::move(driver)), behavior(behavior) {}
    std::shared_ptr<Driver> driver;
    int behavior;
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        if (behavior == 1) { return InvocationStart{AsynchronousInvocation{}}; }
        if (behavior == 4) { throw std::runtime_error("before launch"); }
        auto control = std::make_shared<Control>(driver);
        take(completion.register_control(control));
        driver->completion = completion; driver->context = context;
        driver->control = control;
        if (behavior == 2) { throw std::runtime_error("after launch"); }
        if (behavior == 3) { return artificial_error(); }
        if (behavior == 5) { take(completion.complete(completed(*context))); throw std::runtime_error("after provisional completion"); }
        if (auto probe = std::exchange(driver->on_inline_exit, {})) {
            take(completion.complete(completed(*context)));
            take(completion.mark_exited());
            probe(*context);
        }
        return InvocationStart{AsynchronousInvocation{control}};
    }
};
struct ProbeLease final : ResourceLease {
    ProbeLease(ResourceReference reference, std::function<void()> destroyed = {})
        : ResourceLease(std::move(reference)), destroyed(std::move(destroyed)) {}
    ~ProbeLease() override { if (destroyed) { destroyed(); } }
    std::function<void()> destroyed;
};

struct ExitProbe final : InvocationWakeup {
    void signal(const ToolInvocationIdentity&, InvocationWaitTarget target) noexcept override
    {
        if (target == InvocationWaitTarget::PhysicalExit) { ++exits; }
    }
    std::atomic<int> exits{0};
};

struct ReleaseLatch {
    std::latch& gate;
    ~ReleaseLatch() { gate.count_down(); }
};

void exit_during_cancel(const ToolExecutionConfiguration& configuration, const SchemaId& slot,
    const ResourceReference& resource, bool reentrant)
{
    auto driver = std::make_shared<Driver>();
    std::weak_ptr<const ResourceLease> weak_lease;
    std::weak_ptr<ToolInstance> weak_instance;
    const auto [operation, result] = [&] {
        const auto lease = std::make_shared<ProbeLease>(resource);
        weak_lease = lease;
        auto values = context_data(configuration);
        values.resources = {{slot, lease}};
        const auto context = take(InvocationContext::create(values));
        const auto instance = std::make_shared<AsyncInstance>(configuration, driver);
        weak_instance = instance;
        return std::pair{take(InvocationOperation::start(instance, context)), completed(context)};
    }();
    const auto token = *driver->completion;
    const auto wakeup = std::make_shared<ExitProbe>();
    take(operation.notify_when(InvocationWaitTarget::PhysicalExit, wakeup));

    const auto report_exit = [&] {
        take(token.complete(result));
        take(token.mark_exited());
        check(operation.poll().completion.has_value() && !operation.physically_exited() && wakeup->exits == 0,
            "a backend exit reported during cancellation does not publish physical exit");
        check(!driver->context.expired() && !weak_lease.expired() && !weak_instance.expired() && !driver->control.expired(),
            "the operation retains the context, instance, control and lease until cancellation returns");
        check(take(operation.wait_for(InvocationWaitTarget::PhysicalExit, std::chrono::milliseconds{0})).outcome
                == InvocationWaitOutcome::TimedOut,
            "physical wait remains pending while the cancellation callback owns access");
        check(take(operation.request_cancel()) == CancellationRequest::CleanupInProgress,
            "a reported exit defers cleanup without replaying cancellation");
        rejects(token.mark_exited(), ErrorCode::InvalidState, "duplicate exit is rejected even during deferred cleanup");
        rejects(token.complete(result), ErrorCode::InvalidState, "duplicate completion cannot overwrite the result during deferred cleanup");
    };
    if (reentrant) {
        driver->on_cancel = report_exit;
        driver->cancellation_throws = true;
        rejects(operation.request_cancel(), ErrorCode::ExecutionBoundaryFailure,
            "a reentrant cancellation can report exit and still propagate a typed callback failure");
    } else {
        std::latch entered{1}, allow_return{1};
        driver->on_cancel = [&] {
            entered.count_down();
            allow_return.wait();
        };
        Result<CancellationRequest> disposition;
        {
            std::jthread caller([&] { disposition = operation.request_cancel(); });
            const ReleaseLatch release{allow_return};
            entered.wait();
            report_exit();
        }
        check(take(disposition) == CancellationRequest::Requested, "concurrent cancellation returns after deferred cleanup");
    }
    check(operation.physically_exited() && wakeup->exits == 1 && driver->cancellation_calls == 1,
        "deferred physical exit is published exactly once when cancellation returns");
    check(driver->context.expired() && weak_lease.expired() && weak_instance.expired() && driver->control.expired(),
        "physical exit releases all invocation ownership");
    rejects(token.complete(result), ErrorCode::InvalidState, "late completion stays rejected after deferred exit");
}

void tool_execution()
{
    static_assert(!std::is_default_constructible_v<ToolExecutionConfiguration>);
    static_assert(!std::is_default_constructible_v<InvocationContext>);
    static_assert(!std::is_default_constructible_v<InvocationOperation>);
    static_assert(!std::is_constructible_v<FixedToolInput, ParameterId, DataValue>);
    auto editable = configured();
    const auto configuration = take(ToolExecutionConfiguration::create(editable));
    editable.tool.display_name = "Edited"; editable.parameters[0].value = ConfigValue::integer(99);
    check(configuration.data().tool.display_name == "Tool" && *configuration.data().parameters[0].value.as_int64() == 10,
        "execution configuration detaches editable descriptor and parameter containers");
    auto moved_configuration = configuration;
    const auto moved = std::move(moved_configuration);
    check(&moved.data() == &moved_configuration.data(), "configuration move retains a valid immutable source");
    auto bad_configuration = configured(); bad_configuration.parameters.clear();
    rejects(ToolExecutionConfiguration::create(bad_configuration), ErrorCode::MissingRequiredValue, "snapshot parameters cannot remain unresolved");
    bad_configuration = configured(); bad_configuration.parameters.push_back(bad_configuration.parameters[0]);
    rejects(ToolExecutionConfiguration::create(bad_configuration), ErrorCode::InvalidValue, "fixed parameter duplicates are rejected");
    for (unsigned member = 0; member < 6; ++member) {
        auto bad = configured();
        if (member == 0) { bad.tool.execution.mode = static_cast<ToolExecutionMode>(99); }
        if (member == 1) { bad.tool.execution.lifetime = static_cast<ToolInstanceLifetime>(99); }
        if (member == 2) { bad.tool.execution.reentrancy = static_cast<ToolReentrancy>(99); }
        if (member == 3) { bad.tool.execution.effects = static_cast<ToolEffects>(99); }
        if (member == 4) { bad.tool.execution.cancellation = static_cast<ToolCancellation>(99); }
        if (member == 5) { bad.tool.execution.retry = static_cast<ToolRetry>(99); }
        rejects(ToolExecutionConfiguration::create(bad), ErrorCode::InvalidDescriptor, "unknown execution capability enum is rejected");
    }
    auto values = context_data(configuration);
    const auto context = take(InvocationContext::create(values));
    values.inputs[0].value = DataValue::integer(90);
    check(*context.data().inputs[0].value.as_int64() == 1, "invocation input container is detached before use");
    auto bad_context = context_data(configuration); bad_context.parameters.pop_back();
    rejects(InvocationContext::create(bad_context), ErrorCode::MissingRequiredValue, "default parameters must already be materialized");
    bad_context = context_data(configuration); bad_context.parameters[0].value = DataValue::integer(30);
    rejects(InvocationContext::create(bad_context), ErrorCode::InvalidValue, "invocation cannot replace a snapshot parameter");
    bad_context = context_data(configuration); bad_context.inputs[0].value = DataValue::boolean(false);
    rejects(InvocationContext::create(bad_context), ErrorCode::TypeMismatch, "fixed input exact type is enforced");
    bad_context = context_data(configuration); bad_context.identity.invocation.node = uuid<NodeId>(2);
    rejects(InvocationContext::create(bad_context), ErrorCode::InvalidValue, "invocation identity cannot borrow another node configuration");

    auto factory = sync_factory();
    const auto other_configuration = take(ToolExecutionConfiguration::create(configured(2, 20)));
    auto first = take(create_tool_instance(factory, configuration));
    auto second = take(create_tool_instance(factory, other_configuration));
    check(first != second && first->configuration().data().node != second->configuration().data().node,
        "same tool type at different nodes receives independent factory instances");
    auto operation = take(InvocationOperation::start(first, context));
    const auto progress = operation.poll();
    check(progress.physically_exited && progress.completion && progress.completion->status == ToolCompletionStatus::Succeeded,
        "synchronous return is terminal and physically exited");
    check(progress.completion && *std::get<DataValue>(progress.completion->outputs[0].content).as_int64() == 11,
        "tool sees its own fixed configuration and invocation values");
    auto second_operation = take(InvocationOperation::start(second, take(InvocationContext::create(context_data(other_configuration, 2)))));
    check(*std::get<DataValue>(second_operation.poll().completion->outputs[0].content).as_int64() == 22,
        "same-type nodes do not share editable parameter state");
    check(std::dynamic_pointer_cast<SyncInstance>(first)->last_context.expired(), "synchronous exit releases operation-owned invocation context immediately");
    check(take(operation.wait_for(InvocationWaitTarget::PhysicalExit, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::Reached,
        "completed synchronous physical wait returns immediately");
    check(take(operation.request_cancel()) == CancellationRequest::AlreadyExited, "cancellation cannot replace a finished result");
    rejects(operation.wait_for(InvocationWaitTarget::LogicalCompletion, std::chrono::milliseconds{-1}), ErrorCode::InvalidValue, "negative wait time is rejected");
    rejects(InvocationOperation::start(second, context), ErrorCode::InvalidState, "instance bound to another configuration is rejected before begin");
    Factory wrong{[&](const auto&) -> Result<std::shared_ptr<ToolInstance>> { return second; }};
    rejects(create_tool_instance(wrong, configuration), ErrorCode::InvalidState, "factory cannot return another node's instance");
    Factory cached{[&](const auto&) -> Result<std::shared_ptr<ToolInstance>> { return first; }};
    rejects(create_tool_instance(cached, configuration), ErrorCode::InvalidState, "factory cannot reissue its cached instance");
    Factory null_factory{[](const auto&) -> Result<std::shared_ptr<ToolInstance>> { return std::shared_ptr<ToolInstance>{}; }};
    rejects(create_tool_instance(null_factory, configuration), ErrorCode::InvalidState, "factory null result is rejected");
    Factory failing{[](const auto&) -> Result<std::shared_ptr<ToolInstance>> { return artificial_error(); }};
    rejects(create_tool_instance(failing, configuration), ErrorCode::InvalidValue, "factory typed error is preserved");
    Factory throwing{[](const auto&) -> Result<std::shared_ptr<ToolInstance>> { throw std::runtime_error("factory"); }};
    rejects(create_tool_instance(throwing, configuration), ErrorCode::ExecutionBoundaryFailure, "factory exception becomes a typed diagnostic");
    for (const int behavior : {1, 2, 3, 4}) {
        auto instance = std::make_shared<SyncInstance>(configuration, behavior);
        const auto failed = take(InvocationOperation::start(instance, context)).poll();
        check(failed.physically_exited && failed.completion && failed.completion->status == ToolCompletionStatus::Failed,
            "failed synchronous startup stays observable and physically exited");
        check(failed.completion && !failed.completion->diagnostics.empty(), "startup failure retains its diagnostic");
    }
    auto state_data = configured();
    state_data.tool.state = NodeStateDefinition{DataType::integer(), ConfigValue::integer(0), StateScope::Invocation,
        StateResetPolicy::DomainStart, StateCompatibilityPolicy::AlwaysNewGeneration,
        StateMutationTiming::DuringComputation, StateConcurrencyPolicy::Serialized};
    const auto state_configuration = take(ToolExecutionConfiguration::create(state_data));
    rejects(InvocationOperation::start(std::make_shared<SyncInstance>(state_configuration),
        take(InvocationContext::create(context_data(state_configuration)))), ErrorCode::MissingCapability,
        "stateful invocation explicitly requires the later typed state lease service");

    auto roi_data = configured(); roi_data.tool.inputs[0].type = DataType::region2d();
    const auto roi_configuration = take(ToolExecutionConfiguration::create(roi_data));
    auto roi_context = context_data(roi_configuration);
    const auto frame = key<CoordinateSystemId>("camera");
    const InheritedRegion reference{roi_data.workflow, uuid<NodeId>(9), output_id(), frame, MeasurementUnit::Pixel};
    RegionDefinition expression{uuid<RegionId>(1), frame, MeasurementUnit::Pixel, reference, {}};
    roi_context.inputs[0].value = take(DataValue::region2d(expression));
    rejects(InvocationContext::create(roi_context), ErrorCode::MissingRequiredValue,
        "symbolic ROI cannot enter a tool without its complete frozen source definition");
    auto producer = roi_context.identity; producer.invocation.node = reference.node;
    const RegionDefinition frozen{uuid<RegionId>(2), frame, MeasurementUnit::Pixel,
        RegionPrimitive{frame, MeasurementUnit::Pixel, AxisAlignedRect{0, 0, 10, 10}},
        {RegionPrimitive{frame, MeasurementUnit::Pixel, CircleRegion{2, 2, 1}}}};
    roi_context.regions = {{ToolValueTarget{input_id()}, reference, frozen, producer, 0, {key<FieldId>("source")}}};
    const auto frozen_context = take(InvocationContext::create(roi_context));
    roi_context.regions[0].value.exclusions.clear();
    check(frozen_context.data().regions[0].value.exclusions.size() == 1,
        "frozen ROI keeps the complete detached source expression including exclusions");
    roi_context.regions[0].value = frozen;
    expression.exclusions = {reference}; roi_context.inputs[0].value = take(DataValue::region2d(expression));
    rejects(InvocationContext::create(roi_context), ErrorCode::MissingRequiredValue,
        "one frozen occurrence cannot stand in for two identical ROI references at different paths");
    roi_context.regions.push_back({ToolValueTarget{input_id()}, reference, frozen, producer, 0,
        {key<FieldId>("exclusions"), DiagnosticElementIndex{0}}});
    check(std::holds_alternative<InvocationContext>(InvocationContext::create(roi_context)),
        "each repeated ROI occurrence retains its distinct source/path metadata");
    roi_context.regions.push_back({ToolValueTarget{input_id()}, reference, frozen, producer, 1, {key<FieldId>("source")}});
    rejects(InvocationContext::create(roi_context), ErrorCode::InvalidValue,
        "unused frozen ROI metadata cannot masquerade as part of the invocation");

    auto async_data = configured(); async_data.tool.execution.mode = ToolExecutionMode::Asynchronous;
    async_data.tool.execution.cancellation = ToolCancellation::Cooperative;
    const auto async_configuration = take(ToolExecutionConfiguration::create(async_data));
    const auto async_context = take(InvocationContext::create(context_data(async_configuration)));
    const auto driver = std::make_shared<Driver>();
    auto async_instance = std::make_shared<AsyncInstance>(async_configuration, driver);
    const auto async_operation = take(InvocationOperation::start(async_instance, async_context));
    check(!async_operation.poll().completion && !async_operation.poll().physically_exited, "asynchronous start has separate logical and physical pending states");
    check(take(async_operation.wait_for(InvocationWaitTarget::LogicalCompletion, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::TimedOut,
        "bounded wait does not fabricate completion");
    rejects(InvocationOperation::start(async_instance, async_context), ErrorCode::InvalidState, "serialized instance rejects overlap until physical exit");
    rejects(driver->completion->mark_exited(), ErrorCode::InvalidState, "physical exit cannot precede a terminal result");
    check(take(async_operation.request_cancel()) == CancellationRequest::Requested && driver->cancellation_calls == 1,
        "cooperative cancellation reaches its registered control exactly once");
    check(driver->completion->cancellation_requested() && !async_operation.poll().completion, "cancel request is observable but is not completion");
    check(take(async_operation.request_cancel()) == CancellationRequest::AlreadyRequested && driver->cancellation_calls == 1,
        "duplicate cancel does not replay the backend command");
    auto terminal = completed(async_context);
    auto wrong_identity = terminal; wrong_identity.identity.attempt = count<AttemptId>(2);
    rejects(driver->completion->complete(wrong_identity), ErrorCode::InvalidValue, "callback must match the exact attempt identity");
    take(driver->completion->complete(terminal));
    terminal.outputs[0].content = DataValue::integer(999);
    check(*std::get<DataValue>(async_operation.poll().completion->outputs[0].content).as_int64() == 11,
        "completion detaches caller-owned output containers");
    check(take(async_operation.wait_for(InvocationWaitTarget::LogicalCompletion, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::Reached
        && take(async_operation.wait_for(InvocationWaitTarget::PhysicalExit, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::TimedOut,
        "logical completion never claims physical cleanup has finished");
    rejects(driver->completion->complete(terminal), ErrorCode::InvalidState, "duplicate completion cannot overwrite accepted output");
    take(driver->completion->mark_exited());
    check(async_operation.poll().physically_exited && driver->context.expired(), "physical exit releases invocation ownership");
    rejects(driver->completion->complete(terminal), ErrorCode::InvalidState, "late completion after exit is rejected");
    rejects(driver->completion->mark_exited(), ErrorCode::InvalidState, "duplicate exit notification is rejected");
    const auto reused = take(InvocationOperation::start(async_instance, async_context));
    take(driver->completion->complete(completed(async_context))); take(driver->completion->mark_exited());
    check(reused.poll().physically_exited, "serialized instance becomes reusable immediately after published physical exit");
    for (const int behavior : {1, 2, 3, 4, 5}) {
        auto partial = std::make_shared<Driver>();
        auto instance = std::make_shared<AsyncInstance>(async_configuration, partial, behavior);
        const auto failed = take(InvocationOperation::start(instance, async_context));
        check(failed.poll().completion && failed.poll().completion->status == ToolCompletionStatus::Failed,
            "invalid or throwing asynchronous begin returns an observable failed operation");
        if (behavior == 2 || behavior == 3 || behavior == 5) {
            check(!failed.poll().physically_exited, "registered partially started backend remains physically pending");
            check(take(failed.request_cancel()) == CancellationRequest::Requested && partial->cancellation_calls == 1,
                "partial startup failure still exposes cancellation control");
            check(take(failed.wait_for(InvocationWaitTarget::PhysicalExit, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::TimedOut,
                "partial startup failure remains waitable until backend exit");
            rejects(partial->completion->complete(completed(async_context)), ErrorCode::InvalidState, "late success cannot replace failed startup");
            take(partial->completion->mark_exited());
            check(failed.poll().physically_exited, "partially started backend can explicitly finish cleanup");
        } else { check(failed.poll().physically_exited, "failure before control registration confirms that no asynchronous activity started"); }
    }
    auto inline_driver = std::make_shared<Driver>();
    auto inline_instance = std::make_shared<AsyncInstance>(async_configuration, inline_driver);
    inline_driver->on_inline_exit = [&](const InvocationContext& context) {
        auto overlap = InvocationOperation::start(inline_instance, context);
        rejects(overlap, ErrorCode::InvalidState, "inline exit keeps a serialized instance occupied until begin returns");
        if (std::holds_alternative<InvocationOperation>(overlap)) {
            take(inline_driver->completion->complete(completed(context)));
            take(inline_driver->completion->mark_exited());
        }
    };
    const auto inline_operation = take(InvocationOperation::start(inline_instance, async_context));
    check(inline_operation.physically_exited() && inline_operation.poll().completion->status == ToolCompletionStatus::Succeeded,
        "an inline completion and exit become observable at the end of startup");
    auto cancelling = std::make_shared<Driver>(); cancelling->cancellation_throws = true;
    const auto cancel_failure = take(InvocationOperation::start(std::make_shared<AsyncInstance>(async_configuration, cancelling), async_context));
    rejects(cancel_failure.request_cancel(), ErrorCode::ExecutionBoundaryFailure, "cancellation exception cannot escape the controlled boundary");
    check(!cancel_failure.poll().physically_exited, "cancellation exception does not release active inputs");
    take(cancelling->completion->complete(completed(async_context))); take(cancelling->completion->mark_exited());
    auto unsupported_data = async_data;
    unsupported_data.tool.execution.cancellation = ToolCancellation::Unsupported;
    unsupported_data.tool.execution.lifetime = ToolInstanceLifetime::SolutionSession;
    unsupported_data.tool.execution.retry = ToolRetry::Idempotent;
    const auto unsupported_configuration = take(ToolExecutionConfiguration::create(unsupported_data));
    const auto unsupported_context = take(InvocationContext::create(context_data(unsupported_configuration)));
    const auto unsupported_driver = std::make_shared<Driver>();
    const auto unsupported = take(InvocationOperation::start(std::make_shared<AsyncInstance>(unsupported_configuration, unsupported_driver), unsupported_context));
    rejects(unsupported.request_cancel(), ErrorCode::MissingCapability, "unsupported cancellation is reported rather than silently accepted");
    check(unsupported_driver->cancellation_calls == 0 && !unsupported.poll().completion,
        "lifetime and retry metadata do not create pooling, retries or cancellation commands");
    take(unsupported_driver->completion->complete(completed(unsupported_context))); take(unsupported_driver->completion->mark_exited());

    auto leased_data = async_data;
    const auto slot = key<SchemaId>("image.resource");
    const ResourceReference resource{uuid<ResourceId>(), ResourceKind::Image, 1};
    leased_data.tool.resources = {{slot, ResourceKind::Image, true}};
    leased_data.resources = {{slot, resource}};
    const auto leased_configuration = take(ToolExecutionConfiguration::create(leased_data));
    auto leased_driver = std::make_shared<Driver>();
    std::optional<InvocationOperation> leased_operation;
    std::weak_ptr<const ResourceLease> weak_lease;
    std::weak_ptr<ToolInstance> weak_instance;
    bool destructor_observed = false;
    {
        auto lease = std::make_shared<ProbeLease>(resource, [&] {
            destructor_observed = true;
            check(!leased_operation->poll().physically_exited, "physical exit is not published before lease destruction finishes");
            check(take(leased_operation->wait_for(InvocationWaitTarget::PhysicalExit, std::chrono::milliseconds{0})).outcome == InvocationWaitOutcome::TimedOut,
                "a waiter sees cleanup still pending while a lease destructor is running");
        });
        weak_lease = lease;
        auto leased_values = context_data(leased_configuration); leased_values.resources = {{slot, lease}};
        const auto leased_context = take(InvocationContext::create(leased_values));
        auto leased_instance = std::make_shared<AsyncInstance>(leased_configuration, leased_driver); weak_instance = leased_instance;
        leased_operation = take(InvocationOperation::start(leased_instance, leased_context));
        take(leased_driver->completion->complete(completed(leased_context)));
    }
    check(!weak_lease.expired() && !weak_instance.expired(), "logical completion retains leases and instance while cleanup is pending");
    take(leased_driver->completion->mark_exited());
    check(destructor_observed && weak_lease.expired() && weak_instance.expired() && leased_operation->poll().physically_exited,
        "physical exit publishes only after owned lease and instance cleanup");
    exit_during_cancel(leased_configuration, slot, resource, false);
    exit_during_cancel(leased_configuration, slot, resource, true);

    auto detached_driver = std::make_shared<Driver>();
    std::weak_ptr<ToolInstance> detached_instance;
    {
        auto owned = std::make_shared<AsyncInstance>(async_configuration, detached_driver); detached_instance = owned;
        const auto abandoned = take(InvocationOperation::start(owned, async_context));
        check(!abandoned.poll().physically_exited, "independent backend token initially owns a live invocation");
    }
    check(!detached_instance.expired() && !detached_driver->context.expired(), "dropping the observer handle does not destroy a backend's active inputs or instance");
    take(detached_driver->completion->complete(completed(*detached_driver->context.lock())));
    take(detached_driver->completion->mark_exited());
    check(detached_instance.expired() && detached_driver->context.expired(), "backend token releases ownership only at actual exit");
}
} // namespace

int main()
{
    try { tool_execution(); }
    catch (const std::exception& error) { std::cerr << "Unexpected exception: " << error.what() << '\n'; return 1; }
    catch (...) { std::cerr << "Unexpected non-standard exception\n"; return 1; }
    std::cout << "tool_execution: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
