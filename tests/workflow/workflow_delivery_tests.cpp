#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/workflow_engine.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace std::chrono_literals;

int checks = 0;

void check(bool condition, std::string_view message)
{
    ++checks;
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> result)
{
    if (const auto* error = std::get_if<Diagnostic>(&result)) {
        throw std::runtime_error(std::string(error_code_name(error->code)) + ": " + error->message);
    }
    return std::get<T>(std::move(result));
}

template<class T> Diagnostic rejected(const Result<T>& result, ErrorCode code, std::string_view message)
{
    const auto* failure = std::get_if<Diagnostic>(&result);
    check(failure && failure->code == code, message);
    return *failure;
}

template<UuidIdentity T> T uuid(char suffix = '1')
{
    std::string text = "00000000-0000-4000-8000-000000000000";
    text.back() = suffix;
    return take(IdFactory::parse<T>(text));
}

template<SchemaIdentity T> T key(std::string_view text)
{ return take(SchemaIdFactory::parse<T>(text)); }

enum class Sample {
    First, Next, ShortImage, NegativeCount, NullReadings, InvalidHint, MissingName, InvalidText,
    UnknownCall, FailedOutputStatus, UnknownJudgement
};

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr ImageLayout layout{2, 2, 3, mono8};
constexpr std::array first_pixels{std::byte{17}, std::byte{33}, std::byte{0}, std::byte{49}, std::byte{65}};
constexpr std::array next_pixels{std::byte{82}, std::byte{83}, std::byte{0}, std::byte{84}, std::byte{85}};
constexpr std::string_view first_text{"测量 A"};
constexpr std::string_view next_text{"测量 B"};
constexpr std::array<std::uint8_t, 3> first_binary{0, 0xb1, 0xff};
constexpr std::array<std::uint8_t, 3> next_binary{0, 0xb2, 0xff};

struct OutputName { std::string_view foreign; std::string_view stable; };
constexpr std::array names{OutputName{"图像", "image"}, OutputName{"数值", "zero"}, OutputName{"读数", "readings"},
    OutputName{"说明", "text"}, OutputName{"匹配", "match"}, OutputName{"提示", "hint"}, OutputName{"原始数据", "binary"}};
constexpr std::array enumeration{names[5].foreign, names[2].foreign, names[1].foreign, names[4].foreign,
    names[3].foreign, names[0].foreign, names[6].foreign};

template<class T> struct BorrowedArray { const T* data; std::ptrdiff_t count; };

struct ForeignResult {
    std::int64_t call_code;
    std::int64_t output_code;
    std::int64_t judgement_code;
    std::int64_t hint_code;
    std::span<const std::string_view> enumerated_names;
    BorrowedArray<std::byte> image;
    BorrowedArray<std::int64_t> readings;
    std::string_view text;
    BorrowedArray<std::uint8_t> binary;
    std::optional<std::chrono::nanoseconds> native_elapsed;
};

class ForeignBackend {
public:
    ForeignResult capture(Sample sample)
    {
        ++calls;
        const bool next = sample == Sample::Next;
        pixels_ = next ? next_pixels : first_pixels;
        binary_ = next ? next_binary : first_binary;
        const auto text = sample == Sample::InvalidText ? std::string_view{"\xc0\xaf"} : next ? next_text : first_text;
        std::ranges::copy(text, text_.begin());
        auto discovered = std::span<const std::string_view>{enumeration};
        if (sample == Sample::MissingName) { discovered = discovered.first(discovered.size() - 1); }
        std::ptrdiff_t count = next ? 2 : 0;
        if (sample == Sample::NegativeCount) { count = -1; }
        if (sample == Sample::NullReadings) { count = 2; }
        return {sample == Sample::UnknownCall ? -913 : 0, sample == Sample::FailedOutputStatus ? 0 : 1,
            sample == Sample::UnknownJudgement ? 9 : next ? 1 : 0,
            sample == Sample::InvalidHint ? 0 : 2, discovered,
            {pixels_.data(), sample == Sample::ShortImage ? 4 : 5},
            {sample == Sample::NullReadings ? nullptr : readings_.data(), count}, {text_.data(), text.size()},
            {binary_.data(), 3}, next ? std::optional<std::chrono::nanoseconds>{0ns} : std::nullopt};
    }

