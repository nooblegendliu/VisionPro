#pragma once

#include <visionpro/core/tool_execution.hpp>

#include <chrono>
#include <stop_token>

namespace visionpro::infrastructure {

struct CommunicationEndpoint {
    std::string host;
    std::uint16_t port;
    std::uint8_t unit;
};

[[nodiscard]] bool is_communication_adapter(const core::SchemaId& adapter) noexcept;
[[nodiscard]] core::Result<core::ConfigValue> communication_config(std::string_view host,
    std::int64_t port, std::int64_t unit);
[[nodiscard]] core::Result<CommunicationEndpoint> communication_endpoint(const core::ResourceDefinition& resource);

class CommunicationLease final : public core::ResourceLease {
public:
    ~CommunicationLease() override;
    CommunicationLease(const CommunicationLease&) = delete;
    CommunicationLease& operator=(const CommunicationLease&) = delete;

    [[nodiscard]] core::Result<std::monostate> send_frame(const std::vector<std::uint8_t>& payload,
        std::chrono::milliseconds timeout, std::stop_token stop = {}) const;
    [[nodiscard]] core::Result<std::vector<std::uint8_t>> receive_frame(
        std::chrono::milliseconds timeout, std::stop_token stop = {}) const;
    [[nodiscard]] core::Result<std::uint16_t> read_register(std::uint16_t address,
        std::chrono::milliseconds timeout, std::stop_token stop = {}) const;
    [[nodiscard]] core::Result<std::monostate> write_register(std::uint16_t address, std::uint16_t value,
        std::chrono::milliseconds timeout, std::stop_token stop = {}) const;
private:
    struct Connection;
    CommunicationLease(core::ResourceReference resource, std::unique_ptr<Connection> connection);
    std::unique_ptr<Connection> connection_;
    friend core::Result<std::shared_ptr<CommunicationLease>> open_communication(const core::ResourceDefinition&);
};

// Creates one provider-owned endpoint. The first explicit I/O operation opens its socket.
[[nodiscard]] core::Result<std::shared_ptr<CommunicationLease>> open_communication(const core::ResourceDefinition& resource);

} // namespace visionpro::infrastructure
