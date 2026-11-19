#include <visionpro/infrastructure/mvs_camera.hpp>
#include <visionpro/infrastructure/camera.hpp>
#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <iostream>

int main()
{
    using namespace visionpro::core;
    using namespace visionpro::infrastructure;
    const auto listed = query_mvs_cameras();
    if (const auto* failure = std::get_if<Diagnostic>(&listed)) {
        std::cerr << failure->message;
        if (failure->external) { std::cerr << " (0x" << std::hex << failure->external->raw_code << ")"; }
        return 1;
    }
    const auto& inventory = std::get<MvsCameraInventory>(listed);
    std::cout << "MVS SDK " << inventory.sdk_version << "; GigE/USB devices: " << inventory.devices.size() << '\n';
    for (const auto& camera : inventory.devices) {
        std::cout << camera.transport << ' ' << camera.model << " firmware " << camera.firmware << '\n';
    }
    // A serial longer than the SDK's serial-number fields can never open a real device.
    const std::string absent(300, 'x');
    auto opened = open_mvs_camera(absent);
    if (std::holds_alternative<Diagnostic>(opened)) { return 1; }
    const auto source = std::get<ResourceId>(IdFactory::parse<ResourceId>("00000000-0000-4000-8000-000000000001"));
    const auto frame = std::get<CoordinateSystemId>(SchemaIdFactory::parse<CoordinateSystemId>("camera"));
    auto device = std::get<std::shared_ptr<CameraDevice>>(opened);
    const auto result = device->capture({source, frame, CameraMode::Single, std::chrono::seconds(3)}, {});
    device->close();
    const auto* failure = std::get_if<Diagnostic>(&result);
    if (!failure || failure->code != ErrorCode::ResourceNotFound) { return 1; }
    std::cout << "Unlisted serial: " << error_code_name(failure->code) << '\n';
    if (std::holds_alternative<Diagnostic>(device->finish())) { return 1; }
    return 0;
}
