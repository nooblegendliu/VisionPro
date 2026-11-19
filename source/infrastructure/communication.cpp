#include <winsock2.h>
#include <ws2tcpip.h>

#include <visionpro/infrastructure/communication.hpp>
#include <visionpro/core/schema_id_factory.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <span>

namespace visionpro::infrastructure {
namespace {
using namespace core;
using namespace std::chrono_literals;
using Check = Result<std::monostate>;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_frame = 64 * 1024;
constexpr std::string_view tcp = "visionpro.tcp";
constexpr std::string_view modbus = "visionpro.modbus.tcp";

template<class Id> Id key(std::string_view text)
{ return std::get<Id>(SchemaIdFactory::parse<Id>(text)); }

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

template<class T> Result<T> at_resource(Result<T> result, const ResourceId& id)
{
    if (auto* failure = std::get_if<Diagnostic>(&result)) { failure->location.resource = id; }
    return result;
}

const DataType& config_type()
{
    static const auto type = std::get<DataType>(DataType::record(key<SchemaId>("visionpro.communication.endpoint"), 1,
        {{key<FieldId>("host"), DataType::string()}, {key<FieldId>("port"), DataType::integer()},
         {key<FieldId>("unit"), DataType::integer()}}));
    return type;
}

Result<CommunicationEndpoint> endpoint(std::string_view host, std::int64_t port, std::int64_t unit)
{
    const std::string spelling(host);
    IN_ADDR ip{};
    if (spelling.empty() || spelling.find('\0') != spelling.npos || InetPtonA(AF_INET, spelling.c_str(), &ip) != 1) {
        return error(ErrorCode::InvalidResource, "Communication host requires a dotted-decimal IPv4 address.");
    }
    if (port < 1 || port > 65535 || unit < 0 || unit > 255) {
        return error(ErrorCode::ConstraintViolation, "TCP port must be 1..65535 and unit identifier must be 0..255.");
    }
    return CommunicationEndpoint{spelling, static_cast<std::uint16_t>(port), static_cast<std::uint8_t>(unit)};
}

Diagnostic socket_error(std::string operation, int native)
{
    auto code = ErrorCode::InvalidResource;
    auto mapping = ExternalCodeMapping::Mapped;
    switch (native) {
    case WSAETIMEDOUT: code = ErrorCode::SourceTimedOut; break;
    case WSAEACCES: case WSAECONNRESET: case WSAECONNREFUSED: case WSAENETRESET: case WSAECONNABORTED:
    case WSAENETUNREACH: case WSAEHOSTUNREACH: case WSAENOTCONN: case WSAESHUTDOWN: case 0: break;
    default: mapping = ExternalCodeMapping::Unmapped; break;
    }
    const auto message = native == 0 ? "TCP peer closed the connection during " + operation + "."
        : "Winsock " + operation + " failed (" + std::to_string(native) + ").";
    return {code, Severity::Error, {}, message, ExternalDiagnostic{key<SchemaId>("winsock"), std::move(operation),
        ExternalStatusDomain::Call, native, mapping, key<SchemaId>("winsock.status")}};
}

Diagnostic protocol_error(std::string_view provider, std::string_view field, std::uint32_t actual, std::string message)
{
    return {ErrorCode::InvalidValue, Severity::Error, {}, std::move(message),
        ExternalDiagnostic{key<SchemaId>(provider), std::string(field), ExternalStatusDomain::Output, actual,
            ExternalCodeMapping::Mapped, key<SchemaId>(field)}};
}

struct Deadline {
    Clock::time_point end;
    std::stop_token stop;

    std::optional<Diagnostic> interrupted() const
    {
        if (stop.stop_requested()) { return error(ErrorCode::SourceCancelled, "Communication operation cancelled."); }
        if (Clock::now() >= end) { return error(ErrorCode::SourceTimedOut, "Communication operation deadline expired."); }
        return {};
    }

