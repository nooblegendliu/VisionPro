#include "camera_editor.hpp"
#include "background_result.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/mvs_camera.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

namespace visionpro::ui {
namespace {
using namespace core;
using namespace workflow;
constexpr auto simulated = "visionpro.camera.simulated";
constexpr auto mvs = "visionpro.camera.mvs";

void add_modes(QComboBox* combo, CameraMode selected)
{
    for (const auto& [name, mode] : {std::pair{"单次采集", CameraMode::Single}, {"连续采集", CameraMode::Continuous},
             {"软触发", CameraMode::Software}, {"硬触发", CameraMode::Hardware}}) {
        combo->addItem(QString::fromUtf8(name), static_cast<int>(mode));
    }
    combo->setCurrentIndex(combo->findData(static_cast<int>(selected)));
}

void add_device(QComboBox& combo, const ResourceDefinition& resource)
{
    const auto& device = std::get<DeviceResource>(resource.location);
    const auto serial = infrastructure::camera_serial(resource);
    combo.addItem(QStringLiteral("%1 · %2 · %3").arg(QString::fromStdString(resource.name),
        QString::fromUtf8(device.adapter.text()), std::holds_alternative<std::string>(serial)
            ? QString::fromStdString(std::get<std::string>(serial)) : QStringLiteral("配置无效")));
}
}

bool is_camera_device(const ResourceDefinition& definition)
{
    const auto* device = std::get_if<DeviceResource>(&definition.location);
    return definition.kind == ResourceKind::Device && device && (device->adapter.text() == simulated || device->adapter.text() == mvs);
}

std::optional<ResourceDefinition> edit_camera_device(QWidget* parent, const ResourceDefinition* existing)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(existing ? QStringLiteral("编辑全局相机") : QStringLiteral("添加全局相机"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(existing ? QString::fromStdString(existing->name) : QStringLiteral("相机"), &dialog);
    name->setObjectName(QStringLiteral("cameraDeviceName"));
    auto* adapter = new QComboBox(&dialog);
    adapter->setObjectName(QStringLiteral("cameraAdapter"));
    adapter->addItem(QStringLiteral("模拟相机"), QString::fromLatin1(simulated));
    adapter->addItem(QStringLiteral("Hikrobot MVS"), QString::fromLatin1(mvs));
    auto* serial = new QLineEdit(&dialog);
    serial->setObjectName(QStringLiteral("cameraSerial"));
    serial->setPlaceholderText(QStringLiteral("设备序列号"));
    auto* channels = new QSpinBox(&dialog);
    channels->setObjectName(QStringLiteral("cameraStrobeChannels"));
    channels->setRange(0, 3);
    channels->setSpecialValueText(QStringLiteral("关闭"));
    std::optional<infrastructure::CameraAcquisitionSettings> settings{std::in_place};
    if (existing) {
        const auto& device = std::get<DeviceResource>(existing->location);
        adapter->setCurrentIndex(adapter->findData(QString::fromUtf8(device.adapter.text())));
        const auto value = infrastructure::camera_serial(*existing);
        if (const auto* failure = std::get_if<Diagnostic>(&value); failure && failure->code == ErrorCode::IncompatibleSchemaVersion) {
            QMessageBox::warning(parent, dialog.windowTitle(), QString::fromStdString(failure->message));
            return {};
        }
        if (const auto* number = std::get_if<std::string>(&value)) { serial->setText(QString::fromStdString(*number)); }
        const auto strobe = infrastructure::camera_strobe_channels(*existing);
        if (const auto* count = std::get_if<unsigned>(&strobe)) { channels->setValue(static_cast<int>(*count)); }
        const auto acquisition = infrastructure::camera_acquisition_settings(*existing);
        if (const auto* failure = std::get_if<Diagnostic>(&acquisition)) {
            QMessageBox::warning(parent, dialog.windowTitle(), QString::fromStdString(failure->message));
            return {};
        }
        settings = std::get<std::optional<infrastructure::CameraAcquisitionSettings>>(acquisition);
    } else { serial->setText(QStringLiteral("SIM-001")); }
    QObject::connect(adapter, &QComboBox::currentIndexChanged, serial, [serial](int index) {
        if (index == 1 && serial->text() == QStringLiteral("SIM-001")) { serial->clear(); }
        if (index == 0 && serial->text().isEmpty()) { serial->setText(QStringLiteral("SIM-001")); }
    });
    form->addRow(QStringLiteral("相机名称"), name);
    form->addRow(QStringLiteral("驱动"), adapter);
    form->addRow(QStringLiteral("设备序列号"), serial);
    form->addRow(QStringLiteral("模拟分时频闪通道数"), channels);
    const auto channel_visibility = [form, channels](int index) { form->setRowVisible(channels, index == 0); };
    channel_visibility(adapter->currentIndex());
    QObject::connect(adapter, &QComboBox::currentIndexChanged, channels, channel_visibility);
    auto* discovery = new QWidget(&dialog);
    auto* row = new QHBoxLayout(discovery);
    row->setContentsMargins(0, 0, 0, 0);
    auto* detected = new QComboBox(discovery);
    detected->setObjectName(QStringLiteral("detectedMvsCameras"));
    detected->setPlaceholderText(QStringLiteral("可用设备"));
    auto* scan = new QPushButton(QStringLiteral("枚举设备"), discovery);
    scan->setObjectName(QStringLiteral("scanMvsCameras"));
    row->addWidget(detected, 1);
    row->addWidget(scan);
    form->addRow(discovery);
    discovery->setVisible(adapter->currentIndex() == 1);
    QObject::connect(adapter, &QComboBox::currentIndexChanged, discovery, [discovery](int index) { discovery->setVisible(index == 1); });
    QObject::connect(detected, &QComboBox::activated, serial, [detected, serial](int index) {
        serial->setText(detected->itemData(index).toString());
    });
    QObject::connect(scan, &QPushButton::clicked, &dialog, [scan, detected] {
        scan->setEnabled(false);
        detected->clear();
        detected->setPlaceholderText(QStringLiteral("正在枚举…"));
        on_worker(scan, infrastructure::query_mvs_cameras, [scan, detected](Result<infrastructure::MvsCameraInventory> result) {
            scan->setEnabled(true);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) {
                detected->setPlaceholderText(QString::fromStdString(failure->message));
                detected->setToolTip(QString::fromStdString(failure->message));
                return;
            }
            const auto& inventory = std::get<infrastructure::MvsCameraInventory>(result);
            detected->setToolTip(QStringLiteral("MVS SDK %1").arg(QString::fromStdString(inventory.sdk_version)));
            detected->setPlaceholderText(inventory.devices.empty() ? QStringLiteral("未发现相机") : QStringLiteral("请选择设备"));
            for (const auto& device : inventory.devices) {
                detected->addItem(QStringLiteral("%1 · %2 · %3").arg(QString::fromStdString(device.serial),
                    QString::fromStdString(device.model), QString::fromStdString(device.transport)), QString::fromStdString(device.serial));
            }
            detected->setCurrentIndex(-1);
        });
    });
    auto* use_settings = new QCheckBox(QStringLiteral("统一设置图像源的主动取帧参数"), &dialog);
    use_settings->setObjectName(QStringLiteral("cameraAcquisitionFromDevice"));
    use_settings->setToolTip(QStringLiteral("适用于关联此相机并选择「执行时取帧」的图像源。关闭后沿用各节点的采集参数。"));
    use_settings->setChecked(settings.has_value());
    form->addRow(use_settings);
    const auto current = settings.value_or(infrastructure::CameraAcquisitionSettings{});
    auto* modes = new QComboBox(&dialog);
    modes->setObjectName(QStringLiteral("cameraAcquisitionMode"));
    add_modes(modes, current.mode);
    auto* timeout = new QSpinBox(&dialog);
    timeout->setObjectName(QStringLiteral("cameraAcquisitionTimeout"));
    timeout->setRange(1, 60000);
    timeout->setValue(static_cast<int>(current.timeout.count()));
    form->addRow(QStringLiteral("采集模式"), modes);
    form->addRow(QStringLiteral("取帧超时（ms）"), timeout);
    const auto show_settings = [form, modes, timeout](bool enabled) {
        form->setRowVisible(modes, enabled);
        form->setRowVisible(timeout, enabled);
    };
    show_settings(settings.has_value());
    QObject::connect(use_settings, &QCheckBox::toggled, &dialog, show_settings);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    while (dialog.exec() == QDialog::Accepted) {
        const auto title = name->text().trimmed();
        const auto channels_count = adapter->currentIndex() == 0 ? static_cast<unsigned>(channels->value()) : 0U;
        std::optional<infrastructure::CameraAcquisitionSettings> selected_settings;
        if (use_settings->isChecked()) {
            selected_settings = infrastructure::CameraAcquisitionSettings{
                static_cast<CameraMode>(modes->currentData().toInt()), std::chrono::milliseconds{timeout->value()}};
        }
        auto configuration = infrastructure::camera_device_config(serial->text().trimmed().toStdString(), channels_count, selected_settings);
        if (title.isEmpty() || std::holds_alternative<Diagnostic>(configuration)) {
            QMessageBox::warning(&dialog, dialog.windowTitle(), title.isEmpty() ? QStringLiteral("请输入相机名称。")
                : QString::fromStdString(std::get<Diagnostic>(configuration).message));
            continue;
        }
        auto id = existing ? Result<ResourceId>{existing->id} : IdFactory::generate<ResourceId>();
        if (const auto* failure = std::get_if<Diagnostic>(&id)) {
            QMessageBox::warning(&dialog, dialog.windowTitle(), QString::fromStdString(failure->message));
            return {};
        }
        auto selected = std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>(adapter->currentData().toString().toStdString()));
        return ResourceDefinition{std::get<ResourceId>(id), title.toStdString(), ResourceKind::Device,
            DeviceResource{selected, std::get<ConfigValue>(std::move(configuration))}};
    }
    return {};
}

