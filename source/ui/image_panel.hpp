#pragma once

#include "image_view.hpp"

#include <QWidget>

class QLabel;

namespace visionpro {
namespace workflow { class NodeResult; }
namespace ui {

class ImagePanel final : public QWidget {
    Q_OBJECT

public:
    explicit ImagePanel(QWidget* parent = nullptr);

    const std::optional<core::ImageHandle>& frame() const noexcept { return frame_; }
    [[nodiscard]] core::Result<std::monostate> set_frame(const core::ImageHandle& image);
    [[nodiscard]] core::Result<std::monostate> set_overlays(const std::optional<core::RegionDefinition>& region,
        const std::optional<core::RasterMask>& mask);
    [[nodiscard]] core::Result<std::monostate> present_result(const workflow::NodeResult& image_result,
        const core::OutputId& image_output, const workflow::NodeResult* roi_result,
        const workflow::NodeResult* blob_result);
    void clear_overlays();
    void clear_frame();

signals:
    void frame_changed();

private:
    ImageView* view_;
    QLabel* image_info_;
    QLabel* pixel_info_;
    std::optional<core::ImageHandle> frame_;
};

} // namespace ui
} // namespace visionpro