    // Share one monotonic deadline across lock, connect, all writes and all reads.
    std::chrono::microseconds slice() const
    {
        return std::clamp(std::chrono::ceil<std::chrono::microseconds>(end - Clock::now()), 0us, 20000us);
    }
};

Check wait_socket(SOCKET socket, bool writing, const Deadline& deadline)
{
    for (;;) {
        if (auto failure = deadline.interrupted()) { return *failure; }
        fd_set ready, exceptional;
        FD_ZERO(&ready);
        FD_ZERO(&exceptional);
        FD_SET(socket, &ready);
        FD_SET(socket, &exceptional);
        const timeval wait{0, static_cast<long>(deadline.slice().count())};
        const auto status = select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &exceptional, &wait);
        if (status == SOCKET_ERROR) { return socket_error("select", WSAGetLastError()); }
        if (status == 0) { continue; }
        if (FD_ISSET(socket, &exceptional)) {
            int pending{}, length = sizeof(pending);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) == SOCKET_ERROR) {
                return socket_error("getsockopt(SO_ERROR)", WSAGetLastError());
            }
            if (pending != 0) { return socket_error("socket readiness", pending); }
        }
        return std::monostate{};
    }
}

struct Wire {
    SOCKET socket;
    const Deadline& deadline;
    std::size_t sent{}, received{};
    bool valid_exception{}, rejected{};

    Check send_all(std::span<const std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            if (auto failure = deadline.interrupted()) { return *failure; }
            const int count = send(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (count > 0) {
                sent += count;
                bytes = bytes.subspan(count);
                continue;
            }
            const int code = count == 0 ? 0 : WSAGetLastError();
            if (code != WSAEWOULDBLOCK) { return socket_error("send", code); }
            auto ready = wait_socket(socket, true, deadline);
            if (auto* failure = std::get_if<Diagnostic>(&ready)) { return *failure; }
        }
        return std::monostate{};
    }

    Check read_exact(std::span<std::uint8_t> bytes)
    {
        while (!bytes.empty()) {
            if (auto failure = deadline.interrupted()) { return *failure; }
            const int count = recv(socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            if (count > 0) {
                received += count;
                bytes = bytes.subspan(count);
                continue;
            }
            const int code = count == 0 ? 0 : WSAGetLastError();
            if (code != WSAEWOULDBLOCK) { return socket_error("recv", code); }
            auto ready = wait_socket(socket, false, deadline);
            if (auto* failure = std::get_if<Diagnostic>(&ready)) { return *failure; }
        }
        return std::monostate{};
    }
};

std::uint16_t word(const std::uint8_t* bytes)
{ return static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]); }
void store_word(std::uint8_t* bytes, std::uint16_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
}

Diagnostic exception_response(Wire& wire, bool write, std::uint8_t code)
{
    wire.valid_exception = true;
    auto reason = ErrorCode::ExecutionBoundaryFailure;
    auto mapping = ExternalCodeMapping::Mapped;
    switch (code) {
    case 1: reason = ErrorCode::MissingCapability; wire.rejected = true; break;
    case 2: reason = ErrorCode::IndexOutOfRange; wire.rejected = true; break;
    case 3: reason = ErrorCode::ConstraintViolation; wire.rejected = true; break;
    case 6: reason = ErrorCode::InputUnavailable; wire.rejected = true; break;
    case 10: reason = ErrorCode::ResourceNotFound; wire.rejected = true; break;
    case 4: case 5: case 8: break;
    case 11: reason = ErrorCode::SourceTimedOut; break;
    default: mapping = ExternalCodeMapping::Unmapped; break;
    }
    return {reason, Severity::Error, {}, "Modbus server returned exception " + std::to_string(code) + ".",
        ExternalDiagnostic{key<SchemaId>(modbus), write ? "FC06" : "FC03", ExternalStatusDomain::Call,
            code, mapping, key<SchemaId>("modbus.exception")}};
}

