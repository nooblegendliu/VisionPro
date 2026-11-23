#pragma once

#include "image_view.hpp"
#include <visionpro/core/class_map.hpp>
#include <visionpro/workflow/model.hpp>

#include <QWidget>

class QLabel;
class QTreeWidget;
class QComboBox;
class QPushButton;

namespace visionpro {
namespace workflow { class NodeResult; }
namespace ui {

class ImagePanel final : public QWidget {
    Q_OBJECT

public:
    explicit ImagePanel(QWidget* parent = nullptr);

    const std::optional<core::ImageHandle>& frame() const noexcept { return frame_; }
    [[nodiscard]] core::Result<std::monostate> set_frame(const core::ImageHandle& image);
    [[nodiscard]] core::Result<std::monostate> present_class_map(const core::ClassMap& map, const core::WorkflowId& flow,
        const core::NodeId& node, workflow::ClassPalette palette);
    [[nodiscard]] core::Result<std::monostate> refresh_class_palette(const workflow::Solution& draft);
    [[nodiscard]] core::Result<std::monostate> set_overlays(const std::optional<core::RegionDefinition>& region,
        const std::optional<core::RasterMask>& mask);
    [[nodiscard]] core::Result<std::monostate> present_result(const workflow::NodeResult& image_result,
        const core::OutputId& image_output, const workflow::NodeResult* roi_result,
        const workflow::NodeResult* blob_result);
    [[nodiscard]] core::Result<std::monostate> present_blob(const core::ImageHandle& input_image,
        const workflow::NodeResult& blob_result);
    [[nodiscard]] core::Result<std::monostate> present_measurement(const core::ImageHandle& input_image,
        const workflow::NodeResult& result) { return present_image(input_image, &result, nullptr); }
    void set_provenance(const QString& text);
    void set_sources(const QStringList& labels, int selected);
    int selected_source() const;
    void set_manual_view(bool manual);
    void clear_overlays();
    void clear_frame();

signals:
    void frame_changed();
    void source_changed();
    void follow_requested();

private:
    void describe_frame(const core::ImageHandle& image);
    void color_legend();
    [[nodiscard]] core::Result<std::monostate> present_image(const core::ImageHandle& image,
        const workflow::NodeResult* roi_result, const workflow::NodeResult* blob_result);

    ImageView* view_;
    QLabel* image_info_;
    QLabel* provenance_;
    QLabel* pixel_info_;
    QTreeWidget* legend_;
    QComboBox* sources_;
    QPushButton* follow_;
    std::optional<std::pair<core::WorkflowId, core::NodeId>> class_producer_;
    workflow::ClassPalette palette_;
    std::optional<core::ImageHandle> frame_;
};

} // namespace ui
} // namespace visionpro
