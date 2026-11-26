#include <visionpro/workflow/trigger_control.hpp>
#include <visionpro/workflow/validation.hpp>
#include "runtime_values.hpp"

#include <visionpro/core/runtime_id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace visionpro::workflow {
namespace {
using namespace core;

Diagnostic failure(ErrorCode code, WorkflowId workflow, std::string message)
{
    DiagnosticLocation location;
    location.workflow = workflow;
    return {code, Severity::Error, std::move(location), std::move(message)};
}

std::optional<TriggerControl::Clock::time_point> next_tick(TriggerControl::Clock::time_point now,
    std::uint32_t interval_ms)
{
    const auto interval = std::chrono::milliseconds(interval_ms);
    if (now > TriggerControl::Clock::time_point::max() - interval) { return {}; }
    return now + interval;
}

// Entry filters can be decided before admission when both values are already available.
// Bindings produced inside the run are evaluated by the node in its execution scope.
std::optional<DataValue> event_parameter(const Node& node, const ParameterDescriptor& parameter,
    const RunRequest& request)
{
    const auto binding = std::ranges::find(node.bindings, BindingTarget{parameter.id}, &Binding::target);
    const auto convert = [](const ConfigValue& value) -> std::optional<DataValue> {
        auto converted = detail::materialize_config_value(value);
        if (const auto* data = std::get_if<DataValue>(&converted)) { return *data; }
        return {};
    };
    if (binding == node.bindings.end()) { return parameter.default_value ? convert(*parameter.default_value) : std::nullopt; }
    if (binding->sources.size() != 1) { return {}; }
    const auto& source = binding->sources.front();
    std::optional<DataValue> result;
    if (const auto* fixed = std::get_if<ConfigValue>(&source)) { result = convert(*fixed); }
    if (const auto* input = std::get_if<ScopedInputReference>(&source); input && !input->group && input->selector.empty()) {
        if (const auto* value = request.input(input->input)) { result = value->value; }
    }
    if (const auto* global = std::get_if<GlobalReference>(&source); global && global->selector.empty()) {
        const auto entries = request.globals();
        const auto found = std::ranges::find(entries, global->variable, &GlobalVariableEntry::variable);
        if (found != entries.end() && (!global->required_revision || found->revision == *global->required_revision)) {
            result = found->current_value;
        }
    }
    if (result && binding->conversion == ConversionKind::WrapOptional) { result = DataValue::some(*result); }
    return result && result->type() == parameter.type ? result : std::nullopt;
}

bool entry_filtered(const Node& node, const ToolDescriptor& descriptor, const RunRequest& request)
{
    std::vector<FixedInvocationParameter> values;
    for (const auto& id : {descriptor.message_filter->input, descriptor.message_filter->expected}) {
        const auto parameter = std::ranges::find(descriptor.parameters, id, &ParameterDescriptor::id);
        if (parameter == descriptor.parameters.end()) { return false; }
        auto value = event_parameter(node, *parameter, request);
        if (!value) { return false; }
        values.push_back({id, std::move(*value)});
    }
    return message_filtered(descriptor, values);
}
} // namespace

struct TriggerControl::Impl {
    Impl(const std::shared_ptr<WorkflowSession>& owner, SolutionSessionId identity, WorkflowId flow,
        WorkflowTrigger entry, CorrelationDomainId domain, Clock::time_point now)
        : session(owner), session_id(identity), workflow(flow), entry(std::move(entry)), domain(std::move(domain)),
          due(next_tick(now, this->entry.interval_ms)) {}

    std::weak_ptr<WorkflowSession> session;
    const SolutionSessionId session_id;
    const WorkflowId workflow;
    const WorkflowTrigger entry;
    const CorrelationDomainId domain;
    std::optional<DeviceResource> camera_device{};
    std::optional<SnapshotIdentity> pinned;
    std::optional<InputId> sdk_input;
    std::optional<std::int64_t> accepted_delivery;
    bool serial_ticks{};
    bool tick_in_progress{};
    std::mutex mutex;
    std::condition_variable idle;
    bool accepting{true};
    std::size_t preparing{};
    std::int64_t last_frame{};
    std::optional<Clock::time_point> due;
    std::vector<RunHandle> accepted;

