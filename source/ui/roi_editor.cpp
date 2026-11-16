#include "roi_editor.hpp"

#include "image_view.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/tool_registry.hpp>
#include <visionpro/workflow/model.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <numbers>
#include <string_view>

namespace visionpro::ui {
namespace {
using namespace core;
using workflow::Node;
using workflow::Workflow;

constexpr double radians_per_degree = std::numbers::pi / 180.0;

QString text(std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

Diagnostic invalid(ErrorCode code, std::string message)
{
    return {code, Severity::Error, {}, std::move(message)};
}

template<class E> void add_choice(QComboBox* box, QString name, E value)
{
    box->addItem(std::move(name), static_cast<int>(value));
}

template<class E> E choice(const QComboBox* box)
{
    return static_cast<E>(box->currentData().toInt());
}

template<class E> void select(QComboBox* box, E value)
{
    box->setCurrentIndex(box->findData(static_cast<int>(value)));
}

template<class E> bool contains(const std::vector<E>& values, E value)
{
    return std::ranges::find(values, value) != values.end();
}

QComboBox* combo(QWidget* owner, const char* name)
{
    auto* control = new QComboBox(owner);
    control->setObjectName(QString::fromLatin1(name));
    return control;
}

QString number(double value)
{
    return QString::number(value, 'g', std::numeric_limits<double>::max_digits10);
}

QLineEdit* field(QWidget* owner, const char* name, const QString& value = {})
{
    auto* control = new QLineEdit(value, owner);
    control->setObjectName(QString::fromLatin1(name));
    return control;
}

double read_number(const QLineEdit* field)
{
    bool ok{};
    const auto value = field->text().toDouble(&ok);
    return ok ? value : std::numeric_limits<double>::quiet_NaN();
}

std::size_t read_size(const QLineEdit* field)
{
    bool ok{};
    const auto input = field->text().trimmed();
    if (input.startsWith(QLatin1Char('-'))) { return 0; }
    const auto value = input.toULongLong(&ok);
    return ok && value <= std::numeric_limits<std::size_t>::max() ? static_cast<std::size_t>(value) : 0;
}

QString size_text(std::size_t value)
{
    return value ? QString::number(static_cast<qulonglong>(value)) : QString{};
}

struct Frame {
    CoordinateSystemId id;
    MeasurementUnit unit;
};

QString frame_name(const Frame& frame)
{
    return QStringLiteral("%1 (%2)").arg(text(frame.id.text()),
        frame.unit == MeasurementUnit::Pixel ? QStringLiteral("px") : QStringLiteral("mm"));
}

class FrameBox final : public QComboBox {
public:
    FrameBox(QWidget* owner, const char* name, const RoiEditContext& context,
        MeasurementUnit unit = MeasurementUnit::Unspecified, const std::optional<CoordinateSystemId>& fixed = {})
        : QComboBox(owner)
    {
        setObjectName(QString::fromLatin1(name));
        for (const auto& frame : context.solution.coordinates) {
            if (frame.unit != MeasurementUnit::Pixel && frame.unit != MeasurementUnit::Millimetre) { continue; }
            if ((unit != MeasurementUnit::Unspecified && frame.unit != unit) || (fixed && frame.id != *fixed)) { continue; }
            frames_.push_back({frame.id, frame.unit});
            addItem(frame_name(frames_.back()));
        }
    }

    const Frame* frame() const
    {
        return currentIndex() < 0 ? nullptr : &frames_[static_cast<std::size_t>(currentIndex())];
    }

    void find_frame(const CoordinateSystemId& id)
    {
        const auto it = std::ranges::find(frames_, id, &Frame::id);
        const QSignalBlocker blocked(this);
        setCurrentIndex(it == frames_.end() ? -1 : static_cast<int>(it - frames_.begin()));
    }

private:
    std::vector<Frame> frames_;
};

bool simple_transform(const Transform2D& value)
{
    return (value.model == TransformModel::Similarity || value.model == TransformModel::Affine)
        && !value.calibration && !value.valid_domain;
}

class TransformForm final : public QWidget {
public:
    TransformForm(QWidget* owner, const std::optional<Transform2D>& original, bool mask)
        : QWidget(owner), original_(original)
    {
        auto* layout = new QFormLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        constexpr std::array values{1., 0., 0., 0., 1., 0.};
        constexpr std::array labels{"X from X", "X from Y", "X offset", "Y from X", "Y from Y", "Y offset"};
        constexpr std::array roi_names{"roiCorrectionXX", "roiCorrectionXY", "roiCorrectionX",
            "roiCorrectionYX", "roiCorrectionYY", "roiCorrectionY"};
        constexpr std::array mask_names{"roiMaskXX", "roiMaskXY", "roiMaskX", "roiMaskYX", "roiMaskYY", "roiMaskY"};
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            fields_[i] = field(this, (mask ? mask_names : roi_names)[i], number(original ? original->matrix[i] : values[i]));
            layout->addRow(QString::fromLatin1(labels[i]), fields_[i]);
            if (original && !simple_transform(*original)) { fields_[i]->setReadOnly(true); }
        }
        if (original && !simple_transform(*original)) {
            auto* notice = new QLabel(QStringLiteral("This existing correction is preserved as configured."), this);
            notice->setWordWrap(true);
            layout->addRow(notice);
        }
    }

    Transform2D value(const Frame& from, const Frame& to) const
    {
        if (original_ && !simple_transform(*original_)) { return *original_; }
        auto result = original_.value_or(Transform2D{from.id, to.id, from.unit, to.unit});
        result.from = from.id;
        result.to = to.id;
        result.from_unit = from.unit;
        result.to_unit = to.unit;
        for (std::size_t i = 0; i < fields_.size(); ++i) { result.matrix[i] = read_number(fields_[i]); }
        if (result.model == TransformModel::Similarity && std::holds_alternative<Diagnostic>(validate_transform(result))) {
            result.model = TransformModel::Affine;
        }
        return result;
    }

private:
    std::optional<Transform2D> original_;
    std::array<QLineEdit*, 6> fields_{};
};

QString depth_name(PixelDepth depth)
{
    switch (depth) {
    case PixelDepth::UInt8: return QStringLiteral("UInt8");
    case PixelDepth::UInt16: return QStringLiteral("UInt16");
    case PixelDepth::Float32: return QStringLiteral("Float32");
    case PixelDepth::Float64: return QStringLiteral("Float64");
    }
    return {};
}

class MaskForm final : public QWidget {
public:
    MaskForm(QWidget* owner, const RoiEditContext& context, const MaskConfig* original)
        : QWidget(owner), context_(context), original_(original)
    {
        auto* layout = new QFormLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        resource_ = combo(this, "roiMaskResource");
        resource_->addItem(QStringLiteral("Select a mask resource"));
        for (const auto& resource : context.solution.resources) {
            if (resource.kind != ResourceKind::Mask) { continue; }
            resources_.push_back(resource.id);
            resource_->addItem(QStringLiteral("%1 [%2]").arg(text(resource.name), text(resource.id.to_string()).left(8)));
            if (original && original->resource.id == resource.id) { resource_->setCurrentIndex(resource_->count() - 1); }
        }
        layout->addRow(QStringLiteral("Mask resource"), resource_);
        depth_ = combo(this, "roiMaskDepth");
        for (const auto depth : {PixelDepth::UInt8, PixelDepth::UInt16, PixelDepth::Float32, PixelDepth::Float64}) {
            if (!context.capabilities || contains(context.capabilities->mask_depths, depth)
                || (original && original->format.depth == depth)) { add_choice(depth_, depth_name(depth), depth); }
        }
        if (original) { select(depth_, original->format.depth); }
        layout->addRow(QStringLiteral("Mask sample type"), depth_);
        const auto width = context.image ? context.image->width() : 0;
        const auto height = context.image ? context.image->height() : 0;
        width_ = field(this, "roiMaskWidth", size_text(original ? original->width : width));
        height_ = field(this, "roiMaskHeight", size_text(original ? original->height : height));
        layout->addRow(QStringLiteral("Mask width"), width_);
        layout->addRow(QStringLiteral("Mask height"), height_);
        sampling_ = combo(this, "roiMaskSampling");
        add_choice(sampling_, QStringLiteral("Direct (same frame and size)"), MaskSampling::None);
        add_choice(sampling_, QStringLiteral("Nearest neighbour"), MaskSampling::Nearest);
        if (original) { select(sampling_, original->mapping.sampling); }
        layout->addRow(QStringLiteral("Mask mapping"), sampling_);
        mask_frame_ = new FrameBox(this, "roiMaskFrame", context, MeasurementUnit::Pixel);
        if (original) { mask_frame_->find_frame(original->mask_frame); }
        else if (context.image) { mask_frame_->find_frame(context.image->coordinate_frame()); }
        layout->addRow(QStringLiteral("Mask coordinate frame"), mask_frame_);
        target_width_ = field(this, "roiMaskTargetWidth", size_text(original ? original->mapping.target_width : width));
        target_height_ = field(this, "roiMaskTargetHeight", size_text(original ? original->mapping.target_height : height));
        layout->addRow(QStringLiteral("Mapped width"), target_width_);
        layout->addRow(QStringLiteral("Mapped height"), target_height_);
        transform_ = new TransformForm(this, original ? original->mapping.transform : std::nullopt, true);
        layout->addRow(QStringLiteral("Mask → ROI"), transform_);
        mode_ = combo(this, "roiMaskPixelRule");
        add_choice(mode_, QStringLiteral("Nonzero"), MaskValueMode::NonZero);
        add_choice(mode_, QStringLiteral("At least threshold"), MaskValueMode::AtLeastThreshold);
        if (original) { select(mode_, original->valid_pixels.mode); }
        layout->addRow(QStringLiteral("Valid pixels"), mode_);
        threshold_ = field(this, "roiMaskThreshold", number(original ? original->valid_pixels.threshold : 0));
        layout->addRow(QStringLiteral("Threshold"), threshold_);
        const auto refresh = [=, this] {
            const auto nearest = choice<MaskSampling>(sampling_) == MaskSampling::Nearest;
            layout->setRowVisible(mask_frame_, nearest);
            layout->setRowVisible(target_width_, nearest);
            layout->setRowVisible(target_height_, nearest);
            layout->setRowVisible(transform_, nearest);
            layout->setRowVisible(threshold_, choice<MaskValueMode>(mode_) == MaskValueMode::AtLeastThreshold);
        };
        connect(sampling_, &QComboBox::currentIndexChanged, this, refresh);
        connect(mode_, &QComboBox::currentIndexChanged, this, refresh);
        refresh();
    }