Result<std::uint16_t> register_transaction(Wire& wire, std::uint16_t transaction, std::uint8_t unit,
    std::uint16_t address, std::optional<std::uint16_t> write_value)
{
    const std::uint8_t function = write_value ? 6 : 3;
    std::array<std::uint8_t, 12> request{};
    store_word(request.data(), transaction);
    store_word(request.data() + 4, 6);
    request[6] = unit;
    request[7] = function;
    store_word(request.data() + 8, address);
    store_word(request.data() + 10, write_value.value_or(1));
    auto io = wire.send_all(request);
    if (auto* failure = std::get_if<Diagnostic>(&io)) { return *failure; }
    std::array<std::uint8_t, 7> header;
    io = wire.read_exact(header);
    if (auto* failure = std::get_if<Diagnostic>(&io)) { return *failure; }

    if (word(header.data()) != transaction) {
        return protocol_error(modbus, "modbus.transaction", word(header.data()), "Modbus response transaction does not match the request.");
    }
    if (word(header.data() + 2) != 0) {
        return protocol_error(modbus, "modbus.protocol", word(header.data() + 2), "Unexpected MBAP protocol identifier.");
    }
    if (header[6] != unit) { return protocol_error(modbus, "modbus.unit", header[6], "Unexpected Modbus unit identifier."); }
    const auto length = word(header.data() + 4);
    if (length != (write_value ? 6 : 5) && length != 3) {
        return protocol_error(modbus, "modbus.length", length, "MBAP length does not fit the requested single-register response.");
    }
    std::array<std::uint8_t, 5> pdu{};
    io = wire.read_exact(std::span{pdu}.first(length - 1));
    if (auto* failure = std::get_if<Diagnostic>(&io)) { return *failure; }
    if (length == 3 && pdu[0] == (function | 0x80)) { return exception_response(wire, write_value.has_value(), pdu[1]); }
    if (pdu[0] != function) { return protocol_error(modbus, "modbus.function", pdu[0], "Unexpected Modbus response function."); }
    if (length == 3) { return protocol_error(modbus, "modbus.length", length, "Truncated normal Modbus response."); }
    if (write_value) {
        if (word(pdu.data() + 1) != address) {
            return protocol_error(modbus, "modbus.echo.address", word(pdu.data() + 1), "FC06 confirmation echoed a different address.");
        }
        if (word(pdu.data() + 3) != *write_value) {
            return protocol_error(modbus, "modbus.echo.value", word(pdu.data() + 3), "FC06 confirmation echoed a different value.");
        }
        return *write_value;
    }
    if (pdu[1] != 2) { return protocol_error(modbus, "modbus.byte_count", pdu[1], "FC03 expected two data bytes."); }
    return word(pdu.data() + 2);
}

enum class Operation { Send, Receive, ReadRegister, WriteRegister };
} // namespace

bool is_communication_adapter(const SchemaId& adapter) noexcept
{ return adapter.text() == tcp || adapter.text() == modbus; }

