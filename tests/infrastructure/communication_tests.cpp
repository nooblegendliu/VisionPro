#include <winsock2.h>
#include <ws2tcpip.h>

#include <visionpro/core/id_factory.hpp>
#include <visionpro/core/schema_id_factory.hpp>
#include <visionpro/infrastructure/communication.hpp>

#include <array>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
using namespace visionpro::core;
using namespace visionpro::infrastructure;
using namespace std::chrono_literals;

void check(bool condition, const char* message)
{ if (!condition) { throw std::runtime_error(message); } }

template<class T> T take(Result<T> value)
{
    if (const auto* failure = std::get_if<Diagnostic>(&value)) { throw std::runtime_error(failure->message); }
    return std::get<T>(std::move(value));
}

template<class T> Diagnostic rejects(Result<T> value, ErrorCode code)
{
    const auto* failure = std::get_if<Diagnostic>(&value);
    check(failure && failure->code == code, "Unexpected diagnostic for communication operation.");
    return *failure;
}

struct Socket {
    SOCKET value;
    explicit Socket(SOCKET handle) : value(handle) { check(value != INVALID_SOCKET, "Loopback fixture could not open a socket."); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { closesocket(value); }
};

struct Listener {
    Socket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    sockaddr_in address{};
    Listener()
    {
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0, "Loopback bind failed.");
        int length = sizeof(address);
        check(getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &length) == 0, "No bound loopback port.");
        check(listen(socket.value, 2) == 0, "Loopback listen failed.");
    }
    Socket accept_client()
    {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(socket.value, &ready);
        const timeval wait{2, 0};
        check(select(0, &ready, nullptr, nullptr, &wait) == 1, "The communication client did not connect.");
        const auto handle = accept(socket.value, nullptr, nullptr);
        const DWORD timeout = 2000;
        if (handle != INVALID_SOCKET && setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
            closesocket(handle);
            throw std::runtime_error("Could not set fixture deadline.");
        }
        return Socket{handle};
    }
    ResourceDefinition resource(std::string_view adapter) const
    {
        return {take(IdFactory::generate<ResourceId>()), "local peer", ResourceKind::Device,
            DeviceResource{take(SchemaIdFactory::parse<SchemaId>(adapter)), take(communication_config("127.0.0.1", ntohs(address.sin_port), 255))}};
    }
};

void receive(const Socket& peer, std::span<std::uint8_t> bytes)
{
    while (!bytes.empty()) {
        const int count = recv(peer.value, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
        check(count > 0, "The loopback peer did not receive a complete request.");
        bytes = bytes.subspan(count);
    }
}

void plc_cancel()
{
    Listener listener;
    const auto resource = listener.resource("visionpro.modbus.tcp");
    auto lease = take(open_communication(resource));
    std::stop_source stop;
    auto write = std::async(std::launch::async, [&] { return lease->write_register(65535, 0, 3s, stop.get_token()); });
    const auto peer = listener.accept_client();
    std::array<std::uint8_t, 12> request;
    receive(peer, request);
    check(request[6] == 255 && request[7] == 6 && request[8] == 255 && request[9] == 255 && request[10] == 0
        && request[11] == 0, "Unexpected FC06 request for the configured unit, address and zero value.");

    // The first write still owns this transaction. A concurrent command expires before it can send anything.
    const auto waiting = rejects(lease->read_register(1, 40ms), ErrorCode::SourceTimedOut);
    check(waiting.location.resource == resource.id, "Queued timeout lost its resource identity.");
    stop.request_stop();
    const auto interrupted = rejects(write.get(), ErrorCode::ExternalEffectUnknown);
    check(interrupted.location.resource == resource.id && interrupted.external && interrupted.external->provider_domain
        && interrupted.external->provider_domain->text() == "visionpro.communication.runtime"
        && interrupted.external->raw_code == static_cast<std::int64_t>(ErrorCode::SourceCancelled),
        "An unconfirmed FC06 cancelled after transmission must retain the cancellation cause.");
    char excess;
    check(recv(peer.value, &excess, 1, 0) == 0, "A queued operation or an implicit write replay reached the old connection.");

    auto next = std::async(std::launch::async, [&] { return lease->read_register(65535, 2s); });
    const auto reopened = listener.accept_client();
    std::array<std::uint8_t, 12> read;
    receive(reopened, read);
    check(read[7] == 3 && read[10] == 0 && read[11] == 1 && (read[0] != request[0] || read[1] != request[1]),
        "The next explicit transaction must issue a fresh single-register FC03.");
    const std::array<std::uint8_t, 11> reply{read[0], read[1], 0, 0, 0, 5, 255, 3, 2, 0xff, 0xff};
    check(send(reopened.value, reinterpret_cast<const char*>(reply.data()), static_cast<int>(reply.size()), 0) == static_cast<int>(reply.size()),
        "Loopback reply failed.");
    check(take(next.get()) == 65535, "FC03 lost the unsigned register value on a fresh connection.");
}

void frame_boundaries()
{
    Listener listener;
    const auto resource = listener.resource("visionpro.tcp");
    auto lease = take(open_communication(resource));
    rejects(lease->write_register(0, 0, 1s), ErrorCode::ResourceKindMismatch);
    std::vector<std::uint8_t> payload(65537, 0xab);
    rejects(lease->send_frame(payload, 1s), ErrorCode::SizeOverflow);
    payload.pop_back();
    payload.front() = 0;
    payload.back() = 0;
    auto pending = std::async(std::launch::async, [&] { return lease->send_frame(payload, 2s); });
    const auto peer = listener.accept_client();
    std::array<std::uint8_t, 4> prefix;
    receive(peer, prefix);
    check(prefix == std::array<std::uint8_t, 4>{0, 1, 0, 0}, "64 KiB frame must have a 32-bit big-endian prefix.");
    std::vector<std::uint8_t> actual(payload.size());
    receive(peer, actual);
    take(pending.get());
    check(actual == payload, "Maximum-sized frame changed on the wire.");
}
}

int main()
{
    try {
        rejects(communication_config("localhost", 502, 1), ErrorCode::InvalidResource);
        rejects(communication_config("127.0.0.1", 0, 1), ErrorCode::ConstraintViolation);
        rejects(communication_config("127.0.0.1", 502, 256), ErrorCode::ConstraintViolation);
        WSADATA data{};
        check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Could not initialize fixture Winsock.");
        struct Cleanup { ~Cleanup() { WSACleanup(); } } cleanup;
        plc_cancel();
        frame_boundaries();
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