    RunRejected stopped() const
    { return {failure(ErrorCode::InvalidState, workflow, "The trigger subscription has stopped.")}; }

    bool same_device(const ResourceDefinition& resource) const
    {
        const auto* device = std::get_if<DeviceResource>(&resource.location);
        return camera_device && device && resource.kind == ResourceKind::Device && resource.id == entry.camera->device
            && camera_device->adapter == device->adapter && camera_device->configuration == device->configuration;
    }

    void prune_quiescent()
    {
        std::erase_if(accepted, [this](const RunHandle& run) {
            const auto inspected = run.inspect();
            const auto* progress = std::get_if<RunProgress>(&inspected);
            if (progress && progress->end_continuous && entry.mode == TriggerMode::Continuous) { accepting = false; }
            return progress && progress->quiescent;
        });
    }

    RunSubmission submit(const RunTrigger& trigger, std::int64_t frame, const std::optional<CorrelationId>& correlation,
        const ImageFrame* image, std::span<const RunInput> supplied)
    try
    {
        const auto owner = session.lock();
        if (!owner) { return stopped(); }
        const auto current = owner->inspect();
        if (current.status != WorkflowSessionStatus::Active || !current.active
            || current.active->identity().session != session_id) {
            return RunRejected{failure(ErrorCode::ResultIdentityMismatch, workflow, "The trigger's solution session is no longer active.")};
        }
        const auto* flow = find_workflow(current.active->solution(), workflow);
        if (!flow || !flow->enabled || (pinned ? current.active->identity() != *pinned : flow->trigger != entry)) {
            return RunRejected{failure(ErrorCode::InvalidRunRequest, workflow, "The workflow's configured trigger entry has changed.")};
        }
        if ((entry.camera.has_value() || sdk_input.has_value()) != (image != nullptr)) {
            return RunRejected{failure(ErrorCode::InvalidRunRequest, workflow, "Camera trigger entries require a captured image and frame number together.")};
        }
        std::vector<RunInput> inputs{supplied.begin(), supplied.end()};
        if (!pinned) { inputs.push_back({entry.frame_input, DataValue::integer(frame)}); }
        if (entry.camera) {
            const auto& catalog = current.active->solution().resources;
            const auto resource = std::ranges::find(catalog, entry.camera->device, &ResourceDefinition::id);
            if (resource == catalog.end() || !same_device(*resource)) {
                return RunRejected{failure(ErrorCode::ResourceVersionMismatch, workflow, "The armed camera configuration changed. Reopen its stream.")};
            }
        }
        if (image) {
            auto value = DataValue::image(*image);
            if (auto* error = std::get_if<Diagnostic>(&value)) { return RunRejected{std::move(*error)}; }
            if (sdk_input) {
                const auto input = std::ranges::find(flow->interface.inputs, *sdk_input, &InputDescriptor::id);
                auto checked = detail::validate_runtime_domain(std::get<DataValue>(value), current.active->solution(), input->semantics);
                if (auto* error = std::get_if<Diagnostic>(&checked)) { return RunRejected{*error}; }
                for (const auto& node : flow->nodes) {
                    if (!node.enabled) { continue; }
                    const auto* descriptor = current.active->find_tool(node);
                    if (!descriptor || descriptor->image_formats.empty()) { continue; }
                    for (const auto& binding : node.bindings) {
                        const auto* target = std::get_if<InputId>(&binding.target);
                        if (!target || binding.sources.size() != 1) { continue; }
                        const auto port = std::ranges::find(descriptor->inputs, *target, &InputDescriptor::id);
                        if (port == descriptor->inputs.end() || port->type != DataType::image()) { continue; }
                        const auto* ref = std::get_if<ScopedInputReference>(&binding.sources.front());
                        if (ref && !ref->group && ref->workflow == workflow && ref->input == *sdk_input) {
                            auto format = validate_image_support(*descriptor, image->image);
                            if (auto* error = std::get_if<Diagnostic>(&format)) { return RunRejected{*error}; }
                        }
                    }
                }
            }
            inputs.push_back({sdk_input ? *sdk_input : entry.camera->image_input, std::get<DataValue>(std::move(value))});
        }
        auto request = RunRequest::create(*current.active, WorkflowTarget{workflow}, inputs, trigger, correlation);
        if (auto* error = std::get_if<Diagnostic>(&request)) { return RunRejected{std::move(*error)}; }
        auto& fixed = std::get<RunRequest>(request);
        std::vector<const Node*> roots;
        for (const auto& id : flow->entries) {
            if (const auto* node = find_node(*flow, id); node && node->enabled) { roots.push_back(node); }
        }
        if (!roots.empty() && std::ranges::all_of(roots, [&](const Node* node) {
                const auto* descriptor = current.active->find_tool(*node);
                return descriptor && descriptor->message_filter;
            })) {
            if (!current.active->solution().variables.empty()) {
                auto view = owner->shared_preview();
                if (const auto* bad = std::get_if<Diagnostic>(&view)) { return RunRejected{*bad}; }
                auto values = std::get<std::shared_ptr<const SharedData>>(view)->read_all();
                if (const auto* bad = std::get_if<Diagnostic>(&values)) { return RunRejected{*bad}; }
                auto captured = fixed.with_global_snapshot(std::get<std::vector<GlobalVariableEntry>>(values));
                if (const auto* bad = std::get_if<Diagnostic>(&captured)) { return RunRejected{*bad}; }
                fixed = std::get<RunRequest>(std::move(captured));
            }
            if (std::ranges::all_of(roots, [&](const Node* node) {
                    return entry_filtered(*node, *current.active->find_tool(*node), fixed);
                })) {
                return RunRejected{failure(ErrorCode::InputUnavailable, workflow, "Image source message did not match the configured entry filter.")};
            }
        }

        // Share a single ordering point with stop, without holding the subscription
        // lock across file/provider preflight. An event still preparing at stop cannot enter a slot.
        const WorkflowSession::AdmissionGate gate = [&](const auto& admit) -> RunSubmission {
            const std::lock_guard lock(mutex);
            if (!accepting) { return stopped(); }
            if (sdk_input && accepted_delivery && frame <= *accepted_delivery) {
                return RunRejected{failure(ErrorCode::DuplicateIdentity, workflow, "SDK delivery was already accepted or is older than the last delivery.")};
            }
            if (accepted.size() == accepted.capacity()) {
                prune_quiescent();
                if (accepted.size() == accepted.capacity()) {
                    accepted.reserve(accepted.empty() ? 1 : 2 * accepted.size());
                }
            }
            auto result = admit();
            if (const auto* handle = std::get_if<RunHandle>(&result)) {
                accepted.push_back(*handle);
                if (sdk_input) { accepted_delivery = frame; }
            }
            return result;
        };
        return owner->submit_guarded(std::get<RunRequest>(request), &gate);
    }
    catch (...) {
        return RunRejected{failure(ErrorCode::ExecutionBoundaryFailure, workflow, "Could not prepare the trigger event.")};
    }

