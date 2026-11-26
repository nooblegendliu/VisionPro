#include "image_panel.hpp"
#include "workspace_style.hpp"
#include <QComboBox>
#include <QCache>
#include <QListView>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QSignalBlocker>
#include "class_palette.hpp"

#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/workflow/run.hpp>

#include <QHBoxLayout>
#include <QDialog>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace visionpro::ui {
namespace {
using namespace core;
using workflow::NodeResult;

class InfoLabel final : public QLabel {
public:
    explicit InfoLabel(QWidget* parent) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return {0, fontMetrics().height()}; }
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
            fontMetrics().elidedText(text().simplified(), Qt::ElideRight, contentsRect().width()));
    }
};

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

Result<std::vector<BlobAnnotation>> read_blobs(const NodeResult& result, const OutputId& port = key<OutputId>("components"))
{
    const auto* value = output_value(result, port);
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

class SourceThumbnailDelegate final : public QStyledItemDelegate {
public:
    explicit SourceThumbnailDelegate(QListView* view) : QStyledItemDelegate(view), view_(view) {}

    void reset(ImagePanel::ThumbnailProvider provider) {
        provider_ = std::move(provider);
        icons_.clear();
    }
    void invalidate() { icons_.clear(); view_->viewport()->update(); }
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return view_->gridSize(); }

    void paint(QPainter* painter, const QStyleOptionViewItem& options, const QModelIndex& index) const override
    {
        QStyleOptionViewItem item(options);
        initStyleOption(&item, index);
        if (item.text.startsWith(QStringLiteral("来源图 · "))) {
            item.text.remove(0, QStringLiteral("来源图 · ").size());
            if (item.text.endsWith(QStringLiteral(" · 检测图"))) { item.text.chop(QStringLiteral(" · 检测图").size()); }
        } else if (const auto split = item.text.lastIndexOf(QStringLiteral(" · ")); split >= 0) {
            item.text = item.text.mid(split + 3);
        }
        if (provider_) {
            auto* icon = icons_.object(index.row());
            if (!icon) {
                QIcon rendered;
                try {
                    const auto preview = provider_(index.row());
                    if (const auto* image = std::get_if<QImage>(&preview)) { rendered = QPixmap::fromImage(*image); }
                } catch (...) {
                    // Selecting an unavailable preview still opens the full source diagnostic.
                }
                if (rendered.isNull()) { rendered = view_->style()->standardIcon(QStyle::SP_MessageBoxWarning); }
                icon = new QIcon(std::move(rendered));
                icons_.insert(index.row(), icon);
            }
            item.icon = *icon;
            item.features |= QStyleOptionViewItem::HasDecoration;
            item.decorationPosition = QStyleOptionViewItem::Top;
            item.decorationAlignment = Qt::AlignCenter;
            item.decorationSize = view_->iconSize();
            item.displayAlignment = Qt::AlignHCenter | Qt::AlignVCenter;
        }
        view_->style()->drawControl(QStyle::CE_ItemViewItem, &item, painter, view_);
    }
private:
    QListView* view_;
    ImagePanel::ThumbnailProvider provider_;
    mutable QCache<int, QIcon> icons_{24};
};

