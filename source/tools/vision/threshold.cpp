#include "threshold.hpp"

#include "opencv_image.hpp"
#include "roi.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace visionpro::tools::vision {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

Result<ImageFrame> threshold_image(const ImageFrame& image, const RasterMask& mask, std::int64_t cutoff, int mode)
{
    auto source = to_cv_mat(image.image);
    if (auto* failure = std::get_if<Diagnostic>(&source)) { return std::move(*failure); }
    auto membership = to_cv_mat(mask.pixels);
    if (auto* failure = std::get_if<Diagnostic>(&membership)) { return std::move(*failure); }

    const auto& input = std::get<cv::Mat>(source);
    cv::Mat binary;
    try {
        cv::threshold(input, binary, static_cast<double>(cutoff), 255, mode);
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "threshold", failure);
    }
    try {
        // The evaluator publishes a 0/255 mask, so this keeps ROI pixels and clears the rest.
        cv::bitwise_and(binary, std::get<cv::Mat>(membership), binary);
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "bitwise_and", failure);
    }
    return copy_from_cv_mat(binary, image.image.format(), image);
}

class ThresholdTool final : public ToolInstance {
public:
    explicit ThresholdTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        // The context contains each declared input and parameter with its exact type.
        const auto& input = *std::ranges::find(invocation.inputs, key<InputId>("image"), &FixedToolInput::id);
        const auto& image = *input.value.as_image();
        if (auto checked = validate_image_support(invocation.configuration.data().tool, image.image);
            auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.input = input.id;
            return std::move(*failure);
        }
        const auto parameter = [&](std::string_view name) -> const FixedInvocationParameter& {
            return *std::ranges::find(invocation.parameters, key<ParameterId>(name), &FixedInvocationParameter::id);
        };
        const auto& threshold = parameter("threshold");
        const auto cutoff = *threshold.value.as_int64();
        if (cutoff < 0 || cutoff > 255) {
            DiagnosticLocation location;
            location.parameter = threshold.id;
            return Diagnostic{ErrorCode::ConstraintViolation, Severity::Error, std::move(location),
                "UInt8 threshold requires an integer in [0, 255]."};
        }
        const auto& mode_parameter = parameter("mode");
        const auto mode = mode_parameter.value.as_enum()->text();
        if (mode != "Binary" && mode != "BinaryInverse") {
            DiagnosticLocation location;
            location.parameter = mode_parameter.id;
            return Diagnostic{ErrorCode::InvalidEnumValue, Severity::Error, std::move(location), "Unsupported threshold mode."};
        }
        const auto& roi = parameter("roi");
        auto evaluated = evaluate_roi(image.image, *roi.value.as_region2d(), ToolValueTarget{roi.id},
            invocation.regions, invocation.resources);
        if (auto* failure = std::get_if<Diagnostic>(&evaluated)) { return std::move(*failure); }
        auto& region = std::get<RoiEvaluation>(evaluated);
        if (auto* failure = std::get_if<Diagnostic>(&region.effective_region)) { return std::move(*failure); }
        auto effective_roi = DataValue::region2d(std::get<RegionDefinition>(region.effective_region));
        if (auto* failure = std::get_if<Diagnostic>(&effective_roi)) { return std::move(*failure); }

        auto thresholded = threshold_image(image, region.mask, cutoff,
            mode == "Binary" ? cv::THRESH_BINARY : cv::THRESH_BINARY_INV);
        if (auto* failure = std::get_if<Diagnostic>(&thresholded)) { return std::move(*failure); }
        auto binary = DataValue::image(std::get<ImageFrame>(std::move(thresholded)));
        if (auto* failure = std::get_if<Diagnostic>(&binary)) { return std::move(*failure); }
        auto mask = DataValue::raster_mask(std::move(region.mask));
        if (auto* failure = std::get_if<Diagnostic>(&mask)) { return std::move(*failure); }

        return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
            {{key<OutputId>("binary"), std::get<DataValue>(std::move(binary))},
                {key<OutputId>("effective_roi"), std::get<DataValue>(std::move(effective_roi))},
                {key<OutputId>("roi_mask"), std::get<DataValue>(std::move(mask))}}, {}}};
    }
};

class ThresholdFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<ThresholdTool>(configuration)}; }
};
} // namespace

core::ToolRegistration threshold_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<ThresholdFactory>()};
}

} // namespace visionpro::tools::vision
