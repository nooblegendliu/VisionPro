#include "inspection.hpp"
#include "opencv_image.hpp"
#include "read_file.hpp"
#include "../image_source_pixels.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/file_resources.hpp>

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace visionpro::tools::vision {
namespace {
using namespace core;
constexpr auto pixels = MeasurementUnit::Pixel;
constexpr PixelFormat bgr8{PixelDepth::UInt8, ChannelOrder::BGR};
constexpr PixelFormat rgb8{PixelDepth::UInt8, ChannelOrder::RGB};

Result<cv::Mat> inspection_matrix(const ImageHandle& image)
{
    if (image.format() != rgb8) { return to_cv_mat(image); }
    auto converted = image_source::convert(image, ChannelOrder::BGR);
    if (const auto* issue = std::get_if<Diagnostic>(&converted)) { return *issue; }
    return to_cv_mat(std::get<ImageHandle>(converted));
}

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

template<SchemaIdentity Id> Id key(std::string_view name)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

bool constant_pattern(const cv::Mat& pattern)
{
    const auto channels = pattern.channels();
    const auto* first = pattern.ptr<unsigned char>();
    for (int y = 0; y < pattern.rows; ++y) {
        const auto* row = pattern.ptr<unsigned char>(y);
        for (int x = 0; x < pattern.cols; ++x, row += channels) {
            if (!std::equal(row, row + channels, first)) { return false; }
        }
    }
    return true;
}
} // namespace

Result<TranslationMatch> locate_translation(const ImageHandle& scene, const ImageHandle& pattern,
    const Point2D& reference_origin, double minimum_score)
{
    if (auto valid = validate_point(reference_origin); auto* failure = std::get_if<Diagnostic>(&valid)) { return *failure; }
    if (reference_origin.unit != pixels) {
        return error(ErrorCode::CoordinateMismatch, "Reference top-left must use pixel coordinates.");
    }
    if (!std::isfinite(minimum_score) || minimum_score < 0 || minimum_score > 1) {
        return error(ErrorCode::ConstraintViolation, "Minimum correlation must be in [0, 1].");
    }
    const auto normalized = [](PixelFormat format) { return format == rgb8 ? bgr8 : format; };
    if (normalized(pattern.format()) != normalized(scene.format())) {
        return error(ErrorCode::UnsupportedPixelFormat, "Template and search image require matching monochrome or color formats.");
    }
    if (pattern.width() > scene.width() || pattern.height() > scene.height()) {
        return error(ErrorCode::ImageBounds, "Template dimensions exceed the search image.");
    }
    auto templ = inspection_matrix(pattern);
    if (const auto* failure = std::get_if<Diagnostic>(&templ)) { return *failure; }
    const auto& patch = std::get<cv::Mat>(templ);
    if (constant_pattern(patch)) {
        return error(ErrorCode::InvalidImage, "Normalized correlation requires a spatially varying template.");
    }
    auto source = inspection_matrix(scene);
    if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
    try {
        cv::Mat correlations;
        const auto& search = std::get<cv::Mat>(source);
        cv::matchTemplate(search, patch, correlations, cv::TM_CCOEFF_NORMED);
        double best{};
        cv::Point hit;
        cv::minMaxLoc(correlations, nullptr, &best, nullptr, &hit);
        const auto candidate = search(cv::Rect{hit, patch.size()});
        // Normalized correlation is undefined for a flat search window. OpenCV reports zero.
        TranslationMatch result{best, {}, {}};
        if (constant_pattern(candidate)) { return result; }
        // An identical patch can score a few Float32 ULPs below 1 after convolution.
        if (cv::norm(candidate, patch, cv::NORM_INF) == 0) { result.score = 1; }
        if (result.score < minimum_score) { return result; }
        if (correlations.total() > 1) {
            correlations.at<float>(hit) = -std::numeric_limits<float>::infinity();
            double second{};
            cv::minMaxLoc(correlations, nullptr, &second);
            if (best - second <= 1e-6) { return result; }
        }
        result.position = Point2D{scene.coordinate_frame(), pixels, static_cast<double>(hit.x), static_cast<double>(hit.y)};
        result.transform = Transform2D{reference_origin.coordinate_frame, scene.coordinate_frame(), pixels, pixels,
            TransformModel::Similarity, {1, 0, hit.x - reference_origin.x, 0, 1, hit.y - reference_origin.y, 0, 0, 1}};
        return result;
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "matchTemplate", failure);
    }
}

Result<std::array<double, 3>> mean_bgr(const ImageHandle& image, const RasterMask& mask)
{
    if (image.format() != bgr8 && image.format() != rgb8) { return error(ErrorCode::UnsupportedPixelFormat, "Color measurement requires UInt8 RGB or BGR."); }
    if (auto checked = validate_raster_mask(mask, image); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    // This operation consumes the direct nonzero membership mask produced by the ROI evaluator.
    if (mask.mapping.transform || mask.mapping.sampling != MaskSampling::None || mask.valid_pixels != MaskValueRule{}
        || mask.pixels.format() != PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) {
        return error(ErrorCode::InvalidMask, "Color measurement requires the evaluated target-space membership mask.");
    }
    auto source = inspection_matrix(image);
    if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
    auto membership = to_cv_mat(mask.pixels);
    if (const auto* failure = std::get_if<Diagnostic>(&membership)) { return *failure; }
    try {
        const auto& members = std::get<cv::Mat>(membership);
        if (!cv::hasNonZero(members)) { return error(ErrorCode::EmptyRegion, "A color mean requires at least one ROI member."); }
        const auto result = cv::mean(std::get<cv::Mat>(source), members);
        return std::array{result[0], result[1], result[2]};
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "mean", failure);
    }
}