Result<ConfigValue> communication_config(std::string_view host, std::int64_t port, std::int64_t unit)
{
    auto checked = endpoint(host, port, unit);
    if (const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
    return ConfigValue::record(config_type(), {{key<FieldId>("host"), std::get<ConfigValue>(ConfigValue::string(host))},
        {key<FieldId>("port"), ConfigValue::integer(port)}, {key<FieldId>("unit"), ConfigValue::integer(unit)}});
}

Result<CommunicationEndpoint> communication_endpoint(const ResourceDefinition& resource)
{
    const auto invalid = [&](ErrorCode code, std::string message) {
        auto failure = error(code, std::move(message));
        failure.location.resource = resource.id;
        return failure;
    };
    const auto* device = std::get_if<DeviceResource>(&resource.location);
    if (resource.kind != ResourceKind::Device || !device) { return invalid(ErrorCode::ResourceKindMismatch, "Expected communication device resource."); }
    if (!is_communication_adapter(device->adapter)) { return invalid(ErrorCode::MissingCapability, "Unsupported communication adapter."); }
    const auto& config = device->configuration;
    if (config.type() != config_type()) { return invalid(ErrorCode::IncompatibleSchemaVersion, "Expected visionpro.communication.endpoint v1 (host, port, unit)."); }
    return at_resource(endpoint(*config.field(key<FieldId>("host"))->as_string(), *config.field(key<FieldId>("port"))->as_int64(),
        *config.field(key<FieldId>("unit"))->as_int64()), resource.id);
}

struct CommunicationLease::Connection {
    CommunicationEndpoint endpoint;
    bool plc;
    std::timed_mutex mutex;
    SOCKET socket{INVALID_SOCKET};
    bool winsock_started{};
    std::uint16_t transaction{};

    Connection(CommunicationEndpoint target, bool modbus_device) : endpoint(std::move(target)), plc(modbus_device) {}
    ~Connection() { disconnect(); if (winsock_started) { WSACleanup(); } }
    void disconnect() noexcept
    {
        if (socket != INVALID_SOCKET) { closesocket(socket); socket = INVALID_SOCKET; }
    }

    Check connect_if_needed(const Deadline& deadline)
    {
        if (socket != INVALID_SOCKET) { return std::monostate{}; }
        if (!winsock_started) {
            WSADATA data{};
            const int status = WSAStartup(MAKEWORD(2, 2), &data);
            if (status != 0) { return socket_error("WSAStartup", status); }
            winsock_started = true;
        }
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) { return socket_error("socket", WSAGetLastError()); }
        u_long nonblocking = 1;
        if (ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { return socket_error("ioctlsocket", WSAGetLastError()); }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        InetPtonA(AF_INET, endpoint.host.c_str(), &address.sin_addr);
        if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) { return std::monostate{}; }
        const auto status = WSAGetLastError();
        if (status != WSAEWOULDBLOCK && status != WSAEINPROGRESS) { return socket_error("connect", status); }
        auto ready = wait_socket(socket, true, deadline);
        if (auto* failure = std::get_if<Diagnostic>(&ready)) { return *failure; }
        int pending{}, length = sizeof(pending);
        if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) != 0) {
            return socket_error("getsockopt(SO_ERROR)", WSAGetLastError());
        }
        if (pending != 0) { return socket_error("connect", pending); }
        return std::monostate{};
    }

    template<class T, class Action> Result<T> run(Operation operation, std::chrono::milliseconds timeout,
        std::stop_token stop, Action action)
    {
        const bool modbus_operation = operation == Operation::ReadRegister || operation == Operation::WriteRegister;
        if (plc != modbus_operation) { return error(ErrorCode::ResourceKindMismatch, "Communication node and device adapter do not match."); }
        if (timeout < 1ms || timeout > 60s) { return error(ErrorCode::ConstraintViolation, "Communication timeout must be 1..60000 ms."); }
        const Deadline deadline{Clock::now() + timeout, stop};
        std::unique_lock lock(mutex, std::defer_lock);
        while (!lock.try_lock_for(deadline.slice())) {
            if (auto failure = deadline.interrupted()) { return *failure; }
        }
        if (auto failure = deadline.interrupted()) { return *failure; }
        // Drop a broken stream, including any partial header or payload. A later operation opens a new connection.
        auto connected = connect_if_needed(deadline);
        if (auto* failure = std::get_if<Diagnostic>(&connected)) { disconnect(); return *failure; }
        Wire wire{socket, deadline};
        Result<T> result = [&]() -> Result<T> {
            try { return action(wire); }
            catch (...) { return error(ErrorCode::ExecutionBoundaryFailure, "Communication operation could not finish."); }
        }();
        if (auto* failure = std::get_if<Diagnostic>(&result)) {
            if (!wire.valid_exception) { disconnect(); }
            const bool uncertain_write = (operation == Operation::Send || operation == Operation::WriteRegister)
                && wire.sent && !wire.rejected;
            if (uncertain_write || (operation == Operation::Receive && wire.received)) {
                if (!failure->external) {
                    // An application deadline or stop token has no native socket status.
                    // Keep its original error when promoting the diagnostic to an unknown outcome.
                    failure->external = ExternalDiagnostic{key<SchemaId>(plc ? modbus : tcp),
                        std::string(error_code_name(failure->code)), ExternalStatusDomain::Call,
                        static_cast<std::int64_t>(failure->code), ExternalCodeMapping::Mapped,
                        key<SchemaId>("visionpro.communication.runtime")};
                }
                failure->message += uncertain_write ? " The transmitted operation has no confirmed outcome."
                    : " The interrupted stream already consumed bytes from the frame.";
                failure->code = ErrorCode::ExternalEffectUnknown;
            }
        }
        return result;
    }
};

