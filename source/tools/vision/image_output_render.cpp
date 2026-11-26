#include "../image_source_pixels.hpp"
#include "image_output.hpp"
#include "opencv_image.hpp"
#include "roi.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace visionpro::tools::vision::image_output
{
namespace
{
using namespace core;
using Check = Result<std::monostate>;
Diagnostic invalid(std::string message, ErrorCode code = ErrorCode::ConstraintViolation)
{
    return {code, Severity::Error, {}, std::move(message)};
}
const DataValue& member(const DataValue& value, std::string_view field)
{
    return *value.field(std::get<FieldId>(SchemaIdFactory::parse<FieldId>(field)));
}

Result<cv::Mat> native_matrix(const ImageHandle& image, ChannelOrder desired)
{
    auto converted = image_source::convert(image, desired);
    if (const auto* error = std::get_if<Diagnostic>(&converted)) {
        return *error;
    }
    return to_cv_mat(std::get<ImageHandle>(converted));
}

Result<cv::Scalar> color_value(std::string_view text, bool mono)
{
    unsigned rgb{};
    if (text.size() != 7 || text.front() != '#') {
        return invalid("Annotation color requires #RRGGBB.");
    }
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data() + 1, end, rgb, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return invalid("Invalid annotation color.");
    }
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    if (mono) {
        return cv::Scalar((4899 * r + 9617 * g + 1868 * b + 8192) >> 14);
    }
    return cv::Scalar(b, g, r);
}

Result<cv::Point> point(const Point2D& value, const ImageHandle& base)
{
    double x = value.x, y = value.y;
    if (value.unit != MeasurementUnit::Pixel) {
        return invalid("An annotation must use pixel units.", ErrorCode::CoordinateMismatch);
    }
    if (value.coordinate_frame != base.coordinate_frame()) {
        if (value.coordinate_frame != base.root_frame()) {
            return invalid("Annotation and image coordinate frames differ.", ErrorCode::CoordinateMismatch);
        }
        x -= static_cast<double>(base.origin_x());
        y -= static_cast<double>(base.origin_y());
    }
    constexpr double limit = std::numeric_limits<int>::max() / 4;
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > limit || std::abs(y) > limit) {
        return invalid("Annotation coordinate exceeds the rasterizer range.");
    }
    return cv::Point{cvRound(x), cvRound(y)};
}

struct Stroke
{
    cv::Scalar color;
    int thickness;
    int line_type;
};

Check draw_mask(cv::Mat& canvas, const ImageHandle& base, const RasterMask& mask, const Stroke& stroke)
{
    auto checked = validate_raster_mask(mask, base);
    if (const auto* error = std::get_if<Diagnostic>(&checked)) {
        return *error;
    }
    if (mask.mapping.transform || mask.pixels.format() != PixelFormat{PixelDepth::UInt8, ChannelOrder::Mono}) {
        return invalid("Overlay requires a directly mapped Mono8 ROI mask.", ErrorCode::InvalidMask);
    }
    auto bytes = to_cv_mat(mask.pixels);
    if (const auto* error = std::get_if<Diagnostic>(&bytes)) {
        return *error;
    }
    cv::Mat selected;
    const bool nonzero = mask.valid_pixels.mode == MaskValueMode::NonZero;
    cv::compare(std::get<cv::Mat>(bytes), nonzero ? 0.0 : mask.valid_pixels.threshold, selected,
                nonzero ? cv::CMP_NE : cv::CMP_GE);
    if (stroke.thickness > 1) {
        const int size = 2 * stroke.thickness - 1;
        cv::dilate(selected, selected, cv::getStructuringElement(cv::MORPH_ELLIPSE, {size, size}));
    }
    canvas.setTo(stroke.color, selected);
    return std::monostate{};
}

Check draw_point(cv::Mat& canvas, const ImageHandle& base, const Point2D& value, const Stroke& stroke)
{
    auto selected = point(value, base);
    if (const auto* error = std::get_if<Diagnostic>(&selected)) {
        return *error;
    }
    cv::drawMarker(canvas, std::get<cv::Point>(selected), stroke.color, cv::MARKER_CROSS,
                   std::max(3, stroke.thickness * 4 + 1), stroke.thickness, stroke.line_type);
    return std::monostate{};
}

Check draw_box(cv::Mat& canvas, const ImageHandle& base, const DataValue& bounds, const Stroke& stroke)
{
    auto start = point(*member(bounds, "origin").as_point2d(), base);
    if (const auto* error = std::get_if<Diagnostic>(&start)) {
        return *error;
    }
    const auto w = *member(bounds, "width").as_int64(), h = *member(bounds, "height").as_int64();
    constexpr auto limit = std::numeric_limits<int>::max() / 2;
    if (w <= 0 || h <= 0 || w > limit || h > limit) {
        return invalid("Invalid component bounding box.");
    }
    const auto origin = std::get<cv::Point>(start);
    cv::rectangle(canvas, origin, origin + cv::Point(static_cast<int>(w) - 1, static_cast<int>(h) - 1), stroke.color,
                  stroke.thickness, stroke.line_type);
    return std::monostate{};
}

