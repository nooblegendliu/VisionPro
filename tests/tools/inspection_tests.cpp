#include "vision/inspection.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::tools::vision;

void require(bool passed, const char* message)
{ if (!passed) { throw std::runtime_error(message); } }

template<class T> T take(Result<T> value)
{
    if (auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}
template<class T> void reject(const Result<T>& value, ErrorCode code)
{ require(std::holds_alternative<Diagnostic>(value) && std::get<Diagnostic>(value).code == code, "Unexpected diagnostic"); }

const auto reference = take(SchemaIdFactory::parse<CoordinateSystemId>("workpiece"));
const auto camera = take(SchemaIdFactory::parse<CoordinateSystemId>("camera"));
constexpr auto pixels = MeasurementUnit::Pixel;
constexpr PixelFormat mono{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr PixelFormat bgr{PixelDepth::UInt8, ChannelOrder::BGR};

void matches()
{
    const std::array<unsigned char, 6> pattern{0, 72, 25, 183, 2, 248};
    const auto patch = take(ImageHandle::copy({3, 2, 3, mono}, std::as_bytes(std::span{pattern}), reference));
    // The first known origin (1,1) is translated to (4,3) in the search image.
    std::array<unsigned char, 80> samples{};
    const auto insert = [&](std::size_t left, std::size_t top) {
        for (std::size_t y = 0; y < 2; ++y) {
            std::copy_n(pattern.begin() + 3*y, 3, samples.begin() + (top+y)*10 + left);
        }
    };
    const auto scene = [&] { return take(ImageHandle::copy({10, 8, 10, mono}, std::as_bytes(std::span{samples}), camera)); };
    const Point2D origin{reference, pixels, 1, 1};
    require(!take(locate_translation(scene(), patch, origin, 0.95)).transform, "Blank scene must not yield a transform");
    insert(4, 3);
    auto hit = take(locate_translation(scene(), patch, origin, 0.95));
    require(hit.position == Point2D{camera, pixels, 4, 3} && hit.transform && hit.score >= 0.99999
        && hit.transform->from == reference && hit.transform->to == camera && hit.transform->matrix[2] == 3
        && hit.transform->matrix[5] == 2, "Expected (+3,+2) and distinct source and image frames");
    require(take(locate_translation(scene(), patch, origin, 1.0)).position == hit.position,
        "Byte-identical match reaches an inclusive minimum score of one");
    insert(0, 0);
    hit = take(locate_translation(scene(), patch, origin, 0.95));
    require(!hit.position && !hit.transform && hit.score >= 0.99999, "Repeated matches have no unique placement");
    reject(locate_translation(patch, scene(), origin, 0.95), ErrorCode::ImageBounds);
    std::array<unsigned char, 6> constant{};
    const auto flat = take(ImageHandle::copy({3, 2, 3, mono}, std::as_bytes(std::span{constant}), reference));
    require(!take(locate_translation(flat, patch, origin, 0)).position, "Flat search window cannot yield a placement");
    reject(locate_translation(scene(), flat, origin, 0.95), ErrorCode::InvalidImage);
    reject(locate_translation(scene(), patch, {reference, MeasurementUnit::Millimetre, 1, 1}, 0.95), ErrorCode::CoordinateMismatch);
}

void located_color()
{
    const auto region_id = take(IdFactory::parse<RegionId>("00000000-0000-4000-8000-000000000001"));
    const auto binding = take(SchemaIdFactory::parse<ParameterId>("roi"));
    const Transform2D shift{reference, camera, pixels, pixels, TransformModel::Similarity, {1,0,3,0,1,2,0,0,1}};
    RegionDefinition region{region_id, reference, pixels, RegionPrimitive{reference, pixels, AxisAlignedRect{0,0,3,1}},
        {RegionPrimitive{reference, pixels, AxisAlignedRect{1,0,1,1}}}};
    std::array<unsigned char, 3*23> scene{}; // Two bytes of row padding.
    for (std::size_t x = 0; x < 3; ++x) {
        const auto offset = 2*23 + 3*(3+x);
        scene[offset] = std::array<unsigned char,3>{40,220,41}[x];
        scene[offset+1] = 68;
        scene[offset+2] = 156;
    }
    const auto image = take(ImageHandle::copy({7,3,23,bgr}, std::as_bytes(std::span{scene}), camera));
    const auto evaluated = take(evaluate_roi(image, region, binding, {}, {}, shift));
    require(take(mean_bgr(image, evaluated.mask)) == std::array{40.5,68.0,156.0}, "BGR means preserve fractional data and channel order");
    const auto effective = take(evaluated.effective_region);
    require(effective.coordinate_frame == camera && take(evaluate_roi(image, effective, binding)).member_count == 2,
        "Located ROI is reusable in the captured image frame");
    auto wrong = shift;
    wrong.from = camera;
    reject(evaluate_roi(image, region, binding, {}, {}, wrong), ErrorCode::CoordinateMismatch);
    region.source = RegionPrimitive{reference, pixels, EmptyRegionShape{}};
    region.empty_policy = EmptyRegionPolicy::ValidEmptyOutput;
    reject(mean_bgr(image, take(evaluate_roi(image, region, binding, {}, {}, shift)).mask), ErrorCode::EmptyRegion);
}
}

int main()
{
    try {
        matches();
        located_color();
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
