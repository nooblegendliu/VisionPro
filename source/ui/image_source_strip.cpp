#include "image_source_strip.hpp"
#include "control_scene.hpp"
#include "image_source_fields.hpp"
#include <visionpro/core/id_factory.hpp>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;

QString path_text(const std::filesystem::path& path) { return QString::fromStdU16String(path.u16string()); }

QString absolute_file(const ResourcePath& path, const std::optional<std::filesystem::path>& base,
    std::span<const infrastructure::NamedResourceRoot> roots)
{
    const auto name = QString::fromStdString(path.path);
    if (path.base == PathBase::Absolute) { return name; }
    if (path.base == PathBase::SolutionDirectory) {
        return base ? QDir(path_text(*base)).filePath(name) : QString{};
    }
    const auto found = std::ranges::find_if(roots, [&](const auto& root) { return root.id == path.named_root; });
    return found == roots.end() ? QString{} : QDir(path_text(found->directory)).filePath(name);
}

QStringList folder_images(const QString& path, const QString& pattern = {})
{
    QDir dir(path);
    const QStringList filters = pattern.isEmpty() || pattern == QStringLiteral("*")
        ? QStringList{"*.bmp","*.png","*.jpg","*.jpeg","*.tif","*.tiff","*.pgm","*.ppm","*.webp"} : QStringList{pattern};
    QStringList result;
    for (const auto& file : dir.entryList(filters, QDir::Files | QDir::Readable, QDir::Name)) {
        result.push_back(dir.filePath(file));
    }
    return result;
}

Node* edit_target(Solution& solution, const NodeTarget& target)
{
    const auto flow = std::ranges::find(solution.workflows, target.workflow, &Workflow::id);
    if (flow == solution.workflows.end()) { return nullptr; }
    const auto node = std::ranges::find(flow->nodes, target.node, &Node::id);
    return node != flow->nodes.end() && is_image_source(&*node) ? &*node : nullptr;
}

template<class Work, class Apply>
void read_images(QWidget* owner, Work work, Apply apply)
{
    using Value = std::invoke_result_t<Work>;
    auto result = std::make_shared<Value>();
    auto* thread = QThread::create([result, work = std::move(work)] {
        try { *result = work(); } catch (...) { *result = {}; }
    });
    QObject::connect(thread, &QThread::finished, owner, [result, apply = std::move(apply)]() mutable { apply(std::move(*result)); });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

std::vector<ConfigValue> checked_indices(const QListWidget& list, int omit = -1)
{
    std::vector<ConfigValue> indices;
    for (int i = 0; i < list.count(); ++i) {
        if (i != omit && list.item(i)->checkState() == Qt::Checked) {
            indices.emplace_back(ConfigValue::integer(omit >= 0 && i > omit ? i - 1 : i));
        }
    }
    return indices;
}
} // namespace