    std::atomic<unsigned> calls{0};

private:
    std::array<std::byte, 5> pixels_{};
    std::array<std::int64_t, 2> readings_{11, 27};
    std::array<char, 16> text_{};
    std::array<std::uint8_t, 3> binary_{};
};

Diagnostic external(ErrorCode code, Severity severity, ExternalStatusDomain domain, std::int64_t raw,
    ExternalCodeMapping mapping, std::string operation, std::string message)
{
    return {code, severity, {}, std::move(message),
        ExternalDiagnostic{key<SchemaId>("s36.foreign"), std::move(operation), domain, raw, mapping,
            key<SchemaId>("s36.foreign.status"), {}, "1.0"}};
}

template<class T> Result<std::span<const T>> checked_span(BorrowedArray<T> array)
{
    if (array.count < 0) {
        return Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "Foreign array length is negative."};
    }
    if (array.count == 0) { return std::span<const T>{}; }
    if (!array.data) {
        return Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "Positive foreign array length requires a buffer."};
    }
    return std::span<const T>{array.data, static_cast<std::size_t>(array.count)};
}

Result<DataValue> image_value(BorrowedArray<std::byte> borrowed)
{
    auto bytes = checked_span(borrowed);
    if (const auto* failure = std::get_if<Diagnostic>(&bytes)) { return *failure; }
    auto image = ImageHandle::copy(layout, std::get<std::span<const std::byte>>(bytes),
        key<CoordinateSystemId>("s36.camera"));
    if (const auto* failure = std::get_if<Diagnostic>(&image)) { return *failure; }
    return DataValue::image({std::get<ImageHandle>(std::move(image)), {}, {}});
}

Result<DataValue> readings_value(BorrowedArray<std::int64_t> borrowed)
{
    auto readings = checked_span(borrowed);
    if (const auto* failure = std::get_if<Diagnostic>(&readings)) { return *failure; }
    std::vector<DataValue> values;
    for (const auto reading : std::get<std::span<const std::int64_t>>(readings)) {
        values.push_back(DataValue::integer(reading));
    }
    return DataValue::list(DataType::integer(), values);
}

Result<DataValue> binary_value(BorrowedArray<std::uint8_t> borrowed)
{
    auto bytes = checked_span(borrowed);
    if (const auto* failure = std::get_if<Diagnostic>(&bytes)) { return *failure; }
    const auto view = std::get<std::span<const std::uint8_t>>(bytes);
    return DataValue::binary({view.begin(), view.end()});
}

