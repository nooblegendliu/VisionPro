#include "image_panel.hpp"
#include "workspace_style.hpp"
#include <QComboBox>
#include <QSignalBlocker>
#include "class_palette.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run.hpp>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace visionpro::ui {
namespace {
using namespace core;
using workflow::NodeResult;

template<class Id>
Id key(std::string_view name) { return std::get<Id>(SchemaIdFactory::parse<Id>(name)); }

Diagnostic issue(ErrorCode code, std::string message) { return {code, Severity::Error, {}, std::move(message)}; }

QList<QRgb> color_table(const workflow::ClassPalette& palette)
{
    QList<QRgb> colors;
    colors.reserve(256);
    for (int id = 0; id < 256; ++id) { colors.append(class_color(palette, static_cast<std::uint8_t>(id)).rgb()); }
    return colors;
}

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
    : QWidget(parent), view_(new ImageView(this)), image_info_(new QLabel(this)), provenance_(new QLabel(this)),
      pixel_info_(new QLabel(this)), legend_(new QTreeWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 4);
    layout->setSpacing(4);
    auto* controls = new QHBoxLayout;
    auto* fit = new QPushButton(QStringLiteral("适应窗口"), this);
    fit->setObjectName(QStringLiteral("fitImage"));
    fit->setToolTip(QStringLiteral("适应图像窗口。滚轮缩放，拖动平移。"));
    connect(fit, &QPushButton::clicked, view_, &ImageView::fit_image);
    image_info_->setObjectName(QStringLiteral("imageInfo"));
    image_info_->setWordWrap(true);
    provenance_->setObjectName(QStringLiteral("imageProvenance"));
    provenance_->setTextFormat(Qt::PlainText);
    provenance_->setWordWrap(true);
    pixel_info_->setObjectName(QStringLiteral("imagePixel"));
    const auto secondary = workspace_font(WorkspaceText::Secondary);
    image_info_->setFont(secondary);
    provenance_->setFont(secondary);
    pixel_info_->setFont(secondary);
    view_->setObjectName(QStringLiteral("imageView"));
    view_->setMinimumSize(260, 170);
    sources_ = new QComboBox(this);
    sources_->setObjectName(QStringLiteral("workspaceImageSource"));
    sources_->setAccessibleName(QStringLiteral("输出图来源"));
    sources_->setMinimumContentsLength(7);
    sources_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    sources_->setPlaceholderText(QStringLiteral("选择来源图"));
    sources_->hide();
    follow_ = new QPushButton(QStringLiteral("返回运行输出"), this);
    follow_->setObjectName(QStringLiteral("resumeWorkspaceImage"));
    follow_->hide();
    connect(sources_, &QComboBox::currentIndexChanged, this, &ImagePanel::source_changed);
    connect(sources_, &QComboBox::currentTextChanged, sources_, &QWidget::setToolTip);
    connect(follow_, &QPushButton::clicked, this, &ImagePanel::follow_requested);
    controls->addWidget(fit);
    controls->addWidget(sources_, 2);
    controls->addWidget(follow_);
    controls->addWidget(image_info_, 1);
    layout->addLayout(controls);
    layout->addWidget(provenance_);
    layout->addWidget(view_, 1);
    legend_->setObjectName(QStringLiteral("classLegend"));
    legend_->setHeaderLabels({QStringLiteral("Class ID"), QStringLiteral("Name"), QStringLiteral("Pixels")});
    legend_->setRootIsDecorated(false);
    legend_->setMaximumHeight(130);
    layout->addWidget(legend_);
    layout->addWidget(pixel_info_);
    connect(view_, &ImageView::pixel_changed, pixel_info_, &QLabel::setText);
    clear_frame();
}

Result<std::monostate> ImagePanel::set_frame(const ImageHandle& image)
{
    if (auto shown = view_->set_image(image); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    class_producer_.reset();
    palette_.clear();
    legend_->clear();
    legend_->hide();
    describe_frame(image);
    return std::monostate{};
}

void ImagePanel::describe_frame(const ImageHandle& image)
{
    frame_ = image;
    const auto frame = image.coordinate_frame().text();
    image_info_->setText(QStringLiteral("%1 × %2  ·  %3")
        .arg(image.width()).arg(image.height()).arg(QString::fromUtf8(frame.data(), static_cast<qsizetype>(frame.size()))));
    set_provenance({});
    pixel_info_->clear();
    emit frame_changed();
}

Result<std::monostate> ImagePanel::present_class_map(const ClassMap& map, const WorkflowId& flow, const NodeId& node,
    workflow::ClassPalette palette)
{
    if (auto shown = view_->set_image(map.pixels.image, color_table(palette)); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    class_producer_ = std::pair{flow, node};
    palette_ = std::move(palette);
    legend_->clear();
    for (const auto& item : map.classes) {
        auto* row = new QTreeWidgetItem(legend_, {QString::number(item.id), QString::fromStdString(item.name), QString::number(item.count)});
        row->setData(0, Qt::UserRole, item.id);
    }
    color_legend();
    legend_->show();
    describe_frame(map.pixels.image);
    return std::monostate{};
}

void ImagePanel::color_legend()
{
    for (int i = 0; i < legend_->topLevelItemCount(); ++i) {
        auto* row = legend_->topLevelItem(i);
        row->setData(0, Qt::DecorationRole, class_color(palette_, static_cast<std::uint8_t>(row->data(0, Qt::UserRole).toUInt())));
    }
}

Result<std::monostate> ImagePanel::refresh_class_palette(const workflow::Solution& draft)
{
    if (!class_producer_) { return std::monostate{}; }
    const auto* saved = workflow::find_placement(draft, class_producer_->first, class_producer_->second);
    auto colors = saved ? saved->class_palette : workflow::ClassPalette{};
    if (colors == palette_) { return std::monostate{}; }
    if (auto shown = view_->recolor_labels(color_table(colors)); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    palette_ = std::move(colors);
    color_legend();
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
    return present_image(frame->image, roi_result, blob_result);
}

Result<std::monostate> ImagePanel::present_blob(const ImageHandle& input_image, const NodeResult& blob_result)
{
    return present_image(input_image, nullptr, &blob_result);
}

Result<std::monostate> ImagePanel::present_image(const ImageHandle& image, const NodeResult* roi_result,
    const NodeResult* blob_result)
{
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
    if (auto shown = set_frame(image); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    return view_->set_overlays(region, mask, blobs);
}

void ImagePanel::set_provenance(const QString& text)
{
    provenance_->setText(text);
    provenance_->setVisible(!text.isEmpty());
}

void ImagePanel::set_sources(const QStringList& labels, int selected)
{
    const QSignalBlocker block(sources_);
    sources_->clear();
    sources_->addItems(labels);
    sources_->setCurrentIndex(selected);
    sources_->setToolTip(sources_->currentText());
    sources_->setVisible(!labels.isEmpty());
}

int ImagePanel::selected_source() const { return sources_->currentIndex(); }

void ImagePanel::set_manual_view(bool manual)
{
    follow_->setVisible(manual);
    sources_->setVisible(!manual && sources_->count() > 0);
}

void ImagePanel::clear_overlays()
{
    if (frame_) { (void)view_->set_overlays({}, {}); }
}

void ImagePanel::clear_frame()
{
    view_->clear_image();
    frame_.reset();
    class_producer_.reset();
    palette_.clear();
    legend_->clear();
    legend_->hide();
    image_info_->setText(QStringLiteral("暂无图像"));
    set_provenance({});
    pixel_info_->clear();
    emit frame_changed();
}

} // namespace visionpro::ui
