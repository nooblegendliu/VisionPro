#include "image_source.hpp"
#include "image_source_model.hpp"
#include "image_source_pixels.hpp"
#ifdef VISIONPRO_WITH_OPENCV
#include "vision/read_file.hpp"
#endif

#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <algorithm>
#include <limits>
#include <numeric>
#include <set>

namespace visionpro::tools {
namespace {
using namespace core;
using namespace infrastructure;
using image_source::key;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

// A checked adapter failure crosses helpers and is converted to a ToolCompletion by the worker.
template<class T> T take(Result<T> value)
{
    if (auto* failure = std::get_if<Diagnostic>(&value)) { throw std::move(*failure); }
    return std::get<T>(std::move(value));
}
[[noreturn]] void fail(ErrorCode code, std::string message)
{ throw Diagnostic{code, Severity::Error, {}, std::move(message)}; }

template<class Range> const auto& argument(const Range& values, std::string_view id)
{
    const auto item = std::ranges::find_if(values, [&](const auto& value){ return value.id.text() == id; });
    if (item == values.end()) { fail(ErrorCode::MissingRequiredValue, "Image source requires " + std::string(id)); }
    return item->value;
}

struct Settings {
    const InvocationContextData& call;
    const ConfigValue& fixed(std::string_view id) const { return argument(call.configuration.data().parameters, id); }
    const DataValue& bound(std::string_view id) const { return argument(call.parameters, id); }
    bool flag(std::string_view id) const { return *fixed(id).as_bool(); }
    std::int64_t number(std::string_view id) const { return *fixed(id).as_int64(); }
    std::string_view choice(std::string_view id) const { return fixed(id).as_enum()->text(); }
    CoordinateSystemId origin(std::string_view id) const {
        const auto& point = *fixed(id).as_point2d();
        if (point.unit != MeasurementUnit::Pixel || point.x != 0 || point.y != 0) {
            fail(ErrorCode::CoordinateMismatch, "Image origin must be (0,0) in pixels.");
        }
        return point.coordinate_frame;
    }
};

DataValue text(std::string_view value) { return take(DataValue::string(value)); }
std::string path_text(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path native_path(std::string_view value)
{ return fs::path{std::u8string(value.begin(), value.end())}; }

struct State {
    explicit State(const DataValue& value)
        : type(value.type()), published(*value.field(key<FieldId>("published"))->as_int64()),
          cursor(*value.field(key<FieldId>("cursor"))->as_int64()), previous(*value.field(key<FieldId>("previous"))),
          cache(*value.field(key<FieldId>("cache"))), seen(*value.field(key<FieldId>("seen"))) {}
    DataValue value() const {
        return take(DataValue::record(type, {{key<FieldId>("published"), DataValue::integer(published)},
            {key<FieldId>("cursor"), DataValue::integer(cursor)}, {key<FieldId>("previous"), previous}, {key<FieldId>("cache"), cache},
            {key<FieldId>("seen"), seen}}));
    }
    DataType type;
    std::int64_t published;
    std::int64_t cursor;
    DataValue previous;
    DataValue cache;
    DataValue seen;
};

void put(ToolCompletion& result, std::string_view id, OutputContent content)
{
    const auto item = std::ranges::find_if(result.outputs, [&](const auto& out){ return out.id.text() == id; });
    if (item != result.outputs.end()) { item->content = std::move(content); }
}

struct SourceFrame {
    ImageFrame pixels;
    std::optional<fs::path> path{};
    std::size_t file_index{};
    bool final_file{};
};

#ifdef VISIONPRO_WITH_OPENCV
ImageHandle decode(const Settings& settings, std::span<const std::byte> bytes)
{
    const auto mode = settings.choice("decode_mode");
    auto image = take(vision::decode_file_image(bytes, mode == "Gray8" ? vision::FileImageDecodeMode::Gray8 :
        vision::FileImageDecodeMode::Native8, settings.origin("frame_origin")));
    return mode == "RGB24" ? take(image_source::convert(image, ChannelOrder::RGB)) : image;
}

ImageHandle load_member(const Settings& settings, State& state, const fs::path& path, std::optional<std::string_view> digest)
{
    const auto capacity = static_cast<std::size_t>(settings.number("cache_size"));
    const auto name = path_text(path);
    std::error_code io;
    const auto stamp = fs::last_write_time(path, io).time_since_epoch().count();
    const bool has_stamp = !io;
    const auto file_size = fs::file_size(path, io);
    const bool cacheable = capacity && has_stamp && !io && file_size <= INT64_MAX;
    auto verified = digest ? take(read_file_bytes(path, digest)) : nullptr;
    if (cacheable) {
        for (const auto& item : *state.cache.elements()) {
            if (*item.field(key<FieldId>("path"))->as_string() == name && *item.field(key<FieldId>("stamp"))->as_int64() == stamp
                && *item.field(key<FieldId>("size"))->as_int64() == static_cast<std::int64_t>(file_size)) {
                return item.field(key<FieldId>("frame"))->as_image()->image;
            }
        }
    }
    auto bytes = verified ? std::move(verified) : take(read_file_bytes(path));
    auto image = decode(settings, *bytes);
    if (cacheable) {
        auto entries = *state.cache.elements();
        std::erase_if(entries, [&](const auto& item){ return *item.field(key<FieldId>("path"))->as_string() == name; });
        if (entries.size() >= capacity) { entries.erase(entries.begin()); }
        const auto type = *state.cache.type().element_type();
        entries.push_back(take(DataValue::record(type, {{key<FieldId>("path"), text(name)},
            {key<FieldId>("stamp"), DataValue::integer(stamp)}, {key<FieldId>("size"), DataValue::integer(static_cast<std::int64_t>(file_size))},
            {key<FieldId>("frame"), take(DataValue::image({image, {}, {}}))}})));
        state.cache = take(DataValue::list(type, entries));
    }
    return image;
}
#endif

std::optional<SourceFrame> local_frame(const Settings& settings, State& state, std::stop_token stop)
{
#ifndef VISIONPRO_WITH_OPENCV
    (void)settings; (void)state; (void)stop;
    fail(ErrorCode::MissingCapability, "File image decoding requires the OpenCV build.");
#else
    const FileResourceLease* single = nullptr;
    const FileSequenceLease* sequence = nullptr;
    std::span<const fs::path> files;
    std::vector<fs::path> directory;
    std::size_t initial = 0;
    std::size_t count = 0;
    if (settings.flag("subscribe_folder")) {
        const auto& folder = *settings.bound("folder_path").as_string();
        directory = take(image_directory_files(native_path(folder)));
        files = directory;
        count = files.size();
    } else {
        if (settings.call.resources.size() != 1) { fail(ErrorCode::InvalidResource, "Select one file or one image sequence."); }
        const auto* lease = settings.call.resources.front().lease.get();
        single = dynamic_cast<const FileResourceLease*>(lease);
        if (single) { count = 1; }
        else if ((sequence = dynamic_cast<const FileSequenceLease*>(lease))) {
            files = sequence->files(); initial = sequence->initial_index(); count = files.size();
        } else { fail(ErrorCode::ResourceKindMismatch, "Local image source needs a file or sequence lease."); }
    }
    if (!count) {
        if (settings.flag("subscribe_folder")) {
            state.seen = take(DataValue::list(DataType::string(), {}));
            return {};
        }
        fail(ErrorCode::InputUnavailable, "The image sequence is empty.");
    }
    std::vector<std::size_t> active;
    if (settings.flag("selected_only") && settings.flag("subscribe_folder")) {
        std::set<std::string> selected;
        for (const auto& item : *settings.fixed("selected_files").elements()) { selected.insert(*item.as_string()); }
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (selected.contains(path_text(files[i].filename()))) { active.push_back(i); }
        }
        if (active.empty()) {
            if (settings.call.changed_image_files) { return {}; }
            fail(ErrorCode::InputUnavailable, "None of the selected image files are present in this directory.");
        }
    } else if (settings.flag("selected_only")) {
        for (const auto& selected : *settings.fixed("selected_indices").elements()) {
            const auto index = *selected.as_int64();
            if (index < 0 || static_cast<std::uint64_t>(index) >= count) {
                fail(ErrorCode::IndexOutOfRange, "Selected file index is outside the current image sequence.");
            }
            active.push_back(static_cast<std::size_t>(index));
        }
        std::ranges::sort(active);
        active.erase(std::unique(active.begin(), active.end()), active.end());
        if (active.empty()) { fail(ErrorCode::InputUnavailable, "Select at least one image to run."); }
    } else {
        active.resize(count);
        std::iota(active.begin(), active.end(), std::size_t{0});
    }
    auto start = std::ranges::lower_bound(active, initial);
    const auto first = start == active.end() ? 0 : static_cast<std::size_t>(start - active.begin());
    auto cursor = state.cursor < 0 ? first : static_cast<std::size_t>(state.cursor);
    const bool folder_event = settings.flag("subscribe_folder") && settings.call.changed_image_files.has_value();
    if (folder_event) {
        std::set<fs::path> changed;
        for (const auto& path : *settings.call.changed_image_files) { changed.insert(native_path(path).lexically_normal()); }
        const auto match = std::ranges::find_if(active, [&](auto index){ return changed.contains(files[index].lexically_normal()); });
        if (match == active.end()) { return {}; }
        cursor = static_cast<std::size_t>(match - active.begin());
    } else if (settings.flag("subscribe_folder") && settings.flag("auto_next")) {
        std::set<std::string> present;
        for (const auto index : active) { present.insert(path_text(files[index])); }
        std::set<std::string> seen;
        for (const auto& path : *state.seen.elements()) {
            if (present.contains(*path.as_string())) { seen.insert(*path.as_string()); }
        }
        const auto next = std::ranges::find_if(active, [&](auto index) { return !seen.contains(path_text(files[index])); });
        if (next == active.end()) { seen.clear(); }
        const auto position = next == active.end() ? 0 : static_cast<std::size_t>(next - active.begin());
        seen.insert(path_text(files[active[position]]));
        std::vector<DataValue> values;
        for (const auto& member : seen) { values.push_back(text(member)); }
        state.seen = take(DataValue::list(DataType::string(), values));
        cursor = position;
    }
    // The last dispatched image already signals EndContinuous to the controlling host.
    // A later explicit invocation begins the next pass.
    const auto position = cursor % active.size();
    const auto index = active[position];
    const auto& path = single ? single->path() : files[index];
    auto image = single ? decode(settings, single->bytes()) :
        load_member(settings, state, path, sequence ? sequence->digest(index) : std::nullopt);
    // A fixed legacy/single file remains runnable on each explicit call.
    const bool advancing = settings.flag("auto_next") && !single;
    const bool final = !folder_event && settings.flag("auto_next") && settings.flag("stop_at_end") && (settings.flag("subscribe_folder")
        ? state.seen.elements()->size() == active.size() : position + 1 == active.size());
    state.cursor = static_cast<std::int64_t>(advancing ? (position + 1) % active.size() : position);
    if (advancing && !folder_event) {
        const auto ahead = std::min(static_cast<std::size_t>(settings.number("cache_size")), active.size() - 1);
        for (std::size_t offset = 1; offset <= ahead && !stop.stop_requested(); ++offset) {
            if (settings.flag("stop_at_end") && position + offset >= active.size()) { break; }
            const auto next = active[(position + offset) % active.size()];
            try { (void)load_member(settings, state, files[next], sequence ? sequence->digest(next) : std::nullopt); }
            catch (const Diagnostic&) {
                // A speculative decode must not change the current frame or consume the failed member.
                // The owning invocation reports its error if that member is actually selected.
                break;
            }
        }
    }
    return SourceFrame{{std::move(image), {}, {}}, path, index, final};
#endif
}

SourceFrame input_frame(const Settings& settings)
{
    const auto& frame = argument(settings.call.inputs, "image");
    if (!frame.as_image()) { fail(ErrorCode::InvalidImage, "The delivered input must contain an image."); }
    if (settings.choice("source_type") == "Camera" && !frame.as_image()->frame) {
        fail(ErrorCode::InvalidImage, "A delivered camera image requires a frame identity.");
    }
    return {*frame.as_image()};
}

SourceFrame capture(const Settings& settings, std::stop_token stop, ToolCompletion& result)
{
    const auto& leases = settings.call.resources;
    const auto* lease = leases.size() == 1 ? dynamic_cast<const CameraLease*>(leases.front().lease.get()) : nullptr;
    if (!lease) { fail(ErrorCode::InvalidResource, "Camera acquisition requires an exclusive camera lease."); }
    auto acquisition = lease->acquisition_settings();
    if (!acquisition) {
        const auto choice = settings.choice("mode");
        const auto mode = choice == "Single" ? CameraMode::Single : choice == "Continuous" ? CameraMode::Continuous :
            choice == "Software" ? CameraMode::Software : CameraMode::Hardware;
        acquisition = CameraAcquisitionSettings{mode, std::chrono::milliseconds{settings.number("timeout_ms")}};
    }
    CameraCaptureControls controls;
    if (settings.flag("control_exposure")) { controls.exposure_us = *settings.bound("exposure_us").as_float64(); }
    if (settings.flag("control_gain")) { controls.gain = *settings.bound("gain").as_float64(); }
    auto acquired = lease->capture(settings.origin("frame_origin"), acquisition->mode, acquisition->timeout, stop, controls);
    auto finished = lease->finish();
    if (auto* bad = std::get_if<Diagnostic>(&acquired)) {
        if (auto* close_bad = std::get_if<Diagnostic>(&finished)) { result.diagnostics.push_back(*close_bad); }
        throw *bad;
    }
    take(std::move(finished));
    auto frame = std::get<CameraFrame>(std::move(acquired));
    result.timings.push_back({TimingScope::ToolModule, TimingSource::AdapterReported, frame.device_duration_unit,
        frame.device_duration, key<SchemaId>("camera")});
    return {std::move(frame.image)};
}

void camera_results(ToolCompletion& result, const SourceFrame& source)
{
    if (source.pixels.frame) {
        const auto seq = source.pixels.frame->sequence;
        if (seq > INT64_MAX) { fail(ErrorCode::SizeOverflow, "Camera sequence exceeds Int64."); }
        put(result, "frame_number", DataValue::integer(static_cast<std::int64_t>(seq)));
    }
    if (!source.pixels.acquisition) {
        for (const auto id : {"serial", "timestamp_source", "device_ticks", "tick_frequency_hz", "device_frame_number",
                              "camera_frame_number", "lost_frames", "lost_packets", "acquire_failures"}) {
            put(result, id, NotProduced{OutputAbsenceReason::ToolDidNotProduce});
        }
        return;
    }
    const auto& camera = *source.pixels.acquisition;
    if (!camera.serial.empty()) { put(result, "serial", text(camera.serial)); }
    else { put(result, "serial", NotProduced{OutputAbsenceReason::ToolDidNotProduce}); }
    if (!camera.timestamp_source.empty()) { put(result, "timestamp_source", text(camera.timestamp_source)); }
    else { put(result, "timestamp_source", NotProduced{OutputAbsenceReason::ToolDidNotProduce}); }
    const auto legacy = [&](std::string_view name, std::optional<std::uint64_t> number) {
        put(result, name, number ? DataValue::some(text(std::to_string(*number))) : DataValue::none(DataType::string()));
    };
    legacy("device_ticks", camera.device_ticks); legacy("tick_frequency_hz", camera.tick_frequency_hz);
    legacy("device_frame_number", camera.device_frame_number);
    const auto metric = [&](std::string_view name, std::optional<std::uint64_t> value) {
        if (!value) { put(result, name, NotProduced{OutputAbsenceReason::ToolDidNotProduce}); return; }
        if (*value > INT64_MAX) {
            put(result, name, NotProduced{OutputAbsenceReason::ToolDidNotProduce});
            Diagnostic issue{ErrorCode::SizeOverflow, Severity::Warning, {}, "Device metric exceeds Int64."};
            issue.location.output = key<OutputId>(name);
            result.diagnostics.push_back(std::move(issue));
            return;
        }
        put(result, name, DataValue::integer(static_cast<std::int64_t>(*value)));
    };
    metric("camera_frame_number", camera.device_frame_number); metric("lost_frames", camera.lost_frames);
    metric("lost_packets", camera.lost_packets); metric("acquire_failures", camera.acquire_failures);
}

void publish_image(ToolCompletion& result, const std::string& name, const ImageFrame& frame)
{
    const auto format = frame.image.format();
    const auto order = format.order;
    if (format.depth != PixelDepth::UInt8 || (order != ChannelOrder::Mono && order != ChannelOrder::RGB && order != ChannelOrder::BGR)) {
        fail(ErrorCode::UnsupportedPixelFormat, "Image source output requires UInt8 Mono, RGB or BGR.");
    }
    put(result, name, take(DataValue::image(frame)));
    put(result, name + "_width", DataValue::integer(static_cast<std::int64_t>(frame.image.width())));
    put(result, name + "_height", DataValue::integer(static_cast<std::int64_t>(frame.image.height())));
    put(result, name == "image" ? "pixel_format" : name + "_pixel_format",
        DataValue::integer(order == ChannelOrder::Mono ? 17301505 : order == ChannelOrder::RGB ? 35127316 : 35127317));
}

void execute(const InvocationContextData& call, ToolCompletion& result, std::stop_token stop)
{
    const Settings settings{call};
    for (const auto& output : call.configuration.data().tool.outputs) {
        result.outputs.push_back({output.id, NotProduced{OutputAbsenceReason::ConditionNotMet}});
    }
    if (settings.flag("filter_enabled")) {
        const auto* message = settings.bound("message").optional_value();
        const auto& filter = *settings.fixed("trigger_text").as_string();
        if (!message || (!filter.empty() && *message->as_string() != filter)) {
            result.flow = InvocationFlow::CloseBranch;
            return;
        }
    }
    if (!call.state) { fail(ErrorCode::InvalidState, "Image source needs node session state."); }
    State state{take(call.state->read())};
    const auto type = settings.choice("source_type");
    std::optional<SourceFrame> source;
    if (type == "Local") { source = local_frame(settings, state, stop); }
    else if (type == "Camera" && settings.choice("camera_input") == "Acquire") { source = capture(settings, stop, result); }
    else { source = input_frame(settings); }
    if (!source) {
        result.flow = InvocationFlow::CloseBranch;
        if (settings.flag("subscribe_folder")) { take(call.state->write(state.value())); }
        return;
    }
    const auto& raw = source->pixels;
    auto main = raw;
    if (settings.flag("stitch")) {
        const bool clear = settings.flag("clear_on_trigger") && *settings.bound("clear_value").as_int64() != 0;
        const auto previous = clear ? nullptr : state.previous.optional_value();
        main.image = take(image_source::stitch(raw.image, previous ? &previous->as_image()->image : nullptr,
            static_cast<std::size_t>(settings.number("stitch_start")), static_cast<std::size_t>(settings.number("stitch_height")),
            settings.origin("stitch_origin")));
        // A composite has new coordinates. Preserve its device facts and publish the sensor planes separately.
        if (main.acquisition) { main.acquisition->strobe_channels.clear(); }
        state.previous = DataValue::some(take(DataValue::image(raw)));
    }
    const bool split = type == "Camera" && raw.acquisition && !raw.acquisition->strobe_channels.empty();
    if (split && settings.flag("output_mono8")) {
        main.image = take(image_source::convert(main.image, ChannelOrder::Mono));
        main.acquisition->strobe_channels.clear();
    } else {
        if (settings.flag("output_mono8") && main.image.format().order != ChannelOrder::Mono) {
            auto gray = main;
            gray.image = take(image_source::convert(main.image, ChannelOrder::Mono));
            publish_image(result, "gray_image", gray);
        }
        if (split) {
            for (std::size_t i = 0; i < raw.acquisition->strobe_channels.size(); ++i) {
                publish_image(result, "image_" + std::to_string(i + 1), {raw.acquisition->strobe_channels[i], raw.frame, raw.captured_at});
            }
        }
    }
    publish_image(result, "image", main);
    if (source->path) {
        put(result, "file_path", text(path_text(*source->path)));
        const auto& names = *settings.fixed("source_names").elements();
        put(result, "file_name", source->file_index < names.size() ? text(*names[source->file_index].as_string()) : text(path_text(source->path->filename())));
    }
    if (type == "Camera") { camera_results(result, *source); }
    const auto* subscribed = type == "Local" ? settings.bound("sn_subscription").optional_value() : nullptr;
    const auto base = subscribed ? *subscribed->as_int64() : settings.number("sn_initial");
    const auto advance = subscribed ? std::int64_t{1} : state.published;
    if (base > INT64_MAX - advance || state.published == INT64_MAX) { fail(ErrorCode::IdentityExhausted, "Image SN exceeds Int64."); }
    put(result, "sn", DataValue::integer(base + advance));
    ++state.published;
    if (stop.stop_requested()) { fail(ErrorCode::SourceCancelled, "Image source was cancelled before publication."); }
    take(call.state->write(state.value()));
    if (source->final_file) { result.flow = InvocationFlow::EndContinuous; }
}

class Control final : public InvocationControl {
public:
    std::stop_source stop;
    void request_cancel() override { stop.request_stop(); }
};

class SourceTool final : public ToolInstance {
public:
    explicit SourceTool(const ToolExecutionConfiguration& config) : ToolInstance(config) {}
private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion completion) override
    {
        auto control = std::make_shared<Control>();
        auto registered = completion.register_control(control);
        if (const auto* bad = std::get_if<Diagnostic>(&registered)) { return *bad; }
        const auto token = control->stop.get_token();
        const auto identity = context->data().identity;
        auto work = [context = std::move(context), completion, token]() mutable {
            const auto start = Clock::now();
            ToolCompletion result{context->data().identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
            try {
                execute(context->data(), result, token);
                put(result, "elapsed_ms", take(DataValue::real(std::chrono::duration<double, std::milli>(Clock::now()-start).count())));
            } catch (const Diagnostic& issue) {
                result.status = issue.code == ErrorCode::SourceTimedOut ? ToolCompletionStatus::TimedOut :
                    issue.code == ErrorCode::SourceCancelled ? ToolCompletionStatus::Cancelled : ToolCompletionStatus::Failed;
                result.outputs.clear();
                result.flow = InvocationFlow::Continue;
                result.diagnostics.insert(result.diagnostics.begin(), issue);
            } catch (...) {
                result.status = ToolCompletionStatus::Failed;
                result.outputs.clear();
                result.flow = InvocationFlow::Continue;
                result.diagnostics.push_back({ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Image source worker failed."});
            }
            result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start);
            context.reset();
            if (auto reported = completion.complete(result); const auto* bad = std::get_if<Diagnostic>(&reported)) {
                result.status = ToolCompletionStatus::Failed;
                result.outputs.clear(); result.timings.clear(); result.flow = InvocationFlow::Continue;
                result.diagnostics = {*bad};
                (void)completion.complete(result);
            }
            (void)completion.mark_exited();
        };
        try {
            std::jthread thread(std::move(work));
            try { thread.detach(); } catch (...) { control->request_cancel(); }
        } catch (...) {
            ToolCompletion failure{identity, ToolCompletionStatus::Failed, ToolJudgement::NotEvaluated, {},
                {{ErrorCode::ExecutionBoundaryFailure, Severity::Error, {}, "Could not launch image source worker."}}};
            (void)completion.complete(failure);
            (void)completion.mark_exited();
        }
        return InvocationStart{AsynchronousInvocation{control}};
    }
};

class Factory final : public ToolInstanceFactory {
public:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>(std::make_shared<SourceTool>(config)); }
};
}

core::ToolRegistration image_source_registration()
{
    auto tool = image_source::descriptor();
    const auto schema = tool.schema.id;
    return {std::move(tool), {schema, 1, 1, {}}, std::make_shared<Factory>()};
}
}
