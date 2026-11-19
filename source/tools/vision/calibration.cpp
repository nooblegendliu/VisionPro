#include "calibration.hpp"
#include "opencv_image.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace visionpro::tools::vision {
namespace {
using namespace core;
using Json = nlohmann::json;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

struct Normalization {
    cv::Point2d center;
    double scale;

    cv::Point2d apply(cv::Point2d point) const { return (point - center) * scale; }
    cv::Matx33d matrix() const
    { return {scale, 0, -center.x * scale, 0, scale, -center.y * scale, 0, 0, 1}; }
    cv::Matx33d inverse() const
    { return {1 / scale, 0, center.x, 0, 1 / scale, center.y, 0, 0, 1}; }
};

Result<Normalization> normalize(const std::vector<cv::Point2d>& points)
{
    if (std::ranges::all_of(points, [&](const auto& p) { return p == points.front(); })) {
        return error(ErrorCode::InvalidTransform, "Calibration requires distinct point coordinates.");
    }
    cv::Point2d center{};
    for (const auto& point : points) { center += point * (1.0 / points.size()); }
    double mean_radius = 0;
    for (const auto& point : points) {
        mean_radius += std::hypot(point.x - center.x, point.y - center.y) / points.size();
    }
    const double scale = std::sqrt(2.0) / mean_radius;
    if (!(scale > 0) || !std::isfinite(scale)) {
        return error(ErrorCode::InvalidTransform, "Calibration observations have no usable spatial extent.");
    }
    return Normalization{center, scale};
}

bool has_rank(const cv::Mat& singular_values, int rank)
{
    return singular_values.rows >= rank && singular_values.at<double>(rank - 1) > 1e-12 * singular_values.at<double>(0);
}

// Normalize both coordinate systems before testing rank. DLT uses the full
// homogeneous solution, including valid homographies whose bottom-right entry is zero.
Result<cv::Matx33d> fit_matrix(TransformModel model, const std::vector<cv::Point2d>& pixels,
    const std::vector<cv::Point2d>& physical)
{
    const auto normalized_from = normalize(pixels);
    const auto normalized_to = normalize(physical);
    if (const auto* failure = std::get_if<Diagnostic>(&normalized_from)) { return *failure; }
    if (const auto* failure = std::get_if<Diagnostic>(&normalized_to)) { return *failure; }
    const auto& from = std::get<Normalization>(normalized_from);
    const auto& to = std::get<Normalization>(normalized_to);

    if (model == TransformModel::Similarity) {
        // Centering removes translation. Fit rotation/scale and reflection/scale
        // separately because the image and physical frames can have opposite handedness.
        double energy = 0;
        cv::Point2d direct{}, reflected{};
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto p = from.apply(pixels[i]);
            const auto q = to.apply(physical[i]);
            energy += p.dot(p);
            direct += cv::Point2d(p.x * q.x + p.y * q.y, p.x * q.y - p.y * q.x);
            reflected += cv::Point2d(p.x * q.x - p.y * q.y, p.x * q.y + p.y * q.x);
        }
        // The larger correlation minimizes the squared residual. Two distinct
        // pairs cannot distinguish handedness, so choose the direct similarity on ties.
        const bool reverse = pixels.size() > 2 && cv::norm(reflected) > cv::norm(direct) * (1 + 1e-12);
        const auto v = (reverse ? reflected : direct) * (1.0 / energy);
        const cv::Matx33d fitted(v.x, reverse ? v.y : -v.y, 0, v.y, reverse ? -v.x : v.x, 0, 0, 0, 1);
        return to.inverse() * fitted * from.matrix();
    }
    const bool projective = model == TransformModel::Projective;
    const int columns = projective ? 9 : 6;
    const int rows = static_cast<int>(pixels.size() * 2);
    cv::Mat design = cv::Mat::zeros(rows, columns, CV_64F);
    cv::Mat target(rows, 1, CV_64F);
    for (int i = 0; i < rows / 2; ++i) {
        const auto p = from.apply(pixels[i]);
        const auto q = to.apply(physical[i]);
        auto* x = design.ptr<double>(2 * i);
        auto* y = design.ptr<double>(2 * i + 1);
        x[0] = p.x; x[1] = p.y; x[2] = 1;
        y[3] = p.x; y[4] = p.y; y[5] = 1;
        if (projective) {
            x[6] = -q.x * p.x; x[7] = -q.x * p.y; x[8] = -q.x;
            y[6] = -q.y * p.x; y[7] = -q.y * p.y; y[8] = -q.y;
        }
        target.at<double>(2 * i) = q.x;
        target.at<double>(2 * i + 1) = q.y;
    }
    const cv::SVD svd(design, rows < columns ? cv::SVD::FULL_UV : 0);
    if (!has_rank(svd.w, projective ? 8 : columns)) {
        return error(ErrorCode::InvalidTransform, "Calibration point pairs do not constrain the selected planar model.");
    }
    cv::Matx33d fitted = cv::Matx33d::eye();
    if (projective) {
        std::copy_n(svd.vt.ptr<double>(8), 9, fitted.val);
    } else {
        cv::Mat solution;
        svd.backSubst(target, solution);
        std::copy_n(solution.ptr<double>(), 6, fitted.val);
    }
    if (!has_rank(cv::SVD(cv::Mat(fitted), cv::SVD::NO_UV).w, 3)) {
        return error(ErrorCode::InvalidTransform, "Calibration produces a singular coordinate mapping.");
    }
    return to.inverse() * fitted * from.matrix();
}