ImagePanel::ImagePanel(QWidget* parent)
    : QWidget(parent), view_(new ImageView(this)), image_info_(new InfoLabel(this)), provenance_(new InfoLabel(this)),
      pixel_info_(new InfoLabel(this)), legend_(new QTreeWidget(this))
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
    provenance_->setObjectName(QStringLiteral("imageProvenance"));
    pixel_info_->setObjectName(QStringLiteral("imagePixel"));
    const auto secondary = workspace_font(WorkspaceText::Secondary);
    image_info_->setFont(secondary);
    provenance_->setFont(secondary);
    pixel_info_->setFont(secondary);
    view_->setObjectName(QStringLiteral("imageView"));
    view_->setAccessibleName(tr("输出图像"));
    view_->setAccessibleDescription(tr("方向键逐像素取样，加号和减号缩放，数字 1 恢复原始像素大小。"));
    view_->setToolTip(view_->accessibleDescription());
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
    connect(sources_, &QComboBox::currentTextChanged, sources_, &QWidget::setToolTip);
    connect(follow_, &QPushButton::clicked, this, &ImagePanel::follow_requested);
    controls->addWidget(sources_, 2);
    controls->addWidget(follow_);
    controls->addWidget(provenance_, 2);
    layout->addLayout(controls);
    layout->addWidget(view_, 1);
    thumbnails_ = new QListView(this);
    thumbnails_->setObjectName(QStringLiteral("workspaceImageThumbnails"));
    thumbnails_->setAccessibleName(tr("来源图缩略图"));
    thumbnails_->setFont(secondary);
    thumbnails_->setViewMode(QListView::IconMode);
    thumbnails_->setFlow(QListView::LeftToRight);
    thumbnails_->setWrapping(false);
    thumbnails_->setMovement(QListView::Static);
    thumbnails_->setUniformItemSizes(true);
    thumbnails_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    thumbnails_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    thumbnails_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    thumbnails_->setIconSize({80, 48});
    const int row_height = thumbnails_->iconSize().height() + thumbnails_->fontMetrics().height() + 8;
    thumbnails_->setGridSize({100, row_height});
    thumbnails_->setFixedHeight(row_height + thumbnails_->style()->pixelMetric(QStyle::PM_ScrollBarExtent)
        + 2 * thumbnails_->frameWidth());
    thumbnails_->setModel(sources_->model());
    thumbnail_delegate_ = new SourceThumbnailDelegate(thumbnails_);
    thumbnails_->setItemDelegate(thumbnail_delegate_);
    thumbnails_->hide();
    layout->addWidget(thumbnails_);
    connect(thumbnails_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& current) {
        if (current.isValid()) { sources_->setCurrentIndex(current.row()); }
    });
    connect(sources_, &QComboBox::currentIndexChanged, this, [this](int row) {
        thumbnails_->setCurrentIndex(sources_->model()->index(row, 0));
        emit source_changed();
    });
    legend_->setObjectName(QStringLiteral("classLegend"));
    legend_->setHeaderLabels({tr("类别 ID"), tr("名称"), tr("像素数")});
    legend_->setRootIsDecorated(false);
    legend_->setMaximumHeight(130);
    layout->addWidget(legend_);
    auto* inspection = new QHBoxLayout;
    inspection->setSpacing(workspace_size::small_gap);
    inspection->addWidget(image_info_, 1);
    inspection->addWidget(pixel_info_, 1);
    auto* zoom_level = new QLabel(this);
    zoom_level->setObjectName(QStringLiteral("imageZoomLevel"));
    zoom_level->setFont(workspace_font(WorkspaceText::Technical));
    zoom_level->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    zoom_level->setMinimumWidth(zoom_level->fontMetrics().horizontalAdvance(QStringLiteral("9999%")));
    inspection->addWidget(zoom_level);
    inspection->addWidget(fit);
    fit->setMinimumHeight(workspace_size::control_height);
    connect(view_, &ImageView::zoom_changed, zoom_level, [zoom_level](double ratio) {
        zoom_level->setText(QString::number(qRound(100 * ratio)) + u'%');
    });
    connect(this, &ImagePanel::frame_changed, this, [this, fit, zoom_level] {
        fit->setEnabled(frame_.has_value());
        zoom_level->setVisible(frame_.has_value());
    });
    const auto button = [&](const QString& name, const QString& label, const QString& description, auto invoke) {
        auto* control = new QToolButton(this);
        control->setObjectName(name);
        control->setText(label);
        control->setAccessibleName(description);
        control->setToolTip(description);
        control->setMinimumHeight(workspace_size::control_height);
        inspection->addWidget(control);
        connect(control, &QToolButton::clicked, this, invoke);
        connect(this, &ImagePanel::frame_changed, control, [this, control] { control->setEnabled(frame_.has_value()); });
    };
    button("imageZoomOut", QStringLiteral("−"), tr("缩小图像 (−)"), [this] { view_->zoom_by(1 / 1.2); });
    button("imageZoomIn", QStringLiteral("+"), tr("放大图像 (+)"), [this] { view_->zoom_by(1.2); });
    button("imageActualSize", QStringLiteral("1:1"), tr("原始像素大小 (1)"), [this] { view_->actual_size(); });
    button("imageFullscreen",tr("全屏"),tr("全屏查看图像"),[this,layout] {
        if (auto* open = findChild<QDialog*>("fullScreenImage"); open && open->isVisible()) { open->raise(); return; }
        auto* full = new QDialog(this);
        full->setObjectName("fullScreenImage");
        full->setAttribute(Qt::WA_DeleteOnClose);
        full->setWindowTitle(tr("全屏图像"));
        auto* content = new QVBoxLayout(full);
        auto* close = new QPushButton(tr("退出全屏（Esc）"),full);
        const int position = layout->indexOf(view_);
        content->addWidget(view_,1);
        content->addWidget(close,0,Qt::AlignRight);
        connect(close,&QPushButton::clicked,full,&QDialog::accept);
        connect(full,&QDialog::finished,this,[this,layout,position] {
            layout->insertWidget(position,view_,1);
            view_->show();
            view_->fit_image();
        });
        full->showFullScreen();
        view_->show();
        view_->fit_image();
    });
    layout->addLayout(inspection);
    connect(view_, &ImageView::pixel_changed, this, [this](const QString& sample) {
        pixel_info_->setText(sample);
        pixel_info_->setToolTip(sample);
    });
    clear_frame();
}

