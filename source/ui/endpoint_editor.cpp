#include "endpoint_editor.hpp"

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/communication.hpp>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSpinBox>

namespace visionpro::ui {
namespace {
using namespace core;
constexpr std::string_view tcp = "visionpro.tcp", modbus = "visionpro.modbus.tcp";
}

std::optional<std::string_view> endpoint_adapter(const ToolTypeId& tool)
{
    if (tool.text() == "visionpro.tcp.send" || tool.text() == "visionpro.tcp.receive") { return tcp; }
    if (tool.text() == "visionpro.plc.read" || tool.text() == "visionpro.plc.write") { return modbus; }
    return {};
}

bool matches_endpoint(const ResourceDefinition& device, std::string_view adapter)
{
    const auto* location = std::get_if<DeviceResource>(&device.location);
    return device.kind == ResourceKind::Device && location && location->adapter.text() == adapter;
}

QString endpoint_address(const ResourceDefinition& resource)
{
    const auto parsed = infrastructure::communication_endpoint(resource);
    if (const auto* issue = std::get_if<Diagnostic>(&parsed)) { return QString::fromStdString(issue->message); }
    const auto& endpoint = std::get<infrastructure::CommunicationEndpoint>(parsed);
    auto address = QStringLiteral("%1:%2").arg(QString::fromStdString(endpoint.host)).arg(endpoint.port);
    if (matches_endpoint(resource, modbus)) { address += QStringLiteral(" · unit %1").arg(endpoint.unit); }
    return address;
}

std::optional<ResourceDefinition> edit_endpoint(QWidget* parent, std::string_view adapter, const ResourceDefinition* existing)
{
    const bool plc = adapter == modbus;
    QDialog dialog(parent);
    const auto protocol = plc ? QStringLiteral("Modbus TCP") : QStringLiteral("TCP");
    dialog.setWindowTitle((existing ? QStringLiteral("Edit %1 endpoint") : QStringLiteral("New %1 endpoint")).arg(protocol));
    infrastructure::CommunicationEndpoint endpoint{"127.0.0.1", static_cast<std::uint16_t>(plc ? 502 : 9000), 1};
    if (existing) {
        auto parsed = infrastructure::communication_endpoint(*existing);
        if (const auto* failure = std::get_if<Diagnostic>(&parsed)) {
            QMessageBox::warning(parent, dialog.windowTitle(), QString::fromStdString(failure->message));
            return {};
        }
        endpoint = std::get<infrastructure::CommunicationEndpoint>(std::move(parsed));
    }
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(existing ? QString::fromStdString(existing->name) : QStringLiteral("%1 endpoint").arg(protocol), &dialog);
    name->setObjectName(QStringLiteral("endpointName"));
    auto* host = new QLineEdit(QString::fromStdString(endpoint.host), &dialog);
    host->setObjectName(QStringLiteral("endpointHost"));
    auto* port = new QSpinBox(&dialog);
    port->setObjectName(QStringLiteral("endpointPort"));
    port->setRange(1, 65535);
    port->setValue(endpoint.port);
    auto* unit = new QSpinBox(&dialog);
    unit->setObjectName(QStringLiteral("endpointUnit"));
    unit->setRange(0, 255);
    unit->setValue(endpoint.unit);
    form->addRow(QStringLiteral("Resource name"), name);
    form->addRow(QStringLiteral("IPv4 address"), host);
    form->addRow(QStringLiteral("Port"), port);
    if (plc) { form->addRow(QStringLiteral("Unit ID"), unit); }
    else { unit->hide(); }
    auto* error = new QLabel(&dialog);
    error->setObjectName(QStringLiteral("endpointError"));
    error->setWordWrap(true);
    form->addRow(error);
    error->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    std::optional<ResourceDefinition> selected;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto title = name->text().trimmed();
        auto config = infrastructure::communication_config(host->text().trimmed().toStdString(), port->value(), unit->value());
        if (title.isEmpty() || std::holds_alternative<Diagnostic>(config)) {
            error->setText(title.isEmpty() ? QStringLiteral("Enter a resource name.") : QString::fromStdString(std::get<Diagnostic>(config).message));
            error->show();
            return;
        }
        const auto id = existing ? Result<ResourceId>{existing->id} : IdFactory::generate<ResourceId>();
        if (const auto* failure = std::get_if<Diagnostic>(&id)) {
            error->setText(QString::fromStdString(failure->message));
            error->show();
            return;
        }
        selected.emplace(ResourceDefinition{std::get<ResourceId>(id), title.toStdString(), ResourceKind::Device,
            DeviceResource{std::get<SchemaId>(SchemaIdFactory::parse<SchemaId>(adapter)), std::get<ConfigValue>(std::move(config))}});
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
    return selected;
}

} // namespace visionpro::ui
