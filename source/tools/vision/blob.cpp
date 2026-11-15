#include "blob.hpp"

#include "opencv_image.hpp"
#include "roi.hpp"
#include "roi_mask.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <limits>

namespace visionpro::tools::vision {
namespace {
using namespace core;

template<SchemaIdentity Id> Id key(std::string_view spelling)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(spelling)); }

struct LabelStatistics {
    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    int label_count{};
};

Result<LabelStatistics> measure(const ImageHandle& image, const std::optional<RasterMask>& mask, int connectivity)
{
    auto source = to_cv_mat(image);
    if (auto* failure = std::get_if<Diagnostic>(&source)) { return std::move(*failure); }
    auto& pixels = std::get<cv::Mat>(source);
    if (mask) {
        auto membership = to_cv_mat(mask->pixels);
        if (auto* failure = std::get_if<Diagnostic>(&membership)) { return std::move(*failure); }
        try {
            // A 0/255 membership mask preserves any nonzero foreground value inside the ROI.
            cv::bitwise_and(pixels, std::get<cv::Mat>(membership), pixels);
        } catch (const cv::Exception& failure) {
            return opencv_error(ErrorCode::ExecutionBoundaryFailure, "bitwise_and", failure);
        }
    }

    LabelStatistics result;
    try {
        result.label_count = cv::connectedComponentsWithStats(pixels, result.labels, result.stats, result.centroids,
            connectivity, CV_32S);
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::ExecutionBoundaryFailure, "connectedComponentsWithStats", failure);
    }
    return result;
}

std::vector<int> labels_in_pixel_order(const LabelStatistics& measured)
{
    std::vector<int> order;
    order.reserve(static_cast<std::size_t>(measured.label_count - 1));
    std::vector<bool> seen(static_cast<std::size_t>(measured.label_count));
    seen.front() = true; // Skip the background label.
    for (int y = 0; y < measured.labels.rows; ++y) {
        const auto* row = measured.labels.ptr<int>(y);
        for (int x = 0; x < measured.labels.cols; ++x) {
            const auto label = row[x];
            const auto index = static_cast<std::size_t>(label);
            if (!seen[index]) {
                seen[index] = true;
                order.push_back(label);
            }
        }
    }
    return order;
}

Result<DataValue> component_record(const DataType& component_type, const DataType& bounds_type,
    const CoordinateSystemId& frame, const int* stats, const double* centroid)
{
    const auto pixels = MeasurementUnit::Pixel;
    auto center = DataValue::point2d({frame, pixels, centroid[0], centroid[1]});
    if (auto* failure = std::get_if<Diagnostic>(&center)) { return std::move(*failure); }
    auto origin = DataValue::point2d({frame, pixels, static_cast<double>(stats[cv::CC_STAT_LEFT]),
        static_cast<double>(stats[cv::CC_STAT_TOP])});
    if (auto* failure = std::get_if<Diagnostic>(&origin)) { return std::move(*failure); }
    auto bounds = DataValue::record(bounds_type, {
        {key<FieldId>("origin"), std::get<DataValue>(std::move(origin))},
        {key<FieldId>("width"), DataValue::integer(stats[cv::CC_STAT_WIDTH])},
        {key<FieldId>("height"), DataValue::integer(stats[cv::CC_STAT_HEIGHT])}});
    if (auto* failure = std::get_if<Diagnostic>(&bounds)) { return std::move(*failure); }
    return DataValue::record(component_type, {
        {key<FieldId>("area_pixels"), DataValue::integer(stats[cv::CC_STAT_AREA])},
        {key<FieldId>("centroid"), std::get<DataValue>(std::move(center))},
        {key<FieldId>("bounds"), std::get<DataValue>(std::move(bounds))}});
}

