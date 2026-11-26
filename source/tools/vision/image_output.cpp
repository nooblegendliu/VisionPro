#include "image_output.hpp"
#include "opencv_image.hpp"

#include <algorithm>
#include <chrono>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/image_archive.hpp>

namespace visionpro::tools::vision
{
namespace
{
using namespace core;
using image_output::input;
using image_output::parameter;
template <SchemaIdentity T> T key(std::string_view name)
{
    return std::get<T>(SchemaIdFactory::parse<T>(name));
}
Diagnostic issue(ErrorCode code, std::string message)
{
    return {code, Severity::Error, {}, std::move(message)};
}
std::string path_text(const std::filesystem::path& path)
{
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}
bool enabled(const InvocationContextData& call, std::string_view name)
{
    return *parameter(call, name).as_bool();
}
std::string_view choice(const InvocationContextData& call, std::string_view name)
{
    return parameter(call, name).as_enum()->text();
}
std::int64_t number(const InvocationContextData& call, std::string_view name)
{
    return *parameter(call, name).as_int64();
}
const std::string& text(const InvocationContextData& call, std::string_view name)
{
    return *parameter(call, name).as_string();
}

ImageSaveOptions save_options(const InvocationContextData& call, const std::string& verdict)
{
    ImageSaveOptions options;
    options.date_directories = enabled(call, "date_directory");
    options.sync_storage = enabled(call, "sync_storage");
    options.verdict = verdict;
    options.interval = enabled(call, "trigger_enabled") ? static_cast<std::size_t>(number(call, "save_interval")) : 0;
    options.max_files = static_cast<std::size_t>(number(call, "max_files"));
    options.retention_days = static_cast<unsigned>(number(call, "retention_days"));
    const std::uint64_t mib = 1024 * 1024;
    options.minimum_free_bytes =
        static_cast<std::uint64_t>(number(call, "min_free")) * (choice(call, "space_unit") == "GB" ? mib * 1024 : mib);
    options.overwrite_oldest = choice(call, "storage_policy") == "Overwrite";
    if (enabled(call, "ftp_enabled")) {
        options.ftp = ImageFtpSettings{text(call, "ftp_host"),     static_cast<std::uint16_t>(number(call, "ftp_port")),
                                       text(call, "ftp_user"),     text(call, "ftp_credential"),
                                       text(call, "ftp_encoding"), text(call, "ftp_path")};
    }
    return options;
}

Result<std::monostate> select_directory(const InvocationContextData& call, std::string_view slot, ArchiveImage& image,
                                        ImageArchiveWriter*& writer)
{
    const auto bound = std::ranges::find_if(call.resources, [&](const auto& item) { return item.slot.text() == slot; });
    if (bound == call.resources.end()) {
        if (call.image_archive_writer || (enabled(call, "ftp_enabled") && writer)) {
            return std::monostate{};
        }
        return issue(ErrorCode::MissingRequiredValue, "Select an output directory for " + std::string(slot) + ".");
    }
    const auto* lease = dynamic_cast<const infrastructure::ImageOutputLease*>(bound->lease.get());
    if (!lease) {
        return issue(ErrorCode::ResourceKindMismatch,
                     "Expected an output directory lease for " + std::string(slot) + ".");
    }
    image.directory = lease->directory();
    image.capacity_bytes = lease->capacity_bytes();
    if (!call.image_archive_writer) {
        if (writer && writer != &lease->archive()) {
            return issue(ErrorCode::InvalidResource, "Selected directories must use the same archive service.");
        }
        writer = &lease->archive();
    }
    return std::monostate{};
}

Result<ImageSaveReport> save_images(const InvocationContextData& call, const ImageFrame& original,
                                    const ImageFrame* rendered, const std::string& verdict,
                                    const InvocationCompletion& completion)
{
    const auto options = save_options(call, verdict);
    const auto format = choice(call, "file_format");
    const std::string extension = format == "JPEG" ? "jpg" : format == "BMP" ? "bmp" : "png";
    auto* archive = call.image_archive_writer.get();
    if (!archive && options.ftp) {
        archive = call.image_storage_writer.get();
    }
    std::vector<ArchiveImage> images;
    for (auto kind : {ArchiveKind::Original, ArchiveKind::Rendered}) {
        const std::string name = kind == ArchiveKind::Original ? "original" : "rendered";
        if (!enabled(call, "save_" + name)) {
            continue;
        }
        ArchiveImage image;
        image.kind = kind;
        image.prefix = text(call, name + "_prefix");
        if (image.prefix.empty()) {
            return issue(ErrorCode::InvalidPath, "The " + name + " file prefix is empty.");
        }
        const auto suffix = choice(call, name + "_suffix");
        image.suffix = suffix == "Sequence" ? ArchiveSuffix::Sequence
                       : suffix == "Time"   ? ArchiveSuffix::Time
                                            : ArchiveSuffix::None;
        image.extension = extension;
        image.cache_limit = static_cast<std::size_t>(number(call, name + "_cache"));
        if (options.interval > image.cache_limit) {
            return issue(ErrorCode::ConstraintViolation,
                         "The triggered image interval exceeds the " + name + " cache.");
        }
        auto slot = name;
        if (!verdict.empty()) {
            slot += verdict == "OK" ? "_ok" : "_ng";
        }
        const auto directory = select_directory(call, slot + "_path", image, archive);
        if (const auto* error = std::get_if<Diagnostic>(&directory)) {
            return *error;
        }
        if (completion.cancellation_requested()) {
            return issue(ErrorCode::SourceCancelled, "Image archival was cancelled before encoding.");
        }
        const auto& frame = kind == ArchiveKind::Original ? original : *rendered;
        auto bytes = image_output::encode(frame.image, extension, static_cast<int>(number(call, "jpeg_quality")));
        if (auto* failure = std::get_if<Diagnostic>(&bytes)) {
            return *failure;
        }
        image.bytes = std::get<std::vector<std::uint8_t>>(std::move(bytes));
        images.push_back(std::move(image));
    }
    if (images.empty()) {
        return issue(ErrorCode::MissingRequiredValue, "Select original or rendered images for storage.");
    }
    auto report = archive->save(call.identity, options, std::move(images),
                                [&completion] { return completion.cancellation_requested(); });
    if (report.failure) {
        for (const auto& receipt : report.images) {
            report.failure->message += " Committed: " +
                                       (receipt.local_file ? path_text(*receipt.local_file)
                                        : receipt.package  ? path_text(*receipt.package) + " / " + receipt.name
                                                           : receipt.name) +
                                       ".";
        }
        return *report.failure;
    }
    return report;
}

std::string trigger_verdict(const DataValue& value)
{
    if (const auto number = value.as_int64()) {
        return *number == 1 ? "OK" : *number == 0 ? "NG" : "";
    }
    const std::string_view text = value.as_enum() ? value.as_enum()->text()
        : value.as_string() ? std::string_view{*value.as_string()} : std::string_view{};
    return text == "OK" || text == "NG" ? std::string(text) : std::string{};
}

Result<InvocationStart> run(const InvocationContextData& call, const InvocationCompletion& completion)
{
    const auto started = std::chrono::steady_clock::now();
    const auto& base = *input(call, "image")->as_image();
    const bool publish = enabled(call, "output_enabled");
    const bool triggered = enabled(call, "trigger_enabled");
    const auto condition = choice(call, "save_condition");
    bool save = enabled(call, "save_enabled") && (!triggered || condition != "None");
    std::string verdict;
    if (save && triggered) {
        verdict = trigger_verdict(*input(call, "trigger"));
        if (verdict.empty() && condition != "All") {
            return issue(ErrorCode::InputUnavailable, "Conditional image storage requires an OK/NG judgement, integer 1/0, or string OK/NG.");
        }
        save = condition == "All" || condition == verdict;
    }
    std::optional<ImageFrame> rendered;
    if (publish || (save && enabled(call, "save_rendered"))) {
        auto result = image_output::render(call, base);
        if (auto* failure = std::get_if<Diagnostic>(&result)) {
            return *failure;
        }
        rendered = std::get<ImageFrame>(std::move(result));
    }
    ToolCompletion terminal{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated, {}, {}};
    const auto emit = [&](std::string_view id, DataValue value) {
        terminal.outputs.push_back({key<OutputId>(id), std::move(value)});
    };
    if (publish) {
        emit("image", std::get<DataValue>(DataValue::image(*rendered)));
        emit("image_width", DataValue::integer(static_cast<std::int64_t>(rendered->image.width())));
        emit("image_height", DataValue::integer(static_cast<std::int64_t>(rendered->image.height())));
        emit("pixel_format",
             DataValue::integer(rendered->image.format().order == ChannelOrder::Mono ? 17301505 : 35127316));
    }
    if (save) {
        auto result = save_images(call, base, rendered ? &*rendered : nullptr, verdict, completion);
        if (auto* failure = std::get_if<Diagnostic>(&result)) {
            return *failure;
        }
        std::optional<std::filesystem::path> package;
        for (const auto& receipt : std::get<ImageSaveReport>(result).images) {
            const std::string prefix = receipt.kind == ArchiveKind::Original ? "original" : "rendered";
            emit(prefix + "_name", std::get<DataValue>(DataValue::string(receipt.name)));
            if (receipt.local_file) {
                emit(prefix + "_path",
                     std::get<DataValue>(DataValue::string(path_text(receipt.local_file->parent_path()))));
            }
            if (receipt.package) {
                package = receipt.package;
                emit(prefix + "_big_file_path", std::get<DataValue>(DataValue::string(path_text(*receipt.package))));
            }
        }
        if (package) {
            emit("big_file_path", std::get<DataValue>(DataValue::string(path_text(*package))));
        }
    }
    emit("module_status", DataValue::integer(1));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    terminal.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    emit("elapsed_ms",
         std::get<DataValue>(DataValue::real(std::chrono::duration<double, std::milli>(elapsed).count())));
    for (const auto& port : call.configuration.data().tool.outputs) {
        if (std::ranges::find(terminal.outputs, port.id, &ToolOutput::id) == terminal.outputs.end()) {
            const auto name = port.id.text();
            const bool selected = (name.starts_with("original_") && enabled(call, "save_original"))
                || (name.starts_with("rendered_") && enabled(call, "save_rendered"))
                || name == "big_file_path";
            const bool requested = selected && (!name.ends_with("big_file_path") || call.image_archive_writer);
            terminal.outputs.push_back(
                {port.id, NotProduced{save && requested ? OutputAbsenceReason::ToolDidNotProduce
                                                        : OutputAbsenceReason::ConditionNotMet}});
        }
    }
    return InvocationStart{std::move(terminal)};
}

class OutputTool final : public ToolInstance
{
  public:
    explicit OutputTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}

  private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context,
                                  InvocationCompletion completion) override
    {
        try {
            return run(context->data(), completion);
        } catch (const cv::Exception& error) {
            return opencv_error(ErrorCode::ExecutionBoundaryFailure, "image output", error);
        }
    }
};
class OutputFactory final : public ToolInstanceFactory
{
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        return std::shared_ptr<ToolInstance>{std::make_shared<OutputTool>(configuration)};
    }
};
} // namespace

core::ToolRegistration image_output_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<OutputFactory>()};
}
} // namespace visionpro::tools::vision