CameraTriggerEditor::CameraTriggerEditor(QWidget* parent, const Solution& solution, const Workflow& flow)
    : QGroupBox(QStringLiteral("相机采集"), parent), devices_(new QComboBox(this)), inputs_(new QComboBox(this)),
      coordinates_(new QComboBox(this)), modes_(new QComboBox(this)), timeout_(new QSpinBox(this)),
      add_device_(new QPushButton(QStringLiteral("添加相机…"), this))
{
    const auto current = flow.trigger ? flow.trigger->camera : std::nullopt;
    auto* form = new QFormLayout(this);
    devices_->setObjectName(QStringLiteral("triggerCameraDevice"));
    inputs_->setObjectName(QStringLiteral("triggerCameraImage"));
    coordinates_->setObjectName(QStringLiteral("triggerCameraCoordinate"));
    modes_->setObjectName(QStringLiteral("triggerCameraMode"));
    timeout_->setObjectName(QStringLiteral("triggerCameraTimeout"));
    for (const auto& resource : solution.resources) {
        if (!is_camera_device(resource)) { continue; }
        add_device(*devices_, resource);
        device_ids_.push_back(resource.id);
        if (current && current->device == resource.id) { devices_->setCurrentIndex(devices_->count() - 1); }
    }
    add_device_->setObjectName(QStringLiteral("newTriggerCameraDevice"));
    connect(add_device_, &QPushButton::clicked, this, [this] {
        auto device = edit_camera_device(this);
        if (!device) { return; }
        if (created_) { devices_->removeItem(devices_->count() - 1); device_ids_.pop_back(); }
        created_ = std::move(device);
        add_device(*devices_, *created_);
        device_ids_.push_back(created_->id);
        devices_->setCurrentIndex(devices_->count() - 1);
    });
    auto* row = new QHBoxLayout;
    row->addWidget(devices_, 1);
    row->addWidget(add_device_);
    form->addRow(QStringLiteral("关联相机"), row);
    for (const auto& port : flow.interface.inputs) {
        if (port.type != DataType::image() || (port.semantics.unit != MeasurementUnit::Unspecified && port.semantics.unit != MeasurementUnit::Pixel)) {
            continue;
        }
        inputs_->addItem(QStringLiteral("%1 (%2)").arg(QString::fromStdString(port.display_name), QString::fromUtf8(port.id.text())),
            port.semantics.coordinate_system ? QString::fromUtf8(port.semantics.coordinate_system->text()) : QString{});
        input_ids_.push_back(port.id);
        if (current && current->image_input == port.id) { inputs_->setCurrentIndex(inputs_->count() - 1); }
    }
    for (const auto& frame : solution.coordinates) {
        if (frame.unit != MeasurementUnit::Pixel) { continue; }
        coordinates_->addItem(QString::fromUtf8(frame.id.text()));
        frame_ids_.push_back(frame.id);
        if (current && current->coordinate_frame == frame.id) { coordinates_->setCurrentIndex(coordinates_->count() - 1); }
    }
    const auto select_coordinates = [this] {
        const auto required = inputs_->currentData().toString();
        coordinates_->setEnabled(required.isEmpty());
        if (!required.isEmpty()) { coordinates_->setCurrentIndex(coordinates_->findText(required)); }
    };
    connect(inputs_, &QComboBox::currentIndexChanged, this, select_coordinates);
    select_coordinates();
    add_modes(modes_, current ? current->mode : CameraMode::Single);
    timeout_->setRange(1, 60000);
    timeout_->setValue(current ? static_cast<int>(current->timeout_ms) : 1000);
    form->addRow(QStringLiteral("流程图像输入"), inputs_);
    form->addRow(QStringLiteral("图像坐标系"), coordinates_);
    form->addRow(QStringLiteral("采集模式"), modes_);
    form->addRow(QStringLiteral("取帧超时（ms）"), timeout_);
}