Result<InvocationStart> complete(const InvocationContextData& invocation, const ImageHandle& image,
    const LabelStatistics& measured)
{
    const auto components_id = key<OutputId>("components");
    const auto& outputs = invocation.configuration.data().tool.outputs;
    const auto& component_type = *std::ranges::find(outputs, components_id, &OutputDescriptor::id)->type.element_type();
    const auto& bounds_type = std::ranges::find(component_type.fields(), key<FieldId>("bounds"), &FieldSchema::id)->type;
    const auto order = labels_in_pixel_order(measured);
    std::vector<DataValue> records;
    records.reserve(order.size());
    std::int64_t total_area = 0;
    std::int64_t largest_area = 0;
    for (const auto label : order) {
        const auto* stats = measured.stats.ptr<int>(label);
        const auto area = static_cast<std::int64_t>(stats[cv::CC_STAT_AREA]);
        auto record = component_record(component_type, bounds_type, image.coordinate_frame(), stats,
            measured.centroids.ptr<double>(label));
        if (auto* failure = std::get_if<Diagnostic>(&record)) { return std::move(*failure); }
        records.push_back(std::get<DataValue>(std::move(record)));
        total_area += area;
        largest_area = std::max(largest_area, area);
    }
    auto components = DataValue::list(component_type, records);
    if (auto* failure = std::get_if<Diagnostic>(&components)) { return std::move(*failure); }
    auto largest = records.empty() ? DataValue::none(DataType::integer()) : DataValue::some(DataValue::integer(largest_area));

    return InvocationStart{ToolCompletion{invocation.identity, ToolCompletionStatus::Succeeded, ToolJudgement::NotEvaluated,
        {{key<OutputId>("count"), DataValue::integer(static_cast<std::int64_t>(records.size()))},
            {components_id, std::get<DataValue>(std::move(components))},
            {key<OutputId>("total_area_pixels"), DataValue::integer(total_area)},
            {key<OutputId>("largest_area_pixels"), std::move(largest)}}, {}}};
}

class BlobTool final : public ToolInstance {
public:
    explicit BlobTool(const ToolExecutionConfiguration& configuration) : ToolInstance(configuration) {}

private:
    Result<InvocationStart> begin(std::shared_ptr<const InvocationContext> context, InvocationCompletion) override
    {
        const auto& invocation = context->data();
        const auto& input = *std::ranges::find(invocation.inputs, key<InputId>("binary"), &FixedToolInput::id);
        const auto& image = input.value.as_image()->image;
        if (auto checked = validate_image_support(invocation.configuration.data().tool, image);
            auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.input = input.id;
            return std::move(*failure);
        }
        // The OpenCV statistics and labels use signed 32-bit integers, including background.
        constexpr auto max_pixels = static_cast<std::size_t>(std::numeric_limits<int>::max()) - 1;
        if (image.width() > max_pixels / image.height()) {
            DiagnosticLocation location;
            location.input = input.id;
            return Diagnostic{ErrorCode::SizeOverflow, Severity::Error, std::move(location),
                "Image pixel count exceeds the range of OpenCV connected-component statistics."};
        }

        const auto& roi = *std::ranges::find(invocation.parameters, key<ParameterId>("roi"), &FixedInvocationParameter::id);
        std::optional<RasterMask> mask;
        if (const auto* configured = roi.value.optional_value()) {
            auto evaluated = evaluate_roi(image, *configured->as_region2d(), ToolValueTarget{roi.id},
                invocation.regions, invocation.resources);
            if (auto* failure = std::get_if<Diagnostic>(&evaluated)) { return std::move(*failure); }
            mask = std::move(std::get<RoiEvaluation>(evaluated).mask);
        } else if (auto checked = RoiMaskResource{invocation.resources}.finish(); auto* failure = std::get_if<Diagnostic>(&checked)) {
            failure->location.parameter = roi.id;
            return std::move(*failure);
        }

        const auto& connectivity = *std::ranges::find(invocation.parameters, key<ParameterId>("connectivity"), &FixedInvocationParameter::id);
        auto measured = measure(image, mask, static_cast<int>(*connectivity.value.as_int64()));
        if (auto* failure = std::get_if<Diagnostic>(&measured)) { return std::move(*failure); }
        return complete(invocation, image, std::get<LabelStatistics>(measured));
    }
};

class BlobFactory final : public ToolInstanceFactory {
    Result<std::shared_ptr<ToolInstance>> create(const ToolExecutionConfiguration& configuration) override
    { return std::shared_ptr<ToolInstance>{std::make_shared<BlobTool>(configuration)}; }
};
} // namespace

core::ToolRegistration blob_registration(core::ToolDescriptor descriptor)
{
    const auto schema = descriptor.schema.id;
    return {std::move(descriptor), {schema, 1, 1, {}}, std::make_shared<BlobFactory>()};
}

} // namespace visionpro::tools::vision
