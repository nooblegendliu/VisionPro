#include "shared_tools.hpp"

#include <visionpro/core/correlation_join.hpp>
#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <thread>

namespace visionpro::tools {
namespace {
using namespace core;

enum class Command { Read, ReadMany, Write, Send, Receive, Join };

template<SchemaIdentity Id> Id key(std::string_view literal)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(literal)); }

DataType record_type(std::string_view name, const std::vector<FieldSchema>& fields)
{ return std::get<DataType>(DataType::record(key<SchemaId>(name), 1, fields)); }

DataType global_entry_type()
{
    return record_type("visionpro.shared.global_entry", {{key<FieldId>("variable_id"), DataType::string()},
        {key<FieldId>("revision"), DataType::string()}, {key<FieldId>("value"), DataType::integer()}});
}

DataType message_type(std::string_view name, const DataType& payload)
{
    const auto string = DataType::string();
    return record_type(name, {{key<FieldId>("message_id"), string}, {key<FieldId>("queue_id"), string},
        {key<FieldId>("sequence"), string}, {key<FieldId>("domain"), string}, {key<FieldId>("key"), string},
        {key<FieldId>("payload"), payload}});
}

std::string_view status_name(CorrelationJoinStatus status)
{
    switch (status) {
    case CorrelationJoinStatus::Pending: return "Pending";
    case CorrelationJoinStatus::Complete: return "Complete";
    case CorrelationJoinStatus::Duplicate: return "Duplicate";
    case CorrelationJoinStatus::Late: return "Late";
    case CorrelationJoinStatus::Capacity: return "Capacity";
    }
    throw std::logic_error("Unrecognized correlation join status.");
}

DataType status_type()
{
    std::vector<EnumMember> members;
    for (const auto status : {CorrelationJoinStatus::Pending, CorrelationJoinStatus::Complete,
            CorrelationJoinStatus::Duplicate, CorrelationJoinStatus::Late, CorrelationJoinStatus::Capacity}) {
        const auto name = status_name(status);
        members.push_back({key<EnumMemberId>(name), std::string(name)});
    }
    return std::get<DataType>(DataType::enumeration(key<SchemaId>("visionpro.shared.join_status"), 1, members));
}

DataType statistics_type()
{
    std::vector<FieldSchema> fields;
    for (auto name : {"batch", "pending", "retained_keys", "completed", "duplicates", "late", "expired", "abandoned_at_rollover",
            "pending_capacity_rejections", "batch_capacity_rejections", "join_capacity_rejections"}) {
        fields.push_back({key<FieldId>(name), DataType::string()});
    }
    return record_type("visionpro.shared.join_statistics", fields);
}

const DataValue& input(const InvocationContextData& call, std::string_view name)
{ return std::ranges::find(call.inputs, name, [](const auto& item) { return item.id.text(); })->value; }

const ConfigValue& parameter(const ToolExecutionConfiguration& config, std::string_view name)
{ return std::ranges::find(config.data().parameters, name, [](const auto& item) { return item.id.text(); })->value; }

DataValue text(std::string_view value) { return std::get<DataValue>(DataValue::string(value)); }

DataValue statistics(const CorrelationJoinSnapshot& stats)
{
    const auto field = [](std::string_view name, std::uint64_t value) {
        return DataFieldValue{key<FieldId>(name), text(std::to_string(value))};
    };
    static const auto type = statistics_type();
    return std::get<DataValue>(DataValue::record(type, {field("batch", stats.batch), field("pending", stats.pending),
        field("retained_keys", stats.retained_keys), field("completed", stats.completed), field("duplicates", stats.duplicates),
        field("late", stats.late), field("expired", stats.expired), field("abandoned_at_rollover", stats.abandoned_at_rollover),
        field("pending_capacity_rejections", stats.pending_capacity_rejections), field("batch_capacity_rejections", stats.batch_capacity_rejections),
        field("join_capacity_rejections", stats.join_capacity_rejections)}));
}

UpdateOrigin invocation_origin(const ToolInvocationIdentity& identity)
{ return SessionInvocationKey{identity.session, identity.invocation}; }

