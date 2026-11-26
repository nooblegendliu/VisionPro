#pragma once

#include <visionpro/infrastructure/file_resources.hpp>
#include <visionpro/workflow/model.hpp>
#include <visionpro/workflow/run.hpp>
#include <QWidget>
#include <QPointer>
#include <functional>

class QAction;
class QCheckBox;
class QComboBox;
class QFileSystemWatcher;
class QLabel;
class QListWidget;
class QPushButton;

namespace visionpro::ui {

class ControlGraphModel;
class ControlScene;

class ImageSourceStrip final : public QWidget {
    Q_OBJECT
public:
    explicit ImageSourceStrip(QWidget* parent = nullptr);
    void set_selection(ControlGraphModel* model, ControlScene* scene, std::optional<workflow::NodeTarget> target,
        const std::optional<std::filesystem::path>& directory,
        std::span<const infrastructure::NamedResourceRoot> roots, bool editable);
    void add_files(const QStringList& files);
    std::function<std::optional<QString>(const workflow::GlobalReference&)> read_folder_variable;
    void refresh_bound_folder();

signals:
    void preview_requested(const QString& file, core::CoordinateSystemId frame, bool gray);
    void notice(const QString& message);

private:
    const workflow::Node* selected_node() const;
    void load_entries(bool force = false);
    void set_paths(std::vector<core::ResourcePath> paths, QStringList resolved);
    void write_list(std::vector<core::ResourcePath> files, std::vector<core::ConfigValue> checked, int initial);
    void set_parameter(std::string_view name, core::ConfigValue value);
    void preview(int row);
    void update_checks();

    QPointer<ControlGraphModel> model_;
    QPointer<ControlScene> scene_;
    std::optional<workflow::NodeTarget> target_;
    std::optional<std::filesystem::path> directory_;
    std::vector<infrastructure::NamedResourceRoot> roots_;
    std::vector<core::ResourcePath> paths_;
    QStringList resolved_;
    QString watched_path_;
    QString directory_pattern_;
    bool directory_descending_{};
    std::uint64_t generation_{};
    bool editable_{};
    bool watching_{};
    QFileSystemWatcher* watcher_;
    QListWidget* list_;
    QCheckBox* next_;
    QComboBox* range_;
    QLabel* count_;
    QPushButton* add_;
    QPushButton* folder_;
    QAction* remove_;
    QAction* clear_;
    QAction* start_;
};

} // namespace visionpro::ui