ImageSourceStrip::ImageSourceStrip(QWidget* parent) : QWidget(parent), watcher_(new QFileSystemWatcher(this))
{
    setObjectName(QStringLiteral("imageSourceStrip"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(3);
    count_ = new QLabel(tr("图像源"), this);
    next_ = new QCheckBox(tr("自动切换"), this);
    next_->setObjectName("imageSourceAutoNext");
    range_ = new QComboBox(this);
    range_->addItems({tr("运行全部"), tr("运行选中")});
    range_->setAccessibleName(tr("图像源运行范围"));
    range_->setObjectName("imageSourceRange");
    add_ = new QPushButton(tr("添加图片"), this);
    add_->setObjectName("addSourceImages");
    folder_ = new QPushButton(tr("添加文件夹"), this);
    folder_->setObjectName("addSourceFolder");
    auto* more = new QToolButton(this);
    more->setText(tr("更多")); more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    more->setMenu(menu);
    const auto menu_button = [&](const QString& label, auto fn) {
        return menu->addAction(label, this, std::move(fn));
    };
    connect(folder_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, tr("添加文件夹内的图像"));
        if (path.isEmpty()) { return; }
        const auto target = target_;
        read_images(this, [path] { return folder_images(path); }, [this, target](QStringList files) {
            if (target != target_ || !editable_) { return; }
            if (files.empty()) { emit notice(tr("文件夹内没有可读图像")); return; }
            add_files(files);
        });
    });
    menu_button(tr("显示首张"), [this] { if (list_->count()) { list_->setCurrentRow(0); preview(0); } });
    start_ = menu_button(tr("从当前图像开始运行"), [this] {
        if (list_->currentRow() >= 0) { write_list(paths_, checked_indices(*list_), list_->currentRow()); }
    });
    remove_ = menu_button(tr("移除当前项"), [this] {
        const int i = list_->currentRow();
        if (i < 0 || static_cast<std::size_t>(i) >= paths_.size()) { return; }
        auto paths = paths_;
        paths.erase(paths.begin() + i);
        write_list(std::move(paths), checked_indices(*list_, i), 0);
    });
    clear_ = menu_button(tr("清空图像清单"), [this] { write_list({}, {}, 0); });
    for (QWidget* w : std::initializer_list<QWidget*>{count_,next_,range_,add_,folder_,more}) { bar->addWidget(w); }
    bar->addStretch();
    layout->addLayout(bar);
    list_ = new QListWidget(this);
    list_->setObjectName("imageSourceThumbnails");
    list_->setAccessibleName(tr("本地图像缩略图与运行选择"));
    list_->setFlow(QListView::LeftToRight);
    list_->setViewMode(QListView::IconMode);
    list_->setWrapping(false);
    list_->setMovement(QListView::Static);
    list_->setIconSize({64, 38});
    list_->setGridSize({88, 64});
    list_->setFixedHeight(80);
    auto* navigator = new QHBoxLayout;
    navigator->setSpacing(2);
    const auto arrow = [&](Qt::ArrowType type, int offset, const QString& label) {
        auto* button = new QToolButton(this);
        button->setArrowType(type);
        button->setAccessibleName(label);
        button->setToolTip(label);
        connect(button, &QToolButton::clicked, this, [this,offset] {
            const int row = list_->currentRow() + offset;
            if (row >= 0 && row < list_->count()) { list_->setCurrentRow(row); preview(row); }
        });
        return button;
    };
    navigator->addWidget(arrow(Qt::LeftArrow,-1,tr("预览上一张图像")));
    navigator->addWidget(list_,1);
    navigator->addWidget(arrow(Qt::RightArrow,1,tr("预览下一张图像")));
    layout->addLayout(navigator);
    connect(add_, &QPushButton::clicked, this, [this] {
        add_files(QFileDialog::getOpenFileNames(this, tr("添加图像"), {}, tr("图像 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.pgm *.ppm *.webp);;所有文件 (*)")));
    });
    connect(next_, &QCheckBox::toggled, this, [this](bool on) { set_parameter("auto_next", ConfigValue::boolean(on)); });
    connect(range_, &QComboBox::activated, this, [this](int index) { set_parameter("selected_only", ConfigValue::boolean(index == 1)); });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem*) {
        const auto* node = selected_node();
        if (node && source_flag(*node, "subscribe_folder")) {
            std::vector<ConfigValue> names;
            for (int i = 0; i < list_->count(); ++i) {
                if (list_->item(i)->checkState() == Qt::Checked) {
                    names.push_back(std::get<ConfigValue>(ConfigValue::string(QFileInfo(resolved_.at(i)).fileName().toStdString())));
                }
            }
            set_parameter("selected_files",std::get<ConfigValue>(ConfigValue::list(DataType::string(),names)));
        } else { set_parameter("selected_indices", std::get<ConfigValue>(ConfigValue::list(DataType::integer(), checked_indices(*list_)))); }
    });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { preview(list_->row(item)); });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { preview(list_->row(item)); });
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { if (watching_) { load_entries(true); } });
    hide();
}

const Node* ImageSourceStrip::selected_node() const
{
    const auto* flow = target_ && model_ ? find_workflow(model_->draft(), target_->workflow) : nullptr;
    return flow ? find_node(*flow, target_->node) : nullptr;
}