ToolCompletion adapt(const InvocationContextData& context, const ForeignResult& source)
{
    const auto failed = [&](Diagnostic diagnostic) {
        return ToolCompletion{context.identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated,
            {}, {std::move(diagnostic)}};
    };
    if (source.call_code != 0) {
        return failed(external(ErrorCode::InvalidValue, Severity::Error, ExternalStatusDomain::Call,
            source.call_code, ExternalCodeMapping::Unmapped, "execute", "Unknown foreign call status."));
    }
    if (source.output_code != 1) {
        const auto mapping = source.output_code == 0 ? ExternalCodeMapping::Mapped : ExternalCodeMapping::Unmapped;
        return failed(external(ErrorCode::InvalidValue, Severity::Error, ExternalStatusDomain::Output,
            source.output_code, mapping, "getOutput", "Foreign output did not report success."));
    }
    if (source.judgement_code != 0 && source.judgement_code != 1) {
        return failed(external(ErrorCode::InvalidValue, Severity::Error, ExternalStatusDomain::Judgement,
            source.judgement_code, ExternalCodeMapping::Unmapped, "getJudgement", "Unknown foreign judgement."));
    }
    for (const auto& name : names) {
        if (std::ranges::find(source.enumerated_names, name.foreign) == source.enumerated_names.end()) {
            DiagnosticLocation location;
            location.schema = context.configuration.data().tool.schema.id;
            location.output = key<OutputId>(name.stable);
            return failed({ErrorCode::IncompleteSchema, Severity::Error, std::move(location),
                "Foreign output enumeration omitted a declared field."});
        }
    }

    ToolCompletion result{context.identity, ToolCompletionStatus::Succeeded,
        source.judgement_code == 1 ? ToolJudgement::OK : ToolJudgement::NG, {},
        {external(ErrorCode::InvalidValue, Severity::Info, ExternalStatusDomain::Call,
            source.call_code, ExternalCodeMapping::Mapped, "execute", "Foreign call succeeded."),
         external(ErrorCode::InvalidValue, Severity::Info, ExternalStatusDomain::Output,
            source.output_code, ExternalCodeMapping::Mapped, "getOutput", "Foreign output reported success."),
         external(ErrorCode::InvalidValue, Severity::Info, ExternalStatusDomain::Judgement,
            source.judgement_code, ExternalCodeMapping::Mapped, "getJudgement", "Foreign judgement mapped.")},
        source.native_elapsed, {},
        {{TimingScope::ToolModule, TimingSource::ToolReported, TimingUnit::Unknown, 4.25, key<SchemaId>("s36.foreign")},
         {TimingScope::ToolAlgorithm, TimingSource::ToolReported, TimingUnit::Milliseconds, 0.0, key<SchemaId>("s36.foreign")},
         {TimingScope::ResultMaterialization, TimingSource::AdapterReported, TimingUnit::Microseconds}}};

    for (auto& [name, value] : std::array{
        std::pair{std::string_view{"image"}, image_value(source.image)},
        std::pair{std::string_view{"readings"}, readings_value(source.readings)},
        std::pair{std::string_view{"text"}, DataValue::string(source.text)},
        std::pair{std::string_view{"binary"}, binary_value(source.binary)}}) {
        const auto id = key<OutputId>(name);
        if (const auto* failure = std::get_if<Diagnostic>(&value)) {
            auto diagnostic = external(failure->code, Severity::Error, ExternalStatusDomain::Output,
                source.output_code, ExternalCodeMapping::Mapped, "getOutput", failure->message);
            diagnostic.location.output = id;
            result.status = ToolCompletionStatus::Failed;
            result.judgement = ToolJudgement::NotEvaluated;
            result.outputs = {{id, InvalidOutput{std::move(diagnostic)}}};
            return result;
        }
        result.outputs.push_back({id, std::get<DataValue>(std::move(value))});
    }
    result.outputs.push_back({key<OutputId>("zero"), DataValue::integer(0)});
    result.outputs.push_back({key<OutputId>("match"), DataValue::none(DataType::integer())});
    OutputContent hint = NotProduced{OutputAbsenceReason::ConditionNotMet};
    if (source.hint_code == 0) {
        // Deliberately contradictory backend to exercise the invocation boundary.
        hint = InvalidOutput{external(ErrorCode::InvalidValue, Severity::Error, ExternalStatusDomain::Output,
            source.hint_code, ExternalCodeMapping::Mapped, "getHint", "Foreign non-required hint is invalid.")};
    }
    result.outputs.push_back({key<OutputId>("hint"), std::move(hint)});
    return result;
}

class ForeignTool final : public ToolInstance {
public:
    ForeignTool(ToolExecutionConfiguration configuration, std::shared_ptr<ForeignBackend> backend)
        : ToolInstance(std::move(configuration)), backend_(std::move(backend)) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto sample = static_cast<Sample>(*context->data().inputs.front().value.as_int64());
        return InvocationStart{adapt(context->data(), backend_->capture(sample))};
    }

    std::shared_ptr<ForeignBackend> backend_;
};