CommunicationLease::CommunicationLease(ResourceReference ref, std::unique_ptr<Connection> connection)
    : ResourceLease(std::move(ref)), connection_(std::move(connection)) {}
CommunicationLease::~CommunicationLease() = default;

Result<std::shared_ptr<CommunicationLease>> open_communication(const ResourceDefinition& resource)
{
    auto target = communication_endpoint(resource);
    if (auto* failure = std::get_if<Diagnostic>(&target)) { return *failure; }
    const bool plc = std::get<DeviceResource>(resource.location).adapter.text() == modbus;
    auto connection = std::make_unique<CommunicationLease::Connection>(std::get<CommunicationEndpoint>(std::move(target)), plc);
    return std::shared_ptr<CommunicationLease>(new CommunicationLease({resource.id, ResourceKind::Device, {}}, std::move(connection)));
}

Check CommunicationLease::send_frame(const Bytes& payload, std::chrono::milliseconds timeout, std::stop_token stop) const
{
    if (payload.size() > max_frame) {
        return at_resource<std::monostate>(error(ErrorCode::SizeOverflow, "TCP frame exceeds the 64 KiB payload limit."), reference().id);
    }
    auto result = connection_->run<std::monostate>(Operation::Send, timeout, stop, [&](Wire& wire) -> Check {
        const auto size = static_cast<std::uint32_t>(payload.size());
        std::array<std::uint8_t, 4> header{};
        store_word(header.data(), static_cast<std::uint16_t>(size >> 16));
        store_word(header.data() + 2, static_cast<std::uint16_t>(size));
        auto sent = wire.send_all(header);
        if (auto* failure = std::get_if<Diagnostic>(&sent)) { return *failure; }
        return wire.send_all(payload);
    });
    return at_resource(std::move(result), reference().id);
}

Result<Bytes> CommunicationLease::receive_frame(std::chrono::milliseconds timeout, std::stop_token stop) const
{
    auto result = connection_->run<Bytes>(Operation::Receive, timeout, stop, [](Wire& wire) -> Result<Bytes> {
        std::array<std::uint8_t, 4> header;
        auto read = wire.read_exact(header);
        if (auto* failure = std::get_if<Diagnostic>(&read)) { return *failure; }
        const auto size = (static_cast<std::uint32_t>(word(header.data())) << 16) | word(header.data() + 2);
        if (size > max_frame) { return protocol_error(tcp, "tcp.frame_length", size, "Peer announced a TCP frame larger than 64 KiB."); }
        Bytes body(size);
        read = wire.read_exact(body);
        if (auto* failure = std::get_if<Diagnostic>(&read)) { return *failure; }
        return body;
    });
    return at_resource(std::move(result), reference().id);
}

Result<std::uint16_t> CommunicationLease::read_register(std::uint16_t address,
    std::chrono::milliseconds timeout, std::stop_token stop) const
{
    auto result = connection_->run<std::uint16_t>(Operation::ReadRegister, timeout, stop, [&](Wire& wire) {
        return register_transaction(wire, ++connection_->transaction, connection_->endpoint.unit, address, {});
    });
    return at_resource(std::move(result), reference().id);
}

Check CommunicationLease::write_register(std::uint16_t address, std::uint16_t value,
    std::chrono::milliseconds timeout, std::stop_token stop) const
{
    auto result = connection_->run<std::monostate>(Operation::WriteRegister, timeout, stop, [&](Wire& wire) -> Check {
        const auto result = register_transaction(wire, ++connection_->transaction, connection_->endpoint.unit, address, value);
        if (const auto* failure = std::get_if<Diagnostic>(&result)) { return *failure; }
        return std::monostate{};
    });
    return at_resource(std::move(result), reference().id);
}
} // namespace visionpro::infrastructure