void ImageSourceStrip::set_selection(ControlGraphModel* model, ControlScene* scene, std::optional<NodeTarget> target,
    const std::optional<std::filesystem::path>& directory, std::span<const infrastructure::NamedResourceRoot> roots, bool editable)
{
    const bool changed = target_ != target || model_ != model;
    model_ = model; scene_ = scene; target_ = target; directory_ = directory; roots_.assign(roots.begin(), roots.end());
    const auto* node = selected_node();
    const bool local = is_image_source(node) && source_enum(*node, "source_type", "Local") == "Local";
    editable_ = editable && local;
    setVisible(local);
    const bool previously_watching = watching_;
    watching_ = local && source_flag(*node, "subscribe_folder") && source_flag(*node, "refresh_thumbnails");
    if (!local || changed) {
        ++generation_;
        if (!watcher_->directories().isEmpty()) { watcher_->removePaths(watcher_->directories()); }
        watched_path_.clear(); paths_.clear(); resolved_.clear(); list_->clear();
    }
    if (!local) { return; }
    const QSignalBlocker a(next_), b(range_);
    next_->setChecked(source_flag(*node, "auto_next", true));
    range_->setCurrentIndex(source_flag(*node, "selected_only") ? 1 : 0);
    const bool files_editable = editable_ && !source_flag(*node, "subscribe_folder");
    next_->setEnabled(editable_); range_->setEnabled(editable_); add_->setEnabled(files_editable);
    folder_->setEnabled(files_editable); clear_->setEnabled(files_editable); remove_->setEnabled(files_editable);
    start_->setEnabled(files_editable);
    load_entries(watching_ && !previously_watching);
}

void ImageSourceStrip::load_entries(bool force)
{
    const auto* node = selected_node();
    if (!node || !is_image_source(node)) { return; }
    std::vector<ResourcePath> paths;
    QString folder, pattern;
    bool descending = false;
    if (source_flag(*node, "subscribe_folder")) {
        const auto* value = source_value(*node, "folder_path");
        std::optional<QString> current;
        if (!value) {
            const auto binding = std::ranges::find(node->bindings, BindingTarget{source_key<ParameterId>("folder_path")}, &Binding::target);
            const auto* ref = binding != node->bindings.end() && binding->sources.size()==1
                ? std::get_if<GlobalReference>(&binding->sources.front()) : nullptr;
            if (ref && ref->selector.empty()) {
                if (read_folder_variable) { current = read_folder_variable(*ref); }
                if (!current && (!ref->required_revision || *ref->required_revision == 0)) {
                    const auto& vars = model_->draft().variables;
                    const auto it = std::ranges::find(vars,ref->variable,&GlobalVariableDefinition::id);
                    if (it != vars.end()) { value = &it->initial_value; }
                }
            }
        }
        if (current) { folder = *current; }
        else if (value && value->as_string()) { folder = QString::fromUtf8(*value->as_string()); }
        if (!QDir::isAbsolutePath(folder)) { folder.clear(); }
    } else {
        const auto& resources = model_->draft().resources;
        for (const auto& binding : node->resources) {
            if (binding.slot.text() != "source" && binding.slot.text() != "sequence") { continue; }
            const auto it = std::ranges::find(resources, binding.resource.id, &ResourceDefinition::id);
            if (it == resources.end()) { continue; }
            if (const auto* file = std::get_if<ManagedResource>(&it->location)) { paths.push_back(file->path); }
            if (const auto* file = std::get_if<ExternalFileResource>(&it->location)) { paths.push_back(file->path); }
            if (const auto* seq = std::get_if<ImageSequenceResource>(&it->location)) {
                if (const auto* files = std::get_if<std::vector<ResourcePath>>(&seq->selection)) { paths = *files; }
                else {
                    const auto& dir = std::get<DirectorySelection>(seq->selection);
                    folder = absolute_file(dir.directory, directory_, roots_); pattern = QString::fromStdString(dir.filename_pattern);
                    descending = seq->sort == SequenceSort::NameDescending;
                }
            }
        }
    }
    const bool changed = watched_path_ != folder || directory_pattern_ != pattern || directory_descending_ != descending;
    if (changed || (!watching_ && !watcher_->directories().isEmpty())) {
        if (!watcher_->directories().isEmpty()) { watcher_->removePaths(watcher_->directories()); }
        watched_path_ = folder;
        directory_pattern_ = pattern;
        directory_descending_ = descending;
    }
    if (watching_ && !folder.isEmpty() && !watcher_->directories().contains(folder)) { watcher_->addPath(folder); }
    if (!folder.isEmpty()) {
        if (!changed && !force) { update_checks(); return; }
        const auto generation = ++generation_;
        read_images(this, [folder,pattern,descending] {
            auto files = folder_images(folder, pattern); if (descending) { std::ranges::reverse(files); } return files;
        }, [this,generation](QStringList files) {
            if (generation != generation_) { return; }
            std::vector<ResourcePath> entries;
            for (const auto& f : files) { entries.push_back({PathBase::Absolute, f.toStdString(), {}}); }
            set_paths(std::move(entries), std::move(files));
        });
    } else {
        if (changed) { ++generation_; }
        QStringList resolved;
        for (const auto& path : paths) { resolved.push_back(absolute_file(path, directory_, roots_)); }
        set_paths(std::move(paths), std::move(resolved));
    }
}