    Result<RegionShape> value(const Frame& roi_frame, bool corrected) const
    {
        if (resource_->currentIndex() < 1 || depth_->currentIndex() < 0 || roi_frame.unit != MeasurementUnit::Pixel) {
            return invalid(ErrorCode::InvalidMask, "Select a mask resource, supported sample type and pixel coordinate frame.");
        }
        const auto id = resources_[static_cast<std::size_t>(resource_->currentIndex() - 1)];
        const auto reference = original_ && original_->resource.id == id
            ? original_->resource : ResourceReference{id, ResourceKind::Mask, std::nullopt};
        if (auto checked = validate_resource_reference(reference, context_.solution.resources);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        const auto width = read_size(width_);
        const auto height = read_size(height_);
        const auto nearest = choice<MaskSampling>(sampling_) == MaskSampling::Nearest;
        const auto* from = nearest ? mask_frame_->frame() : &roi_frame;
        if (!from) { return invalid(ErrorCode::InvalidMask, "Select the mask's pixel coordinate frame."); }
        const auto mapped_width = nearest ? read_size(target_width_) : width;
        const auto mapped_height = nearest ? read_size(target_height_) : height;
        if (!corrected && context_.image && roi_frame.id == context_.image->coordinate_frame()
            && (mapped_width != context_.image->width() || mapped_height != context_.image->height())) {
            return invalid(ErrorCode::CoordinateMismatch, "Mask mapping dimensions must match the target preview image.");
        }
        const auto mode = choice<MaskValueMode>(mode_);
        return RegionShape{MaskConfig{reference, from->id, width, height, {choice<PixelDepth>(depth_), ChannelOrder::Mono},
            {roi_frame.id, mapped_width, mapped_height,
                nearest ? std::optional{transform_->value(*from, roi_frame)} : std::nullopt,
                choice<MaskSampling>(sampling_)},
            {mode, mode == MaskValueMode::NonZero ? 0.0 : read_number(threshold_)}}};
    }

private:
    const RoiEditContext& context_;
    const MaskConfig* original_{};
    std::vector<ResourceId> resources_;
    QComboBox* resource_{};
    QComboBox* depth_{};
    QComboBox* sampling_{};
    QComboBox* mode_{};
    FrameBox* mask_frame_{};
    QLineEdit* width_{};
    QLineEdit* height_{};
    QLineEdit* target_width_{};
    QLineEdit* target_height_{};
    QLineEdit* threshold_{};
    TransformForm* transform_{};
};

enum class SourceKind { FullImage, Empty, AxisAligned, Rotated, Inherited, Mask, Existing };

SourceKind source_kind(const RegionSource& source)
{
    if (std::holds_alternative<InheritedRegion>(source)) { return SourceKind::Inherited; }
    const auto& geometry = std::get<RegionPrimitive>(source).geometry;
    if (std::holds_alternative<FullImageRegion>(geometry)) { return SourceKind::FullImage; }
    if (std::holds_alternative<EmptyRegionShape>(geometry)) { return SourceKind::Empty; }
    if (std::holds_alternative<AxisAlignedRect>(geometry)) { return SourceKind::AxisAligned; }
    if (std::holds_alternative<RotatedRect>(geometry)) { return SourceKind::Rotated; }
    if (const auto* mask = std::get_if<MaskConfig>(&geometry)) {
        if (mask->mapping.sampling != MaskSampling::Linear
            && (!mask->mapping.transform || simple_transform(*mask->mapping.transform))) { return SourceKind::Mask; }
    }
    return SourceKind::Existing;
}

QString source_name(SourceKind source)
{
    switch (source) {
    case SourceKind::FullImage: return QStringLiteral("Full image");
    case SourceKind::Empty: return QStringLiteral("Empty region");
    case SourceKind::AxisAligned: return QStringLiteral("Axis-aligned rectangle");
    case SourceKind::Rotated: return QStringLiteral("Rotated rectangle");
    case SourceKind::Inherited: return QStringLiteral("Inherited region");
    case SourceKind::Mask: return QStringLiteral("Raster mask");
    case SourceKind::Existing: return QStringLiteral("Preserve existing source");
    }
    return {};
}

struct OutputChoice {
    NodeId node;
    OutputId output;
    ValueSemantics semantics;
};

std::optional<NodeId> parent_of(const Workflow& flow, const NodeId& node)
{
    const auto it = std::ranges::find(flow.containment, node, &workflow::Containment::child);
    return it == flow.containment.end() ? std::nullopt : std::optional{it->parent};
}

std::span<const OutputDescriptor> outputs(const ToolRegistry& registry, const Node& node)
{
    if (const auto* group = std::get_if<workflow::GroupConfiguration>(&node.configuration)) { return group->interface.outputs; }
    const auto& configured = std::get<workflow::ToolConfiguration>(node.configuration);
    const auto* tool = registry.find_descriptor(configured.type);
    return tool && tool->config_version == configured.config_version ? std::span<const OutputDescriptor>{tool->outputs}
                                                                   : std::span<const OutputDescriptor>{};
}

class SourceForm final : public QWidget {
public:
    SourceForm(QWidget* owner, const RoiEditContext& context, const RegionSource* original, bool exclusion)
        : QWidget(owner), context_(context), original_(original)
    {
        layout_ = new QFormLayout(this);
        layout_->setContentsMargins(0, 0, 0, 0);
        source_ = combo(this, "roiSource");
        frame_ = new FrameBox(this, "roiSourceFrame", context);
        inherited_ = combo(this, "roiInheritedOutput");
        inherited_->addItem(QStringLiteral("Select a Region2D output"));
        add_outputs(exclusion);
        const auto* previous_output = original ? std::get_if<InheritedRegion>(original) : nullptr;
        if (previous_output && context.workflow && previous_output->workflow == context.workflow->id) {
            const auto it = std::ranges::find_if(outputs_, [&](const OutputChoice& entry) {
                return entry.node == previous_output->node && entry.output == previous_output->output;
            });
            if (it != outputs_.end()) { inherited_->setCurrentIndex(static_cast<int>(it - outputs_.begin()) + 1); }
        }

        const auto* capabilities = context.capabilities;
        const auto add_shape = [&](SourceKind kind, RegionShapeKind shape) {
            if (!capabilities || contains(capabilities->shapes, shape)) { add_choice(source_, source_name(kind), kind); }
        };
        add_shape(SourceKind::FullImage, RegionShapeKind::FullImage);
        add_shape(SourceKind::Empty, RegionShapeKind::Empty);
        add_shape(SourceKind::AxisAligned, RegionShapeKind::AxisAlignedRect);
        add_shape(SourceKind::Rotated, RegionShapeKind::RotatedRect);
        if (!outputs_.empty() && (!capabilities || capabilities->inheritance)) {
            add_choice(source_, source_name(SourceKind::Inherited), SourceKind::Inherited);
        }
        add_shape(SourceKind::Mask, RegionShapeKind::RasterMask);
        if (original) {
            auto previous = source_kind(*original);
            if (source_->findData(static_cast<int>(previous)) < 0 || (previous_output && inherited_->currentIndex() == 0)) {
                previous = SourceKind::Existing;
                add_choice(source_, source_name(previous), previous);
            }
            select(source_, previous);
            std::visit([&](const auto& item) { frame_->find_frame(item.coordinate_frame); }, *original);
        }
        layout_->addRow(QStringLiteral("Source"), source_);
        existing_ = new QLabel(QStringLiteral("This existing source uses a shape, mapping or reference outside the editor's available choices."), this);
        existing_->setWordWrap(true);
        layout_->addRow(existing_);
        layout_->addRow(QStringLiteral("Sibling output"), inherited_);
        layout_->addRow(QStringLiteral("Source coordinate frame"), frame_);

        const auto* primitive = original ? std::get_if<RegionPrimitive>(original) : nullptr;
        const auto* axis = primitive ? std::get_if<AxisAlignedRect>(&primitive->geometry) : nullptr;
        const auto* rotated = primitive ? std::get_if<RotatedRect>(&primitive->geometry) : nullptr;
        shape_angle_ = rotated ? rotated->angle_radians : 0.0;
        x_ = field(this, "roiX", number(axis ? axis->x : (rotated ? rotated->center_x : 0.0)));
        y_ = field(this, "roiY", number(axis ? axis->y : (rotated ? rotated->center_y : 0.0)));
        width_ = field(this, "roiWidth", number(axis ? axis->width : (rotated ? rotated->width : 1.0)));
        height_ = field(this, "roiHeight", number(axis ? axis->height : (rotated ? rotated->height : 1.0)));
        angle_ = field(this, "roiAngle", angle_text(shape_angle_));
        layout_->addRow(QStringLiteral("X"), x_);
        layout_->addRow(QStringLiteral("Y"), y_);
        layout_->addRow(QStringLiteral("Width"), width_);
        layout_->addRow(QStringLiteral("Height"), height_);
        layout_->addRow(QStringLiteral("Rectangle angle (°)"), angle_);
        mask_ = new MaskForm(this, context, primitive ? std::get_if<MaskConfig>(&primitive->geometry) : nullptr);
        layout_->addRow(mask_);
        direction_angle_ = original ? std::visit([](const auto& source) { return source.local_direction_radians; }, *original) : 0.0;
        direction_ = field(this, "roiLocalDirection", angle_text(direction_angle_));
        layout_->addRow(QStringLiteral("Local direction (°)"), direction_);
        const auto transform = original ? std::visit([](const auto& source) { return source.transform; }, *original) : std::nullopt;
        correction_ = new QCheckBox(QStringLiteral("Apply position correction"), this);
        correction_->setObjectName(QStringLiteral("roiCorrection"));
        correction_->setChecked(transform.has_value());
        correction_->setEnabled(!capabilities || capabilities->transforms);
        layout_->addRow(correction_);
        transform_ = new TransformForm(this, transform, false);
        layout_->addRow(QStringLiteral("Source → target"), transform_);

        connect(source_, &QComboBox::currentIndexChanged, this, [this] { update_form(); });
        connect(inherited_, &QComboBox::currentIndexChanged, this, [this] { update_form(); });
        connect(correction_, &QCheckBox::toggled, this, [this] { update_form(); });
        connect(frame_, &QComboBox::currentIndexChanged, this, [this] { update_form(); });
    }