Result<CorrelationId> correlation(const InvocationContextData& call)
{
    const auto domain = SchemaIdFactory::parse<CorrelationDomainId>(*input(call, "domain").as_string());
    if (auto* failure = std::get_if<Diagnostic>(&domain)) {
        auto diagnostic = *failure;
        diagnostic.location.input = key<InputId>("domain");
        return diagnostic;
    }
    auto value = RuntimeIdFactory::correlation(std::get<CorrelationDomainId>(domain), *input(call, "key").as_string());
    if (auto* failure = std::get_if<Diagnostic>(&value)) { failure->location.input = key<InputId>("key"); }
    return value;
}

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

ToolCompletion success(const ToolInvocationIdentity& identity)
{ return {identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}}; }

using Check = Result<std::monostate>;

Check globals(Command command, const InvocationContextData& call, ToolCompletion& result)
{
    if (!call.shared_data) { return error(ErrorCode::MissingCapability, "This tool requires session shared data."); }
    const auto& targets = call.configuration.data().shared_targets;
    if (command == Command::ReadMany) {
        std::vector<VariableId> variables;
        for (const auto& target : targets) { variables.push_back(std::get<VariableId>(target)); }
        const auto read = call.shared_data->read_many(variables);
        if (const auto* failure = std::get_if<Diagnostic>(&read)) { return *failure; }
        const auto type = global_entry_type();
        std::vector<DataValue> entries;
        for (const auto& entry : std::get<std::vector<GlobalVariableEntry>>(read)) {
            auto record = DataValue::record(type, {{key<FieldId>("variable_id"), text(entry.variable.to_string())},
                {key<FieldId>("revision"), text(std::to_string(entry.revision))}, {key<FieldId>("value"), entry.current_value}});
            if (const auto* failure = std::get_if<Diagnostic>(&record)) { return *failure; }
            entries.push_back(std::get<DataValue>(std::move(record)));
        }
        result.outputs = {{key<OutputId>("entries"), std::get<DataValue>(DataValue::list(type, entries))}};
    } else {
        const auto variable = std::get<VariableId>(targets.front());
        const auto entry = command == Command::Read ? call.shared_data->read(variable)
            : call.shared_data->write(variable, input(call, "value"), invocation_origin(call.identity));
        if (const auto* failure = std::get_if<Diagnostic>(&entry)) { return *failure; }
        const auto& read = std::get<GlobalVariableEntry>(entry);
        result.outputs = {{key<OutputId>("value"), read.current_value},
            {key<OutputId>("revision"), text(std::to_string(read.revision))}};
    }
    return std::monostate{};
}

Result<CorrelationJoinConfig> join_config(const ToolExecutionConfiguration& config)
{
    auto identity = SchemaIdFactory::parse<SchemaId>(*parameter(config, "join").as_string());
    if (auto* failure = std::get_if<Diagnostic>(&identity)) {
        failure->location.parameter = key<ParameterId>("join");
        return *failure;
    }
    const auto type = dimensions_type();
    const auto& sources = *parameter(config, "sources").elements();
    std::set<std::string> declared;
    for (const auto& source : sources) { declared.insert(*source.as_string()); }
    if (sources.size() != type.fields().size() || !std::ranges::all_of(type.fields(),
            [&](const auto& field) { return declared.contains(std::string(field.id.text())); })) {
        auto diagnostic = error(ErrorCode::InvalidSchema, "Dimensions requires the two distinct source IDs width and height.");
        diagnostic.location.parameter = key<ParameterId>("sources");
        return diagnostic;
    }
    const auto capacity = *parameter(config, "capacity").as_int64();
    return CorrelationJoinConfig{std::get<SchemaId>(identity), type, static_cast<std::size_t>(capacity),
        std::chrono::milliseconds{*parameter(config, "timeout_ms").as_int64()}};
}