void ImageSourceStrip::set_paths(std::vector<ResourcePath> paths, QStringList resolved)
{
    paths_ = std::move(paths);
    if (resolved_ == resolved && list_->count() == resolved.size()) { update_checks(); return; }
    const auto previous = resolved_.value(list_->currentRow());
    resolved_ = std::move(resolved);
    const auto generation = ++generation_;
    const QSignalBlocker block(list_);
    list_->clear();
    for (std::size_t i = 0; i < paths_.size(); ++i) {
        const auto shown = QString::fromStdString(paths_[i].path);
        auto* item = new QListWidgetItem(QFileInfo(shown).fileName(), list_);
        item->setToolTip(shown);
    }
    list_->setCurrentRow(std::max(0, static_cast<int>(resolved_.indexOf(previous))));
    update_checks();
    if (resolved_.isEmpty()) { return; }
    read_images(this, [files = resolved_] {
        QList<QImage> images;
        for (const auto& path : files) {
            QImageReader reader(path); const auto size = reader.size();
            if (size.isValid()) { reader.setScaledSize(size.scaled(64, 38, Qt::KeepAspectRatio)); }
            images << reader.read();
        }
        return images;
    }, [this, generation](QList<QImage> icons) {
        if (generation != generation_) { return; }
        const QSignalBlocker blocked(list_);
        for (int i = 0; i < list_->count() && i < icons.size(); ++i) {
            if (!icons[i].isNull()) { list_->item(i)->setIcon(QPixmap::fromImage(icons[i])); }
        }
    });
}

void ImageSourceStrip::refresh_bound_folder()
{
    const auto* node = selected_node();
    if (watching_ && node && !source_value(*node, "folder_path")) { load_entries(); }
}

