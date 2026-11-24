#pragma once

#include <visionpro/core/result.hpp>
#include <visionpro/core/runtime_id.hpp>
#include <visionpro/workflow/run.hpp>

#include <optional>
#include <map>
#include <vector>
#include <QImage>
#include <QString>

namespace visionpro {
namespace workflow { class RunHandle; struct Solution; }
namespace ui {

class ImagePanel;

struct WorkspaceImageSource {
    workflow::ResultIdentity result;
    std::optional<core::OutputId> output;
    QString label;
    bool upstream{};
};

// The window caches addresses only. The selected run owns the underlying images.
struct WorkspaceImageSelection {
    std::optional<workflow::ResultIdentity> consumer;
    std::vector<WorkspaceImageSource> sources;
    int rendered_index{-2};
    bool waiting_for_result{};
    std::optional<core::WorkflowId> workflow;
    std::optional<core::NodeId> node;
    std::uint64_t edit_generation{};
    std::map<workflow::SnapshotIdentity, std::uint64_t> capture_generations;
    std::optional<workflow::SnapshotIdentity> last_publication;
    enum class Mode { Follow, DraftPreview, RunDetails };
    Mode mode{Mode::Follow};
};

[[nodiscard]] std::vector<WorkspaceImageSource> workspace_image_sources(
    const workflow::RunHandle& run, const workflow::NodeResult& selected);
[[nodiscard]] core::Result<QImage> workspace_thumbnail(const workflow::RunHandle& run,
    const WorkspaceImageSource& source, const workflow::Solution& display_config);

[[nodiscard]] core::Result<std::monostate> show_run_image(ImagePanel& panel, const workflow::RunHandle& run,
    const core::InvocationKey& invocation, const std::optional<core::OutputId>& output,
    const workflow::Solution* display_config = nullptr, bool exact_port = false);

} // namespace ui
} // namespace visionpro