    TriggerReceipt dispatch(TriggerKind kind, std::int64_t frame, std::optional<CorrelationId> correlation,
        const ImageFrame* image = nullptr, std::span<const RunInput> inputs = {}, std::vector<DirectoryChange> changes = {})
    {
        const auto published_kind = !pinned ? kind : (sdk_input ? TriggerKind::Sdk :
            (entry.mode == TriggerMode::Continuous ? TriggerKind::ImageSequence : TriggerKind::ImageDirectory));
        const RunTrigger trigger{published_kind, entry.source, std::move(changes)};
        if (!correlation) {
            auto key = RuntimeIdFactory::correlation(domain, std::to_string(frame));
            if (auto* error = std::get_if<Diagnostic>(&key)) {
                return {trigger, entry.frame_input, frame, {}, RunRejected{std::move(*error)}};
            }
            correlation = std::get<CorrelationId>(std::move(key));
        }
        {
            const std::lock_guard lock(mutex);
            if (!accepting) { return {trigger, entry.frame_input, frame, correlation, stopped()}; }
            if ((kind == TriggerKind::External) != (entry.mode == TriggerMode::External)) {
                return {trigger, entry.frame_input, frame, correlation,
                    RunRejected{failure(ErrorCode::InvalidRunRequest, workflow, "Event kind does not match the trigger entry.")}};
            }
            ++preparing;
        }
        struct Finish {
            Impl& self;
            ~Finish() {
                const std::lock_guard lock(self.mutex);
                if (--self.preparing == 0) { self.idle.notify_all(); }
            }
        } finish{*this};
        return {trigger, entry.frame_input, frame, correlation, submit(trigger, frame, correlation, image, inputs)};
    }
};