Check draw_region(cv::Mat& canvas, const ImageHandle& base, const RegionDefinition& region, const Stroke& stroke)
{
    static const auto port = std::get<InputId>(SchemaIdFactory::parse<InputId>("regions"));
    const auto outline = [&](const RegionSource& source) -> Check {
        const auto* shape = std::get_if<RegionPrimitive>(&source);
        if (!shape) {
            return invalid("Only resolved ROI results can be rendered.", ErrorCode::UnsupportedRegion);
        }
        // A raster mask has a separate display item. The vector outline only draws named geometry.
        if (std::holds_alternative<MaskConfig>(shape->geometry)) {
            return std::monostate{};
        }
        RegionDefinition component{
            region.id, region.coordinate_frame, region.unit, source,
            {},        region.boundary_policy,  region.fill, EmptyRegionPolicy::ValidEmptyOutput};
        auto evaluated = evaluate_roi(base, component, port);
        if (const auto* error = std::get_if<Diagnostic>(&evaluated)) {
            return *error;
        }
        auto image = to_cv_mat(std::get<RoiEvaluation>(evaluated).mask.pixels);
        if (const auto* error = std::get_if<Diagnostic>(&image)) {
            return *error;
        }
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(std::get<cv::Mat>(image), contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
        cv::drawContours(canvas, contours, -1, stroke.color, stroke.thickness, stroke.line_type);
        return std::monostate{};
    };
    auto inclusion = outline(region.source);
    if (const auto* error = std::get_if<Diagnostic>(&inclusion)) {
        return *error;
    }
    for (const auto& excluded : region.exclusions) {
        auto result = outline(excluded);
        if (const auto* error = std::get_if<Diagnostic>(&result)) {
            return *error;
        }
    }
    return std::monostate{};
}

Check draw_item(cv::Mat& canvas, const ImageHandle& base, std::string_view item, const DataValue& value,
                const Stroke& stroke)
{
    if (const auto* mask = value.as_raster_mask()) {
        return draw_mask(canvas, base, *mask, stroke);
    }
    if (const auto* region = value.as_region2d()) {
        return draw_region(canvas, base, *region, stroke);
    }
    if (const auto* components = value.elements()) {
        for (const auto& component : *components) {
            auto drawn = item == "bounds"
                             ? draw_box(canvas, base, member(component, "bounds"), stroke)
                             : draw_point(canvas, base, *member(component, "centroid").as_point2d(), stroke);
            if (const auto* error = std::get_if<Diagnostic>(&drawn)) {
                return *error;
            }
        }
    }
    else if (const auto* line = value.as_line2d()) {
        auto start = point(line->start, base);
        auto end = point(line->end, base);
        if (const auto* error = std::get_if<Diagnostic>(&start)) {
            return *error;
        }
        if (const auto* error = std::get_if<Diagnostic>(&end)) {
            return *error;
        }
        cv::line(canvas, std::get<cv::Point>(start), std::get<cv::Point>(end), stroke.color, stroke.thickness,
                 stroke.line_type);
    }
    else if (const auto* circle = value.as_circle2d()) {
        auto center = point(circle->center, base);
        if (const auto* error = std::get_if<Diagnostic>(&center)) {
            return *error;
        }
        if (circle->radius > std::numeric_limits<int>::max() / 4) {
            return invalid("Annotation radius exceeds the rasterizer range.");
        }
        cv::circle(canvas, std::get<cv::Point>(center), cvRound(circle->radius), stroke.color, stroke.thickness,
                   stroke.line_type);
    }
    else if (const auto* position = value.as_point2d()) {
        return draw_point(canvas, base, *position, stroke);
    }
    else {
        return invalid("Unsupported annotation input type.", ErrorCode::TypeMismatch);
    }
    return std::monostate{};
}

const ResolvedDisplayItem* source_style(const InvocationContextData& call, ToolValueTarget target, std::size_t index)
{
    const auto found = std::ranges::find_if(call.display_items, [&](const auto& source) {
        return source.target == target && source.element_index == index;
    });
    return found == call.display_items.end() ? nullptr : &*found;
}

Result<cv::Scalar> item_color(const InvocationContextData& call, const DisplayItemStyle& style,
                              const ResolvedDisplayItem* source, bool mono)
{
    if (source && source->judgement == ToolJudgement::OK && !style.ok_color.empty()) {
        return color_value(style.ok_color, mono);
    }
    if (source && source->judgement == ToolJudgement::NG && !style.ng_color.empty()) {
        return color_value(style.ng_color, mono);
    }
    return color_value(style.color.empty() ? *parameter(call, "color").as_string() : style.color, mono);
}

Check draw_shapes(cv::Mat& canvas, const ImageHandle& base, const InvocationContextData& call, double line_ratio,
                  int line_type)
{
    for (const auto& port : call.inputs) {
        const auto* sources = port.value.elements();
        if (!sources) {
            continue;
        }
        for (std::size_t index = 0; index < sources->size(); ++index) {
            const auto* source = source_style(call, port.id, index);
            const DisplayItemStyle defaults{{}, {}, port.id.text() == "masks" ? 35 : 100, 1};
            const auto& style = source && source->style ? *source->style : defaults;
            auto color = item_color(call, style, source, canvas.channels() == 1);
            if (const auto* error = std::get_if<Diagnostic>(&color)) {
                return *error;
            }
            const int thickness = std::clamp(cvRound(line_ratio * style.thickness), 1, 255);
            // Legacy masks use the original mask boundary at every global line scale.
            const Stroke stroke{std::get<cv::Scalar>(color),
                                port.id.text() == "masks" && style.thickness == 1 ? 1 : thickness, line_type};
            cv::Mat layer = style.opacity == 100 ? canvas : canvas.clone();
            auto drawn = draw_item(layer, base, port.id.text(), (*sources)[index], stroke);
            if (const auto* error = std::get_if<Diagnostic>(&drawn)) {
                return *error;
            }
            if (style.opacity != 100) {
                const auto alpha = style.opacity / 100.0;
                cv::addWeighted(layer, alpha, canvas, 1.0 - alpha, 0, canvas);
            }
        }
    }
    return std::monostate{};
}

Check draw_text(cv::Mat& canvas, std::string_view utf8, cv::Point origin, int size, double width_scale,
                const cv::Scalar& color, int line_type, int opacity, int thickness)
{
    if (utf8.empty() || origin.x >= canvas.cols || origin.y >= canvas.rows) {
        return std::monostate{};
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) {
        return invalid("Annotation text is too long.");
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!length) {
        return invalid("Annotation text is not valid UTF-8.");
    }
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), text.data(), length);
    struct Surface
    {
        HDC dc{CreateCompatibleDC(nullptr)};
        HBITMAP bitmap{};
        HFONT font{};
        ~Surface()
        {
            if (dc) {
                DeleteDC(dc);
            }
            if (bitmap) {
                DeleteObject(bitmap);
            }
            if (font) {
                DeleteObject(font);
            }
        }
    } surface;
    if (!surface.dc) {
        return invalid("Could not create a text rendering surface.", ErrorCode::ExecutionBoundaryFailure);
    }
    surface.font = CreateFontW(
        -size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        line_type == cv::LINE_AA ? ANTIALIASED_QUALITY : NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    if (!surface.font || !SelectObject(surface.dc, surface.font)) {
        return invalid("Could not select the annotation font.");
    }
    RECT measured{0, 0, 0, 0};
    const UINT flags = DT_LEFT | DT_TOP | DT_NOPREFIX | DT_EXPANDTABS;
    if (!DrawTextW(surface.dc, text.data(), length, &measured, flags | DT_CALCRECT)) {
        return invalid("Could not measure annotation text.");
    }
    const int available = canvas.cols - origin.x;
    const int w = static_cast<int>(std::min<double>(std::ceil(available / width_scale), measured.right)),
              h = std::min<int>(canvas.rows - origin.y, measured.bottom);
    if (w <= 0 || h <= 0) {
        return std::monostate{};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* memory{};
    surface.bitmap = CreateDIBSection(surface.dc, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
    if (!surface.bitmap || !SelectObject(surface.dc, surface.bitmap)) {
        return invalid("Could not allocate annotation pixels.");
    }
    cv::Mat mask(h, w, CV_8UC4, memory);
    mask.setTo(cv::Scalar::all(0));
    SetBkMode(surface.dc, TRANSPARENT);
    SetTextColor(surface.dc, RGB(255, 255, 255));
    RECT area{0, 0, w, h};
    if (!DrawTextW(surface.dc, text.data(), length, &area, flags)) {
        return invalid("Could not draw annotation text.");
    }
    GdiFlush();
    cv::Mat coverage;
    cv::extractChannel(mask, coverage, 0);
    if (thickness > 1) {
        cv::dilate(coverage, coverage, cv::getStructuringElement(cv::MORPH_ELLIPSE, {thickness, thickness}));
    }
    const int output_width = static_cast<int>(std::min<double>(available, std::ceil(w * width_scale)));
    auto target = canvas(cv::Rect(origin.x, origin.y, output_width, h));
    for (int y = 0; y < h; ++y) {
        const auto* src = coverage.ptr<std::uint8_t>(y);
        auto* dst = target.ptr<std::uint8_t>(y);
        for (int x = 0; x < output_width; ++x) {
            const auto source_x = std::min(w - 1, static_cast<int>(x / width_scale));
            const unsigned alpha = (src[source_x] * opacity + 50) / 100;
            for (int c = 0; c < canvas.channels(); ++c, ++dst) {
                *dst = static_cast<std::uint8_t>(
                    ((255 - alpha) * (*dst) + alpha * static_cast<unsigned>(color[c]) + 127) / 255);
            }
        }
    }
    return std::monostate{};
}
} // namespace

core::Result<core::ImageFrame> render(const core::InvocationContextData& call, const core::ImageFrame& source)
{
    const auto number = [&](std::string_view key) { return static_cast<int>(*parameter(call, key).as_int64()); };
    const bool mono = parameter(call, "pixel_mode").as_enum()->text() == "Mono8";
    const auto mode = parameter(call, "scale_mode").as_enum()->text();
    const auto ratio = mode == "Display" ? static_cast<double>(source.image.width()) / 500.0 : 1.0;
    const auto line_ratio = mode == "Custom" ? *parameter(call, "line_scale").as_float64() : ratio;
    const auto text_width = mode == "Custom" ? *parameter(call, "text_scale").as_float64() : 1.0;
    const int font = std::max(1, static_cast<int>(std::min(8192.0, std::round(number("font_size") * ratio))));
    const auto style = parameter(call, "line_type").as_enum()->text();
    const int line_type = style == "Antialias" ? cv::LINE_AA : style == "Four" ? cv::LINE_4 : cv::LINE_8;
    const DisplayItemStyle text_style{*parameter(call, "text_ok_color").as_string(),
                                      *parameter(call, "text_ng_color").as_string(), number("text_opacity"),
                                      number("text_thickness"), *parameter(call, "text_color").as_string()};
    const auto text_id = std::get<ParameterId>(SchemaIdFactory::parse<ParameterId>("text"));
    auto color = item_color(call, text_style, source_style(call, text_id, 0), mono);
    if (auto* error = std::get_if<Diagnostic>(&color)) {
        return *error;
    }
    const auto order = mono ? ChannelOrder::Mono : ChannelOrder::BGR;
    auto result = native_matrix(source.image, order);
    if (auto* error = std::get_if<Diagnostic>(&result)) {
        return *error;
    }
    auto& canvas = std::get<cv::Mat>(result);
    auto drawn = draw_shapes(canvas, source.image, call, line_ratio, line_type);
    if (auto* error = std::get_if<Diagnostic>(&drawn)) {
        return *error;
    }
    auto text = draw_text(canvas, *parameter(call, "text").as_string(), {number("text_x"), number("text_y")}, font,
                          text_width, std::get<cv::Scalar>(color), line_type, text_style.opacity, text_style.thickness);
    if (auto* error = std::get_if<Diagnostic>(&text)) {
        return *error;
    }
    auto published = copy_from_cv_mat(canvas, {PixelDepth::UInt8, order}, source);
    if (auto* error = std::get_if<Diagnostic>(&published)) {
        return *error;
    }
    auto frame = std::get<ImageFrame>(std::move(published));
    if (!mono) {
        auto rgb = image_source::convert(frame.image, ChannelOrder::RGB);
        if (auto* error = std::get_if<Diagnostic>(&rgb)) {
            return *error;
        }
        frame.image = std::get<ImageHandle>(std::move(rgb));
    }
    frame.acquisition = source.acquisition;
    return frame;
}

core::Result<std::vector<std::uint8_t>> encode(const core::ImageHandle& image, std::string_view extension, int quality)
{
    auto result =
        native_matrix(image, image.format().order == ChannelOrder::Mono ? ChannelOrder::Mono : ChannelOrder::BGR);
    if (auto* error = std::get_if<Diagnostic>(&result)) {
        return *error;
    }
    std::vector<std::uint8_t> encoded;
    std::vector<int> options;
    if (extension == "jpg") {
        options = {cv::IMWRITE_JPEG_QUALITY, quality};
    }
    if (!cv::imencode("." + std::string(extension), std::get<cv::Mat>(result), encoded, options)) {
        return invalid("Image encoding failed.", ErrorCode::ExecutionBoundaryFailure);
    }
    return encoded;
}
} // namespace visionpro::tools::vision::image_output