Result<std::monostate> ImagePanel::set_pixels(const ImageHandle& image, const std::optional<RunImageSource>& source,
    const QList<QRgb>& palette)
{
    auto shown = view_->set_image(image, palette, source && source == source_);
    if (!std::holds_alternative<Diagnostic>(shown)) { source_ = source; }
    return shown;
}

Result<std::monostate> ImagePanel::set_frame(const ImageHandle& image, const std::optional<RunImageSource>& source)
{
    if (auto shown = set_pixels(image, source); std::holds_alternative<Diagnostic>(shown)) { return shown; }
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
    image_info_->setText(QStringLiteral("%1 × %2 · %3").arg(image.width()).arg(image.height())
        .arg(image.format().order == ChannelOrder::Mono ? QStringLiteral("Mono8")
            : image.format().order == ChannelOrder::RGB ? QStringLiteral("RGB24") : QStringLiteral("BGR8")));
    image_info_->setToolTip(image_info_->text() + QStringLiteral(" · ") + QString::fromUtf8(frame));
    set_provenance({});
    emit frame_changed();
}

Result<std::monostate> ImagePanel::present_class_map(const ClassMap& map, const RunImageSource& source,
    workflow::ClassPalette palette)
{
    if (auto shown = set_pixels(map.pixels.image, source, color_table(palette)); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    class_producer_ = std::pair{source.workflow, source.node};
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
    // A draft edit can recolor a thumbnail even when another source occupies the main view.
    thumbnail_delegate_->invalidate();
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
    const auto& id = image_result.data().identity;
    return present_image(frame->image, {id.run.snapshot.session, id.run.workflow, id.invocation.node, image_output},
        roi_result, blob_result);
}

Result<std::monostate> ImagePanel::present_image(const ImageHandle& image, const RunImageSource& source, const NodeResult* roi_result,
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
    if (auto shown = set_frame(image, source); std::holds_alternative<Diagnostic>(shown)) { return shown; }
    return view_->set_overlays(region, mask, blobs);
}

void ImagePanel::set_provenance(const QString& text)
{
    provenance_->setText(text);
    provenance_->setToolTip(text);
    provenance_->setVisible(!text.isEmpty());
}

Result<std::monostate> ImagePanel::append_graphic(const NodeResult& source,OutputId port,SchemaId item)
{
    const auto* value = output_value(source,port);
    if (!value) { return issue(ErrorCode::InputUnavailable,"The selected graphic was not produced."); }
    if (item.text() == "bounds" || item.text() == "centroids") {
        auto blobs = read_blobs(source,port);
        if (auto* error = std::get_if<Diagnostic>(&blobs)) { return *error; }
        return view_->set_overlays({},{},std::get<std::vector<BlobAnnotation>>(blobs),true,
            item.text() == "bounds",item.text() == "centroids");
    }
    if (const auto* mask = value->as_raster_mask()) { return view_->set_overlays({},*mask,{},true); }
    if (const auto* region = value->as_region2d()) { return view_->set_overlays(*region,{},{},true); }
    return view_->append_geometry(*value);
}

void ImagePanel::set_sources(const QStringList& labels, int selected, ThumbnailProvider thumbnail)
{
    const QSignalBlocker block(sources_);
    const QSignalBlocker selection(thumbnails_->selectionModel());
    thumbnail_delegate_->reset(std::move(thumbnail));
    sources_->clear();
    sources_->addItems(labels);
    for (int row = 0; row < sources_->count(); ++row) { sources_->setItemData(row, labels[row], Qt::ToolTipRole); }
    sources_->setCurrentIndex(selected);
    thumbnails_->setCurrentIndex(sources_->model()->index(selected, 0));
    sources_->setToolTip(sources_->currentText());
    sources_->setVisible(!labels.isEmpty());
    thumbnails_->setVisible(labels.size() > 1);
}

int ImagePanel::selected_source() const { return sources_->currentIndex(); }
void ImagePanel::select_source(int index) { sources_->setCurrentIndex(index); }

void ImagePanel::set_manual_view(bool manual)
{
    follow_->setVisible(manual);
    sources_->setVisible(!manual && sources_->count() > 0);
    thumbnails_->setVisible(!manual && sources_->count() > 1);
}

void ImagePanel::clear_overlays()
{
    if (frame_) { (void)view_->set_overlays({}, {}); }
}

void ImagePanel::clear_frame(bool retain_view)
{
    view_->clear_image(retain_view);
    frame_.reset();
    if (!retain_view) { source_.reset(); }
    class_producer_.reset();
    palette_.clear();
    legend_->clear();
    legend_->hide();
    image_info_->setText(QStringLiteral("暂无图像"));
    image_info_->setToolTip({});
    set_provenance({});
    pixel_info_->clear();
    emit frame_changed();
}

} // namespace visionpro::ui