    void set_target(const Frame* frame)
    {
        target_ = frame ? std::optional{*frame} : std::nullopt;
        update_form();
    }

    void monitor_drawing(QCheckBox& draw, ImageView& view)
    {
        draw_ = &draw;
        view_ = &view;
        connect(draw_, &QCheckBox::toggled, this, [this] { update_drawing(); });
        connect(view_, &ImageView::rectangle_drawn, this, [this](const QRectF& rectangle) {
            const auto position = kind() == SourceKind::Rotated ? rectangle.center() : rectangle.topLeft();
            x_->setText(number(position.x()));
            y_->setText(number(position.y()));
            width_->setText(number(rectangle.width()));
            height_->setText(number(rectangle.height()));
        });
        update_drawing();
    }

    Result<RegionSource> value() const
    {
        if (!target_ || source_->currentIndex() < 0) {
            return invalid(ErrorCode::InvalidGeometry, "Select a supported ROI source and declared target coordinate frame.");
        }
        if (kind() == SourceKind::Existing && original_) { return *original_; }
        const auto* frame = frame_->frame();
        if (!frame) { return invalid(ErrorCode::InvalidCoordinate, "Select a declared source coordinate frame."); }
        const auto direction = read_angle(direction_, direction_angle_);
        const auto correction = correction_->isChecked() ? std::optional{transform_->value(*frame, *target_)} : std::nullopt;
        if (kind() == SourceKind::Inherited) {
            const auto* output = inherited_output();
            if (!output) { return invalid(ErrorCode::InvalidBinding, "Select a sibling Region2D output."); }
            if (frame->unit != output->semantics.unit) {
                return invalid(ErrorCode::CoordinateMismatch, "The source frame unit must match the inherited output's unit.");
            }
            return RegionSource{InheritedRegion{context_.workflow->id, output->node, output->output,
                frame->id, frame->unit, direction, correction}};
        }
        auto shape = geometry(*frame);
        if (const auto* failure = std::get_if<Diagnostic>(&shape)) { return *failure; }
        return RegionSource{RegionPrimitive{frame->id, frame->unit, std::get<RegionShape>(std::move(shape)), direction, correction}};
    }

private:
    static QString angle_text(double radians) { return number(radians / radians_per_degree); }

