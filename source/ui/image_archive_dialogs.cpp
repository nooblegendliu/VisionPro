#include "image_archive_dialogs.hpp"
#include "background_result.hpp"
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/image_archive.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <limits>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace infrastructure;
using namespace workflow;

QString text(std::string_view value) { return QString::fromUtf8(value); }
std::filesystem::path native_path(const QString& value) { return std::filesystem::path(value.toStdU16String()); }

struct PackageItem {
    QString path;
    std::vector<ImagePackageEntry> entries;
};

Result<std::vector<PackageItem>> load_packages(const QString& path)
{
    QStringList files{path};
    if (QFileInfo(path).isDir()) {
        files.clear();
        for (const auto& file : QDir(path).entryInfoList({"*.dat"}, QDir::Files, QDir::Name)) {
            files.push_back(file.absoluteFilePath());
        }
    }
    std::vector<PackageItem> packages;
    for (const auto& file : files) {
        auto entries = list_image_package(native_path(file));
        if (auto* error = std::get_if<Diagnostic>(&entries)) { return *error; }
        packages.push_back({file, std::get<std::vector<ImagePackageEntry>>(std::move(entries))});
    }
    return packages;
}

class PackageBrowser final : public QDialog {
public:
    explicit PackageBrowser(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle(tr("存图大数据解压工具"));
        setObjectName(QStringLiteral("imagePackageBrowser"));
        setAttribute(Qt::WA_DeleteOnClose);
        setAcceptDrops(true);
        auto* layout = new QVBoxLayout(this);
        auto* toolbar = new QHBoxLayout;
        auto* file = new QPushButton(tr("导入文件…"), this);
        auto* directory = new QPushButton(tr("导入文件夹…"), this);
        toolbar->addWidget(file);
        toolbar->addWidget(directory);
        toolbar->addStretch();
        layout->addLayout(toolbar);
        auto* split = new QSplitter(Qt::Horizontal, this);
        tree_ = new QTreeWidget(split);
        tree_->setObjectName(QStringLiteral("packageEntries"));
        tree_->setHeaderLabels({tr("文件"), tr("来源")});
        tree_->setColumnWidth(0, 235);
        preview_ = new QLabel(split);
        preview_->setTextFormat(Qt::PlainText);
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setMinimumSize(140, 130);
        preview_->setText(tr("选择图像即可预览"));
        split->addWidget(tree_);
        split->addWidget(preview_);
        split->setStretchFactor(1, 1);
        layout->addWidget(split, 1);
        folder_ = new QCheckBox(tr("解压到与 .dat 同名的子目录"), this);
        folder_->setChecked(true);
        layout->addWidget(folder_);
        status_ = new QLabel(tr("导入文件可预览，拖入单个 .dat 文件将直接解压。"), this);
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        layout->addWidget(status_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        extract_ = buttons->addButton(tr("解压"), QDialogButtonBox::ActionRole);
        extract_->setObjectName(QStringLiteral("extractImagePackages"));
        extract_->setEnabled(false);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
        connect(file, &QPushButton::clicked, this, [this] {
            load_path(QFileDialog::getOpenFileName(this, tr("导入存图大文件"), {}, tr("存图大文件 (*.dat)")));
        });
        connect(directory, &QPushButton::clicked, this, [this] {
            load_path(QFileDialog::getExistingDirectory(this, tr("导入包含 .dat 的目录")));
        });
        connect(extract_, &QPushButton::clicked, this, [this] {
            const auto generation = generation_;
            extract_->setEnabled(false);
            status_->setText(tr("正在解压…"));
            on_worker(this, [path=native_path(selected_path_), folders=folder_->isChecked()] {
                return extract_image_packages(path, folders);
            }, [this, generation](auto result) {
                if (generation != generation_) { return; }
                if (const auto* error = std::get_if<Diagnostic>(&result)) { status_->setText(text(error->message)); }
                else { status_->setText(tr("已解压 %1 张图像。").arg(std::get<std::vector<std::filesystem::path>>(result).size())); }
                extract_->setEnabled(true);
            });
        });
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* row) {
            const auto request = ++preview_generation_;
            preview_->clear();
            if (!row || !row->parent()) { return; }
            preview_->setText(tr("正在读取图像…"));
            on_worker(this, [path=native_path(row->parent()->data(0, Qt::UserRole).toString()), index=row->data(0, Qt::UserRole).toULongLong()]() -> Result<QImage> {
                auto data = read_image_package_entry(path, index);
                if (auto* error = std::get_if<Diagnostic>(&data)) { return *error; }
                const auto& bytes = std::get<std::vector<std::uint8_t>>(data);
                auto image = QImage::fromData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
                if (image.isNull()) { return Diagnostic{ErrorCode::InvalidImage, Severity::Error, {}, "The archived image could not be decoded."}; }
                return image.scaled(720, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }, [this,request](auto result) {
                if (request != preview_generation_) { return; }
                if (auto* error = std::get_if<Diagnostic>(&result)) { preview_->setText(text(error->message)); return; }
                preview_->setPixmap(QPixmap::fromImage(std::get<QImage>(std::move(result))).scaled(preview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            });
        });
        resize(780, 485);
    }
protected:
    static QString dropped_path(const QMimeData* data)
    {
        const auto urls = data->urls();
        if (urls.size() != 1 || !urls.front().isLocalFile()) { return {}; }
        const QFileInfo file(urls.front().toLocalFile());
        return file.isDir() || file.suffix().compare(QStringLiteral("dat"), Qt::CaseInsensitive) == 0 ? file.absoluteFilePath() : QString{};
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!dropped_path(event->mimeData()).isEmpty()) { event->acceptProposedAction(); }
    }
    void dropEvent(QDropEvent* event) override
    {
        const auto path = dropped_path(event->mimeData());
        if (!path.isEmpty()) { event->acceptProposedAction(); load_path(path, QFileInfo(path).isFile()); }
    }
private:
    void load_path(const QString& path, bool extract_on_load = false)
    {
        if (path.isEmpty()) { return; }
        const auto request = ++generation_;
        ++preview_generation_;
        selected_path_ = path;
        tree_->clear();
        preview_->clear();
        status_->setText(tr("正在读取 %1…").arg(QDir::toNativeSeparators(path)));
        extract_->setEnabled(false);
        on_worker(this, [path] { return load_packages(path); }, [this, request, extract_on_load](auto result) {
            if (generation_ != request) { return; }
            if (auto* error = std::get_if<Diagnostic>(&result)) { status_->setText(text(error->message)); return; }
            int count = 0;
            for (const auto& package : std::get<std::vector<PackageItem>>(result)) {
                auto* top = new QTreeWidgetItem(tree_, {QFileInfo(package.path).fileName()});
                top->setData(0, Qt::UserRole, package.path);
                top->setToolTip(0, package.path);
                for (const auto& entry : package.entries) {
                    const auto index = top->childCount();
                    auto* row = new QTreeWidgetItem(top, {text(entry.name), text(entry.origin)});
                    row->setData(0, Qt::UserRole, index);
                    row->setToolTip(1, text(entry.origin));
                    ++count;
                }
                top->setExpanded(true);
            }
            if (count && tree_->topLevelItem(0)) { tree_->setCurrentItem(tree_->topLevelItem(0)->child(0)); }
            status_->setText(tr("已导入 %1 个包，包含 %2 张图像。").arg(tree_->topLevelItemCount()).arg(count));
            extract_->setEnabled(count > 0);
            if (extract_on_load && count) { extract_->click(); }
        });
    }
    QString selected_path_;
    quint64 generation_{}, preview_generation_{};
    QTreeWidget* tree_{};
    QLabel* preview_{};
    QLabel* status_{};
    QPushButton* extract_{};
    QCheckBox* folder_{};
};
} // namespace