Check offer(const CorrelationJoinConfig& config, const InvocationContextData& call, ToolCompletion& result)
{
    if (!call.correlation_join) { return error(ErrorCode::MissingCapability, "This tool requires a correlation join service."); }
    const auto workpiece = correlation(call);
    if (const auto* failure = std::get_if<Diagnostic>(&workpiece)) { return *failure; }
    auto source = SchemaIdFactory::parse<FieldId>(*input(call, "source").as_string());
    if (auto* failure = std::get_if<Diagnostic>(&source)) {
        failure->location.input = key<InputId>("source");
        return *failure;
    }
    const auto batch = static_cast<std::uint64_t>(*input(call, "batch").as_int64());
    const auto joined = call.correlation_join->offer(config, batch, std::get<CorrelationId>(workpiece),
        std::get<FieldId>(source), input(call, "value"));
    if (const auto* failure = std::get_if<Diagnostic>(&joined)) { return *failure; }
    const auto& outcome = std::get<CorrelationJoinResult>(joined);
    const bool complete = outcome.status == CorrelationJoinStatus::Complete;
    if (complete != outcome.record.has_value()) { return error(ErrorCode::InvalidValue, "Join completion and its record disagree."); }
    result.outputs = {{key<OutputId>("status"), std::get<DataValue>(DataValue::enumeration(status_type(), key<EnumMemberId>(status_name(outcome.status))))},
        {key<OutputId>("record"), complete ? OutputContent{*outcome.record} : OutputContent{NotProduced{OutputAbsenceReason::ConditionNotMet}}},
        {key<OutputId>("statistics"), statistics(outcome.stats)}};
    const auto route = complete ? "ready" : outcome.status == CorrelationJoinStatus::Pending ? "waiting" : "ignored";
    result.selected_control_outputs.push_back(key<ControlOutputId>(route));
    return std::monostate{};
}

Check queue(Command command, const InvocationContextData& call, ToolCompletion& result, std::stop_token stop)
{
    const auto& targets = call.configuration.data().shared_targets;
    if (!call.shared_data) { return error(ErrorCode::MissingCapability, "This tool requires session shared data."); }
    const auto origin = invocation_origin(call.identity);
    if (command == Command::Send) {
        const auto workpiece = correlation(call);
        if (const auto* failure = std::get_if<Diagnostic>(&workpiece)) { return *failure; }
        auto payload = input(call, "payload");
        if (payload.as_string()) {
            auto record = DataValue::record(call.configuration.data().tool.shared_target->type, {{key<FieldId>("text"), payload}});
            if (const auto* failure = std::get_if<Diagnostic>(&record)) { return *failure; }
            payload = std::get<DataValue>(std::move(record));
        }
        const auto sent = call.shared_data->send(std::get<QueueId>(targets.front()), payload,
            std::get<CorrelationId>(workpiece), origin, stop);
        if (const auto* failure = std::get_if<Diagnostic>(&sent)) { return *failure; }
        const auto& message = std::get<QueueMessage>(sent);
        result.outputs = {{key<OutputId>("sequence"), text(std::to_string(message.sequence))},
            {key<OutputId>("message_id"), text(message.id.to_string())}};
        return std::monostate{};
    }
    const auto count = static_cast<std::size_t>(*parameter(call.configuration, "count").as_int64());
    const auto& descriptor = call.configuration.data().tool;
    const auto& messages = *std::ranges::find(descriptor.outputs, key<OutputId>("messages"), &OutputDescriptor::id);
    const auto& envelope = *messages.type.element_type();
    std::vector<QueueRead> requests;
    for (const auto& target : targets) {
        requests.push_back({std::get<QueueId>(target), count, descriptor.shared_target->type});
    }
    const auto received = call.shared_data->receive_many(requests, origin, stop);
    if (const auto* failure = std::get_if<Diagnostic>(&received)) { return *failure; }
    std::vector<DataValue> items;
    for (const auto& message : std::get<std::vector<QueueMessage>>(received)) {
        auto record = DataValue::record(envelope, {{key<FieldId>("payload"), message.payload},
            {key<FieldId>("message_id"), text(message.id.to_string())}, {key<FieldId>("queue_id"), text(message.queue.to_string())},
            {key<FieldId>("sequence"), text(std::to_string(message.sequence))}, {key<FieldId>("domain"), text(message.correlation.domain().text())},
            {key<FieldId>("key"), text(message.correlation.key())}});
        if (const auto* failure = std::get_if<Diagnostic>(&record)) { return *failure; }
        items.push_back(std::get<DataValue>(std::move(record)));
    }
    result.outputs = {{key<OutputId>("count"), DataValue::integer(static_cast<std::int64_t>(items.size()))},
        {messages.id, std::get<DataValue>(DataValue::list(envelope, items))}};
    return std::monostate{};
}

class QueueControl final : public InvocationControl {
public:
    void request_cancel() override { stop_.request_stop(); }
    std::stop_token token() const { return stop_.get_token(); }
private:
    std::stop_source stop_;
};