    static double read_angle(const QLineEdit* edit, double original)
    {
        return edit->text() == angle_text(original) ? original : read_number(edit) * radians_per_degree;
    }

    void add_outputs(bool exclusion)
    {
        if (exclusion || !context_.workflow || !context_.node) { return; }
        const auto scope = parent_of(*context_.workflow, context_.node->id);
        for (const auto& node : context_.workflow->nodes) {
            if (node.id == context_.node->id || parent_of(*context_.workflow, node.id) != scope) { continue; }
            for (const auto& output : outputs(context_.registry, node)) {
                if (output.type != DataType::region2d() || (output.semantics.unit != MeasurementUnit::Pixel
                    && output.semantics.unit != MeasurementUnit::Millimetre)) { continue; }
                outputs_.push_back({node.id, output.id, output.semantics});
                inherited_->addItem(QStringLiteral("%1 [%2] → %3").arg(text(node.name), text(node.id.to_string()).left(8),
                    text(output.display_name)));
            }
        }
    }

    SourceKind kind() const { return choice<SourceKind>(source_); }

    const OutputChoice* inherited_output() const
    {
        return inherited_->currentIndex() > 0 ? &outputs_[static_cast<std::size_t>(inherited_->currentIndex() - 1)] : nullptr;
    }

    void update_form()
    {
        const auto source = kind();
        const bool existing = source == SourceKind::Existing;
        const bool inherited = source == SourceKind::Inherited;
        const bool rectangle = source == SourceKind::AxisAligned || source == SourceKind::Rotated;
        const bool rotated = source == SourceKind::Rotated;
        const bool corrected = correction_->isChecked();
        const auto* output = inherited ? inherited_output() : nullptr;
        const auto fixed_frame = output ? output->semantics.coordinate_system : std::nullopt;
        if (fixed_frame) { frame_->find_frame(*fixed_frame); }
        else if (!corrected && target_) { frame_->find_frame(target_->id); }
        frame_->setEnabled(corrected && !fixed_frame);
        layout_->setRowVisible(existing_, existing);
        layout_->setRowVisible(inherited_, inherited);
        layout_->setRowVisible(frame_, !existing && (corrected || inherited));
        for (auto* field : {x_, y_, width_, height_}) { layout_->setRowVisible(field, rectangle); }
        layout_->setRowVisible(angle_, rotated);
        static_cast<QLabel*>(layout_->labelForField(x_))->setText(rotated ? QStringLiteral("Center X") : QStringLiteral("X"));
        static_cast<QLabel*>(layout_->labelForField(y_))->setText(rotated ? QStringLiteral("Center Y") : QStringLiteral("Y"));
        layout_->setRowVisible(mask_, source == SourceKind::Mask);
        layout_->setRowVisible(direction_, !existing);
        layout_->setRowVisible(correction_, !existing);
        layout_->setRowVisible(transform_, !existing && corrected);
        update_drawing();
    }

