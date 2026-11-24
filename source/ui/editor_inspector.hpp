#pragma once

#include <visionpro/core/image.hpp>
#include <visionpro/workflow/run.hpp>

#include <QWidget>
#include <functional>

class QScrollArea;
class QTabBar;

namespace visionpro {
namespace core { class ToolRegistry; }
namespace ui {

class ControlGraphModel;
class ControlScene;

class EditorInspector final : public QWidget {
public:
    using HashFile = std::function<void(const QString&, std::function<void(std::string)>)>;
    EditorInspector(ControlGraphModel& model, ControlScene& scene, const core::ToolRegistry& registry,
        QWidget* parent = nullptr, HashFile hash_file = {});
    void set_preview_image(std::optional<core::ImageHandle> image);
    void edit_workflow_trigger(QWidget* owner, core::WorkflowId flow);
    void pin(std::optional<workflow::NodeTarget> target);
    const std::optional<workflow::NodeTarget>& pinned_node() const noexcept { return pinned_; }
    void refresh();
    bool focus_diagnostic_field(const core::DiagnosticLocation& location);

private:
    void queue_refresh();

    ControlGraphModel& model_;
    ControlScene& scene_;
    const core::ToolRegistry& registry_;
    HashFile hash_file_;
    QScrollArea* scroll_;
    QTabBar* tabs_;
    std::optional<core::ImageHandle> preview_image_;
    std::optional<core::NodeId> displayed_node_;
    std::optional<workflow::NodeTarget> pinned_;
    bool refresh_pending_{false};
};

} // namespace ui
} // namespace visionpro
