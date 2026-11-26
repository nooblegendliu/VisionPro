#include "mvs_frame.hpp"
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;

void check(bool passed, const char* message)
{ if (!passed) { throw std::runtime_error(message); } }

template<class T> T take(Result<T> result)
{
    if (const auto* failure = std::get_if<Diagnostic>(&result)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(result));
}
void frame_copy()
{
    const auto source = take(IdFactory::parse<ResourceId>("00000000-0000-4000-8000-000000000001"));
    const auto coordinates = take(SchemaIdFactory::parse<CoordinateSystemId>("camera"));
    const CameraCaptureRequest request{source, coordinates, CameraMode::Continuous, std::chrono::milliseconds{50}};
    MV_FRAME_OUT frame{};
    std::array<unsigned char, 3> borrowed{7, 0, 19};
    frame.pBufAddr = borrowed.data();
    auto& info = frame.stFrameInfo;
    info.nWidth = info.nHeight = 1;
    info.nFrameLen = 3;
    info.enPixelType = PixelType_Gvsp_BGR8_Packed;
    info.nFrameNum = 1;
    info.nDevTimeStampHigh = 2;
    info.nDevTimeStampLow = 5;
    const auto first = take(mvs_detail::copy_frame(request, frame, 1000));
    borrowed[0] = 31;
    info.nDevTimeStampLow = 6;
    const auto second = take(mvs_detail::copy_frame(request, frame, 1000));
    check(first.image.image.copy_pixels() == std::vector<std::byte>{std::byte{7}, std::byte{0}, std::byte{19}}
        && second.image.image.copy_pixels() == std::vector<std::byte>{std::byte{31}, std::byte{0}, std::byte{19}},
        "Native buffer reuse must preserve each retained BGR frame, including embedded zero bytes.");
    check(first.device_ticks == ((2ULL << 32) | 5) && first.tick_frequency_hz == 1000
        && first.timestamp_source == "host_receive_utc" && first.image.captured_at.has_value(), "Clock metadata mismatch.");
    check(first.image.frame && second.image.frame && first.image.frame->source == source
        && second.image.frame->sequence > first.image.frame->sequence && first.device_frame_number == 1
        && second.device_frame_number == 1, "Reset native counters must retain distinct published identities.");
    info.enPixelType = PixelType_Gvsp_RGB8_Packed;
    const auto rgb = take(mvs_detail::copy_frame(request, frame, {}));
    check(rgb.image.image.format().order == ChannelOrder::RGB &&
        rgb.image.image.copy_pixels() == std::vector<std::byte>{std::byte{31}, std::byte{0}, std::byte{19}},
        "Packed RGB24 must retain its declared channel order and native bytes.");
    info.enPixelType = PixelType_Gvsp_Mono8;
    info.nExtendWidth = 3;
    const auto mono = take(mvs_detail::copy_frame(request, frame, {}));
    check(mono.image.image.width() == 3 && mono.image.image.format().order == ChannelOrder::Mono
        && !mono.tick_frequency_hz, "Extended width, native Mono8 or unknown clock frequency lost.");
    info.nFrameLenEx = 4;
    check(std::get<Diagnostic>(mvs_detail::copy_frame(request, frame, {})).code == ErrorCode::InvalidImage, "Unexpected packing accepted.");
    info.nFrameLenEx = 3;
    info.nLostPacket = 1;
    check(std::get<Diagnostic>(mvs_detail::copy_frame(request, frame, {})).code == ErrorCode::InvalidImage, "Incomplete frame accepted.");
    info.nLostPacket = 0;
    info.enPixelType = PixelType_Gvsp_BayerRG8;
    check(std::get<Diagnostic>(mvs_detail::copy_frame(request, frame, {})).code == ErrorCode::UnsupportedPixelFormat, "Bayer misinterpreted.");
    info.enPixelType = PixelType_Gvsp_Mono8;
    frame.pBufAddr = nullptr;
    check(std::get<Diagnostic>(mvs_detail::copy_frame(request, frame, {})).code == ErrorCode::InvalidImage,
        "A declared frame length requires a valid pixel buffer.");
}

void vendor_diagnostics()
{
    const auto known = mvs_detail::sdk_error(ErrorCode::SourceTimedOut, "MV_CC_GetImageBuffer", static_cast<int>(MV_E_NODATA));
    check(known.code == ErrorCode::SourceTimedOut && known.external && known.external->raw_code == MV_E_NODATA
        && known.external->mapping == ExternalCodeMapping::Mapped, "Known timeout lost its vendor status.");
    constexpr std::uint32_t injected = 0x88abcdef;
    const auto unknown = mvs_detail::sdk_error(ErrorCode::ExecutionBoundaryFailure, "injected", static_cast<int>(injected));
    check(unknown.external && unknown.external->raw_code == injected && unknown.external->mapping == ExternalCodeMapping::Unmapped
        && unknown.external->backend_version.has_value(), "Unknown status must retain raw bits and SDK version.");
}
}
int main()
{
    try { frame_copy(); vendor_diagnostics(); }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
