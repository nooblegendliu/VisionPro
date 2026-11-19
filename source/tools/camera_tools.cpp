#include "camera_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>

#include <algorithm>
#include <limits>

namespace visionpro::tools {
namespace {
using namespace core;
using namespace infrastructure;
using Clock = std::chrono::steady_clock;

template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

const ConfigValue& fixed(const ToolExecutionConfiguration& configuration, std::string_view id)
{ return std::ranges::find(configuration.data().parameters, id, [](const auto& parameter) { return parameter.id.text(); })->value; }

DataValue text(std::string_view value)
{ return std::get<DataValue>(DataValue::string(value)); }

DataValue optional_number(std::optional<std::uint64_t> n)
{ return n ? DataValue::some(text(std::to_string(*n))) : DataValue::none(DataType::string()); }

Result<std::vector<ToolOutput>> image_outputs(const ImageFrame& frame)
{
    if (!frame.frame) { return Diagnostic{ErrorCode::InvalidImage, Severity::Error, {}, "Camera frame input lacks acquisition identity."}; }
    if (frame.frame->sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return Diagnostic{ErrorCode::SizeOverflow, Severity::Error, {}, "Frame number exceeds the Int64 workflow input range."};
    }
    auto image = DataValue::image(frame);
    if (auto* failure = std::get_if<Diagnostic>(&image)) { return std::move(*failure); }
    return std::vector<ToolOutput>{{key<OutputId>("image"), std::get<DataValue>(std::move(image))},
        {key<OutputId>("frame_number"), DataValue::integer(static_cast<std::int64_t>(frame.frame->sequence))}};
}

class CaptureControl final : public InvocationControl {
public:
    void request_cancel() override { stop.request_stop(); }
    std::stop_source stop;
};

Result<std::monostate> acquire(const InvocationContextData& call, ToolCompletion& result, std::stop_token stop)
{
    const auto found = std::ranges::find(call.resources, "camera", [](const auto& r) { return r.slot.text(); });
    const auto lease = found == call.resources.end() ? nullptr : std::dynamic_pointer_cast<const CameraLease>(found->lease);
    if (!lease) { return Diagnostic{ErrorCode::InvalidResource, Severity::Error, {}, "Acquisition requires an exclusive camera lease."}; }
    const auto& config = call.configuration;
    const auto& point = *fixed(config, "frame_origin").as_point2d();
    const auto mode = fixed(config, "mode").as_enum()->text();
    const auto selected = mode == "Single" ? CameraMode::Single : mode == "Software" ? CameraMode::Software
        : mode == "Continuous" ? CameraMode::Continuous : CameraMode::Hardware;
    const auto timeout = std::chrono::milliseconds{*fixed(config, "timeout_ms").as_int64()};
    auto captured = lease->capture(point.coordinate_frame, selected, timeout, stop);
    auto closed = lease->finish();
    if (auto* failure = std::get_if<Diagnostic>(&captured)) {
        if (auto* close_failure = std::get_if<Diagnostic>(&closed)) { result.diagnostics.push_back(std::move(*close_failure)); }
        return std::move(*failure);
    }
    if (auto* failure = std::get_if<Diagnostic>(&closed)) { return std::move(*failure); }
    const auto& frame = std::get<CameraFrame>(captured);
    auto image = image_outputs(frame.image);
    if (auto* failure = std::get_if<Diagnostic>(&image)) { return std::move(*failure); }
    result.outputs = std::get<std::vector<ToolOutput>>(std::move(image));
    result.outputs.push_back({key<OutputId>("serial"), text(lease->serial())});
    result.outputs.push_back({key<OutputId>("timestamp_source"), text(frame.timestamp_source)});
    result.outputs.push_back({key<OutputId>("device_ticks"), optional_number(frame.device_ticks)});
    result.outputs.push_back({key<OutputId>("tick_frequency_hz"), optional_number(frame.tick_frequency_hz)});
    result.outputs.push_back({key<OutputId>("device_frame_number"), optional_number(frame.device_frame_number)});
    result.timings.push_back({TimingScope::ToolModule, TimingSource::AdapterReported,
        frame.device_duration_unit, frame.device_duration, key<SchemaId>("camera")});
    return std::monostate{};
}

void capture_worker(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion, std::stop_token stop)
{
    ToolCompletion result{context->data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
    const auto start = Clock::now();
    try {
        const auto checked = acquire(context->data(), result, stop);
        if (const auto* failure = std::get_if<Diagnostic>(&checked)) {
            result.status = failure->code == ErrorCode::SourceCancelled ? ToolCompletionStatus::Cancelled
                : failure->code == ErrorCode::SourceTimedOut ? ToolCompletionStatus::TimedOut : ToolCompletionStatus::Failed;
            result.diagnostics.insert(result.diagnostics.begin(), *failure);
            result.outputs.clear();
        }
    } catch (...) {
        result.status = ToolCompletionStatus::Failed;
        result.outputs.clear();
        result.diagnostics = {{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Camera capture worker failed."}};
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
    context.reset();
    if (auto reported = completion.complete(result); const auto* failure = std::get_if<Diagnostic>(&reported)) {
        result.status = ToolCompletionStatus::Failed;
        result.outputs.clear();
        result.timings.clear();
        result.diagnostics = {*failure};
        static_cast<void>(completion.complete(result));
    }
    static_cast<void>(completion.mark_exited());
}

class CameraTool final : public ToolInstance {
public:
    CameraTool(const ToolExecutionConfiguration& config, bool capture) : ToolInstance(config), capture_(capture) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        if (!capture_) {
            const auto& call = context->data();
            auto outputs = image_outputs(*call.inputs.front().value.as_image());
            if (auto* failure = std::get_if<Diagnostic>(&outputs)) { return std::move(*failure); }
            return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded,
                ToolJudgement::NotEvaluated, std::get<std::vector<ToolOutput>>(std::move(outputs)), {}}};
        }
        auto control = std::make_shared<CaptureControl>();
        auto registered = completion.register_control(control);
        if (auto* failure = std::get_if<Diagnostic>(&registered)) { return std::move(*failure); }
        try {
            std::jthread worker(capture_worker, context, completion, control->stop.get_token());
            try { worker.detach(); } catch (...) { control->request_cancel(); }
        } catch (...) {
            ToolCompletion failure{context->data().identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
                {{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not start the camera capture worker."}}};
            static_cast<void>(completion.complete(failure));
            static_cast<void>(completion.mark_exited());
        }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    bool capture_;
};

class CameraFactory final : public ToolInstanceFactory {
public:
    explicit CameraFactory(bool capture) : capture_(capture) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    {
        if (capture_) {
            const auto& origin = *fixed(config, "frame_origin").as_point2d();
            if (origin.unit != MeasurementUnit::Pixel || origin.x != 0 || origin.y != 0) {
                DiagnosticLocation where;
                where.parameter = key<ParameterId>("frame_origin");
                return Diagnostic{ErrorCode::CoordinateMismatch, Severity::Error, where, "Camera frame origin must be (0,0) in pixels."};
            }
        }
        return std::shared_ptr<ToolInstance>{std::make_shared<CameraTool>(config, capture_)};
    }
private:
    bool capture_;
};

ToolRegistration registration(bool capture)
{
    const auto id = capture ? "visionpro.camera.acquire" : "visionpro.camera.frame";
    const auto schema = key<SchemaId>(id);
    ToolDescriptor tool{key<ToolTypeId>(id), capture ? "Acquire camera image" : "Read triggered camera frame",
        1, {schema, 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    const ValueSemantics pixels{MeasurementUnit::Pixel, {}};
    tool.outputs = {{key<OutputId>("image"), "Image", DataType::image(), pixels},
        {key<OutputId>("frame_number"), "Frame number", DataType::integer(), {}}};
    tool.image_formats = {{PixelDepth::UInt8, ChannelOrder::Mono}, {PixelDepth::UInt8, ChannelOrder::BGR}};
    if (capture) {
        tool.execution.mode = ToolExecutionMode::Asynchronous;
        tool.execution.effects = ToolEffects::External;
        tool.execution.cancellation = ToolCancellation::Cooperative;
        tool.resources = {{key<SchemaId>("camera"), ResourceKind::Device, true, true}};
        const auto mode = std::get<DataType>(DataType::enumeration(key<SchemaId>("visionpro.camera.acquire.mode"), 1,
            {{key<EnumMemberId>("Single"),"Single"}, {key<EnumMemberId>("Continuous"),"Continuous"},
                {key<EnumMemberId>("Software"),"Software"}, {key<EnumMemberId>("Hardware"),"Hardware"}}));
        tool.parameters = {
            {key<ParameterId>("frame_origin"), "Frame origin", DataType::point2d(), true, {}, {}, {}, pixels},
            {key<ParameterId>("mode"), "Acquisition mode", mode, false, std::get<ConfigValue>(ConfigValue::enumeration(mode, key<EnumMemberId>("Single")))},
            {key<ParameterId>("timeout_ms"), "Capture timeout (ms)", DataType::integer(), false, ConfigValue::integer(1000)}};
        tool.parameters.back().constraints.integer_range = IntegerRange{1, 60000};
        for (auto& parameter : tool.parameters) { parameter.binding.time = BindingTime::SnapshotBuild; }
        for (const auto name : {"serial", "timestamp_source"}) {
            tool.outputs.push_back({key<OutputId>(name), name, DataType::string(), {}});
        }
        for (const auto name : {"device_ticks", "tick_frequency_hz", "device_frame_number"}) {
            tool.outputs.push_back({key<OutputId>(name), name, DataType::optional(DataType::string()), {}});
        }
    } else {
        InputDescriptor image{key<InputId>("image"), "Triggered image", DataType::image(), true, {}, {}, {}, pixels};
        image.binding.allowed_sources = {BindingSourceKind::PublicInput, BindingSourceKind::RunOutput};
        tool.inputs = {std::move(image)};
        tool.execution.retry = ToolRetry::Idempotent;
    }
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<CameraFactory>(capture)};
}
}

std::vector<core::ToolRegistration> camera_registrations()
{ return {registration(true), registration(false)}; }
} // namespace visionpro::tools