QDialog* open_image_packages(QWidget* parent)
{
    auto* browser = new PackageBrowser(parent);
    browser->show();
    return browser;
}

void edit_archive_policy(QWidget* parent, const std::optional<ImageArchivePolicy>& current,
    const std::function<void(std::optional<ImageArchivePolicy>)>& apply)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("imageArchivePolicy"));
    dialog.setWindowTitle(QObject::tr("采集策略"));
    auto* form = new QFormLayout(&dialog);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto* enabled = new QCheckBox(QObject::tr("启用大文件存图"), &dialog);
    enabled->setObjectName(QStringLiteral("archiveEnabled"));
    enabled->setChecked(current.has_value());
    auto* base = new QComboBox(&dialog);
    base->addItem(QObject::tr("方案目录"), static_cast<int>(PathBase::SolutionDirectory));
    base->addItem(QObject::tr("指定资源根"), static_cast<int>(PathBase::NamedRoot));
    base->addItem(QObject::tr("绝对路径"), static_cast<int>(PathBase::Absolute));
    auto* named = new QLineEdit(&dialog);
    auto* path = new QLineEdit(&dialog);
    path->setObjectName(QStringLiteral("archiveDirectory"));
    path->setPlaceholderText(QObject::tr("例如：存图/归档"));
    auto* choose = new QPushButton(QObject::tr("选择目录…"), &dialog);
    auto* field = new QWidget(&dialog);
    auto* directory_row = new QHBoxLayout(field);
    directory_row->setContentsMargins(0,0,0,0);
    directory_row->addWidget(path, 1);
    directory_row->addWidget(choose);
    auto* count = new QLineEdit(QString::number(current ? current->images_per_file : 64u), &dialog);
    count->setObjectName(QStringLiteral("archivePackageSize"));
    if (current) {
        base->setCurrentIndex(base->findData(static_cast<int>(current->directory.base)));
        path->setText(text(current->directory.path));
        if (current->directory.named_root) { named->setText(text(current->directory.named_root->text())); }
    }
    form->addRow(enabled);
    form->addRow(QObject::tr("路径基准"), base);
    form->addRow(QObject::tr("资源根标识"), named);
    form->addRow(QObject::tr("大文件路径"), field);
    form->addRow(QObject::tr("每包图像张数"), count);
    auto* status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    const auto refresh = [&] {
        const bool on = enabled->isChecked();
        form->setRowVisible(base, on);
        form->setRowVisible(named, on && base->currentData().toInt() == static_cast<int>(PathBase::NamedRoot));
        form->setRowVisible(field, on);
        form->setRowVisible(count, on);
        status->setText(on ? QObject::tr("启用后，所有输出图像节点均写入此目录。") : QString{});
    };
    QObject::connect(base, &QComboBox::currentIndexChanged, &dialog, refresh);
    QObject::connect(enabled, &QCheckBox::toggled, &dialog, refresh);
    QObject::connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto selection = QFileDialog::getExistingDirectory(&dialog, QObject::tr("选择大文件目录"));
        if (!selection.isEmpty()) {
            base->setCurrentIndex(base->findData(static_cast<int>(PathBase::Absolute)));
            path->setText(selection);
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        std::optional<ImageArchivePolicy> result;
        if (enabled->isChecked()) {
            bool ok = false;
            const auto size = count->text().toUInt(&ok);
            if (!ok || !size || size > ImageArchivePolicy::max_images_per_file) {
                status->setText(QObject::tr("每包图像张数应在 1 至 %1 之间。").arg(ImageArchivePolicy::max_images_per_file));
                return;
            }
            ResourcePath target{static_cast<PathBase>(base->currentData().toInt()), QDir::fromNativeSeparators(path->text().trimmed()).toStdString(), {}};
            if (target.base == PathBase::NamedRoot) {
                auto key = SchemaIdFactory::parse<SchemaId>(named->text().trimmed().toStdString());
                if (auto* error = std::get_if<Diagnostic>(&key)) { status->setText(text(error->message)); return; }
                target.named_root = std::get<SchemaId>(key);
            }
            auto checked = validate_resource_path(target);
            if (auto* error = std::get_if<Diagnostic>(&checked)) { status->setText(text(error->message)); return; }
            result.emplace(ImageArchivePolicy{std::move(target), size});
        }
        apply(std::move(result));
        dialog.accept();
    });
    refresh();
    dialog.resize(435, 180);
    dialog.exec();
}