Result<CalibrationFit> read_fit(const Json& json)
{
    if (!json.is_object() || !json.at("format_version").is_number_integer() || json.at("format_version") != 1) {
        return error(ErrorCode::IncompatibleSchemaVersion, "Expected planar calibration format_version 1.");
    }
    if (json.size() != 8) { return error(ErrorCode::InvalidResource, "Unexpected fields in planar calibration resource."); }
    if (json.at("from_unit") != "Pixel" || json.at("to_unit") != "Millimetre") {
        return error(ErrorCode::CoordinateMismatch, "Planar calibration requires Pixel and Millimetre endpoints.");
    }
    auto from = SchemaIdFactory::parse<CoordinateSystemId>(json.at("from").get<std::string>());
    auto to = SchemaIdFactory::parse<CoordinateSystemId>(json.at("to").get<std::string>());
    if (const auto* failure = std::get_if<Diagnostic>(&from)) { return *failure; }
    if (const auto* failure = std::get_if<Diagnostic>(&to)) { return *failure; }
    Transform2D transform{std::get<CoordinateSystemId>(from), std::get<CoordinateSystemId>(to),
        MeasurementUnit::Pixel, MeasurementUnit::Millimetre};
    const auto& model = json.at("model");
    if (model == "Similarity") { transform.model = TransformModel::Similarity; }
    else if (model == "Affine") { transform.model = TransformModel::Affine; }
    else if (model == "Projective") { transform.model = TransformModel::Projective; }
    else { return error(ErrorCode::InvalidTransform, "Unknown planar calibration model."); }
    const auto& bounds = json.at("domain");
    if (!bounds.is_array() || bounds.size() != 4) {
        return error(ErrorCode::InvalidTransform, "Calibration domain requires four pixel bounds.");
    }
    const TransformDomain domain{bounds.at(0).get<double>(), bounds.at(1).get<double>(),
        bounds.at(2).get<double>(), bounds.at(3).get<double>()};
    transform.valid_domain = domain;
    if (auto checked = validate_transform(transform); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }

    const auto& pairs = json.at("pairs");
    const unsigned minimum = transform.model == TransformModel::Similarity ? 2 : (transform.model == TransformModel::Affine ? 3 : 4);
    if (!pairs.is_array() || pairs.size() < minimum || pairs.size() > std::numeric_limits<int>::max() / 2) {
        return error(ErrorCode::InvalidTransform, "Invalid number of point pairs for the selected calibration model.");
    }
    std::vector<cv::Point2d> pixels, physical;
    for (const auto& pair : pairs) {
        if (!pair.is_array() || pair.size() != 2 || !pair[0].is_array() || pair[0].size() != 2
            || !pair[1].is_array() || pair[1].size() != 2) {
            return error(ErrorCode::InvalidResource, "Each calibration pair requires a pixel point and a millimetre point.");
        }
        pixels.emplace_back(pair[0][0].get<double>(), pair[0][1].get<double>());
        physical.emplace_back(pair[1][0].get<double>(), pair[1][1].get<double>());
        const auto& p = pixels.back();
        if (p.x < domain.minimum_x || p.x > domain.maximum_x || p.y < domain.minimum_y || p.y > domain.maximum_y) {
            return error(ErrorCode::InvalidTransform, "Calibration observation lies outside its declared pixel domain.");
        }
    }
    auto matrix = fit_matrix(transform.model, pixels, physical);
    if (const auto* failure = std::get_if<Diagnostic>(&matrix)) { return *failure; }
    std::copy_n(std::get<cv::Matx33d>(matrix).val, 9, transform.matrix.begin());
    if (auto checked = validate_transform(transform); const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    if (transform.model == TransformModel::Projective) {
        // A linear homogeneous denominator has its extrema at rectangle corners.
        const auto& m = transform.matrix;
        std::array<double, 4> weights{};
        std::size_t i = 0;
        for (double x : {domain.minimum_x, domain.maximum_x}) {
            for (double y : {domain.minimum_y, domain.maximum_y}) { weights[i++] = m[6] * x + m[7] * y + m[8]; }
        }
        const auto [low, high] = std::ranges::minmax(weights);
        if (low <= 0 && high >= 0) {
            return error(ErrorCode::InvalidTransform, "The projective horizon intersects the declared calibration domain.");
        }
    }
    double norm = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        auto mapped = map_point(transform, {transform.from, MeasurementUnit::Pixel, pixels[i].x, pixels[i].y});
        if (const auto* failure = std::get_if<Diagnostic>(&mapped)) { return *failure; }
        const auto& q = std::get<Point2D>(mapped);
        norm = std::hypot(norm, q.x - physical[i].x, q.y - physical[i].y);
    }
    if (!std::isfinite(norm)) { return error(ErrorCode::InvalidTransform, "Calibration reprojection error is not finite."); }
    return CalibrationFit{std::move(transform), norm / std::sqrt(static_cast<double>(pixels.size()))};
}
} // namespace