class ForeignFactory final : public ToolInstanceFactory {
public:
    explicit ForeignFactory(std::shared_ptr<ForeignBackend> backend) : backend_(std::move(backend)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<ForeignTool>(configuration, backend_)}; }

private:
    std::shared_ptr<ForeignBackend> backend_;
};

struct ActiveImage {
    ToolInvocationIdentity identity;
    InvocationCompletion completion;
    std::weak_ptr<const InvocationContext> context;
};

class ImageConsumer {
public:
    bool publish(const ActiveImage& active)
    {
        const std::lock_guard lock(mutex_);
        if (draining_) { return false; }
        active_ = active;
        ready_.notify_all();
        return true;
    }

    ActiveImage await()
    {
        std::unique_lock lock(mutex_);
        if (!ready_.wait_for(lock, 5s, [&] { return active_.has_value(); })) {
            throw std::runtime_error("The asynchronous image consumer did not start.");
        }
        return *active_;
    }

    void drain()
    {
        std::optional<ActiveImage> pending;
        {
            const std::lock_guard lock(mutex_);
            draining_ = true;
            pending.swap(active_);
        }
        if (pending) {
            static_cast<void>(pending->completion.complete({pending->identity, ToolCompletionStatus::Cancelled,
                ToolJudgement::NotEvaluated, {}, {}}));
            static_cast<void>(pending->completion.mark_exited());
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::optional<ActiveImage> active_;
    bool draining_{false};
};

class ConsumerControl final : public InvocationControl {
    void request_cancel() override {}
};

class ConsumerTool final : public ToolInstance {
public:
    ConsumerTool(ToolExecutionConfiguration configuration, std::shared_ptr<ImageConsumer> consumer)
        : ToolInstance(std::move(configuration)), consumer_(std::move(consumer)) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        const auto control = std::make_shared<ConsumerControl>();
        if (auto registered = completion.register_control(control);
            const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        const auto identity = context->data().identity;
        if (!consumer_->publish({identity, completion, context})) {
            static_cast<void>(completion.complete({identity, ToolCompletionStatus::Cancelled,
                ToolJudgement::NotEvaluated, {}, {}}));
            static_cast<void>(completion.mark_exited());
        }
        return InvocationStart{AsynchronousInvocation{control}};
    }

    std::shared_ptr<ImageConsumer> consumer_;
};

class ConsumerFactory final : public ToolInstanceFactory {
public:
    explicit ConsumerFactory(std::shared_ptr<ImageConsumer> consumer) : consumer_(std::move(consumer)) {}

    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<ConsumerTool>(configuration, consumer_)}; }

private:
    std::shared_ptr<ImageConsumer> consumer_;
};

ToolRegistry registry_for(const std::shared_ptr<ForeignBackend>& backend, const std::shared_ptr<ImageConsumer>& consumer)
{
    const auto integer = DataType::integer();
    auto sample = InputDescriptor{key<InputId>("sample"), "Sample", integer};
    sample.binding.allowed_sources = {BindingSourceKind::PublicInput};
    ToolDescriptor producer{key<ToolTypeId>("s36.source"), "Foreign producer", 1, {key<SchemaId>("s36.source.schema"), 1},
        {}, {sample},
        {{key<OutputId>("image"), "Image", DataType::image()},
         {key<OutputId>("zero"), "Zero", integer},
         {key<OutputId>("readings"), "Readings", DataType::list(integer)},
         {key<OutputId>("text"), "Text", DataType::string()},
         {key<OutputId>("match"), "Match", DataType::optional(integer)},
         {key<OutputId>("hint"), "Hint", integer, {}, false, {OutputAbsenceReason::ConditionNotMet}},
         {key<OutputId>("binary"), "Binary", DataType::binary()}}};
    producer.image_formats = {mono8};
    auto image = InputDescriptor{key<InputId>("image"), "Image", DataType::image()};
    image.binding.allowed_sources = {BindingSourceKind::PublicInput};
    ToolDescriptor sink{key<ToolTypeId>("s36.consumer"), "Image consumer", 1, {key<SchemaId>("s36.consumer.schema"), 1},
        {}, {image}, {{key<OutputId>("pixel"), "Pixel", integer}}};
    sink.image_formats = {mono8};
    sink.execution.mode = ToolExecutionMode::Asynchronous;
    const std::array registrations{
        ToolRegistration{producer, {producer.schema.id, 1, 1, {}}, std::make_shared<ForeignFactory>(backend)},
        ToolRegistration{sink, {sink.schema.id, 1, 1, {}}, std::make_shared<ConsumerFactory>(consumer)}};
    return take(ToolRegistry::create(registrations));
}

