#include "image_panel.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace visionpro::ui {
namespace {
using namespace core;
using workflow::NodeResult;

template<class Id>
Id key(std::string_view name) { return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

Diagnostic issue(ErrorCode code, std::string message) { return {code, Severity::Error, {}, std::move(message)}; }

const DataValue* output_value(const NodeResult& result, const OutputId& output)
{
    const auto* slot = result.output(output);
    return slot ? std::get_if<DataValue>(&slot->content) : nullptr;
}

Result<std::vector<BlobAnnotation>> read_blobs(const NodeResult& result)
{
    const auto* value = output_value(result, key<OutputId>("components"));
    const auto* components = value ? value->elements() : nullptr;
    if (!components) { return issue(ErrorCode::InvalidValue, "The selected result has no Blob component list."); }
    const auto bounds_id = key<FieldId>("bounds");
    const auto origin_id = key<FieldId>("origin");
    const auto width_id = key<FieldId>("width");
    const auto height_id = key<FieldId>("height");
    const auto centroid_id = key<FieldId>("centroid");
    const auto area_id = key<FieldId>("area_pixels");
    std::vector<BlobAnnotation> blobs;
    blobs.reserve(components->size());
    for (const auto& component : *components) {
        const auto* bounds = component.field(bounds_id);
        const auto* origin = bounds ? bounds->field(origin_id) : nullptr;
        const auto* width = bounds ? bounds->field(width_id) : nullptr;
        const auto* height = bounds ? bounds->field(height_id) : nullptr;
        const auto* centroid = component.field(centroid_id);
        const auto* area = component.field(area_id);
        if (!origin || !origin->as_point2d() || !width || !width->as_int64() || !height || !height->as_int64()
            || !centroid || !centroid->as_point2d() || !area || !area->as_int64()) {
            return issue(ErrorCode::InvalidValue, "The selected result contains an invalid Blob component.");
        }
        const auto& corner = *origin->as_point2d();
        const auto& center = *centroid->as_point2d();
        if (corner.unit != MeasurementUnit::Pixel || corner.coordinate_frame != center.coordinate_frame) {
            return issue(ErrorCode::CoordinateMismatch, "Blob bounds and centroid must use the same Pixel frame.");
        }
        blobs.push_back({{corner.x, corner.y, static_cast<double>(*width->as_int64()),
            static_cast<double>(*height->as_int64())}, center, *area->as_int64()});
    }
    return blobs;
}
} // namespace

ImagePanel::ImagePanel(QWidget* parent)
    : QWidget(parent), view_(new ImageView(this)), image_info_(new QLabel(this)), pixel_info_(new QLabel(this))
{
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("Fit Image"), this);
    fit->setObjectName(QStringLiteral("fitImage"));
    fit->setToolTip(QStringLiteral("Fit the image to the view. Scroll to zoom, drag to pan."));
    connect(fit, &QPushButton::clicked, view_, &ImageView::fit_image);
    image_info_->setObjectName(QStringLiteral("imageInfo"));
    image_info_->setWordWrap(true);
    pixel_info_->setObjectName(QStringLiteral("imagePixel"));
    view_->setObjectName(QStringLiteral("imageView"));
    view_->setMinimumSize(260, 170);
    controls->addWidget(fit);
    controls->addWidget(image_info_, 1);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);
    layout->addWidget(pixel_info_);
    connect(view_, &ImageView::pixel_changed, pixel_info_, &QLabel::setText);
    clear_frame();
}

Result<std::monostate> ImagePanel::set_frame(const ImageHandle& image)
{
    if (auto shown = view_->set_image(image); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    frame_ = image;
    const auto frame = image.coordinate_frame().text();
    image_info_->setText(QStringLiteral("%1 × %2  ·  %3")
        .arg(image.width()).arg(image.height()).arg(QString::fromUtf8(frame.data(), static_cast<qsizetype>(frame.size()))));
    pixel_info_->clear();
    emit frame_changed();
    return std::monostate{};
}

Result<std::monostate> ImagePanel::set_overlays(const std::optional<RegionDefinition>& region,
    const std::optional<RasterMask>& mask)
{
    return view_->set_overlays(region, mask);
}

Result<std::monostate> ImagePanel::present_result(const NodeResult& image_result, const OutputId& image_output,
    const NodeResult* roi_result, const NodeResult* blob_result)
{
    for (const auto* result : {roi_result, blob_result}) {
        if (result && result->data().identity.run != image_result.data().identity.run) {
            return issue(ErrorCode::InvalidValue, "Image and overlays must come from the same run.");
        }
    }
    const auto* image = output_value(image_result, image_output);
    const auto* frame = image ? image->as_image() : nullptr;
    if (!frame) { return issue(ErrorCode::InvalidValue, "The selected result has no image at this output."); }
    std::optional<RegionDefinition> region;
    std::optional<RasterMask> mask;
    if (roi_result) {
        const auto* outline = output_value(*roi_result, key<OutputId>("effective_roi"));
        const auto* members = output_value(*roi_result, key<OutputId>("roi_mask"));
        if (!outline || !outline->as_region2d() || !members || !members->as_raster_mask()) {
            return issue(ErrorCode::InvalidValue, "The selected result has no effective ROI and member mask.");
        }
        region = *outline->as_region2d();
        mask = *members->as_raster_mask();
    }
    std::vector<BlobAnnotation> blobs;
    if (blob_result) {
        auto annotations = read_blobs(*blob_result);
        if (auto* failure = std::get_if<Diagnostic>(&annotations)) { return std::move(*failure); }
        blobs = std::get<std::vector<BlobAnnotation>>(std::move(annotations));
    }
    if (auto shown = set_frame(frame->image); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    return view_->set_overlays(region, mask, blobs);
}

void ImagePanel::clear_overlays()
{
    if (frame_) { (void)view_->set_overlays({}, {}); }
}

void ImagePanel::clear_frame()
{
    view_->clear_image();
    frame_.reset();
    image_info_->setText(QStringLiteral("Open an image from the View menu."));
    pixel_info_->clear();
    emit frame_changed();
}

} // namespace visionpro::ui
