#include "vision/class_map_tool.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using visionpro::tools::vision::classify_intensity;

void require(bool condition, const char* message)
{ if (!condition) { throw std::runtime_error(message); } }

template<class T> T take(Result<T> result)
{
    if (auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}

template<class T> Diagnostic rejects(Result<T> result, ErrorCode code)
{
    auto* failure = std::get_if<Diagnostic>(&result);
    require(failure && failure->code == code, "Expected class-map diagnostic.");
    return *failure;
}

template<SchemaIdentity T> T key(std::string_view name)
{ return take(SchemaIdFactory::parse<T>(name)); }

void classification()
{
    const std::array<std::uint8_t, 10> bytes{200,9,10,7,7,200,20,19,7,7};
    constexpr PixelFormat mono{PixelDepth::UInt8, ChannelOrder::Mono};
    const auto root_frame = key<CoordinateSystemId>("camera");
    const auto view_frame = key<CoordinateSystemId>("crop");
    const auto root = take(ImageHandle::copy({3, 2, 5, mono}, std::as_bytes(std::span(bytes)), root_frame));
    const FrameId frame{take(IdFactory::parse<ResourceId>("10000000-0000-4000-8000-000000000007")), 7};
    const ImageFrame input{take(root.crop({1,0,2,2}, view_frame)), frame,
        std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{100}}};
    const std::array<std::int64_t, 2> thresholds{10,20};
    const std::vector<std::string> names{"base","pass","defect","unused"};
    const auto value = take(classify_intensity(input, thresholds, names));
    const auto map = take(read_class_map(value));
    require(map.pixels.image.copy_pixels() == std::vector<std::byte>{std::byte{0},std::byte{1},std::byte{2},std::byte{1}},
        "Labels use inclusive interval starts, skip stride padding and preserve pixel order.");
    require(map.classes == std::vector<ClassMapEntry>{{0,"base",1},{1,"pass",2},{2,"defect",1},{3,"unused",0}},
        "Class table must include exact counts and retain unused classes.");
    require(map.pixels.frame == input.frame && map.pixels.captured_at == input.captured_at
        && map.pixels.image.root_frame() == root_frame && map.pixels.image.coordinate_frame() == view_frame
        && map.pixels.image.origin_x() == 1 && map.pixels.image.origin_y() == 0, "Class map lost its frame or crop's root coordinates.");
    require(input.image.copy_pixels() == std::vector<std::byte>{std::byte{9},std::byte{10},std::byte{20},std::byte{19}}, "Classification modified input.");
    auto missing = rejects(classify_intensity(input, thresholds, std::span(names).first(2)), ErrorCode::MissingField);
    require(missing.location.parameter == key<ParameterId>("class_names") && missing.message.find("2") != std::string::npos,
        "Missing class diagnostic must name the parameter and absent ID.");
    for (const auto invalid : {std::array<std::int64_t,2>{0,2}, {1,256}, {7,7}, {10,9}}) {
        rejects(classify_intensity(input, invalid, names), ErrorCode::ConstraintViolation);
    }

    auto bad = map;
    bad.classes[1].count = 0;
    rejects(make_class_map(bad), ErrorCode::ConstraintViolation);
    bad = map;
    bad.classes[0].id = 1;
    rejects(make_class_map(bad), ErrorCode::ConstraintViolation);
    bad = map;
    bad.classes.erase(bad.classes.begin() + 2);
    rejects(make_class_map(bad), ErrorCode::MissingField);

    // General record construction cannot introduce an inconsistent map.
    const auto classes = key<FieldId>("classes");
    const auto pixels = key<FieldId>("pixels");
    const auto* table = value.field(classes);
    const auto names_without_last_two = take(DataValue::list(*table->type().element_type(),
        {(*table->elements())[0], (*table->elements())[1]}));
    rejects(DataValue::record(class_map_type(), {{pixels,*value.field(pixels)},{classes,names_without_last_two}}), ErrorCode::MissingField);
    rejects(read_class_map(DataValue::integer(2)), ErrorCode::TypeMismatch);
}
} // namespace

int main()
{
    try { classification(); return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