Workflow workflow(char id, std::string_view tool, InputDescriptor input)
{
    const auto flow = uuid<WorkflowId>(id);
    const auto node = uuid<NodeId>(id);
    const auto schema = key<SchemaId>(id == '1' ? "s36.capture.public" : "s36.consumer.public");
    const auto input_id = input.id;
    return {flow, std::string(tool), {schema, 1, {std::move(input)}},
        {{node, std::string(tool), ToolConfiguration{key<ToolTypeId>(tool), 1},
            {{input_id, {ScopedInputReference{flow, {}, input_id, {}}}}}}},
        {}, {}, {node}};
}

Solution solution()
{
    Solution result{uuid<SolutionId>(), "Result delivery", 1, 1,
        {workflow('1', "s36.source", {key<InputId>("sample"), "Sample", DataType::integer()}),
         workflow('2', "s36.consumer", {key<InputId>("image"), "Image", DataType::image()})}};
    result.coordinates = {{key<CoordinateSystemId>("s36.camera"), CoordinateKind::Image, MeasurementUnit::Pixel,
        {0, 0}, {1, 0}, {0, -1}, Handedness::LeftHanded}};
    return result;
}

struct Fixture {
    explicit Fixture(std::size_t limit = 1)
        : engine(take(WorkflowEngine::create(registry, {}, WorkflowEngineOptions{limit}))),
          snapshot(take(ConfigSnapshot::capture(solution(), registry.descriptors(), {engine->session(),
              take(RuntimeIdFactory::from_value<ConfigSnapshotId>(1))}))) {}

    ~Fixture() { consumer->drain(); }

    RunHandle submit(char id, std::string_view input, const DataValue& value)
    {
        const auto request = take(RunRequest::create(snapshot, WorkflowTarget{uuid<WorkflowId>(id)},
            {{key<InputId>(input), value}}, {TriggerKind::Manual, key<SchemaId>("s36.test")}));
        auto submission = engine->submit(request);
        if (const auto* refusal = std::get_if<RunRejected>(&submission)) {
            throw std::runtime_error(refusal->diagnostic.message);
        }
        check(std::holds_alternative<RunHandle>(submission), "The exact fixture request is accepted.");
        return std::get<RunHandle>(std::move(submission));
    }

    RunHandle capture(Sample sample)
    { return submit('1', "sample", DataValue::integer(static_cast<std::int64_t>(sample))); }

    std::shared_ptr<ForeignBackend> backend{std::make_shared<ForeignBackend>()};
    std::shared_ptr<ImageConsumer> consumer{std::make_shared<ImageConsumer>()};
    ToolRegistry registry{registry_for(backend, consumer)};
    std::unique_ptr<WorkflowEngine> engine;
    ConfigSnapshot snapshot;
};

RunProgress wait_for(const RunHandle& run, ExecutionStatus status)
{
    const auto waited = take(run.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
    check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.quiescent && waited.progress.status == status
        && waited.progress.nodes.size() == 1 && waited.progress.nodes.front().invocation,
        "The invocation reaches the expected terminal status and physically exits.");
    return waited.progress;
}