    void update_drawing()
    {
        if (!view_) { return; }
        const bool drawable = (kind() == SourceKind::AxisAligned || kind() == SourceKind::Rotated)
            && target_ && context_.image && target_->unit == MeasurementUnit::Pixel
            && target_->id == context_.image->coordinate_frame() && !correction_->isChecked();
        draw_->setEnabled(drawable);
        view_->set_rectangle_drawing(drawable && draw_->isChecked());
    }

    Result<RegionShape> geometry(const Frame& frame) const
    {
        switch (kind()) {
        case SourceKind::FullImage: return RegionShape{FullImageRegion{}};
        case SourceKind::Empty: return RegionShape{EmptyRegionShape{}};
        case SourceKind::AxisAligned:
            return RegionShape{AxisAlignedRect{read_number(x_), read_number(y_), read_number(width_), read_number(height_)}};
        case SourceKind::Rotated:
            return RegionShape{RotatedRect{read_number(x_), read_number(y_), read_number(width_), read_number(height_),
                read_angle(angle_, shape_angle_)}};
        case SourceKind::Mask: return mask_->value(frame, correction_->isChecked());
        default: return invalid(ErrorCode::InvalidGeometry, "Select a local ROI geometry.");
        }
    }

    const RoiEditContext& context_;
    const RegionSource* original_{};
    std::optional<Frame> target_;
    std::vector<OutputChoice> outputs_;
    double shape_angle_{};
    double direction_angle_{};
    QFormLayout* layout_{};
    QComboBox* source_{};
    QComboBox* inherited_{};
    QLabel* existing_{};
    FrameBox* frame_{};
    QLineEdit* x_{};
    QLineEdit* y_{};
    QLineEdit* width_{};
    QLineEdit* height_{};
    QLineEdit* angle_{};
    QLineEdit* direction_{};
    QCheckBox* correction_{};
    TransformForm* transform_{};
    MaskForm* mask_{};
    QCheckBox* draw_{};
    ImageView* view_{};
};

QString source_summary(const RegionSource& source)
{
    if (const auto* inherited = std::get_if<InheritedRegion>(&source)) {
        return QStringLiteral("Inherited region — %1").arg(text(inherited->output.text()));
    }
    const auto& geometry = std::get<RegionPrimitive>(source).geometry;
    if (const auto* rectangle = std::get_if<AxisAlignedRect>(&geometry)) {
        return QStringLiteral("Rectangle (%1, %2), %3 × %4").arg(number(rectangle->x), number(rectangle->y),
            number(rectangle->width), number(rectangle->height));
    }
    if (const auto* rectangle = std::get_if<RotatedRect>(&geometry)) {
        return QStringLiteral("Rotated rectangle (%1, %2), %3 × %4").arg(number(rectangle->center_x),
            number(rectangle->center_y), number(rectangle->width), number(rectangle->height));
    }
    return source_name(source_kind(source));
}

QLabel* validation_label(QWidget* owner)
{
    auto* error = new QLabel(owner);
    error->setObjectName(QStringLiteral("roiValidation"));
    error->setWordWrap(true);
    error->hide();
    return error;
}

void show_error(QLabel* label, const Diagnostic& failure)
{
    label->setText(text(failure.message));
    label->show();
}

template<class Callback> void guarded(QLabel* label, const Callback& callback)
{
    try { callback(); }
    catch (const std::exception& failure) {
        label->setText(text(failure.what()));
        label->show();
    }
}

QScrollArea* scroll_contents(QWidget& contents, QWidget* owner)
{
    auto* scroll = new QScrollArea(owner);
    scroll->setWidgetResizable(true);
    scroll->setWidget(&contents);
    return scroll;
}

class RoiDialog final : public QDialog {
public:
    RoiDialog(QWidget* parent, const RoiEditContext& context, const RegionDefinition* current)
        : QDialog(parent), context_(context), current_(current),
          exclusions_(current ? current->exclusions : std::vector<RegionSource>{}),
          id_(current ? Result<RegionId>{current->id} : IdFactory::generate<RegionId>())
    {
        setObjectName(QStringLiteral("roiEditor"));
        setWindowTitle(QStringLiteral("Edit ROI"));
        auto* layout = new QVBoxLayout(this);
        auto* panels = new QHBoxLayout;
        layout->addLayout(panels, 1);
        auto* editor = new QWidget;
        auto* editor_layout = new QVBoxLayout(editor);
        auto* form = new QFormLayout;
        editor_layout->addLayout(form);
        frame_ = new FrameBox(editor, "roiFrame", context, context.semantics.unit, context.semantics.coordinate_system);
        if (current) { frame_->find_frame(current->coordinate_frame); }
        else if (context.image && !context.semantics.coordinate_system) {
            frame_->find_frame(context.image->coordinate_frame());
            if (frame_->currentIndex() < 0 && frame_->count() > 0) { frame_->setCurrentIndex(0); }
        }
        form->addRow(QStringLiteral("ROI coordinate frame"), frame_);
        source_ = new SourceForm(editor, context, current ? &current->source : nullptr, false);
        editor_layout->addWidget(source_);

        auto* policies = new QFormLayout;
        editor_layout->addLayout(policies);
        boundary_ = combo(editor, "roiBoundary");
        add_boundary(RegionBoundaryPolicy::Reject, QStringLiteral("Reject outside image"));
        add_boundary(RegionBoundaryPolicy::Intersect, QStringLiteral("Intersect with image"));
        add_boundary(RegionBoundaryPolicy::Fill, QStringLiteral("Fill outside image"));
        if (current) { select(boundary_, current->boundary_policy); }
        policies->addRow(QStringLiteral("Boundary"), boundary_);
        fill_mode_ = combo(editor, "roiFillMode");
        add_choice(fill_mode_, QStringLiteral("Constant"), RegionFillMode::Constant);
        add_choice(fill_mode_, QStringLiteral("Replicate"), RegionFillMode::Replicate);
        add_choice(fill_mode_, QStringLiteral("Reflect"), RegionFillMode::Reflect);
        if (current && current->fill) { select(fill_mode_, current->fill->mode); }
        policies->addRow(QStringLiteral("Fill mode"), fill_mode_);
        fill_value_ = field(editor, "roiFillValue", number(current && current->fill ? current->fill->value : 0));
        policies->addRow(QStringLiteral("Fill value"), fill_value_);
        const auto refresh_fill = [this, policies] {
            const bool fill = choice<RegionBoundaryPolicy>(boundary_) == RegionBoundaryPolicy::Fill;
            policies->setRowVisible(fill_mode_, fill);
            policies->setRowVisible(fill_value_, fill && choice<RegionFillMode>(fill_mode_) == RegionFillMode::Constant);
        };
        connect(boundary_, &QComboBox::currentIndexChanged, this, refresh_fill);
        connect(fill_mode_, &QComboBox::currentIndexChanged, this, refresh_fill);
        refresh_fill();
        empty_ = combo(editor, "roiEmpty");
        add_choice(empty_, QStringLiteral("Report an error"), EmptyRegionPolicy::Error);
        if (!context.capabilities || context.capabilities->valid_empty_output
            || (current && current->empty_policy == EmptyRegionPolicy::ValidEmptyOutput)) {
            add_choice(empty_, QStringLiteral("Allow valid empty output"), EmptyRegionPolicy::ValidEmptyOutput);
        }
        if (current) { select(empty_, current->empty_policy); }
        policies->addRow(QStringLiteral("Empty region"), empty_);
        setup_exclusions(*editor_layout, editor);
        editor_layout->addStretch();

        auto* scroll = scroll_contents(*editor, this);
        scroll->setMinimumWidth(360);
        panels->addWidget(scroll, 2);
        if (context.image) { setup_preview(*panels); }
        connect(frame_, &QComboBox::currentIndexChanged, this, [this] { source_->set_target(frame_->frame()); });
        source_->set_target(frame_->frame());
        error_ = validation_label(this);
        layout->addWidget(error_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            guarded(error_, [this] {
                auto value = candidate();
                if (const auto* failure = std::get_if<Diagnostic>(&value)) {
                    show_error(error_, *failure);
                    return;
                }
                result_ = std::get<RegionDefinition>(std::move(value));
                QDialog::accept();
            });
        });
        if (preview_) { watch_preview(*editor); }
        resize(context.image ? 900 : 490, 610);
    }