TriggerControl::TriggerControl(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TriggerControl::~TriggerControl() = default;

Result<std::shared_ptr<TriggerControl>> TriggerControl::start(std::shared_ptr<WorkflowSession> session,
    WorkflowId workflow, Clock::time_point now)
{
    if (!session) { return failure(ErrorCode::InvalidState, workflow, "Starting a trigger requires a session."); }
    const auto current = session->inspect();
    if (current.status != WorkflowSessionStatus::Active || !current.active) {
        return failure(ErrorCode::InvalidState, workflow, "Starting a trigger requires an active solution.");
    }
    const auto* flow = find_workflow(current.active->solution(), workflow);
    if (!flow || !flow->enabled || !flow->trigger) {
        return failure(ErrorCode::InvalidRunRequest, workflow, "Starting requires an enabled workflow with a trigger entry.");
    }
    auto validated = validate_workflow_trigger(*flow);
    if (auto* error = std::get_if<Diagnostic>(&validated)) { return std::move(*error); }
    auto domain = SchemaIdFactory::parse<CorrelationDomainId>(flow->trigger->source.text());
    if (auto* error = std::get_if<Diagnostic>(&domain)) { return std::move(*error); }
    auto impl = std::make_unique<Impl>(session, current.active->identity().session, workflow, *flow->trigger,
        std::get<CorrelationDomainId>(domain), now);
    impl->serial_ticks = std::ranges::any_of(flow->nodes, [&](const Node& node) {
        const auto* descriptor = current.active->find_tool(node);
        return node.enabled && descriptor && descriptor->may_end_continuous;
    });
    if (const auto& binding = flow->trigger->camera) {
        auto exclusive = validate_camera_trigger_ownership(current.active->solution(), *flow, current.active->tools());
        if (auto* bad = std::get_if<Diagnostic>(&exclusive)) { return *bad; }
        const auto& catalog = current.active->solution().resources;
        const auto resource = std::ranges::find(catalog, binding->device, &ResourceDefinition::id);
        if (resource == catalog.end() || !std::holds_alternative<DeviceResource>(resource->location)) {
            return failure(ErrorCode::InvalidResource, workflow, "Cannot arm a missing or non-device camera resource.");
        }
        impl->camera_device = std::get<DeviceResource>(resource->location);
    }
    return std::shared_ptr<TriggerControl>(new TriggerControl(std::move(impl)));
}

bool TriggerControl::matches_camera(WorkflowId workflow, const WorkflowTrigger& entry, const ResourceDefinition& device) const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->accepting && workflow == impl_->workflow && entry == impl_->entry && impl_->same_device(device);
}