core::Result<CalibrationFit> fit_calibration(std::span<const std::byte> bytes)
{
    try {
        const auto chars = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (chars.find('\0') != std::string_view::npos) {
            return error(ErrorCode::InvalidResource, "Unexpected null byte in calibration JSON.");
        }
        std::set<std::string> fields;
        const auto check = [&](int depth, Json::parse_event_t event, const Json& item) {
            if (depth > 4) { throw error(ErrorCode::InvalidResource, "Unexpected nested value in calibration resource."); }
            if (event == Json::parse_event_t::key && !fields.insert(item.get<std::string>()).second) {
                throw error(ErrorCode::InvalidResource, "Duplicate calibration field " + item.dump() + ".");
            }
            return true;
        };
        return read_fit(Json::parse(chars, check));
    } catch (const Diagnostic& failure) {
        return failure;
    } catch (const Json::exception& failure) {
        return error(ErrorCode::InvalidResource, std::string("Invalid planar calibration JSON: ") + failure.what());
    } catch (const cv::Exception& failure) {
        return opencv_error(ErrorCode::InvalidTransform, "Planar calibration", failure);
    }
}

core::Result<core::Point2D> pixel_to_mm(const Transform2D& transform, const Point2D& point)
{
    if (transform.from_unit != MeasurementUnit::Pixel || transform.to_unit != MeasurementUnit::Millimetre) {
        return error(ErrorCode::CoordinateMismatch, "Coordinate conversion requires a Pixel to Millimetre transform.");
    }
    return map_point(transform, point);
}

core::Result<double> distance_mm(const Point2D& first, const Point2D& second)
{
    if (first.unit != MeasurementUnit::Millimetre || second.unit != first.unit || first.coordinate_frame != second.coordinate_frame) {
        return error(ErrorCode::CoordinateMismatch, "Distance measurement requires two points in the same millimetre coordinate frame.");
    }
    const auto distance = std::hypot(first.x - second.x, first.y - second.y);
    if (!std::isfinite(distance)) { return error(ErrorCode::InvalidGeometry, "Measured distance is not finite."); }
    return distance;
}
} // namespace visionpro::tools::vision