namespace {
const DataValue& input(const InvocationContextData& call, std::string_view name)
{ return std::ranges::find(call.inputs, name, [](const auto& v){ return v.id.text(); })->value; }

const FixedInvocationParameter& parameter(const InvocationContextData& call, std::string_view name)
{ return *std::ranges::find(call.parameters, name, [](const auto& v){ return v.id.text(); }); }

Result<ToolCompletion> locate(const InvocationContextData& call)
{
    const auto found = std::ranges::find(call.resources, "template", [](const auto& lease) { return lease.slot.text(); });
    if (found == call.resources.end()) { return error(ErrorCode::InvalidResource, "Template location requires a file lease."); }
    const auto& reference = found->lease->reference();
    const auto file = std::dynamic_pointer_cast<const infrastructure::FileResourceLease>(found->lease);
    if (!file || !reference.revision) {
        auto failure = error(file ? ErrorCode::ResourceVersionMismatch : ErrorCode::InvalidResource,
            "Template location requires a versioned template file lease.");
        failure.location.resource = reference.id;
        return failure;
    }
    const auto& origin = *parameter(call, "reference_origin").value.as_point2d();
    auto patch = decode_file_image(file->bytes(), FileImageDecodeMode::Native8, origin.coordinate_frame);
    if (auto* failure = std::get_if<Diagnostic>(&patch)) { failure->location.resource = reference.id; return *failure; }
    auto matched = locate_translation(input(call, "image").as_image()->image, std::get<ImageHandle>(patch),
        origin, *parameter(call, "minimum_score").value.as_float64());
    if (auto* failure = std::get_if<Diagnostic>(&matched)) { failure->location.resource = reference.id; return *failure; }
    const auto& match = std::get<TranslationMatch>(matched);
    const bool present = match.position.has_value();
    const NotProduced absent{OutputAbsenceReason::ConditionNotMet};
    ToolCompletion result{call.identity, ToolCompletionStatus::Succeeded, present ? ToolJudgement::OK : ToolJudgement::NG,
        {{key<OutputId>("matched"), DataValue::boolean(present)},
         {key<OutputId>("score"), std::get<DataValue>(DataValue::real(match.score))},
         {key<OutputId>("position"), present ? OutputContent{std::get<DataValue>(DataValue::point2d(*match.position))} : OutputContent{absent}},
         {key<OutputId>("transform"), present ? OutputContent{std::get<DataValue>(DataValue::transform2d(*match.transform))} : OutputContent{absent}}}, {}};
    result.selected_control_outputs.push_back(key<ControlOutputId>(present ? "matched" : "unmatched"));
    return result;
}

Result<ToolCompletion> color_mean(const InvocationContextData& call)
{
    const auto& image = input(call, "image").as_image()->image;
    if (image.format() != bgr8 && image.format() != rgb8) {
        auto failure = error(ErrorCode::UnsupportedPixelFormat, "Color measurement requires UInt8 RGB or BGR.");
        failure.location.input = key<InputId>("image");
        return failure;
    }
    const auto& roi = parameter(call, "roi");
    auto evaluated = evaluate_roi(image, *roi.value.as_region2d(), roi.id, call.regions, call.resources,
        *input(call, "placement").as_transform2d());
    if (const auto* failure = std::get_if<Diagnostic>(&evaluated)) { return *failure; }
    auto& region = std::get<RoiEvaluation>(evaluated);
    if (const auto* failure = std::get_if<Diagnostic>(&region.effective_region)) { return *failure; }
    auto color = mean_bgr(image, region.mask);
    if (auto* failure = std::get_if<Diagnostic>(&color)) { failure->location.parameter = roi.id; return *failure; }
    const auto& [blue, green, red] = std::get<std::array<double, 3>>(color);
    return ToolCompletion{call.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
        {{key<OutputId>("blue"), std::get<DataValue>(DataValue::real(blue))},
         {key<OutputId>("green"), std::get<DataValue>(DataValue::real(green))},
         {key<OutputId>("red"), std::get<DataValue>(DataValue::real(red))},
         {key<OutputId>("effective_roi"), std::get<DataValue>(DataValue::region2d(std::get<RegionDefinition>(region.effective_region)))},
         {key<OutputId>("roi_mask"), std::get<DataValue>(DataValue::raster_mask(std::move(region.mask)))}}, {}};
}

using Inspect = Result<ToolCompletion> (*)(const InvocationContextData&);
class InspectionTool final : public ToolInstance {
public:
    InspectionTool(const ToolExecutionConfiguration& config, Inspect inspect) : ToolInstance(config), inspect_(inspect) {}
private:
    Inspect inspect_;
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> call, InvocationCompletion) override
    {
        auto result = inspect_(call->data());
        if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
        return InvocationStart{std::get<ToolCompletion>(std::move(result))};
    }
};

class InspectionFactory final : public ToolInstanceFactory {
public:
    explicit InspectionFactory(Inspect inspect) : inspect_(inspect) {}
private:
    Inspect inspect_;
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& config) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<InspectionTool>(config, inspect_)}; }
};
}

ToolRegistration locate_registration(ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<InspectionFactory>(locate)};
}

ToolRegistration color_mean_registration(ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<InspectionFactory>(color_mean)};
}
} // namespace visionpro::tools::vision
