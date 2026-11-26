#include "image_source.hpp"
#include "image_fixtures.hpp"
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/core/shared_data.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/infrastructure/solution_file.hpp>
#include <visionpro/workflow/workflow_session.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::workflow;
using namespace visionpro::infrastructure;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
template<class T> T take(Result<T> value) {
    if (auto* issue = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(issue->message); }
    return std::get<T>(std::move(value));
}
template<class T> T key(std::string_view name) { return take(SchemaIdFactory::parse<T>(name)); }
template<class T> T fresh() { return take(IdFactory::generate<T>()); }
void check(bool passed, const char* label) { if (!passed) { throw std::runtime_error(label); } }

struct Fixture {
    const ToolRegistration tool = visionpro::tools::image_source_registration();
    const ToolRegistry registry = take(ToolRegistry::create(std::span{&tool, 1}));
    const CoordinateSystemId raw = key<CoordinateSystemId>("raw"), joined = key<CoordinateSystemId>("joined");
    const NodeId id = fresh<NodeId>();
    const WorkflowId fid = fresh<WorkflowId>();
    Node node{id, "Source", ToolConfiguration{tool.descriptor.id, 1}};
    Workflow flow{fid, "Acquire", {key<SchemaId>("test.flow"),1}, {}, {}, {}, {id}};
    std::vector<ResourceDefinition> resources;
    std::vector<GlobalVariableDefinition> variables;
    void set(std::string_view field, ConfigValue value) { node.bindings.push_back({key<ParameterId>(field), {std::move(value)}}); }
    void select(std::string_view field, std::string_view name) {
        const auto item = std::ranges::find(tool.descriptor.parameters, key<ParameterId>(field), &ParameterDescriptor::id);
        set(field, take(ConfigValue::enumeration(item->type, key<EnumMemberId>(name))));
    }
    void image_input() {
        auto input = tool.descriptor.inputs.front();
        input.binding = {};
        flow.interface.inputs.push_back(input);
        node.bindings.push_back({input.id, {ScopedInputReference{fid, {}, input.id, {}}}});
    }
    void coordinate(std::string_view field, CoordinateSystemId frame) {
        set(field, take(ConfigValue::point2d({frame, MeasurementUnit::Pixel, 0, 0})));
    }
    std::shared_ptr<WorkflowSession> activate(const ToolRegistry* available = nullptr, std::vector<Workflow> other_flows = {}) {
        const auto frame = [](CoordinateSystemId name) {
            return CoordinateFrameDefinition{name, CoordinateKind::Image, MeasurementUnit::Pixel, {0,0},{1,0},{0,-1},Handedness::LeftHanded};
        };
        flow.nodes.push_back(node);
        other_flows.push_back(flow);
        Solution solution{fresh<SolutionId>(), "Image source test", 1, 1, std::move(other_flows), resources, variables, {}, {frame(raw), frame(joined)}, {}};
        const auto provider = take(FileResourceProvider::create(solution, {}));
        auto session = take(WorkflowSession::create(available ? *available : registry, provider));
        take(session->activate(solution));
        return session;
    }
    NodeResult run(WorkflowSession& session, const std::vector<RunInput>& inputs = {}) const {
        auto request = take(RunRequest::create(*session.inspect().active, WorkflowTarget{fid}, inputs,
            {TriggerKind::Manual, key<SchemaId>("test")}));
        auto submitted = session.submit(request);
        if (const auto* bad = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(bad->diagnostic.message); }
        auto handle = std::get<RunHandle>(submitted);
        auto waited = take(handle.wait(RunWaitTarget::Quiescence, std::chrono::steady_clock::now() + 5s));
        if (waited.progress.status != ExecutionStatus::Succeeded && !waited.progress.diagnostics.empty()) {
            throw std::runtime_error(waited.progress.diagnostics.front().message);
        }
        check(waited.progress.status == ExecutionStatus::Succeeded, "Image source invocation failed");
        return *take(handle.lookup(*waited.progress.nodes.front().invocation));
    }
};
const DataValue& output(const NodeResult& result, std::string_view name) {
    const auto* field = result.output(key<OutputId>(name));
    const auto* value = field ? std::get_if<DataValue>(&field->content) : nullptr;
    if (!value) { throw std::runtime_error("Expected a produced output: " + std::string(name)); }
    return *value;
}
bool absent(const NodeResult& result, std::string_view name) {
    return std::holds_alternative<NotProduced>(result.output(key<OutputId>(name))->content);
}
struct DelayedRun {
    std::atomic<unsigned> calls{};
    bool close_first_branch{};
    std::promise<void> first_started, second_started, release;
    std::shared_future<void> proceed = release.get_future().share();
};
class Upstream final : public ToolInstance {
public:
    Upstream(const ToolExecutionConfiguration& configuration, std::shared_ptr<DelayedRun> gate)
        : ToolInstance(configuration), gate_(std::move(gate)) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override {
        const bool first = gate_->calls.fetch_add(1) == 0;
        if (first) {
            gate_->first_started.set_value();
            if (gate_->proceed.wait_for(5s) != std::future_status::ready) {
                return Diagnostic{ErrorCode::SourceFailed, Severity::Error, {}, "Upstream test gate timed out."};
            }
        } else { gate_->second_started.set_value(); }
        ToolCompletion completed{context->data().identity,ToolCompletionStatus::Succeeded,ToolJudgement::NotEvaluated,{},{}};
        if (first && gate_->close_first_branch) { completed.flow = InvocationFlow::CloseBranch; }
        return InvocationStart{std::move(completed)};
    }
    std::shared_ptr<DelayedRun> gate_;
};
class UpstreamFactory final : public ToolInstanceFactory {
public:
    std::shared_ptr<DelayedRun> gate = std::make_shared<DelayedRun>();
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override {
        return std::shared_ptr<ToolInstance>{std::make_shared<Upstream>(config,gate)};
    }
};
NodeId add_order_loop(Fixture& f) {
    const auto loop = fresh<NodeId>();
    const auto port = [](std::string_view name, std::int64_t value) {
        return InputDescriptor{key<InputId>(name),std::string(name),DataType::integer(),false,ConfigValue::integer(value)};
    };
    const auto numbers = key<OutputId>("numbers");
    const auto image = key<InputId>("image");
    auto group_image = f.flow.interface.inputs.front();
    group_image.binding.allowed_sources = {BindingSourceKind::PublicInput};
    GroupConfiguration body{{key<SchemaId>("test.image.loop"),1,
        {port("start",0),port("end",2),port("index",0),group_image},
        {{numbers,"Numbers",DataType::list(DataType::integer())}},{}},{f.id},
        {{numbers,{f.fid,f.id,key<OutputId>("sn"),{}}}}, {}, LoopConfiguration{1,false,50,{{numbers,LoopExportMode::Collect}}}};
    f.node.bindings.back().sources = {ScopedInputReference{f.fid,loop,image,{}}};
    f.flow.nodes.push_back({loop,"Repeat acquisition",body,{{image,{ScopedInputReference{f.fid,{},image,{}}}}}});
    f.flow.containment.push_back({loop,f.id});
    return loop;
}
enum class FirstRun { Complete, Cancel, CloseBranch, Loop };
void ordered(Fixture& f, std::vector<RunInput> inputs, FirstRun first_action = FirstRun::Complete) {
    const auto factory = std::make_shared<UpstreamFactory>();
    factory->gate->close_first_branch = first_action == FirstRun::CloseBranch;
    const auto gate_id = fresh<NodeId>();
    const auto type = key<ToolTypeId>("test.upstream");
    const auto schema = key<SchemaId>("test.upstream");
    ToolDescriptor descriptor{type, "Upstream", 1, {schema,1,{}}, {},{},{},{},{},{},{},{},{},{}};
    const auto registry = take(ToolRegistry::create(std::array{ToolRegistration{descriptor,{schema,1,1,{}},factory},f.tool}));
    auto unrelated = f.flow;
    unrelated.id = fresh<WorkflowId>();
    auto other_source = f.node;
    other_source.id = fresh<NodeId>();
    for (auto& binding : other_source.bindings) {
        for (auto& source : binding.sources) {
            if (auto* port = std::get_if<ScopedInputReference>(&source)) { port->workflow = unrelated.id; }
        }
    }
    unrelated.nodes = {other_source};
    unrelated.entries = {other_source.id};
    const auto mapped_sn = fresh<VariableId>(), mapped_width = fresh<VariableId>();
    if (first_action != FirstRun::Loop) {
        f.variables.push_back({mapped_sn,"Last SN",DataType::integer(),ConfigValue::integer(0)});
        f.flow.local_variables.push_back({mapped_width,"Width",DataType::integer(),ConfigValue::integer(0)});
        f.node.result_variables = {{key<OutputId>("sn"),VariableScope::Global,mapped_sn},
                                   {key<OutputId>("image_width"),VariableScope::Local,mapped_width}};
    }
    const auto target = first_action == FirstRun::Loop ? add_order_loop(f) : f.id;
    f.flow.nodes.push_back({gate_id,"Delay",ToolConfiguration{type,1}});
    f.flow.control_links.push_back({fresh<ControlLinkId>(),gate_id,{},target});
    f.flow.entries = {gate_id};
    f.flow.admission.max_in_flight = 3;
    auto session = f.activate(&registry,{unrelated});
    const auto snapshot = *session->inspect().active;
    const auto submit = [&](RunTarget target, std::optional<DebugStatePolicy> debug = {}) {
        auto request = take(RunRequest::create(snapshot,target,inputs,
            {debug ? TriggerKind::Debug : TriggerKind::Manual,key<SchemaId>("test")},{},debug));
        auto submitted = session->submit(request);
        if (auto* reason = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(reason->diagnostic.message); }
        check(std::holds_alternative<RunHandle>(submitted), "Concurrent run was admitted");
        return std::get<RunHandle>(std::move(submitted));
    };
    const auto result = [](RunHandle handle, NodeId node) {
        const auto waited = take(handle.wait(RunWaitTarget::Quiescence,std::chrono::steady_clock::now()+2s));
        check(waited.outcome == RunWaitOutcome::Satisfied && waited.progress.status == ExecutionStatus::Succeeded,
            "The independent or ordered run completed before its deadline");
        const auto progress = std::ranges::find(waited.progress.nodes,node,&RunNodeProgress::node);
        return *take(handle.lookup(*progress->invocation));
    };
    const auto first = submit(WorkflowTarget{f.fid});
    check(factory->gate->first_started.get_future().wait_for(2s) == std::future_status::ready, "Earlier run reached upstream gate");
    const auto second = submit(WorkflowTarget{f.fid});
    check(factory->gate->second_started.get_future().wait_for(2s) == std::future_status::ready, "Later run overtook upstream");
    const auto independent = result(submit(WorkflowTarget{unrelated.id}),other_source.id);
    const auto isolated = result(submit(NodeTarget{f.fid,f.id},DebugStatePolicy::Isolated),f.id);
    check(*output(independent,"sn").as_int64() == 1 && *output(isolated,"sn").as_int64() == 1,
        "Another node and an isolated state generation progress while the production source is waiting");
    check(take(second.wait(RunWaitTarget::Completion,std::chrono::steady_clock::now()+50ms)).outcome == RunWaitOutcome::DeadlineExpired,
        "A later run may finish upstream first but cannot publish this image source out of admission order");
    if (first_action == FirstRun::Cancel) { (void)take(first.cancel()); }
    factory->gate->release.set_value();
    if (first_action == FirstRun::Loop) {
        const auto first_loop = result(first,target), second_loop = result(second,target);
        const auto expected = [](std::int64_t n) { return take(DataValue::list(DataType::integer(),{DataValue::integer(n),DataValue::integer(n+1)})); };
        check(output(first_loop,"numbers") == expected(1) && output(second_loop,"numbers") == expected(3),
            "A loop retains its turn through its last iteration and the next run starts with SN 3");
        return;
    }
    if (first_action != FirstRun::Complete) {
        const auto stopped = take(first.wait(RunWaitTarget::Quiescence,std::chrono::steady_clock::now()+2s));
        const auto expected = first_action == FirstRun::Cancel ? ExecutionStatus::Cancelled : ExecutionStatus::Succeeded;
        check(stopped.outcome == RunWaitOutcome::Satisfied && stopped.progress.status == expected,
            "The earlier cancelled or skipped call terminates");
        const auto next = result(second,f.id);
        check(*output(next,"sn").as_int64() == 1,"The next admission proceeds after a skipped or cancelled source, without a gap");
        check(take(take(session->shared_preview())->read(mapped_sn)).revision == 1 && stopped.progress.local_variables.front().revision == 0,
            "Cancelled or skipped runs commit neither local nor global mappings");
        return;
    }
    const auto a = result(first,f.id), b = result(second,f.id);
    check(*output(a,"sn").as_int64() == 1 && *output(b,"sn").as_int64() == 2,"Image SN follows admission order");
    check(take(take(session->shared_preview())->read(mapped_sn)).revision == 2
        && take(first.inspect()).local_variables.front().revision == 1 && take(second.inspect()).local_variables.front().revision == 1,
        "Concurrent runs retain independent local revisions while global revisions advance once per accepted image");
    if (inputs.empty()) {
        check(*output(a,"file_name").as_string() == "a.bmp" && *output(b,"file_name").as_string() == "b.png",
            "File sequence cursor follows the same admission order as SN");
    }
}
void sdk_order() {
    for (const auto outcome : {FirstRun::Complete,FirstRun::Cancel,FirstRun::CloseBranch,FirstRun::Loop}) {
        Fixture f;
        f.select("source_type","SDK"); f.image_input();
        std::array pixel{std::byte{1}};
        auto image = take(ImageHandle::copy({1,1,1,{PixelDepth::UInt8,ChannelOrder::Mono}},pixel,f.raw));
        ordered(f,{{key<InputId>("image"),take(DataValue::image({image,{},{}}))}},outcome);
    }
}
RunProgress mapped_run(WorkflowSession& session, const Fixture& f, const DataValue& image, bool debug = false) {
    const RunTarget target = debug ? RunTarget{NodeTarget{f.fid,f.id}} : RunTarget{WorkflowTarget{f.fid}};
    const auto request = take(RunRequest::create(*session.inspect().active,target,{{key<InputId>("image"),image}},
        {debug ? TriggerKind::Debug : TriggerKind::Manual,key<SchemaId>("test")}));
    auto submitted = session.submit(request);
    if (const auto* bad = std::get_if<RunRejected>(&submitted)) { throw std::runtime_error(bad->diagnostic.message); }
    auto result = take(std::get<RunHandle>(submitted).wait(RunWaitTarget::Quiescence,std::chrono::steady_clock::now()+5s));
    if (result.progress.status != ExecutionStatus::Succeeded && !result.progress.diagnostics.empty()) {
        throw std::runtime_error(result.progress.diagnostics.front().message);
    }
    check(result.progress.status == ExecutionStatus::Succeeded,"Result variable run completed");
    return std::move(result.progress);
}
Solution saved_variables(const Fixture& f, const WorkflowSession& session) {
    const auto directory = fs::temp_directory_path() / ("vp-result-bindings-" + fresh<MessageId>().to_string());
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code io; fs::remove_all(path,io); } } cleanup{directory};
    take(write_new_solution(directory,session.inspect().active->solution()));
    const auto loaded = take(load_solution(directory,f.registry));
    check(loaded.can_save(),"Reloaded variable declarations and mappings validate");
    return loaded.solution();
}
void result_variables() {
    Fixture f;
    f.select("source_type","Camera"); f.select("camera_input","Delivered"); f.image_input();
    const auto pixels = fresh<VariableId>(), serial = fresh<VariableId>(), elapsed = fresh<VariableId>();
    const auto sn = fresh<VariableId>(), message = fresh<VariableId>();
    const auto optional_image = DataType::optional(DataType::image());
    const auto optional_text = DataType::optional(DataType::string());
    f.variables = {{pixels,"Picture",optional_image,ConfigValue::none(DataType::image())},
        {serial,"Camera",DataType::string(),take(ConfigValue::string(""))},{elapsed,"Elapsed",DataType::real(),take(ConfigValue::real(0))}};
    f.flow.local_variables = {{sn,"SN",DataType::integer(),ConfigValue::integer(-1)},
        {message,"Clock",optional_text,ConfigValue::none(DataType::string())}};
    f.node.result_variables = {{key<OutputId>("image"),VariableScope::Global,pixels},{key<OutputId>("serial"),VariableScope::Global,serial},
        {key<OutputId>("elapsed_ms"),VariableScope::Global,elapsed},{key<OutputId>("sn"),VariableScope::Local,sn},
        {key<OutputId>("timestamp_source"),VariableScope::Local,message}};
    auto reader = f.node;
    reader.id = fresh<NodeId>();
    reader.result_variables.clear();
    const auto received = fresh<VariableId>();
    const auto reader_time = fresh<VariableId>();
    f.flow.local_variables.push_back({received,"Read back",DataType::integer(),ConfigValue::integer(-1)});
    f.flow.local_variables.push_back({reader_time,"Read time",DataType::real(),take(ConfigValue::real(0))});
    reader.result_variables.push_back({key<OutputId>("sn"),VariableScope::Local,received});
    reader.result_variables.push_back({key<OutputId>("elapsed_ms"),VariableScope::Local,reader_time});
    reader.bindings.push_back({key<ParameterId>("filter_enabled"),{ConfigValue::boolean(true)}});
    reader.bindings.push_back({key<ParameterId>("trigger_text"),{take(ConfigValue::string("device"))}});
    reader.bindings.push_back({key<ParameterId>("message"),{GlobalReference{message,{},{}}}});
    f.flow.nodes.push_back(reader);
    f.flow.control_links.push_back({fresh<ControlLinkId>(),f.id,{},reader.id});
    auto session = f.activate();
    const std::array sample{std::byte{23}};
    const auto camera = fresh<ResourceId>();
    auto buffer = take(ImageHandle::copy({1,1,1,{PixelDepth::UInt8,ChannelOrder::Mono}},sample,f.raw));
    auto frame = take(DataValue::image({buffer,FrameId{camera,1},{},ImageAcquisitionInfo{"SIM","device"}}));
    const auto first = mapped_run(*session,f,frame);
    const auto locals = [](const RunProgress& p, VariableId id) -> const GlobalVariableEntry& {
        const auto found = std::ranges::find(p.local_variables,id,&GlobalVariableEntry::variable);
        check(found != p.local_variables.end(),"Run-local variable is available"); return *found;
    };
    check(locals(first,sn).revision == 1 && *locals(first,sn).current_value.as_int64() == 1
        && locals(first,received).revision == 1,"A later node reads the value that this run just wrote");
    auto globals = take(session->shared_preview());
    check(take(globals->read(pixels)).current_value.optional_value()->as_image()->image.shares_storage_with(buffer)
        && *take(globals->read(serial)).current_value.as_string() == "SIM"
        && *take(globals->read(elapsed)).current_value.as_float64() >= 0,"Integer, string, real and immutable image mappings reach typed variables");
    const auto second = mapped_run(*session,f,take(DataValue::image({buffer,FrameId{camera,2},{}})));
    check(take(globals->read(serial)).revision == 1 && take(globals->read(pixels)).revision == 2
        && locals(second,sn).revision == 1 && *locals(second,sn).current_value.as_int64() == 2
        && locals(second,received).revision == 0 && locals(second,reader_time).revision == 0,
        "Absent fields keep global revisions and a filtered call does not write its elapsed time to variables");
    (void)mapped_run(*session,f,frame,true);
    check(take(globals->read(pixels)).revision == 2,"Isolated node debugging does not write production globals");

    const auto restored = saved_variables(f,*session);
    const auto& saved = restored.workflows.front();
    check(saved.local_variables.size() == 4 && find_node(saved,f.id)->result_variables == f.node.result_variables,
        "Saving and loading preserve variable declarations and result mappings");
}
void group_result_variables() {
    Fixture f;
    f.select("source_type","SDK"); f.image_input();
    const auto loop = add_order_loop(f);
    f.flow.entries = {loop};
    const auto local = fresh<VariableId>(), outer = fresh<VariableId>(), numbers = fresh<VariableId>();
    auto& group = std::get<GroupConfiguration>(f.flow.nodes.front().configuration);
    group.local_variables = {{local,"SN",DataType::integer(),ConfigValue::integer(-1)}};
    f.node.result_variables = {{key<OutputId>("sn"),VariableScope::Local,local}};
    f.flow.local_variables = {{outer,"SN",DataType::integer(),ConfigValue::integer(100)},
        {numbers,"Loop output",DataType::list(DataType::integer()),take(ConfigValue::list(DataType::integer(),{}))}};
    f.flow.nodes.front().result_variables = {{key<OutputId>("numbers"),VariableScope::Local,numbers}};
    auto session = f.activate();
    const std::array sample{std::byte{8}};
    const auto frame = take(DataValue::image({take(ImageHandle::copy({1,1,1,{PixelDepth::UInt8,ChannelOrder::Mono}},sample,f.raw)),{},{}}));
    const auto run = mapped_run(*session,f,frame);
    check(run.group_local_variables.size() == 2 && run.group_local_variables.front().scope != run.group_local_variables.back().scope
        && run.group_local_variables[0].variables.front().revision == 1 && run.group_local_variables[1].variables.front().revision == 1
        && *run.group_local_variables[0].variables.front().current_value.as_int64() == 1
        && *run.group_local_variables[1].variables.front().current_value.as_int64() == 2 && run.local_variables.front().revision == 0,
        "Each loop body owns its own initial values and cannot overwrite its enclosing workflow");
    check(run.local_variables[1].revision == 1 && run.local_variables[1].current_value ==
        take(DataValue::list(DataType::integer(),{DataValue::integer(1),DataValue::integer(2)})),
        "The Group's exported result is mapped in the immediate enclosing workflow");
    const auto restored = saved_variables(f,*session);
    const auto& locals = local_variable_definitions(restored.workflows.front(),f.id);
    check(locals.size() == 1 && locals.front().id == local,"Saved Group local definitions retain ownership and identity");
    const auto debug = mapped_run(*session,f,frame,true);
    check(debug.group_local_variables.size() == 1 && debug.group_local_variables.front().group == loop
        && *debug.group_local_variables.front().variables.front().current_value.as_int64() == 1,"Nested independent debugging uses the immediate Group's isolated locals");
    const auto snapshot = *session->inspect().active;
    auto bad = snapshot.solution();
    auto& nodes = bad.workflows.front().nodes;
    auto writer = std::ranges::find(nodes,f.id,&Node::id);
    writer->result_variables.front().variable = outer;
    const auto rejected = session->publish(snapshot,bad,{});
    check(std::holds_alternative<Diagnostic>(rejected),"Group writers cannot bypass their Group to write a flow-local variable");
}
void sdk() {
    Fixture f;
    f.select("source_type", "SDK"); f.image_input();
    f.set("sn_initial", ConfigValue::integer(7));
    f.set("output_mono8", ConfigValue::boolean(true));
    f.set("stitch", ConfigValue::boolean(true)); f.coordinate("stitch_origin", f.joined);
    f.set("stitch_height", ConfigValue::integer(1));
    f.set("filter_enabled", ConfigValue::boolean(true)); f.set("trigger_text", take(ConfigValue::string("GO")));
    const auto msg = key<InputId>("message");
    f.flow.interface.inputs.push_back({msg, "message", DataType::optional(DataType::string()), false, ConfigValue::none(DataType::string())});
    f.node.bindings.push_back({key<ParameterId>("message"), {ScopedInputReference{f.fid, {}, msg, {}}}});
    auto session = f.activate();
    const auto run = [&](std::array<unsigned char,3> bytes, std::string_view message) {
        const auto image = take(ImageHandle::copy({1,1,3,{PixelDepth::UInt8,ChannelOrder::RGB}}, std::as_bytes(std::span{bytes}), f.raw));
        return f.run(*session, {{key<InputId>("image"), take(DataValue::image({image, {}, {}}))},
            {msg, DataValue::some(take(DataValue::string(message)))}});
    };
    const auto first = run({255,0,0}, "GO");
    check(*output(first,"sn").as_int64() == 7 && *output(first,"image_height").as_int64() == 2, "First SN and composite height");
    check(output(first,"gray_image").as_image()->image.copy_pixels() == std::vector{std::byte{0},std::byte{76}}
        && *output(first,"gray_image_height").as_int64() == 2 && *output(first,"gray_image_pixel_format").as_int64() == 17301505,
        "First stitched gap, red luminance and grayscale output metadata");
    const auto filtered = run({0,255,0}, "skip");
    check(absent(filtered,"image") && absent(filtered,"sn"), "Filtered invocation publishes no pixels or SN");
    const auto second = run({0,0,255}, "GO");
    check(*output(second,"sn").as_int64() == 8, "Filtered image did not advance SN");
    check(output(second,"gray_image").as_image()->image.copy_pixels() == std::vector{std::byte{76},std::byte{29}},
        "Second stitched frame uses previous accepted unstitched pixels");
    check(output(second,"image").as_image()->image.coordinate_frame() == f.joined, "Composite uses its own pixel coordinate system");
}
void camera() {
    Fixture f;
    const auto unavailable = open_camera({fresh<ResourceId>(),"MVS",ResourceKind::Device,
        DeviceResource{key<SchemaId>("visionpro.camera.mvs"),take(camera_device_config("MISSING", 2))}});
    const auto* unsupported = std::get_if<Diagnostic>(&unavailable);
    check(unsupported && unsupported->code == ErrorCode::MissingCapability, "Unverified hardware strobe mappings report the missing capability");
    f.select("source_type","Camera"); f.coordinate("frame_origin", f.raw);
    const auto device = fresh<ResourceId>();
    f.select("mode", "Hardware");
    f.set("timeout_ms", ConfigValue::integer(10));
    simulated_camera()->set_periodic_hardware_pulses(false);
    f.resources.push_back({device,"Simulated camera",ResourceKind::Device,
        DeviceResource{key<SchemaId>("visionpro.camera.simulated"),
            take(camera_device_config("SIM-001", 2, CameraAcquisitionSettings{CameraMode::Software, 1200ms}))}});
    f.node.resources.push_back({key<SchemaId>("camera"),{device,ResourceKind::Device,{}}});
    f.set("control_exposure", ConfigValue::boolean(true)); f.set("exposure_us", take(ConfigValue::real(2000)));
    auto session = f.activate();
    const auto capture = f.run(*session);
    // A legacy per-node hardware trigger would time out without a pulse.
    simulated_camera()->set_periodic_hardware_pulses(true);
    const auto frame = *output(capture,"image").as_image();
    auto invalid = frame;
    invalid.acquisition->strobe_channels.resize(4, frame.image);
    const auto excess = DataValue::image(invalid);
    check(std::get_if<Diagnostic>(&excess) && std::get<Diagnostic>(excess).code == ErrorCode::InvalidImage,
        "An external frame cannot silently drop excess strobe channels");
    const std::array padded{std::byte{1}, std::byte{0}};
    invalid.acquisition->strobe_channels = {take(ImageHandle::copy({1,1,2,{PixelDepth::UInt16,ChannelOrder::Mono}},padded,f.raw))};
    const auto depth = DataValue::image(invalid);
    check(std::get_if<Diagnostic>(&depth) && std::get<Diagnostic>(depth).code == ErrorCode::UnsupportedPixelFormat,
        "An external frame must declare supported strobe formats");
    invalid.acquisition->strobe_channels = {take(ImageHandle::copy({1,1,1,{PixelDepth::UInt8,ChannelOrder::Mono}},padded,f.joined))};
    const auto coordinates = DataValue::image(invalid);
    check(std::get_if<Diagnostic>(&coordinates) && std::get<Diagnostic>(coordinates).code == ErrorCode::CoordinateMismatch,
        "An external frame cannot combine strobe exposures from unrelated coordinate systems");
    const auto level = frame.image.copy_pixels().front();
    check(frame.frame && frame.frame->source == device && (level == std::byte{14} || level == std::byte{62}),
        "Simulated acquisition applies per-node exposure and preserves physical source");
    check(*output(capture,"serial").as_string() == "SIM-001" && absent(capture,"lost_packets")
        && *output(capture,"acquire_failures").as_int64() == 0, "Unavailable telemetry remains distinct from an observed zero");
    check(!absent(capture,"image_1") && !absent(capture,"image_2") && absent(capture,"image_3")
        && *output(capture,"image_1_width").as_int64() == 2 && *output(capture,"image_2_pixel_format").as_int64() == 17301505
        && output(capture,"image_1").as_image()->image.copy_pixels() != output(capture,"image_2").as_image()->image.copy_pixels(),
        "Strobe channels correspond to independently simulated exposures");
    Fixture d;
    d.select("source_type","Camera"); d.select("camera_input","Delivered"); d.image_input();
    const auto delivered_sn = fresh<VariableId>();
    d.variables = {{delivered_sn,"Delivered SN",DataType::integer(),ConfigValue::integer(0)}};
    d.node.result_variables = {{key<OutputId>("sn"),VariableScope::Global,delivered_sn}};
    d.node.execution_policy.max_retries = 1;
    auto delivery = d.activate();
    const auto received = d.run(*delivery, {{key<InputId>("image"),take(DataValue::image(frame))}});
    const auto& copy = *output(received,"image").as_image();
    check(copy.image.shares_storage_with(frame.image) && copy.frame == frame.frame
        && output(received,"serial") == output(capture,"serial") && output(received,"device_ticks") == output(capture,"device_ticks")
        && output(received,"image_2").as_image()->image.shares_storage_with(output(capture,"image_2").as_image()->image),
        "Receiving node publishes the same frame without a new device lease");
    const auto bare = d.run(*delivery, {{key<InputId>("image"),take(DataValue::image({frame.image, frame.frame, frame.captured_at}))}});
    check(absent(bare,"serial") && absent(bare,"image_1"), "An external frame without device metadata does not invent camera metrics");
    const auto bad_input = take(DataValue::image({frame.image,{},{}}));
    const auto request = take(RunRequest::create(*delivery->inspect().active,WorkflowTarget{d.fid},{{key<InputId>("image"),bad_input}},
        {TriggerKind::Manual,key<SchemaId>("test")}));
    auto attempt = delivery->submit(request);
    check(std::holds_alternative<RunHandle>(attempt),"An invalid delivered frame can be diagnosed by the node");
    const auto exhausted = take(std::get<RunHandle>(attempt).wait(RunWaitTarget::Quiescence,std::chrono::steady_clock::now()+5s));
    check(exhausted.progress.status == ExecutionStatus::Failed && exhausted.progress.nodes.front().attempts.size() == 2
        && take(take(delivery->shared_preview())->read(delivered_sn)).revision == 2,"Failed retries never publish mapped variable writes");
    Fixture gray;
    gray.select("source_type","Camera"); gray.select("camera_input","Delivered"); gray.image_input();
    gray.set("output_mono8", ConfigValue::boolean(true));
    auto grayscale = gray.activate();
    const auto one = gray.run(*grayscale, {{key<InputId>("image"),take(DataValue::image(frame))}});
    check(output(one,"image").as_image()->image.format().order == ChannelOrder::Mono
        && output(one,"image").as_image()->acquisition->strobe_channels.empty()
        && absent(one,"gray_image") && absent(one,"image_1") && absent(one,"image_2_width"),
        "Mono8 output on a strobe camera publishes only the monochrome main image");
    simulated_camera()->fail_next(1002);
    bool failed = false;
    try { (void)f.run(*session); } catch (const std::runtime_error&) { failed = true; }
    check(failed, "A failed camera acquisition must fail the run");
    const auto recovered = f.run(*session);
    check(*output(recovered,"acquire_failures").as_int64() == 1
        && *output(recovered,"sn").as_int64() == *output(capture,"sn").as_int64() + 1,
        "The next physical frame reports the intervening failure without consuming an image SN");
}
#ifdef VISIONPRO_WITH_OPENCV
struct Files {
    fs::path directory = fs::temp_directory_path() / ("vp-image-source-" + fresh<MessageId>().to_string());
    Files() { fs::create_directory(directory); }
    ~Files() { std::error_code io; fs::remove_all(directory, io); }
    template<std::size_t N> fs::path write(std::string_view name, const unsigned char (&bytes)[N]) {
        const auto path = directory / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes), N);
        if (!out) { throw std::runtime_error("Could not write sequence fixture"); }
        return path;
    }
};
void sequence() {
    Files files;
    Fixture f;
    f.coordinate("frame_origin", f.raw); f.select("decode_mode","RGB24");
    f.set("output_mono8",ConfigValue::boolean(true)); f.set("cache_size", ConfigValue::integer(2));
    const auto to_path = [](const fs::path& file) { const auto text = file.generic_u8string();
        return ResourcePath{PathBase::Absolute,{text.begin(), text.end()}, {}}; };
    std::vector<ResourcePath> paths{to_path(files.write("a.bmp",visionpro::test_fixtures::bmp)),
        to_path(files.write("b.png",visionpro::test_fixtures::png))};
    const auto id = fresh<ResourceId>();
    f.resources.push_back({id,"Fixture sequence",ResourceKind::DirectorySequence,ImageSequenceResource{paths,SequenceSort::ExplicitOrder,
        SequenceAdvance::PerInvocation, SequenceRepeat::Stop, SequenceMissing::Fail, SequenceTrigger::Manual,0}});
    f.node.resources.push_back({key<SchemaId>("sequence"), {id,ResourceKind::DirectorySequence,{}}});
    f.set("sn_subscription", ConfigValue::some(ConfigValue::integer(40)));
    auto session = f.activate();
    const auto a = f.run(*session), b = f.run(*session), again = f.run(*session);
    check(*output(a,"file_name").as_string() == "a.bmp" && *output(b,"file_name").as_string() == "b.png"
        && b.data().flow == InvocationFlow::EndContinuous && *output(again,"file_name").as_string() == "a.bmp",
        "The last file stops continuous acquisition and a later manual call starts a new pass");
    const auto& image = output(a,"image").as_image()->image;
    check(image.format().order == ChannelOrder::RGB && image.copy_pixels()[2] == std::byte{255}
        && output(a,"gray_image").as_image()->image.copy_pixels()[0] == std::byte{29}, "RGB bytes and same-frame gray are correct");
    check(*output(a,"sn").as_int64() == 41 && *output(b,"sn").as_int64() == 41, "Subscribed SN uses input plus one per accepted frame");
    Fixture legacy;
    legacy.coordinate("frame_origin", legacy.raw); legacy.select("decode_mode","Native8");
    legacy.resources = f.resources;
    legacy.node.resources = f.node.resources;
    auto native_session = legacy.activate();
    const auto native = legacy.run(*native_session);
    const auto& pixels = output(native,"image").as_image()->image;
    check(pixels.format().order == ChannelOrder::BGR && pixels.copy_pixels().front() == std::byte{255}
        && *output(native,"pixel_format").as_int64() == 35127317, "Legacy Native8 retains the decoder's BGR byte order");
    Fixture concurrent;
    concurrent.coordinate("frame_origin", concurrent.raw);
    concurrent.resources = f.resources;
    concurrent.node.resources = f.node.resources;
    ordered(concurrent,{});
}
#endif
}
int main() {
    try {
        sdk(); camera(); sdk_order(); result_variables(); group_result_variables();
#ifdef VISIONPRO_WITH_OPENCV
        sequence();
#endif
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
