#include "camera_editor.hpp"
#include "background_result.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/infrastructure/mvs_camera.hpp>

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

void add_device(QComboBox& combo, const ResourceDefinition& resource)
{
    const auto& device = std::get<DeviceResource>(resource.location);
    const auto serial = infrastructure::camera_serial(resource);
    combo.addItem(QStringLiteral("%1 · %2 · %3").arg(QString::fromStdString(resource.name),
        QString::fromUtf8(device.adapter.text()), std::holds_alternative<std::string>(serial)
            ? QString::fromStdString(std::get<std::string>(serial)) : QStringLiteral("Invalid configuration")));
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
    dialog.setWindowTitle(existing ? QStringLiteral("Edit camera device") : QStringLiteral("New camera device"));
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(existing ? QString::fromStdString(existing->name) : QStringLiteral("Camera"), &dialog);
    name->setObjectName(QStringLiteral("cameraDeviceName"));
    auto* adapter = new QComboBox(&dialog);
    adapter->setObjectName(QStringLiteral("cameraAdapter"));
    adapter->addItem(QStringLiteral("Simulated camera"), QString::fromLatin1(simulated));
    adapter->addItem(QStringLiteral("Hikrobot MVS"), QString::fromLatin1(mvs));
    auto* serial = new QLineEdit(&dialog);
    serial->setObjectName(QStringLiteral("cameraSerial"));
    serial->setPlaceholderText(QStringLiteral("Device serial number"));
    if (existing) {
        const auto& device = std::get<DeviceResource>(existing->location);
        adapter->setCurrentIndex(adapter->findData(QString::fromUtf8(device.adapter.text())));
        const auto value = infrastructure::camera_serial(*existing);
        if (const auto* failure = std::get_if<Diagnostic>(&value); failure && failure->code == ErrorCode::IncompatibleSchemaVersion) {
            QMessageBox::warning(parent, dialog.windowTitle(), QString::fromStdString(failure->message));
            return {};
        }
        if (const auto* number = std::get_if<std::string>(&value)) { serial->setText(QString::fromStdString(*number)); }
    } else { serial->setText(QStringLiteral("SIM-001")); }
    QObject::connect(adapter, &QComboBox::currentIndexChanged, serial, [serial](int index) {
        if (index == 1 && serial->text() == QStringLiteral("SIM-001")) { serial->clear(); }
        if (index == 0 && serial->text().isEmpty()) { serial->setText(QStringLiteral("SIM-001")); }
    });
    form->addRow(QStringLiteral("Resource name"), name);
    form->addRow(QStringLiteral("Adapter"), adapter);
    form->addRow(QStringLiteral("Serial number"), serial);
    auto* discovery = new QWidget(&dialog);
    auto* row = new QHBoxLayout(discovery);
    row->setContentsMargins(0, 0, 0, 0);
    auto* detected = new QComboBox(discovery);
    detected->setObjectName(QStringLiteral("detectedMvsCameras"));
    detected->setPlaceholderText(QStringLiteral("Available devices"));
    auto* scan = new QPushButton(QStringLiteral("Scan devices"), discovery);
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
        detected->setPlaceholderText(QStringLiteral("Scanning…"));
        on_worker(scan, infrastructure::query_mvs_cameras, [scan, detected](Result<infrastructure::MvsCameraInventory> result) {
            scan->setEnabled(true);
            if (const auto* failure = std::get_if<Diagnostic>(&result)) {
                detected->setPlaceholderText(QString::fromStdString(failure->message));
                detected->setToolTip(QString::fromStdString(failure->message));
                return;
            }
            const auto& inventory = std::get<infrastructure::MvsCameraInventory>(result);
            detected->setToolTip(QStringLiteral("MVS SDK %1").arg(QString::fromStdString(inventory.sdk_version)));
            detected->setPlaceholderText(inventory.devices.empty() ? QStringLiteral("No cameras detected") : QStringLiteral("Select a device"));
            for (const auto& device : inventory.devices) {
                detected->addItem(QStringLiteral("%1 · %2 · %3").arg(QString::fromStdString(device.serial),
                    QString::fromStdString(device.model), QString::fromStdString(device.transport)), QString::fromStdString(device.serial));
            }
            detected->setCurrentIndex(-1);
        });
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    while (dialog.exec() == QDialog::Accepted) {
        const auto title = name->text().trimmed();
        auto configuration = infrastructure::camera_device_config(serial->text().trimmed().toStdString());
        if (title.isEmpty() || std::holds_alternative<Diagnostic>(configuration)) {
            QMessageBox::warning(&dialog, dialog.windowTitle(), title.isEmpty() ? QStringLiteral("Enter a resource name.")
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
    : QGroupBox(QStringLiteral("Camera acquisition"), parent), devices_(new QComboBox(this)), inputs_(new QComboBox(this)),
      coordinates_(new QComboBox(this)), modes_(new QComboBox(this)), timeout_(new QSpinBox(this))
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
    auto* add = new QPushButton(QStringLiteral("New device…"), this);
    add->setObjectName(QStringLiteral("newTriggerCameraDevice"));
    connect(add, &QPushButton::clicked, this, [this] {
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
    row->addWidget(add);
    form->addRow(QStringLiteral("Device"), row);
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
    for (const auto& [label, mode] : {std::pair{"Single", CameraMode::Single}, {"Continuous", CameraMode::Continuous},
             {"Software", CameraMode::Software}, {"Hardware", CameraMode::Hardware}}) {
        modes_->addItem(QString::fromLatin1(label), static_cast<int>(mode));
        if (current && current->mode == mode) { modes_->setCurrentIndex(modes_->count() - 1); }
    }
    timeout_->setRange(1, 60000);
    timeout_->setValue(current ? static_cast<int>(current->timeout_ms) : 1000);
    form->addRow(QStringLiteral("Image input"), inputs_);
    form->addRow(QStringLiteral("Pixel coordinates"), coordinates_);
    form->addRow(QStringLiteral("Acquisition mode"), modes_);
    form->addRow(QStringLiteral("Frame timeout (ms)"), timeout_);
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

const ResourceDefinition* CameraTriggerEditor::new_device() const
{
    return created_ && devices_->currentIndex() == devices_->count() - 1 ? &*created_ : nullptr;
}

} // namespace visionpro::ui