void finish_queue(Command command, std::shared_ptr<const InvocationContext> context, InvocationCompletion completion, std::stop_token stop)
{
    auto result = success(context->data().identity);
    try {
        auto checked = queue(command, context->data(), result, stop);
        if (auto* failure = std::get_if<Diagnostic>(&checked)) {
            result.status = failure->code == ErrorCode::SourceCancelled ? ToolCompletionStatus::Cancelled : ToolCompletionStatus::Failed;
            result.diagnostics = {*failure};
            result.outputs.clear();
        }
    } catch (...) {
        result.status = ToolCompletionStatus::Failed;
        result.diagnostics = {error(ErrorCode::ExecutionBoundaryFailure, "Queue service failed during invocation.")};
        result.outputs.clear();
    }
    context.reset();
    if (auto reported = completion.complete(result); auto* failure = std::get_if<Diagnostic>(&reported)) {
        result.status = ToolCompletionStatus::Failed;
        result.outputs.clear();
        result.diagnostics = {*failure};
        static_cast<void>(completion.complete(result));
    }
    static_cast<void>(completion.mark_exited());
}

class SharedTool final : public ToolInstance {
public:
    SharedTool(const ToolExecutionConfiguration& config, Command command, std::optional<CorrelationJoinConfig> join)
        : ToolInstance(config), command_(command), join_(std::move(join)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        if (command_ != Command::Send && command_ != Command::Receive) {
            auto result = success(context->data().identity);
            const auto checked = join_ ? offer(*join_, context->data(), result) : globals(command_, context->data(), result);
            if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
            return InvocationStart{std::move(result)};
        }
        auto control = std::make_shared<QueueControl>();
        const auto registered = completion.register_control(control);
        if (const auto* failure = std::get_if<Diagnostic>(&registered)) { return *failure; }
        try {
            // A waiting queue operation owns the context and exit token; the engine worker remains available.
            std::jthread worker(finish_queue, command_, context, completion, control->token());
            try { worker.detach(); } catch (...) { control->request_cancel(); }
        } catch (...) {
            auto failed = success(context->data().identity);
            failed.status = ToolCompletionStatus::Failed;
            failed.diagnostics = {error(ErrorCode::ExecutionBoundaryFailure, "Could not start the queue operation.")};
            static_cast<void>(completion.complete(failed));
            static_cast<void>(completion.mark_exited());
        }
        return InvocationStart{AsynchronousInvocation{std::move(control)}};
    }

    Command command_;
    std::optional<CorrelationJoinConfig> join_;
};

class SharedFactory final : public ToolInstanceFactory {
public:
    explicit SharedFactory(Command command) : command_(command) {}
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    {
        std::optional<CorrelationJoinConfig> join;
        if (command_ == Command::Join) {
            auto parsed = join_config(config);
            if (auto* failure = std::get_if<Diagnostic>(&parsed)) { return *failure; }
            join = std::get<CorrelationJoinConfig>(std::move(parsed));
        }
        return std::shared_ptr<ToolInstance>{std::make_shared<SharedTool>(config, command_, std::move(join))};
    }
private:
    Command command_;
};

InputDescriptor port(std::string_view name, const DataType& type)
{
    InputDescriptor result{key<InputId>(name), std::string(name), type, true, {}, {}, {}, {}};
    result.binding.allowed_sources = {BindingSourceKind::Constant, BindingSourceKind::RunOutput, BindingSourceKind::PublicInput, BindingSourceKind::GlobalRead};
    return result;
}

ParameterDescriptor fixed(std::string_view name, const DataType& type, std::optional<ConfigValue> value = {})
{
    ParameterDescriptor result{key<ParameterId>(name), std::string(name), type, !value, std::move(value), {}, {}, {}};
    result.binding.time = BindingTime::SnapshotBuild;
    return result;
}

ParameterDescriptor positive(std::string_view name, std::int64_t initial)
{
    auto value = fixed(name, DataType::integer(), ConfigValue::integer(initial));
    value.constraints.integer_range = IntegerRange{1, std::numeric_limits<std::int64_t>::max()};
    return value;
}

OutputDescriptor output(std::string_view name, const DataType& type)
{ return {key<OutputId>(name), std::string(name), type, {}}; }

ToolDescriptor descriptor(std::string_view id, std::string name)
{
    return {key<ToolTypeId>(id), std::move(name), 1, {key<SchemaId>(id), 1, {}}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

ToolRegistration registration(ToolDescriptor tool, Command command)
{
    const auto schema = tool.schema.id;
    if (command != Command::Read && command != Command::ReadMany) { tool.execution.effects = ToolEffects::External; }
    if (command == Command::Send || command == Command::Receive) {
        tool.execution.mode = ToolExecutionMode::Asynchronous;
        tool.execution.cancellation = ToolCancellation::Cooperative;
    }
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<SharedFactory>(command)};
}
} // namespace