void ImageSourceStrip::update_checks()
{
    const auto* node = selected_node(); if (!node) { return; }
    const bool folder = source_flag(*node,"subscribe_folder");
    const auto* value = source_value(*node, folder ? "selected_files" : "selected_indices");
    const auto* indices = value ? value->elements() : nullptr;
    const QSignalBlocker block(list_);
    for (int i = 0; i < list_->count(); ++i) {
        auto* item = list_->item(i);
        item->setFlags(item->flags().setFlag(Qt::ItemIsUserCheckable, editable_));
        const auto name = QFileInfo(resolved_.value(i)).fileName().toStdString();
        const bool checked = indices && std::ranges::any_of(*indices, [i,&name,folder](const auto& v) {
            return folder ? v.as_string() && *v.as_string() == name : v.as_int64() && *v.as_int64() == i;
        });
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    count_->setText(tr("图像 %1/%2").arg(list_->currentRow() + 1).arg(list_->count()));
}

void ImageSourceStrip::set_parameter(std::string_view name, ConfigValue value)
{
    if (!editable_ || !target_) { return; }
    const auto target = *target_;
    scene_->edit_draft(tr("编辑图像序列设置"), [&](Solution& draft) -> Result<std::monostate> {
        if (auto* node = edit_target(draft, target)) { set_source_value(*node, name, value); }
        return std::monostate{};
    });
}

void ImageSourceStrip::add_files(const QStringList& files)
{
    if (files.isEmpty() || !editable_ || !target_ || source_flag(*selected_node(), "subscribe_folder")) { return; }
    auto paths = paths_;
    auto chosen = checked_indices(*list_);
    for (const auto& file : files) {
        if (!QDir::isAbsolutePath(file)) { emit notice(tr("请选择具有绝对路径的图片")); return; }
        chosen.push_back(ConfigValue::integer(static_cast<std::int64_t>(paths.size())));
        paths.push_back({PathBase::Absolute, file.toStdString(), {}});
    }
    write_list(std::move(paths), std::move(chosen), std::max(0,list_->currentRow()));
}

void ImageSourceStrip::write_list(std::vector<ResourcePath> files, std::vector<ConfigValue> checked, int initial)
{
    if (!editable_ || !target_) { return; }
    const auto target = *target_;
    scene_->edit_draft(tr("编辑本地图像清单"), [&](Solution& draft) -> Result<std::monostate> {
        auto* node = edit_target(draft, target);
        if (!node) { return Diagnostic{ErrorCode::UnknownTarget, Severity::Error, {}, "The image source no longer exists."}; }
        if (source_flag(*node, "embed_images") && files.size() > 120) {
            return Diagnostic{ErrorCode::InvalidValue, Severity::Error, {}, "方案存图最多支持 120 张图片。"};
        }
        const auto name = "图像源序列 · " + target.node.to_string();
        const auto bound = std::ranges::find_if(node->resources, [](const auto& slot) { return slot.slot.text() == "sequence"; });
        auto existing = bound == node->resources.end() ? draft.resources.end() : std::ranges::find(draft.resources, bound->resource.id, &ResourceDefinition::id);
        if (existing != draft.resources.end()) {
            const auto shared = std::ranges::any_of(draft.workflows,[&](const auto& flow) {
                return std::ranges::any_of(flow.nodes,[&](const auto& other) {
                    return other.id != target.node && std::ranges::any_of(other.resources,[&](const auto& slot) {
                        return slot.resource.id == existing->id;
                    });
                });
            });
            if (existing->name != name || shared) { existing = draft.resources.end(); }
        }
        std::erase_if(node->resources, [](const auto& x) { return x.slot.text() == "source" || x.slot.text() == "sequence"; });
        if (!files.empty()) {
            const auto start = std::min(files.size() - 1, static_cast<std::size_t>(std::max(0,initial)));
            ImageSequenceResource sequence{std::move(files), SequenceSort::ExplicitOrder, SequenceAdvance::PerInvocation,
                SequenceRepeat::Loop, SequenceMissing::Fail, SequenceTrigger::Manual, start};
            if (existing == draft.resources.end()) {
                auto id = IdFactory::generate<ResourceId>();
                if (const auto* fail = std::get_if<Diagnostic>(&id)) { return *fail; }
                draft.resources.push_back({std::get<ResourceId>(id), name, ResourceKind::DirectorySequence, std::move(sequence)});
                existing = std::prev(draft.resources.end());
            } else { existing->location = std::move(sequence); }
            node->resources.push_back({source_key<SchemaId>("sequence"), {existing->id, ResourceKind::DirectorySequence, {}}});
        }
        set_source_value(*node, "selected_indices", std::get<ConfigValue>(ConfigValue::list(DataType::integer(), checked)));
        return std::monostate{};
    });
}

void ImageSourceStrip::preview(int row)
{
    const auto* node = selected_node();
    if (!node || row < 0 || row >= resolved_.size()) { return; }
    const auto* value = source_value(*node, "frame_origin");
    const auto* origin = value ? value->as_point2d() : nullptr;
    if (!origin || resolved_[row].isEmpty()) { emit notice(tr("请检查图像路径和像素坐标系")); return; }
    count_->setText(tr("图像 %1/%2").arg(row + 1).arg(list_->count()));
    emit preview_requested(resolved_[row], origin->coordinate_frame, source_enum(*node,"decode_mode","Gray8") == "Gray8");
}

} // namespace visionpro::ui