Result<CameraTriggerBinding> CameraTriggerEditor::binding() const
{
    if (devices_->currentIndex() < 0 || inputs_->currentIndex() < 0 || coordinates_->currentIndex() < 0) {
        return Diagnostic{ErrorCode::MissingRequiredValue, Severity::Error, {},
            "Select a camera device, an Image public input and a pixel coordinate frame."};
    }
    return CameraTriggerBinding{device_ids_.at(devices_->currentIndex()), input_ids_.at(inputs_->currentIndex()),
        frame_ids_.at(coordinates_->currentIndex()), static_cast<CameraMode>(modes_->currentData().toInt()),
        static_cast<std::uint32_t>(timeout_->value())};
}

void CameraTriggerEditor::retain_image_input(InputId input)
{
    const auto found = std::ranges::find(input_ids_, input);
    inputs_->setCurrentIndex(found == input_ids_.end() ? -1 : static_cast<int>(found - input_ids_.begin()));
    inputs_->setEnabled(false);
}

void CameraTriggerEditor::retain_binding(const CameraTriggerBinding& current)
{
    retain_image_input(current.image_input);
    const auto device = std::ranges::find(device_ids_, current.device);
    devices_->setCurrentIndex(device == device_ids_.end() ? -1 : static_cast<int>(device - device_ids_.begin()));
    const auto coordinates = std::ranges::find(frame_ids_, current.coordinate_frame);
    coordinates_->setCurrentIndex(coordinates == frame_ids_.end() ? -1 : static_cast<int>(coordinates - frame_ids_.begin()));
    devices_->setEnabled(false);
    add_device_->setEnabled(false);
    coordinates_->setEnabled(false);
}

const ResourceDefinition* CameraTriggerEditor::new_device() const
{
    return created_ && devices_->currentIndex() == devices_->count() - 1 ? &*created_ : nullptr;
}

} // namespace visionpro::ui
