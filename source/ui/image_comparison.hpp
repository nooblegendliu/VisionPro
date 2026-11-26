#pragma once

#include <visionpro/workflow/run_handle.hpp>
#include <visionpro/core/tool_registry.hpp>
#include <QWidget>
#include <array>
#include <map>

class QComboBox;
class QMenu;

namespace visionpro::ui {
class ImagePanel;

class ImageComparison final : public QWidget {
public:
    explicit ImageComparison(QWidget* parent);
    void update_run(const workflow::Workflow* flow,const core::ToolRegistry& registry,
        const std::optional<workflow::RunHandle>& run,std::optional<core::ExecutionScopeId> scope);
    void reset_document();
    void suspend();
private:
    struct Channel {
        core::NodeId node;
        core::OutputId port;
        std::optional<core::SchemaId> item;
        std::size_t occurrence;
        bool operator==(const Channel&) const = default;
    };
    struct Choice {
        Channel channel;
        QString text;
        std::optional<core::InvocationKey> call;
        workflow::ExecutionStatus status;
        bool operator==(const Choice&) const = default;
    };
    struct Selection { std::optional<Channel> image; std::vector<Channel> graphics; };
    struct Pane { QComboBox* source{}; QMenu* graphics{}; ImagePanel* image{}; };
    void populate(int side);
    void render(int side);
    std::array<Pane,2> panes_;
    std::map<core::WorkflowId,std::array<Selection,2>> chosen_;
    std::optional<core::WorkflowId> flow_;
    std::optional<workflow::RunHandle> run_;
    std::optional<core::ExecutionScopeId> scope_;
    bool quiescent_{};
    std::vector<Choice> images_,graphics_;
};
} // namespace visionpro::ui
