#pragma once

#include "image_view.hpp"
#include <visionpro/core/class_map.hpp>
#include <visionpro/workflow/model.hpp>

#include <QWidget>
#include <QImage>
#include <functional>

class QLabel;
class QTreeWidget;
class QComboBox;
class QListView;
class QPushButton;

namespace visionpro {
namespace workflow { class NodeResult; }
namespace ui {
class SourceThumbnailDelegate;

// Run and invocation numbers change between frames. The displayed channel and its
// resolved input path (for an overlay) identify what the operator is inspecting.
struct RunImageSource {
    core::SolutionSessionId session;
    core::WorkflowId workflow;
    core::NodeId node;
    std::variant<core::OutputId, workflow::IndexSource> channel;
    bool operator==(const RunImageSource&) const = default;
};

class ImagePanel final : public QWidget {
    Q_OBJECT

public:
    explicit ImagePanel(QWidget* parent = nullptr);

    const std::optional<core::ImageHandle>& frame() const noexcept { return frame_; }
    [[nodiscard]] core::Result<std::monostate> set_frame(const core::ImageHandle& image,
        const std::optional<RunImageSource>& source = {});
    [[nodiscard]] core::Result<std::monostate> present_class_map(const core::ClassMap& map, const RunImageSource& source,
        workflow::ClassPalette palette);
    [[nodiscard]] core::Result<std::monostate> refresh_class_palette(const workflow::Solution& draft);
    [[nodiscard]] core::Result<std::monostate> set_overlays(const std::optional<core::RegionDefinition>& region,
        const std::optional<core::RasterMask>& mask);
    [[nodiscard]] core::Result<std::monostate> present_result(const workflow::NodeResult& image_result,
        const core::OutputId& image_output, const workflow::NodeResult* roi_result,
        const workflow::NodeResult* blob_result);
    [[nodiscard]] core::Result<std::monostate> present_image(const core::ImageHandle& image,
        const RunImageSource& source, const workflow::NodeResult* roi_result, const workflow::NodeResult* blob_result);
    void set_provenance(const QString& text);
    using ThumbnailProvider = std::function<core::Result<QImage>(int)>;
    void set_sources(const QStringList& labels, int selected, ThumbnailProvider thumbnail = {});
    int selected_source() const;
    void set_manual_view(bool manual);
    void clear_overlays();
    void clear_frame(bool retain_view = false);
    void reset_view_source() { source_.reset(); }

signals:
    void frame_changed();
    void source_changed();
    void follow_requested();

private:
    void describe_frame(const core::ImageHandle& image);
    void color_legend();
    [[nodiscard]] core::Result<std::monostate> set_pixels(const core::ImageHandle& image,
        const std::optional<RunImageSource>& source, const QList<QRgb>& palette = {});

    ImageView* view_;
    QLabel* image_info_;
    QLabel* provenance_;
    QLabel* pixel_info_;
    QTreeWidget* legend_;
    QComboBox* sources_;
    QListView* thumbnails_;
    SourceThumbnailDelegate* thumbnail_delegate_;
    QPushButton* follow_;
    std::optional<std::pair<core::WorkflowId, core::NodeId>> class_producer_;
    workflow::ClassPalette palette_;
    std::optional<core::ImageHandle> frame_;
    std::optional<RunImageSource> source_;
};

} // namespace ui
} // namespace visionpro