Result<std::shared_ptr<TriggerControl>> TriggerControl::start_image_entry(std::shared_ptr<WorkflowSession> session,
    const SnapshotIdentity& target, WorkflowId workflow, SchemaId source, std::optional<InputId> image_input,
    std::uint32_t interval_ms, Clock::time_point now)
{
    if (!session) { return failure(ErrorCode::InvalidState, workflow, "Image source requires an active session."); }
    const auto current = session->inspect();
    if (!current.active || current.status != WorkflowSessionStatus::Active || current.active->identity() != target) {
        return failure(ErrorCode::ResultIdentityMismatch, workflow, "Image source belongs to an inactive configuration.");
    }
    const auto* flow = find_workflow(current.active->solution(), workflow);
    if (!flow || !flow->enabled) { return failure(ErrorCode::InvalidRunRequest, workflow, "Image source workflow is absent or disabled."); }
    if (image_input) {
        const auto input = std::ranges::find(flow->interface.inputs, *image_input, &InputDescriptor::id);
        if (input == flow->interface.inputs.end() || input->type != DataType::image()) {
            return failure(ErrorCode::InvalidRunRequest, workflow, "SDK delivery requires a declared public Image input.");
        }
    }
    auto domain = SchemaIdFactory::parse<CorrelationDomainId>(source.text());
    if (auto* error = std::get_if<Diagnostic>(&domain)) { return *error; }
    WorkflowTrigger entry{interval_ms ? TriggerMode::Continuous : TriggerMode::External, source,
        image_input.value_or(std::get<InputId>(SchemaIdFactory::parse<InputId>("delivery"))), interval_ms};
    auto impl = std::make_unique<Impl>(session, target.session, workflow, std::move(entry), std::get<CorrelationDomainId>(domain), now);
    impl->pinned = target;
    impl->sdk_input = image_input;
    impl->serial_ticks = interval_ms != 0;
    return std::shared_ptr<TriggerControl>(new TriggerControl(std::move(impl)));
}

Result<std::shared_ptr<TriggerControl>> TriggerControl::start_sdk(std::shared_ptr<WorkflowSession> session,
    const SnapshotIdentity& target, WorkflowId workflow, InputId image_input, SchemaId sender)
{
    auto subscription = start_image_entry(session, target, workflow, sender, image_input, 0, Clock::now());
    if (const auto* active = std::get_if<std::shared_ptr<TriggerControl>>(&subscription)) {
        auto bound = session->register_sdk_sender(target, workflow, image_input, sender, *active);
        if (const auto* bad = std::get_if<Diagnostic>(&bound)) { return *bad; }
    }
    return subscription;
}

Result<std::shared_ptr<TriggerControl>> TriggerControl::start_local(std::shared_ptr<WorkflowSession> session,
    const SnapshotIdentity& target, WorkflowId workflow, std::uint32_t interval_ms, Clock::time_point now)
{
    auto source = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>(interval_ms ? "image.sequence" : "image.directory"));
    return start_image_entry(std::move(session), target, workflow, std::move(source), {}, interval_ms, now);
}

TriggerReceipt TriggerControl::sdk_frame(std::int64_t delivery, const ImageFrame& image,
    std::optional<CorrelationId> correlation, std::span<const RunInput> inputs)
{
    if (!impl_->sdk_input || delivery < 0) {
        return {{TriggerKind::Sdk, impl_->entry.source}, impl_->entry.frame_input, delivery, correlation,
            RunRejected{failure(ErrorCode::InvalidRunRequest, impl_->workflow, "SDK delivery needs an SDK subscription and a nonnegative sequence number.")}};
    }
    return impl_->dispatch(TriggerKind::External, delivery, std::move(correlation), &image, inputs);
}

bool TriggerControl::accepting() const
{
    const std::lock_guard lock(impl_->mutex);
    return impl_->accepting;
}

