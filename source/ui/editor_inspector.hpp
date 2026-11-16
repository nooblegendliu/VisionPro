#pragma once

#include <QWidget>

class QScrollArea;

namespace visionpro {
namespace core { class ToolRegistry; }
namespace ui {

class ControlGraphModel;
class ControlScene;

class EditorInspector final : public QWidget {
public:
    EditorInspector(ControlGraphModel& model, ControlScene& scene, const core::ToolRegistry& registry,
        QWidget* parent = nullptr);
    void refresh();

private:
    void queue_refresh();

    ControlGraphModel& model_;
    ControlScene& scene_;
    const core::ToolRegistry& registry_;
    QScrollArea* scroll_;
    bool refresh_pending_{false};
};

} // namespace ui
} // namespace visionpro