NodeResult exact(std::optional<NodeResult> record, const RunHandle& run, const InvocationKey& call)
{
    check(record && record->data().identity.run == run.identity() && record->data().identity.invocation == call,
        "Result lookup returns the exact run and invocation.");
    return *record;
}

const OutputContent& output(const NodeResult& result, std::string_view name)
{
    const auto* slot = result.output(key<OutputId>(name));
    check(slot != nullptr, name);
    return slot->content;
}

const DataValue& value(const NodeResult& result, std::string_view name)
{ return std::get<DataValue>(output(result, name)); }

void image_pixels(const ImageHandle& image, const std::array<std::byte, 5>& expected)
{
    check(image.width() == 2 && image.height() == 2 && image.row_stride() == 3
        && std::ranges::equal(take(image.row(0)), std::span{expected}.first(2))
        && std::ranges::equal(take(image.row(1)), std::span{expected}.subspan(3, 2)),
        "The immutable image preserves its original rows and stride.");
}

NodeResult retain_after_eviction()
{
    Fixture fixture;
    const auto first = fixture.capture(Sample::First);
    const auto first_call = *wait_for(first, ExecutionStatus::Succeeded).nodes.front().invocation;
    const auto held = exact(take(fixture.engine->lookup_result(first.identity(), first_call)), first, first_call);
    const auto image = *value(held, "image").as_image();
    image_pixels(image.image, first_pixels);

    const auto consumer = fixture.submit('2', "image", take(DataValue::image(image)));
    const auto active = fixture.consumer->await();
    check(!take(fixture.engine->lookup_result(consumer.identity(), active.identity.invocation)),
        "An accepted invocation without a terminal record has no available result yet.");
    const auto next = fixture.capture(Sample::Next);
    const auto next_call = *wait_for(next, ExecutionStatus::Succeeded).nodes.front().invocation;
    const auto current = exact(take(fixture.engine->lookup_result(next.identity(), next_call)), next, next_call);
    image_pixels(value(current, "image").as_image()->image, next_pixels);
    rejected(fixture.engine->lookup_result(first.identity(), first_call), ErrorCode::ResultRetired,
        "The one-run archive retires the older completed run.");
    auto wrong = next.identity();
    wrong.snapshot.session = uuid<SolutionSessionId>();
    rejected(fixture.engine->lookup_result(wrong, next_call), ErrorCode::ResultIdentityMismatch,
        "The archive rejects a foreign session.");
    rejected(fixture.engine->lookup_result(next.identity(), first_call), ErrorCode::ResultIdentityMismatch,
        "The archive rejects an invocation from another run.");
    auto wrong_snapshot = next.identity();
    wrong_snapshot.snapshot.snapshot = take(RuntimeIdFactory::from_value<ConfigSnapshotId>(2));
    rejected(fixture.engine->lookup_result(wrong_snapshot, next_call), ErrorCode::ResultIdentityMismatch,
        "The archive verifies the complete retained run identity.");

    {
        const auto context = active.context.lock();
        check(context != nullptr, "The suspended consumer still owns its fixed input.");
        const auto& held_input = context->data().inputs.front().value.as_image()->image;
        image_pixels(held_input, first_pixels);
        const auto row = take(held_input.row(1));
        take(active.completion.complete({active.identity, ToolCompletionStatus::Succeeded, ToolJudgement::OK,
            {{key<OutputId>("pixel"), DataValue::integer(std::to_integer<std::int64_t>(row.front()))}}, {}}));
    }
    take(active.completion.mark_exited());
    const auto consumer_call = *wait_for(consumer, ExecutionStatus::Succeeded).nodes.front().invocation;
    const auto consumed = exact(take(fixture.engine->lookup_result(consumer.identity(), consumer_call)), consumer, consumer_call);
    check(value(consumed, "pixel") == DataValue::integer(49), "The consumer uses the original pixel after retirement.");
    check(fixture.backend->calls == 2, "Lookups and active consumers do not materialize the foreign results again.");
    take(fixture.engine->close());
    rejected(fixture.engine->lookup_result(consumer.identity(), consumer_call), ErrorCode::ResultRetired,
        "Closing the engine releases its completed-run index.");
    const auto original = exact(take(first.lookup(first_call)), first, first_call);
    image_pixels(value(original, "image").as_image()->image, first_pixels);
    return held;
}