TriggerReceipt TriggerControl::sdk_frame(std::int64_t delivery, const ImageLayout& layout,
    std::span<const std::byte> pixels, CoordinateSystemId coordinates, std::optional<FrameId> frame,
    std::optional<std::chrono::sys_time<std::chrono::nanoseconds>> captured_at,
    std::optional<CorrelationId> correlation, std::span<const RunInput> inputs)
{
    auto copied = ImageHandle::copy(layout, pixels, std::move(coordinates));
    if (auto* bad = std::get_if<Diagnostic>(&copied)) {
        return {{TriggerKind::Sdk, impl_->entry.source}, impl_->entry.frame_input, delivery, correlation, RunRejected{std::move(*bad)}};
    }
    return sdk_frame(delivery, ImageFrame{std::get<ImageHandle>(std::move(copied)), frame, captured_at},
        std::move(correlation), inputs);
}

TriggerReceipt TriggerControl::directory_changed(std::int64_t batch, std::vector<DirectoryChange> changes)
{
    return impl_->dispatch(TriggerKind::External, batch, {}, nullptr, {}, std::move(changes));
}
TriggerReceipt TriggerControl::external(std::int64_t frame, std::optional<CorrelationId> correlation)
{ return impl_->dispatch(TriggerKind::External, frame, std::move(correlation)); }

TriggerReceipt TriggerControl::camera_frame(std::int64_t number, const ImageFrame& image, CorrelationId correlation)
{ return impl_->dispatch(TriggerKind::External, number, std::move(correlation), &image); }

std::optional<TriggerReceipt> TriggerControl::tick(Clock::time_point now)
{
    std::int64_t frame;
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting || impl_->entry.mode != TriggerMode::Continuous || !impl_->due || now < *impl_->due) {
            return {};
        }
        if (impl_->serial_ticks) {
            impl_->prune_quiescent();
            if (!impl_->accepting || impl_->tick_in_progress || !impl_->accepted.empty()) { return {}; }
        }
        impl_->tick_in_progress = true;
        frame = ++impl_->last_frame;
        impl_->due = frame == std::numeric_limits<std::int64_t>::max() ? std::nullopt : next_tick(now, impl_->entry.interval_ms);
    }
    auto receipt = impl_->dispatch(TriggerKind::Continuous, frame, {});
    const std::lock_guard lock(impl_->mutex);
    impl_->tick_in_progress = false;
    return receipt;
}

void TriggerControl::disarm()
{
    const std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
}

Result<RunWaitOutcome> TriggerControl::stop(TriggerStopMode mode, Clock::time_point deadline)
{
    if (mode != TriggerStopMode::Drain && mode != TriggerStopMode::Cancel) {
        return failure(ErrorCode::InvalidEnumValue, impl_->workflow, "Unknown trigger stop mode.");
    }
    std::vector<RunHandle> runs;
    {
        const std::lock_guard lock(impl_->mutex);
        impl_->accepting = false;
        runs = impl_->accepted;
    }
    if (mode == TriggerStopMode::Cancel) {
        // Request cancellation of later FIFO entries before an active run can release its slot.
        std::optional<Diagnostic> first_error;
        for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
            auto cancelled = it->cancel();
            if (auto* error = std::get_if<Diagnostic>(&cancelled); error && !first_error) { first_error = std::move(*error); }
        }
        if (first_error) { return std::move(*first_error); }
    }
    for (const auto& run : runs) {
        auto waited = run.wait(RunWaitTarget::Quiescence, deadline);
        if (auto* error = std::get_if<Diagnostic>(&waited)) { return std::move(*error); }
        if (std::get<RunWaitResult>(waited).outcome == RunWaitOutcome::DeadlineExpired) { return RunWaitOutcome::DeadlineExpired; }
    }
    std::unique_lock lock(impl_->mutex);
    impl_->accepted.clear(); // Every admitted handle reached physical exit.
    if (!impl_->idle.wait_until(lock, deadline, [&] { return impl_->preparing == 0; })) { return RunWaitOutcome::DeadlineExpired; }
    return RunWaitOutcome::Satisfied;
}

} // namespace visionpro::workflow