void image_ftp_controls(QWidget* owner, QFormLayout& form, ImageFtpSettings settings,
    const QString& default_credential, std::function<void(const std::string&)> select_credential)
{
    auto* credential = new QPushButton(QObject::tr("设置密码…"), owner);
    credential->setObjectName(QStringLiteral("outputFtpPassword"));
    auto* check = new QPushButton(QObject::tr("校验 FTP 写入"), owner);
    check->setObjectName(QStringLiteral("outputFtpCheck"));
    auto* row = new QHBoxLayout;
    row->addWidget(credential);
    row->addWidget(check);
    form.addRow(QObject::tr("FTP 连接"), row);
    auto* state = new QLabel(owner);
    state->setObjectName(QStringLiteral("outputFtpStatus"));
    state->setWordWrap(true);
    state->setTextFormat(Qt::PlainText);
    state->hide();
    form.addRow(state);
    QObject::connect(credential, &QPushButton::clicked, owner, [=] {
        bool accepted = false;
        const auto password = QInputDialog::getText(owner, QObject::tr("FTP 凭据"), QObject::tr("密码"),
            QLineEdit::Password, {}, &accepted);
        if (!accepted) { return; }
        const auto key = settings.credential.empty() ? default_credential.toStdString() : settings.credential;
        credential->setEnabled(false);
        state->show();
        state->setText(QObject::tr("正在保存凭据…"));
        on_worker(owner, [key, password] { return save_image_ftp_password(key, password.toStdString()); },
            [=](ArchiveCheck result) {
                credential->setEnabled(true);
                if (auto* error = std::get_if<Diagnostic>(&result)) { state->setText(text(error->message)); return; }
                state->setText(QObject::tr("凭据已保存"));
                select_credential(key);
            });
    });
    QObject::connect(check, &QPushButton::clicked, owner, [=] {
        check->setEnabled(false);
        state->show();
        state->setText(QObject::tr("正在校验目标目录的写入权限…"));
        on_worker(owner, [settings] { return check_image_ftp(settings); }, [=](ArchiveCheck result) {
            check->setEnabled(true);
            const auto* error = std::get_if<Diagnostic>(&result);
            state->setText(error ? text(error->message) : QObject::tr("FTP 目标目录可写"));
        });
    });
}
} // namespace visionpro::ui
