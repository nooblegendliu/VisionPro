#include "vision/opencv_image.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <opencv2/core/version.hpp>

#include <array>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::tools::vision;

constexpr PixelFormat mono8{PixelDepth::UInt8, ChannelOrder::Mono};
constexpr PixelFormat bgr8{PixelDepth::UInt8, ChannelOrder::BGR};

void check(bool condition, std::string_view message)
{
    if (!condition) { throw std::runtime_error(std::string(message)); }
}

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> void expect_error(const Result<T>& value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code && is_valid_diagnostic(*failure), "Expected a valid image bridge diagnostic.");
}

CoordinateSystemId coordinates(std::string_view id)
{ return take(SchemaIdFactory::parse<CoordinateSystemId>(id)); }

std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> pixels;
    for (auto value : values) { pixels.push_back(static_cast<std::byte>(value)); }
    return pixels;
}

ImageHandle padded_mono()
{
    return take(ImageHandle::copy({4, 3, 6, mono8},
        bytes({1, 2, 3, 4, 99, 98, 11, 12, 13, 14, 97, 96, 21, 22, 23, 24}), coordinates("root")));
}

void crop_to_mat()
{
    const auto root = padded_mono();
    const auto crop = take(root.crop({1, 1, 3, 2}, coordinates("crop")));
    auto matrix = take(to_cv_mat(crop));
    check(matrix.type() == CV_8UC1 && matrix.cols == 3 && matrix.rows == 2 && matrix.step[0] == 3,
        "A Mono crop becomes a packed matrix of the same size.");
    check(matrix.at<unsigned char>(0, 0) == 12 && matrix.at<unsigned char>(0, 2) == 14
        && matrix.at<unsigned char>(1, 0) == 22 && matrix.at<unsigned char>(1, 2) == 24,
        "A crop reads the intended source pixels across padded rows.");
    matrix.setTo(cv::Scalar(0));
    check(crop.copy_pixels() == bytes({12, 13, 14, 22, 23, 24})
        && root.copy_pixels() == bytes({1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24}),
        "The matrix has no writable alias to the published image storage.");
}

void mat_to_image()
{
    std::array<unsigned char, 24> source{
        1, 2, 3, 10, 20, 30, 40, 50, 60, 99, 98, 97,
        4, 5, 6, 70, 80, 90, 100, 110, 120, 96, 95, 94};
    const cv::Mat matrix(2, 3, CV_8UC3, source.data(), 12);
    const auto view = matrix(cv::Rect{1, 0, 2, 2});
    const auto expected = bytes({10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120});
    check(!view.isContinuous(), "The matrix fixture carries both a crop offset and row padding.");
    const auto image = take(copy_from_cv_mat(view, bgr8, coordinates("image")));
    check(image.format() == bgr8 && image.width() == 2 && image.height() == 2 && image.row_stride() == 6
        && image.copy_pixels() == expected, "Bridge packs matrix ROI rows in declared BGR order.");
    check(image.coordinate_frame() == coordinates("image") && image.root_frame() == image.coordinate_frame()
        && image.origin_x() == 0 && image.origin_y() == 0, "A new image has the supplied root frame.");
    const auto bgr = take(to_cv_mat(image));
    const auto* pixel = bgr.ptr<unsigned char>(0);
    check(bgr.type() == CV_8UC3 && pixel[0] == 10 && pixel[1] == 20 && pixel[2] == 30,
        "BGR components retain their order across the bridge.");

    const auto root = padded_mono();
    const auto crop = take(root.crop({1, 1, 2, 2}, coordinates("matched.crop")));
    const auto resource = take(IdFactory::parse<ResourceId>("00112233-4455-4677-8899-aabbccddeeff"));
    const auto capture = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{1234}};
    const ImageFrame prototype{crop, FrameId{resource, 2}, capture};
    const auto output = take(copy_from_cv_mat(view, bgr8, prototype));
    source.fill(255);
    check(output.image.copy_pixels() == expected && image.copy_pixels() == expected && !output.image.shares_storage_with(crop),
        "Published pixels survive source matrix edits and use independent storage.");
    check(output.image.coordinate_frame() == crop.coordinate_frame() && output.image.root_frame() == root.coordinate_frame()
        && output.image.origin_x() == 1 && output.image.origin_y() == 1 && output.frame == prototype.frame
        && output.captured_at == prototype.captured_at, "Derived images retain the crop mapping and acquisition metadata.");
    expect_error(copy_from_cv_mat(matrix, bgr8, prototype), ErrorCode::InvalidImage);
}

void negative_layouts()
{
    const auto frame = coordinates("invalid");
    expect_error(copy_from_cv_mat(cv::Mat{}, mono8, frame), ErrorCode::InvalidImage);
    const std::array dimensions{2, 2, 2};
    const cv::Mat volume(static_cast<int>(dimensions.size()), dimensions.data(), CV_8UC1);
    expect_error(copy_from_cv_mat(volume, mono8, frame), ErrorCode::InvalidImage);
    expect_error(copy_from_cv_mat(cv::Mat(2, 2, CV_16UC1), mono8, frame), ErrorCode::UnsupportedPixelFormat);
    expect_error(copy_from_cv_mat(cv::Mat(2, 2, CV_8UC3), mono8, frame), ErrorCode::UnsupportedPixelFormat);

    constexpr PixelFormat rgb8{PixelDepth::UInt8, ChannelOrder::RGB};
    expect_error(copy_from_cv_mat(cv::Mat(2, 2, CV_8UC3), rgb8, frame), ErrorCode::UnsupportedPixelFormat);
    expect_error(to_cv_mat(take(ImageHandle::copy({1, 1, 3, rgb8}, bytes({1, 2, 3}), frame))), ErrorCode::UnsupportedPixelFormat);

    cv::Mat malformed(2, 2, CV_8UC1);
    malformed.step[0] = 1;
    expect_error(copy_from_cv_mat(malformed, mono8, frame), ErrorCode::InvalidImage);
    malformed.step[0] = std::numeric_limits<std::size_t>::max();
    expect_error(copy_from_cv_mat(malformed, mono8, frame), ErrorCode::SizeOverflow);
    malformed.step[0] = 2;
    malformed.dataend = malformed.data + 3;
    expect_error(copy_from_cv_mat(malformed, mono8, frame), ErrorCode::InvalidImage);
}

void exception_mapping()
{
    const cv::Exception failure(cv::Error::StsBadArg, "bad matrix", "fixture", "fixture.cpp", 2);
    const auto diagnostic = opencv_error(ErrorCode::InvalidImage, "imdecode", failure);
    check(diagnostic.code == ErrorCode::InvalidImage && is_valid_diagnostic(diagnostic) && diagnostic.external,
        "OpenCV failures produce valid typed diagnostics.");
    const auto& external = *diagnostic.external;
    check(external.provider.text() == "opencv" && external.operation == "imdecode" && external.raw_code == cv::Error::StsBadArg
        && external.backend_version == CV_VERSION && external.mapping == ExternalCodeMapping::Mapped,
        "OpenCV diagnostics preserve provider, operation, original status code and version.");
}

} // namespace

int main()
{
    try {
        crop_to_mat();
        mat_to_image();
        negative_layouts();
        exception_mapping();
        std::cout << "Image bridge layout, ownership, coordinates and diagnostic checks passed\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
