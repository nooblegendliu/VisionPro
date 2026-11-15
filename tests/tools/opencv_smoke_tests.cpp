#include "image_fixtures.hpp"

#include <opencv2/core.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace visionpro::test_fixtures;

constexpr unsigned char binary_pixels[] = {0, 0, 255, 255};

void check(bool success, std::string_view operation)
{
    if (!success) {
        throw std::runtime_error(std::string(operation));
    }
}

void check_pixels(const cv::Mat& image, int type, std::span<const unsigned char> expected,
    std::string_view operation)
{
    check(image.cols == width && image.rows == height && image.type() == type, operation);
    const auto row_bytes = expected.size() / height;
    for (int y = 0; y < height; ++y) {
        const auto expected_row = expected.subspan(static_cast<std::size_t>(y) * row_bytes, row_bytes);
        check(std::ranges::equal(std::span(image.ptr<unsigned char>(y), row_bytes), expected_row), operation);
    }
}

cv::Mat check_codec(const char* extension, std::span<const unsigned char> fixture, std::size_t signature_size,
    int type, std::span<const unsigned char> expected)
{
    const std::string format(extension);
    const std::vector<unsigned char> input(fixture.begin(), fixture.end());
    const auto decoded = cv::imdecode(input, cv::IMREAD_UNCHANGED);
    check_pixels(decoded, type, expected, format + " fixture decode");

    std::vector<unsigned char> encoded;
    check(cv::imencode(extension, decoded, encoded), format + " encode");
    check(encoded.size() >= signature_size &&
        std::ranges::equal(std::span(encoded).first(signature_size), fixture.first(signature_size)),
        format + " output signature");
    check_pixels(cv::imdecode(encoded, cv::IMREAD_UNCHANGED), type, expected, format + " encoded pixel roundtrip");
    return decoded;
}

} // namespace

int main()
{
    try {
        const auto runtime_version = cv::getVersionString();
        check(runtime_version == CV_VERSION, "OpenCV headers and runtime version differ");

        const auto mono = check_codec(".png", png, 8, CV_8UC1, mono_pixels);
        const auto bgr = check_codec(".bmp", bmp, 2, CV_8UC3, bgr_pixels);

        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        check_pixels(gray, CV_8UC1, gray_pixels, "BGR to grayscale conversion");

        cv::Mat binary;
        cv::threshold(mono, binary, 128, 255, cv::THRESH_BINARY);
        check_pixels(binary, CV_8UC1, binary_pixels, "binary threshold at 128");
        check(cv::countNonZero(binary) == 2, "binary foreground count");

        std::cout << "OpenCV " << runtime_version << " PNG/BMP codecs and image processing passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OpenCV smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
