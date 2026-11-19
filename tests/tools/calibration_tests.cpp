#include "vision/calibration.hpp"

#include <visionpro/core/schema_id_factory.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
namespace vision = visionpro::tools::vision;

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> void reject(const Result<T>& value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    if (!failure || failure->code != code) { throw std::runtime_error("Expected " + std::string(error_code_name(code))); }
}

void near(double actual, double expected, double tolerance = 1e-6)
{
    if (!(std::abs(actual - expected) <= tolerance)) {
        throw std::runtime_error("Expected " + std::to_string(expected) + ", measured " + std::to_string(actual));
    }
}

const std::string affine_pairs = "[[[0,0],[10,-4]],[[5,0],[20,-4]],[[0,5],[10,11]],[[5,5],[20,11]]]";

Result<vision::CalibrationFit> fit(std::string_view model, const std::string& pairs,
    std::string_view unit = "Pixel", std::string_view domain = "[0,0,10,10]", int version = 1)
{
    const auto json = std::string(R"({"from":"image","to":"table","from_unit":")") + std::string(unit)
        + R"(","to_unit":"Millimetre","model":")" + std::string(model) + R"(","format_version":)"
        + std::to_string(version) + R"(,"domain":)" + std::string(domain) + R"(,"pairs":)" + pairs + "}";
    return vision::fit_calibration(std::as_bytes(std::span(json)));
}

void solve_models()
{
    const auto affine = take(fit("Affine", affine_pairs));
    const Point2D pixel{affine.transform.from, MeasurementUnit::Pixel, 3, 4};
    const auto mapped = take(vision::pixel_to_mm(affine.transform, pixel));
    near(mapped.x, 16); near(mapped.y, 8); near(affine.rms_error_mm, 0);
    if (mapped.coordinate_frame != affine.transform.to || mapped.unit != MeasurementUnit::Millimetre) {
        throw std::runtime_error("Mapped point lost its millimetre frame.");
    }
    near(take(vision::distance_mm(mapped, {mapped.coordinate_frame, mapped.unit, 13, 4})), 5);
    near(take(vision::distance_mm(mapped, mapped)), 0);
    const auto similarity = take(fit("Similarity", "[[[1,2],[-4,7]],[[3,2],[-4,13]]]"));
    const auto rotated = take(vision::pixel_to_mm(similarity.transform, pixel));
    near(rotated.x, -10); near(rotated.y, 13); near(similarity.rms_error_mm, 0);
    const auto reflected = take(fit("Similarity", "[[[0,0],[1,2]],[[2,0],[5,2]],[[0,3],[1,-4]]]"));
    const auto flipped = take(vision::pixel_to_mm(reflected.transform, pixel));
    near(flipped.x, 7); near(flipped.y, -6); near(reflected.rms_error_mm, 0);
    const auto projective = take(fit("Projective", "[[[0,0],[3,2]],[[10,0],[11.5,1]],[[10,10],[11.5,6]],[[0,10],[3,12]]]"));
    const auto projected = take(vision::pixel_to_mm(projective.transform, pixel));
    near(projected.x, 9 / 1.3); near(projected.y, 6 / 1.3); near(projective.rms_error_mm, 0);
    // One 4 mm residual split across four points gives fitted residuals of magnitude 1 mm each.
    const auto noisy = take(fit("Affine", "[[[0,0],[10,-4]],[[5,0],[20,-4]],[[0,5],[10,11]],[[5,5],[24,11]]]"));
    near(noisy.rms_error_mm, 1);
}

void diagnostics()
{
    reject(fit("Similarity", "[[[0,0],[1,2]]]"), ErrorCode::InvalidTransform);
    reject(fit("Similarity", "[[[1,1],[1,2]],[[1,1],[4,6]]]"), ErrorCode::InvalidTransform);
    reject(fit("Affine", "[[[0,0],[1,2]],[[2,2],[3,4]],[[5,5],[7,8]]]"), ErrorCode::InvalidTransform);
    reject(fit("Affine", "[[[0,0],[1,1]],[[3,0],[3,3]],[[0,3],[7,7]]]"), ErrorCode::InvalidTransform);
    reject(fit("Projective", "[[[0,0],[1,1]],[[5,0],[3,3]],[[0,5],[7,8]]]"), ErrorCode::InvalidTransform);
    reject(fit("Projective", "[[[0,0],[0,0]],[[1,1],[2,2]],[[3,3],[6,6]],[[4,4],[8,8]]]"), ErrorCode::InvalidTransform);
    reject(fit("Projective", "[[[0,0],[0,0]],[[10,0],[-10,0]],[[0,10],[0,10]],[[10,10],[-10,-10]]]"), ErrorCode::InvalidTransform);
    reject(fit("Affine", affine_pairs, "Millimetre"), ErrorCode::CoordinateMismatch);
    reject(fit("Affine", affine_pairs, "Pixel", "[0,0,10,10]", 2), ErrorCode::IncompatibleSchemaVersion);
    reject(fit("Affine", affine_pairs, "Pixel", "[0,0,4,4]"), ErrorCode::InvalidTransform);
    reject(fit("Affine", affine_pairs, "Pixel", "[0,0,0,4]"), ErrorCode::InvalidTransform);
    reject(fit("Affine", "[[[0,0],[0,1]],[[1,1],[1,1]],[[1,2],[3]]]"), ErrorCode::InvalidResource);
    const auto transform = take(fit("Affine", affine_pairs)).transform;
    reject(vision::pixel_to_mm(transform, {transform.from, MeasurementUnit::Pixel, 11, 1}), ErrorCode::InvalidTransform);
    reject(vision::pixel_to_mm(transform, {transform.to, MeasurementUnit::Millimetre, 3, 4}), ErrorCode::CoordinateMismatch);
    reject(vision::distance_mm({transform.to, MeasurementUnit::Millimetre, 0, 0}, {transform.from, MeasurementUnit::Pixel, 0, 0}),
        ErrorCode::CoordinateMismatch);
    const auto other = take(SchemaIdFactory::parse<CoordinateSystemId>("other"));
    reject(vision::distance_mm({transform.to, MeasurementUnit::Millimetre, 0, 0}, {other, MeasurementUnit::Millimetre, 0, 0}),
        ErrorCode::CoordinateMismatch);
}
} // namespace

int main()
{
    try { solve_models(); diagnostics(); return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