void retention()
{
    const auto held = retain_after_eviction();
    image_pixels(value(held, "image").as_image()->image, first_pixels);
    check(value(held, "text") == take(DataValue::string(first_text))
        && value(held, "binary") == DataValue::binary({first_binary.begin(), first_binary.end()}),
        "The held snapshot preserves text and raw bytes after overwrites, eviction and engine destruction.");

    Fixture disabled(0);
    const auto run = disabled.capture(Sample::First);
    const auto call = *wait_for(run, ExecutionStatus::Succeeded).nodes.front().invocation;
    rejected(disabled.engine->lookup_result(run.identity(), call), ErrorCode::ResultRetired,
        "Zero retention releases completed runs immediately from the engine index.");
    image_pixels(value(exact(take(run.lookup(call)), run, call), "image").as_image()->image, first_pixels);
}

const Diagnostic& external_status(const NodeResult& result, ExternalStatusDomain domain, std::int64_t raw,
    ExternalCodeMapping mapping)
{
    const auto found = std::ranges::find_if(result.data().diagnostics, [&](const Diagnostic& diagnostic) {
        return diagnostic.external && diagnostic.external->domain == domain;
    });
    check(found != result.data().diagnostics.end() && found->external->raw_code == raw
        && found->external->mapping == mapping && found->external->provider == key<SchemaId>("s36.foreign")
        && found->external->provider_domain == key<SchemaId>("s36.foreign.status")
        && found->external->backend_version == "1.0", "External status keeps its exact domain and original code.");
    return *found;
}

NodeResult captured(Fixture& fixture, Sample sample, ExecutionStatus status)
{
    const auto run = fixture.capture(sample);
    const auto call = *wait_for(run, status).nodes.front().invocation;
    return exact(take(fixture.engine->lookup_result(run.identity(), call)), run, call);
}

void require_failed_outputs(const NodeResult& result)
{
    check(result.data().status == ExecutionStatus::Failed && result.data().judgement == Judgement::NotEvaluated
        && result.data().outputs.size() == names.size()
        && std::ranges::none_of(result.data().outputs, [](const OutputSlot& slot) {
            return std::holds_alternative<DataValue>(slot.content);
        }), "An invalid result fails atomically without publishing partial algorithm values.");
}

