#include "read_file.hpp"

#include "opencv_image.hpp"
#include "vision_tools.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace visionpro::tools::vision {
namespace {
using namespace core;

enum class DecodeMode { Gray8, Native8 };
constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr PixelFormat bgr8{PixelDepth::UInt8, ChannelOrder::BGR};

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Result<ImageHandle> decode_image(std::span<const std::byte> bytes, DecodeMode mode, const CoordinateSystemId& coordinates)
{
    auto decoded = decode_image_bytes(bytes);
    if (auto* failure = std::get_if<Diagnostic>(&decoded)) { return std::move(*failure); }
    const auto& pixels = std::get<cv::Mat>(decoded);
    if (pixels.type() == CV_8UC1) { return copy_from_cv_mat(pixels, mono8, coordinates); }
    if (pixels.type() != CV_8UC3) {
        return error(ErrorCode::UnsupportedPixelFormat, "Decoded images must be UInt8 Mono or BGR.");
    }
    if (mode == DecodeMode::Native8) { return copy_from_cv_mat(pixels, bgr8, coordinates); }
    cv::Mat gray;
    try {
        cv::cvtColor(pixels, gray, cv::COLOR_BGR2GRAY);
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::InvalidImage, "cvtColor", failure);
    }
    return copy_from_cv_mat(gray, mono8, coordinates);
}

class ReadFileTool final : public ToolInstance {
public:
    ReadFileTool(const ToolExecutionConfiguration& configuration, CoordinateSystemId coordinates, DecodeMode mode)
        : ToolInstance(configuration), coordinates_(std::move(coordinates)), mode_(mode) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto source = std::ranges::find(invocation.resources, "source",
            [](const InvocationResourceLease& binding) { return binding.slot.text(); });
        if (source == invocation.resources.end()) {
            return error(ErrorCode::InvalidResource, "Image reader requires a lease in the source slot.");
        }
        const auto file = std::dynamic_pointer_cast<const infrastructure::FileResourceLease>(source->lease);
        if (!file) {
            auto failure = error(ErrorCode::InvalidResource, "Image reader requires an owning file lease.");
            failure.location.resource = source->lease->reference().id;
            return failure;
        }
        auto decoded = decode_image(file->bytes(), mode_, coordinates_);
        if (auto* failure = std::get_if<Diagnostic>(&decoded)) {
            failure->location.resource = file->reference().id;
            return std::move(*failure);
        }
        auto value = DataValue::image({std::get<ImageHandle>(std::move(decoded)), {}, {}});
        if (auto* failure = std::get_if<Diagnostic>(&value)) { return std::move(*failure); }
        const auto output = std::get<OutputId>(SchemaIdFactory::parse<OutputId>("image"));
        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded,
            ToolJudgement::NotEvaluated, {{output, std::get<DataValue>(std::move(value))}}, {}}};
    }

    CoordinateSystemId coordinates_;
    DecodeMode mode_;
};

class ReadFileFactory final : public ToolInstanceFactory {
public:
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    {
        const auto& parameters = configuration.data().parameters;
        const auto fixed = [&](std::string_view id) -> const ConfigValue* {
            const auto value = std::ranges::find(parameters, id,
                [](const FixedToolParameter& parameter) { return parameter.id.text(); });
            return value == parameters.end() ? nullptr : &value->value;
        };
        const auto* origin = fixed("frame_origin");
        const auto* mode = fixed("decode_mode");
        if (!origin || !origin->as_point2d() || !mode || !mode->as_enum()) {
            return error(ErrorCode::InvalidDescriptor, "Image reader requires a fixed Point2D origin and decode mode.");
        }
        const auto& point = *origin->as_point2d();
        if (auto checked = validate_file_frame_origin(point); const auto* failure = std::get_if<Diagnostic>(&checked)) {
            return *failure;
        }
        const auto decode_mode = mode->as_enum()->text() == "Gray8" ? DecodeMode::Gray8 : DecodeMode::Native8;
        return std::shared_ptr<ToolInstance>{std::make_shared<ReadFileTool>(configuration, point.coordinate_frame, decode_mode)};
    }
};
} // namespace

core::ToolRegistration read_file_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<ReadFileFactory>()};
}

} // namespace visionpro::tools::vision