core::DataType dimensions_type()
{
    return record_type("visionpro.shared.dimensions", {{key<FieldId>("width"), DataType::integer()}, {key<FieldId>("height"), DataType::integer()}});
}

core::DataType measurement_fragment_type()
{
    return record_type("visionpro.shared.fragment", {{key<FieldId>("batch"), DataType::integer()},
        {key<FieldId>("source"), DataType::string()}, {key<FieldId>("value"), DataType::integer()}});
}

std::vector<core::ToolRegistration> shared_registrations()
{
    const auto integer = DataType::integer();
    const auto string = DataType::string();
    const auto dimensions = dimensions_type();
    const auto fragment = measurement_fragment_type();
    auto reader = descriptor("visionpro.shared.global_read_int64", "Read shared integer");
    reader.shared_target = SharedTargetRequirement{SharedTargetKind::Variable, integer, false};
    reader.outputs = {output("value", integer), output("revision", string)};
    auto writer = descriptor("visionpro.shared.global_write_int64", "Write shared integer");
    writer.shared_target = reader.shared_target;
    writer.inputs = {port("value", integer)};
    writer.outputs = reader.outputs;
    auto many = descriptor("visionpro.shared.global_read_many_int64", "Read shared integers");
    many.shared_target = SharedTargetRequirement{SharedTargetKind::Variable, integer, true};
    many.outputs = {output("entries", DataType::list(global_entry_type()))};
    auto join = descriptor("visionpro.shared.join_dimensions", "Join workpiece dimensions");
    join.requires_correlation_join = true;
    const auto sources = std::get<ConfigValue>(ConfigValue::list(string, {
        std::get<ConfigValue>(ConfigValue::string("width")), std::get<ConfigValue>(ConfigValue::string("height"))}));
    join.parameters = {fixed("join", string), fixed("sources", sources.type(), sources), positive("capacity", 256), positive("timeout_ms", 30000)};
    auto batch = port("batch", integer);
    batch.constraints.integer_range = IntegerRange{1, std::numeric_limits<std::int64_t>::max()};
    join.inputs = {std::move(batch), port("domain", string), port("key", string), port("source", string), port("value", integer)};
    auto record = output("record", dimensions);
    record.required = false;
    record.allowed_absence = {OutputAbsenceReason::ConditionNotMet};
    join.outputs = {output("status", status_type()), std::move(record), output("statistics", statistics_type())};
    join.control_outputs = {{key<ControlOutputId>("ready"), "Ready"}, {key<ControlOutputId>("waiting"), "Waiting"},
        {key<ControlOutputId>("ignored"), "Ignored"}};
    std::vector result{registration(std::move(reader), Command::Read), registration(std::move(writer), Command::Write),
        registration(std::move(many), Command::ReadMany), registration(std::move(join), Command::Join)};
    const auto add_queue_tools = [&](std::string schema, const DataType& type, const DataType& payload_input) {
        auto send = descriptor("visionpro.shared.queue_send_" + schema, "Send " + schema);
        send.shared_target = SharedTargetRequirement{SharedTargetKind::Queue, type, false};
        send.inputs = {port("payload", payload_input), port("domain", string), port("key", string)};
        send.outputs = {output("sequence", string), output("message_id", string)};
        auto receive = descriptor("visionpro.shared.queue_receive_" + schema, "Receive " + schema);
        receive.shared_target = SharedTargetRequirement{SharedTargetKind::Queue, type, true};
        receive.parameters = {positive("count", 1)};
        const auto envelope = message_type("visionpro.shared." + schema + "_message", type);
        receive.outputs = {output("count", integer), output("messages", DataType::list(envelope))};
        result.push_back(registration(std::move(send), Command::Send));
        result.push_back(registration(std::move(receive), Command::Receive));
    };
    add_queue_tools("fragment", fragment, fragment);
    add_queue_tools("dimensions", dimensions, dimensions);
    add_queue_tools("text", record_type("visionpro.shared.text", {{key<FieldId>("text"), string}}), string);
    return result;
}
} // namespace visionpro::tools
