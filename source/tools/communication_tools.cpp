#include "communication_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/communication.hpp>

#include <algorithm>

namespace visionpro::tools {
namespace {
using namespace core;
using infrastructure::CommunicationLease;
using Clock = std::chrono::steady_clock;
enum class Command { Send, Receive, Read, Write, EncodeUtf8 };

template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

std::int64_t configured_integer(const ToolExecutionConfiguration& config, std::string_view name)
{
    const auto parameter = std::ranges::find(config.data().parameters, name, [](const auto& p) { return p.id.text(); });
    return *parameter->value.as_int64();
}

Result<DataValue> communicate(Command command, const InvocationContextData& call, std::stop_token stop)
{
    const auto& endpoint = call.resources.front().lease;
    const auto* lease = dynamic_cast<const CommunicationLease*>(endpoint.get());
    if (!lease) {
        DiagnosticLocation location;
        location.resource = endpoint->reference().id;
        return Diagnostic{ErrorCode::ResourceKindMismatch, Severity::Error, location,
            "The endpoint slot requires a communication device."};
    }
    const auto timeout = std::chrono::milliseconds{configured_integer(call.configuration, "timeout_ms")};
    if (command == Command::Send) {
        const auto& payload = *call.inputs.front().value.as_binary();
        auto sent = lease->send_frame(payload, timeout, stop);
        if (auto* failure = std::get_if<Diagnostic>(&sent)) { return std::move(*failure); }
        return DataValue::integer(static_cast<std::int64_t>(payload.size()));
    }
    if (command == Command::Receive) {
        auto received = lease->receive_frame(timeout, stop);
        if (auto* failure = std::get_if<Diagnostic>(&received)) { return std::move(*failure); }
        return DataValue::binary(std::get<std::vector<std::uint8_t>>(received));
    }
    const auto address = static_cast<std::uint16_t>(configured_integer(call.configuration, "address"));
    if (command == Command::Read) {
        auto read = lease->read_register(address, timeout, stop);
        if (auto* failure = std::get_if<Diagnostic>(&read)) { return std::move(*failure); }
        return DataValue::integer(std::get<std::uint16_t>(read));
    }
    const auto value = static_cast<std::uint16_t>(*call.inputs.front().value.as_int64());
    auto written = lease->write_register(address, value, timeout, stop);
    if (auto* failure = std::get_if<Diagnostic>(&written)) { return std::move(*failure); }
    return DataValue::boolean(true);
}

class CommunicationControl final : public InvocationControl {
public:
    void request_cancel() override { stop.request_stop(); }
    std::stop_source stop;
};

void run_operation(Command command, std::shared_ptr<const InvocationContext> context, InvocationCompletion completion,
    std::stop_token stop)
{
    const auto& call = context->data();
    ToolCompletion result{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
    const auto started = Clock::now();
    try {
        auto output = communicate(command, call, stop);
        if (auto* failure = std::get_if<Diagnostic>(&output)) {
            if (command != Command::Read) { failure->operation_key = operation_key(call.identity); }
            const bool cancelled = failure->code == ErrorCode::SourceCancelled
                || (failure->code == ErrorCode::ExternalEffectUnknown && stop.stop_requested());
            result.status = failure->code == ErrorCode::SourceTimedOut ? ToolCompletionStatus::TimedOut
                : cancelled ? ToolCompletionStatus::Cancelled : ToolCompletionStatus::Failed;
            result.diagnostics.push_back(std::move(*failure));
        } else {
            result.outputs.push_back({call.configuration.data().tool.outputs.front().id, std::get<DataValue>(std::move(output))});
        }
        // Neither length-framed TCP nor these Modbus replies report peer processing time.
        result.timings.push_back({TimingScope::ToolModule, TimingSource::AdapterReported, TimingUnit::Unknown, {}, {}});
    } catch (...) {
        result.status = ToolCompletionStatus::Failed;
        result.outputs.clear();
        if (command == Command::Read) {
            result.diagnostics = {{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Communication worker failed during the read."}};
        } else {
            result.diagnostics = {{ErrorCode::ExternalEffectUnknown, Severity::Error, {},
                "Communication worker failed before it could report the outcome.", {}, operation_key(call.identity)}};
        }
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started);
    context.reset();
    if (auto accepted = completion.complete(result); auto* failure = std::get_if<Diagnostic>(&accepted)) {
        result.status = ToolCompletionStatus::Failed;
        result.outputs.clear();
        result.timings.clear();
        std::erase_if(result.diagnostics, [](const Diagnostic& d) { return !is_valid_diagnostic(d); });
        result.diagnostics.push_back(std::move(*failure));
        static_cast<void>(completion.complete(result));
    }
    static_cast<void>(completion.mark_exited());
}

class CommunicationTool final : public ToolInstance {
public:
    CommunicationTool(const ToolExecutionConfiguration& config, Command command) : ToolInstance(config), command_(command) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        if (command_ == Command::EncodeUtf8) {
            const auto& call = context->data();
            const auto& text = *call.inputs.front().value.as_string();
            const std::vector<std::uint8_t> bytes(text.begin(), text.end());
            return InvocationStart{ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
                {{key<OutputId>("payload"), DataValue::binary(bytes)}}, {}}};
        }
        auto control = std::make_shared<CommunicationControl>();
        auto registered = completion.register_control(control);
        if (auto* failure = std::get_if<Diagnostic>(&registered)) { return std::move(*failure); }
        try {
            std::jthread worker(run_operation, command_, context, completion, control->stop.get_token());
            try { worker.detach(); } catch (...) { control->request_cancel(); }
        } catch (...) {
            ToolCompletion failed{context->data().identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
                {{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not start the communication worker."}}};
            static_cast<void>(completion.complete(failed));
            static_cast<void>(completion.mark_exited());
        }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }
    Command command_;
};

class CommunicationFactory final : public ToolInstanceFactory {
public:
    explicit CommunicationFactory(Command command) : command_(command) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<CommunicationTool>(config, command_)}; }
private:
    Command command_;
};

InputDescriptor input(std::string_view id, std::string name, const DataType& type)
{
    InputDescriptor port{key<InputId>(id), std::move(name), type, true, {}, {}, {}, {}};
    port.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput,
        BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
    return port;
}

ParameterDescriptor integer_parameter(std::string_view id, std::string name, std::int64_t initial, IntegerRange range)
{
    ParameterDescriptor p{key<ParameterId>(id), std::move(name), DataType::integer(), false, ConfigValue::integer(initial), {}, {}, {}};
    p.constraints.integer_range = range;
    p.binding.time = BindingTime::SnapshotBuild;
    return p;
}

ToolRegistration registration(Command command, std::string_view id, std::string name)
{
    const auto schema = key<SchemaId>(id);
    ToolDescriptor tool{key<ToolTypeId>(id), std::move(name), 1, {schema, 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    if (command != Command::EncodeUtf8) {
        tool.resources = {{key<SchemaId>("endpoint"), ResourceKind::Device, true, true}};
        tool.parameters = {integer_parameter("timeout_ms", "Operation timeout (ms)", 1000, {1, 60000})};
        tool.execution.mode = ToolExecutionMode::Asynchronous;
        tool.execution.cancellation = ToolCancellation::Cooperative;
    }
    if (command == Command::Read || command == Command::Write) {
        tool.parameters.push_back(integer_parameter("address", "Holding register address", 0, {0, 65535}));
    }
    switch (command) {
    case Command::Send:
        tool.inputs = {input("payload", "Payload", DataType::binary())};
        tool.outputs = {{key<OutputId>("bytes_sent"), "Payload bytes sent to the local network stack", DataType::integer(), {}}};
        break;
    case Command::Receive:
        tool.outputs = {{key<OutputId>("payload"), "Payload", DataType::binary(), {}}};
        break;
    case Command::Read:
        tool.outputs = {{key<OutputId>("value"), "Register value", DataType::integer(), {}}};
        break;
    case Command::Write:
        tool.inputs = {input("value", "Register value", DataType::integer())};
        tool.inputs.front().constraints.integer_range = IntegerRange{0, 65535};
        tool.outputs = {{key<OutputId>("confirmed"), "Write confirmed", DataType::boolean(), {}}};
        break;
    case Command::EncodeUtf8:
        tool.inputs = {input("text", "Text", DataType::string())};
        tool.outputs = {{key<OutputId>("payload"), "UTF-8 bytes", DataType::binary(), {}}};
        break;
    }
    if (command == Command::Read || command == Command::EncodeUtf8) { tool.execution.retry = ToolRetry::Idempotent; }
    else { tool.execution.effects = ToolEffects::External; }
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<CommunicationFactory>(command)};
}
}

std::vector<core::ToolRegistration> communication_registrations()
{
    return {registration(Command::Send, "visionpro.tcp.send", "Send TCP frame"),
        registration(Command::Receive, "visionpro.tcp.receive", "Receive TCP frame"),
        registration(Command::Read, "visionpro.plc.read", "Read holding register"),
        registration(Command::Write, "visionpro.plc.write", "Write holding register"),
        registration(Command::EncodeUtf8, "visionpro.text.encode_utf8", "Encode UTF-8 text")};
}
} // namespace visionpro::tools