    std::optional<RegionDefinition> take_result() { return std::move(result_); }

private:
    void add_boundary(RegionBoundaryPolicy value, QString name)
    {
        if (!context_.capabilities || contains(context_.capabilities->boundary_policies, value)
            || (current_ && current_->boundary_policy == value)) { add_choice(boundary_, std::move(name), value); }
    }

    Result<RegionDefinition> candidate() const
    {
        const auto* frame = frame_->frame();
        if (!frame) { return invalid(ErrorCode::InvalidCoordinate, "Select a declared ROI coordinate frame."); }
        auto source = source_->value();
        if (const auto* failure = std::get_if<Diagnostic>(&source)) { return *failure; }
        if (const auto* failure = std::get_if<Diagnostic>(&id_)) { return *failure; }
        const auto boundary = choice<RegionBoundaryPolicy>(boundary_);
        const auto fill_mode = choice<RegionFillMode>(fill_mode_);
        const auto fill = boundary == RegionBoundaryPolicy::Fill
            ? std::optional{RegionFill{fill_mode, fill_mode == RegionFillMode::Constant ? read_number(fill_value_) : 0.0}}
            : std::nullopt;
        RegionDefinition region{std::get<RegionId>(id_), frame->id, frame->unit, std::get<RegionSource>(std::move(source)),
            exclusions_, boundary, fill, choice<EmptyRegionPolicy>(empty_)};
        if (auto checked = validate_region_frames(region, context_.solution.coordinates);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        if (context_.capabilities) {
            if (auto checked = validate_region(region, *context_.capabilities);
                const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        }
        return region;
    }

    void setup_exclusions(QVBoxLayout& parent, QWidget* owner)
    {
        auto* group = new QGroupBox(QStringLiteral("Exclusions"), owner);
        parent.addWidget(group);
        auto* layout = new QVBoxLayout(group);
        list_ = new QListWidget(group);
        list_->setObjectName(QStringLiteral("roiExclusions"));
        list_->setMinimumHeight(65);
        list_->setMaximumHeight(110);
        layout->addWidget(list_);
        auto* row = new QHBoxLayout;
        layout->addLayout(row);
        auto* add = new QPushButton(QStringLiteral("Add…"), group);
        add->setObjectName(QStringLiteral("addRoiExclusion"));
        auto* edit = new QPushButton(QStringLiteral("Edit…"), group);
        edit->setObjectName(QStringLiteral("editRoiExclusion"));
        auto* remove = new QPushButton(QStringLiteral("Remove"), group);
        remove->setObjectName(QStringLiteral("removeRoiExclusion"));
        row->addWidget(add);
        row->addWidget(edit);
        row->addWidget(remove);
        row->addStretch();
        const auto update_selection = [edit, remove](int index) {
            edit->setEnabled(index >= 0);
            remove->setEnabled(index >= 0);
        };
        connect(list_, &QListWidget::currentRowChanged, this, update_selection);
        const auto edit_selected = [this] { guarded(error_, [this] { edit_exclusion(list_->currentRow()); }); };
        connect(add, &QPushButton::clicked, this, [this] { guarded(error_, [this] { edit_exclusion(-1); }); });
        connect(edit, &QPushButton::clicked, this, edit_selected);
        connect(list_, &QListWidget::itemDoubleClicked, this, edit_selected);
        connect(remove, &QPushButton::clicked, this, [this] {
            const auto index = list_->currentRow();
            if (index < 0) { return; }
            guarded(error_, [this, index] {
                exclusions_.erase(exclusions_.begin() + index);
                delete list_->takeItem(index);
                queue_preview();
            });
        });
        for (const auto& source : exclusions_) { list_->addItem(source_summary(source)); }
        update_selection(list_->currentRow());
    }

    void edit_exclusion(int index)
    {
        const auto* target = frame_->frame();
        if (!target) {
            show_error(error_, invalid(ErrorCode::InvalidCoordinate, "Select a declared ROI coordinate frame before editing exclusions."));
            return;
        }
        const auto* original = index < 0 ? nullptr : &exclusions_[static_cast<std::size_t>(index)];
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("roiExclusionEditor"));
        dialog.setWindowTitle(index < 0 ? QStringLiteral("Add ROI exclusion") : QStringLiteral("Edit ROI exclusion"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* source = new SourceForm(nullptr, context_, original, true);
        source->set_target(target);
        layout->addWidget(scroll_contents(*source, &dialog));
        auto* notice = validation_label(&dialog);
        layout->addWidget(notice);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            guarded(notice, [&] {
                auto value = source->value();
                if (const auto* failure = std::get_if<Diagnostic>(&value)) { show_error(notice, *failure); return; }
                if (const auto* failure = std::get_if<Diagnostic>(&id_)) { show_error(notice, *failure); return; }
                auto exclusion = std::get<RegionSource>(std::move(value));
                const RegionDefinition probe{std::get<RegionId>(id_), target->id, target->unit, exclusion, {},
                    RegionBoundaryPolicy::Reject, std::nullopt, EmptyRegionPolicy::ValidEmptyOutput};
                if (auto checked = validate_region_frames(probe, context_.solution.coordinates);
                    const auto* failure = std::get_if<Diagnostic>(&checked)) { show_error(notice, *failure); return; }
                if (index < 0) {
                    exclusions_.push_back(std::move(exclusion));
                    list_->addItem(source_summary(exclusions_.back()));
                    list_->setCurrentRow(list_->count() - 1);
                } else {
                    exclusions_[static_cast<std::size_t>(index)] = std::move(exclusion);
                    list_->item(index)->setText(source_summary(exclusions_[static_cast<std::size_t>(index)]));
                }
                queue_preview();
                dialog.accept();
            });
        });
        dialog.resize(445, 470);
        dialog.exec();
    }

    void setup_preview(QHBoxLayout& panels)
    {
        auto* panel = new QWidget(this);
        panels.addWidget(panel, 3);
        auto* layout = new QVBoxLayout(panel);
        auto* view = new ImageView(panel);
        view->setObjectName(QStringLiteral("roiImageView"));
        layout->addWidget(view, 1);
        if (auto loaded = view->set_image(*context_.image); const auto* failure = std::get_if<Diagnostic>(&loaded)) {
            auto* notice = validation_label(panel);
            layout->addWidget(notice);
            show_error(notice, *failure);
            return;
        }
        auto* draw = new QCheckBox(QStringLiteral("Draw rectangle on the image"), panel);
        draw->setObjectName(QStringLiteral("roiDrawRectangle"));
        layout->addWidget(draw);
        source_->monitor_drawing(*draw, *view);
        preview_ = view;
        QTimer::singleShot(0, view, &ImageView::fit_image);
    }

    void watch_preview(const QWidget& editor)
    {
        const auto refresh = [this] { queue_preview(); };
        for (auto* control : editor.findChildren<QComboBox*>()) { connect(control, &QComboBox::currentIndexChanged, this, refresh); }
        for (auto* control : editor.findChildren<QLineEdit*>()) { connect(control, &QLineEdit::textChanged, this, refresh); }
        for (auto* control : editor.findChildren<QCheckBox*>()) { connect(control, &QCheckBox::toggled, this, refresh); }
        queue_preview();
    }

    void queue_preview()
    {
        if (!preview_ || preview_pending_) { return; }
        preview_pending_ = true;
        QTimer::singleShot(0, this, [this] {
            preview_pending_ = false;
            guarded(error_, [this] {
                auto draft = candidate();
                if (auto* region = std::get_if<RegionDefinition>(&draft); region
                    && region->coordinate_frame == context_.image->coordinate_frame()
                    && std::holds_alternative<RegionPrimitive>(region->source)) {
                    auto painted = preview_->set_overlays(std::optional{std::move(*region)}, std::nullopt);
                    if (std::holds_alternative<std::monostate>(painted)) { return; }
                }
                (void)preview_->set_overlays(std::nullopt, std::nullopt);
            });
        });
    }

    const RoiEditContext& context_;
    const RegionDefinition* current_{};
    std::vector<RegionSource> exclusions_;
    Result<RegionId> id_;
    std::optional<RegionDefinition> result_;
    FrameBox* frame_{};
    SourceForm* source_{};
    QComboBox* boundary_{};
    QComboBox* fill_mode_{};
    QComboBox* empty_{};
    QLineEdit* fill_value_{};
    QListWidget* list_{};
    QLabel* error_{};
    ImageView* preview_{};
    bool preview_pending_{false};
};
} // namespace

std::optional<core::RegionDefinition> edit_roi(QWidget* parent, const RoiEditContext& context,
    const core::RegionDefinition* current)
{
    RoiDialog editor(parent, context, current);
    if (editor.exec() != QDialog::Accepted) { return std::nullopt; }
    return editor.take_result();
}

} // namespace visionpro::ui