void external_delivery()
{
    Fixture fixture;
    const auto first = captured(fixture, Sample::First, ExecutionStatus::Succeeded);
    check(first.data().judgement == Judgement::NG, "Technical success preserves the separate NG judgement.");
    external_status(first, ExternalStatusDomain::Call, 0, ExternalCodeMapping::Mapped);
    external_status(first, ExternalStatusDomain::Output, 1, ExternalCodeMapping::Mapped);
    external_status(first, ExternalStatusDomain::Judgement, 0, ExternalCodeMapping::Mapped);
    check(value(first, "zero") == DataValue::integer(0) && value(first, "readings").elements()->empty()
        && value(first, "match") == DataValue::none(DataType::integer())
        && std::get<NotProduced>(output(first, "hint")).reason == OutputAbsenceReason::ConditionNotMet,
        "Zero, empty List, Optional None and declared NotProduced retain distinct states.");
    const auto& timing = first.data().timings;
    check(!first.data().elapsed && timing.size() == 3 && timing[0].scope == TimingScope::ToolModule
        && timing[0].source == TimingSource::ToolReported && timing[0].value == 4.25 && timing[0].unit == TimingUnit::Unknown
        && timing[1].scope == TimingScope::ToolAlgorithm && timing[1].unit == TimingUnit::Milliseconds && timing[1].value == 0.0
        && timing[2].scope == TimingScope::ResultMaterialization && timing[2].source == TimingSource::AdapterReported
        && !timing[2].value, "Separate timing scopes retain raw unknown units, missing values and measured zero.");

    const auto next = captured(fixture, Sample::Next, ExecutionStatus::Succeeded);
    check(next.data().judgement == Judgement::OK && next.data().elapsed == 0ns
        && value(next, "readings") == take(DataValue::list(DataType::integer(), {DataValue::integer(11), DataValue::integer(27)}))
        && value(next, "text") == take(DataValue::string(next_text))
        && value(next, "binary") == DataValue::binary({next_binary.begin(), next_binary.end()}),
        "The schema maps foreign names to exact list, UTF-8 text and binary values.");
    external_status(next, ExternalStatusDomain::Judgement, 1, ExternalCodeMapping::Mapped);

    struct InvalidSample { Sample mode; std::string_view port; ErrorCode code; };
    for (const auto [mode, port, code] : std::array{
        InvalidSample{Sample::ShortImage, "image", ErrorCode::InvalidImage},
        InvalidSample{Sample::NegativeCount, "readings", ErrorCode::InvalidValue},
        InvalidSample{Sample::NullReadings, "readings", ErrorCode::InvalidValue},
        InvalidSample{Sample::InvalidText, "text", ErrorCode::InvalidUtf8}}) {
        const auto record = captured(fixture, mode, ExecutionStatus::Failed);
        require_failed_outputs(record);
        const auto& invalid = std::get<InvalidOutput>(output(record, port)).diagnostic;
        check(invalid.code == code && invalid.location.output == key<OutputId>(port)
            && invalid.external && invalid.external->domain == ExternalStatusDomain::Output
            && invalid.external->raw_code == 1, "A malformed buffer keeps its output location and original status.");
    }
    const auto hint = captured(fixture, Sample::InvalidHint, ExecutionStatus::Failed);
    require_failed_outputs(hint);
    const auto& invalid_hint = external_status(hint, ExternalStatusDomain::Output, 0, ExternalCodeMapping::Mapped);
    check(invalid_hint.severity == Severity::Error && invalid_hint.location.output == key<OutputId>("hint"),
        "Invalid non-required output rejects a proposed successful completion.");

    const auto incomplete = captured(fixture, Sample::MissingName, ExecutionStatus::Failed);
    require_failed_outputs(incomplete);
    const auto& missing = incomplete.data().diagnostics.front();
    check(missing.code == ErrorCode::IncompleteSchema && missing.location.output == key<OutputId>("binary")
        && missing.location.schema == key<SchemaId>("s36.source.schema"),
        "Incomplete foreign enumeration identifies the omitted declared field.");

    const auto unknown_call = captured(fixture, Sample::UnknownCall, ExecutionStatus::Failed);
    require_failed_outputs(unknown_call);
    external_status(unknown_call, ExternalStatusDomain::Call, -913, ExternalCodeMapping::Unmapped);
    const auto failed_output = captured(fixture, Sample::FailedOutputStatus, ExecutionStatus::Failed);
    require_failed_outputs(failed_output);
    external_status(failed_output, ExternalStatusDomain::Output, 0, ExternalCodeMapping::Mapped);
    const auto unknown_judgement = captured(fixture, Sample::UnknownJudgement, ExecutionStatus::Failed);
    require_failed_outputs(unknown_judgement);
    external_status(unknown_judgement, ExternalStatusDomain::Judgement, 9, ExternalCodeMapping::Unmapped);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::string_view test = argc == 2 ? argv[1] : "";
        if (test == "retention") { retention(); }
        else if (test == "external") { external_delivery(); }
        else { throw std::runtime_error("Expected retention or external test name."); }
        std::cout << test << ": " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Result delivery failed: " << error.what() << '\n';
        return 1;
    }
}
