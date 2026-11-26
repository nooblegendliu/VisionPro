#include "image_archive_internal.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincred.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <mutex>

namespace visionpro::infrastructure
{
namespace
{
using namespace archive_detail;
bool clean(std::string_view s)
{
    return s.find_first_of("\r\n") == s.npos && s.find('\0') == s.npos;
}
std::wstring wide(std::string_view s)
{
    if (s.empty()) {
        return {};
    }
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!count) {
        throw std::runtime_error("Invalid UTF-8 in FTP configuration.");
    }
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), count);
    return result;
}
std::wstring credential_target(std::string_view ref)
{
    if (ref.empty() || ref.size() > 512 || !clean(ref)) {
        throw std::runtime_error("Invalid FTP credential reference.");
    }
    return L"VisionPro/ImageFTP/" + wide(ref);
}
std::string password_for(const ImageFtpSettings& s)
{
    if (s.credential.empty()) {
        return s.user.empty() || s.user == "anonymous" ? "anonymous@" : "";
    }
    PCREDENTIALW raw{};
    if (!CredReadW(credential_target(s.credential).c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        throw std::runtime_error("Could not read the FTP credential.");
    }
    std::unique_ptr<CREDENTIALW, decltype(&CredFree)> credential(raw, CredFree);
    std::string pass(reinterpret_cast<const char*>(raw->CredentialBlob), raw->CredentialBlobSize);
    if (!clean(pass)) {
        throw std::runtime_error("Invalid FTP credential.");
    }
    return pass;
}
std::string encoded(const std::string& value, const std::string& encoding)
{
    if (encoding == "UTF-8" || encoding == "utf8" || encoding == "utf-8") {
        return value;
    }
    const UINT cp = encoding == "GBK" || encoding == "gbk" || encoding == "GB2312" ? 936u : CP_ACP;
    const auto text = wide(value);
    if (text.empty()) {
        return {};
    }
    BOOL replaced{};
    int size = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                   nullptr, &replaced);
    if (!size || replaced) {
        throw std::runtime_error("FTP path cannot be represented in the selected encoding.");
    }
    std::string bytes(size, '\0');
    if (!WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, text.data(), static_cast<int>(text.size()), bytes.data(),
                             size, nullptr, &replaced) ||
        replaced) {
        throw std::runtime_error("FTP path conversion failed.");
    }
    return bytes;
}
struct Winsock {
    Winsock()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) {
            throw std::runtime_error("Cannot initialize FTP transport.");
        }
    }
    ~Winsock()
    {
        WSACleanup();
    }
};
struct Socket {
    SOCKET value{INVALID_SOCKET};
    explicit Socket(SOCKET s = INVALID_SOCKET) : value(s) {}
    Socket(const Socket&) = delete;
    Socket(Socket&& other) noexcept : value(std::exchange(other.value, INVALID_SOCKET)) {}
    ~Socket()
    {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
    }
    void send_bytes(const char* p, std::size_t count)
    {
        while (count) {
            int size = send(value, p, static_cast<int>(std::min<std::size_t>(count, 65536)), 0);
            if (size <= 0) {
                throw std::runtime_error("FTP transfer interrupted.");
            }
            count -= size;
            p += size;
        }
    }
};
Socket connect_to(const std::string& host, unsigned port)
{
    static Winsock init;
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* raw{};
    auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &raw)) {
        throw std::runtime_error("Cannot resolve FTP server.");
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> list(raw, freeaddrinfo);
    for (auto* p = raw; p; p = p->ai_next) {
        Socket socket_handle(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (socket_handle.value == INVALID_SOCKET) {
            continue;
        }
        u_long mode = 1;
        ioctlsocket(socket_handle.value, FIONBIO, &mode);
        if (connect(socket_handle.value, p->ai_addr, static_cast<int>(p->ai_addrlen)) != 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                continue;
            }
            fd_set ready, failed;
            FD_ZERO(&ready);
            FD_ZERO(&failed);
            FD_SET(socket_handle.value, &ready);
            FD_SET(socket_handle.value, &failed);
            timeval timeout{3, 0};
            if (select(0, nullptr, &ready, &failed, &timeout) <= 0 || FD_ISSET(socket_handle.value, &failed)) {
                continue;
            }
            int status{}, n = sizeof(status);
            if (getsockopt(socket_handle.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&status), &n) ||
                status) {
                continue;
            }
        }
        mode = 0;
        ioctlsocket(socket_handle.value, FIONBIO, &mode);
        DWORD timeout = 5000;
        setsockopt(socket_handle.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        setsockopt(socket_handle.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout));
        return socket_handle;
    }
    throw std::runtime_error("Cannot connect to FTP server.");
}
struct Reply {
    int code{};
    std::string text;
    bool ok() const
    {
        return code / 100 == 2;
    }
};
class FtpConnection
{
    const ImageFtpSettings& config;
    Socket control;
    std::string buffered;
    std::string line()
    {
        while (true) {
            auto pos = buffered.find("\r\n");
            if (pos != buffered.npos) {
                auto text = buffered.substr(0, pos);
                buffered.erase(0, pos + 2);
                return text;
            }
            std::array<char, 2048> buf{};
            int n = recv(control.value, buf.data(), static_cast<int>(buf.size()), 0);
            if (n <= 0 || buffered.size() > 65536) {
                throw std::runtime_error("Invalid or missing FTP server reply.");
            }
            buffered.append(buf.data(), n);
        }
    }
    Reply reply()
    {
        auto text = line();
        int code{};
        if (text.size() < 4 || std::from_chars(text.data(), text.data() + 3, code).ptr != text.data() + 3) {
            throw std::runtime_error("Invalid FTP reply.");
        }
        if (text[3] == '-') {
            const auto end = text.substr(0, 3) + " ";
            for (unsigned n = 0; !text.starts_with(end); ++n) {
                if (n > 512) {
                    throw std::runtime_error("Invalid multiline FTP reply.");
                }
                text = line();
            }
        }
        return {code, std::move(text)};
    }
    Reply command(const std::string& value)
    {
        if (!clean(value)) {
            throw std::runtime_error("Invalid FTP command argument.");
        }
        auto bytes = value + "\r\n";
        control.send_bytes(bytes.data(), bytes.size());
        return reply();
    }
    void expect(const std::string& cmd)
    {
        const auto response = command(cmd);
        if (!response.ok()) {
            throw std::runtime_error("FTP server rejected " + cmd.substr(0, cmd.find(' ')) + " (reply " +
                                     std::to_string(response.code) + ").");
        }
    }
    Socket data_socket()
    {
        auto epsv = command("EPSV");
        unsigned port{};
        if (epsv.code == 229) {
            auto pos = epsv.text.find('('), end = epsv.text.find(')', pos);
            if (pos == epsv.text.npos || end == epsv.text.npos || end <= pos + 5) {
                throw std::runtime_error("Invalid FTP passive endpoint.");
            }
            auto first = epsv.text.data() + pos + 4, last = epsv.text.data() + end - 1;
            if (std::from_chars(first, last, port).ptr != last) {
                throw std::runtime_error("Invalid FTP passive port.");
            }
        } else {
            auto r = command("PASV");
            auto pos = r.text.find('('), end = r.text.find(')', pos);
            if (r.code != 227 || pos == r.text.npos || end == r.text.npos) {
                throw std::runtime_error("FTP passive mode unavailable.");
            }
            auto fields = r.text.substr(pos + 1, end - pos - 1);
            int parts[6]{};
            if (sscanf_s(fields.c_str(), "%d,%d,%d,%d,%d,%d", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4],
                         &parts[5]) != 6 ||
                parts[4] < 0 || parts[4] > 255 || parts[5] < 0 || parts[5] > 255) {
                throw std::runtime_error("Invalid passive FTP endpoint.");
            }
            port = static_cast<unsigned>(parts[4] * 256 + parts[5]);
        }
        if (!port || port > 65535) {
            throw std::runtime_error("Invalid passive FTP endpoint.");
        }
        return connect_to(config.host, port);
    }

  public:
    explicit FtpConnection(const ImageFtpSettings& s) : config(s), control(connect_to(s.host, s.port))
    {
        auto hello = reply();
        if (hello.code == 120) {
            hello = reply();
        }
        if (hello.code != 220) {
            throw std::runtime_error("FTP server unavailable.");
        }
        auto login = command("USER " + (s.user.empty() ? "anonymous" : s.user));
        if (login.code == 331) {
            login = command("PASS " + password_for(s));
        }
        if (!login.ok()) {
            throw std::runtime_error("FTP authentication failed (reply " + std::to_string(login.code) + ").");
        }
        if (s.encoding == "UTF-8") {
            (void)command("OPTS UTF8 ON");
        }
        expect("TYPE I");
    }
    std::string upload(std::span<const std::byte> bytes, const std::string& name, std::stop_token stop)
    {
        if (config.path.starts_with('/')) {
            expect("CWD /");
        }
        for (const auto& folder : path_from(config.path).relative_path()) {
            if (stop.stop_requested()) {
                throw std::runtime_error("FTP recovery stopped.");
            }
            const auto arg = encoded(utf8(folder), config.encoding);
            if (command("CWD " + arg).ok()) {
                continue;
            }
            (void)command("MKD " + arg);
            expect("CWD " + arg);
        }
        auto actual = name;
        const auto temporary = ".visionpro-upload-" + uuid();
        bool created = false;
        try {
            {
                auto channel = data_socket();
                const auto store = command("STOR " + temporary);
                if (store.code / 100 != 1) {
                    throw std::runtime_error("FTP server rejected STOR (reply " + std::to_string(store.code) +
                                             ").");
                }
                created = true;
                while (!bytes.empty()) {
                    if (stop.stop_requested()) {
                        throw std::runtime_error("FTP recovery stopped.");
                    }
                    const auto chunk = bytes.first(std::min<std::size_t>(bytes.size(), 65536));
                    channel.send_bytes(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                    bytes = bytes.subspan(chunk.size());
                }
                shutdown(channel.value, SD_SEND);
            }
            const auto completed = reply();
            if (!completed.ok()) {
                throw std::runtime_error("FTP transfer was not confirmed (reply " + std::to_string(completed.code) +
                                         ").");
            }
            // Current saves and background recovery can publish to the same FTP directory concurrently.
            // Serialize just final-name selection and publication; payload transfers remain independent.
            static std::mutex final_names;
            std::lock_guard publishing(final_names);
            auto remote = encoded(actual, config.encoding);
            const auto requested = path_from(name);
            for (unsigned collision = 2; command("SIZE " + remote).code == 213; ++collision) {
                actual = utf8(requested.stem()) + "-" + std::to_string(collision) + utf8(requested.extension());
                remote = encoded(actual, config.encoding);
            }
            const auto rename = command("RNFR " + temporary);
            if (rename.code != 350) {
                throw std::runtime_error("FTP server rejected RNFR (reply " + std::to_string(rename.code) + ").");
            }
            expect("RNTO " + remote);
        } catch (const std::exception& failure) {
            if (created) {
                std::string cleanup_error;
                try {
                    const auto removed = command("DELE " + temporary);
                    if (!removed.ok()) {
                        cleanup_error = "reply " + std::to_string(removed.code);
                    }
                } catch (const std::exception& cleanup) {
                    cleanup_error = cleanup.what();
                }
                if (!cleanup_error.empty()) {
                    throw std::runtime_error(std::string(failure.what()) + " Temporary FTP file " + temporary +
                                             " could not be cleaned up (" + cleanup_error + ").");
                }
            }
            throw;
        }
        return actual;
    }
    void probe()
    {
        // Exercise the same transfer and atomic publication as a real image, using unique disposable names.
        const std::array payload{std::byte{'V'}, std::byte{'P'}};
        const auto name = upload(payload, ".visionpro-probe-" + uuid(), {});
        const auto removed = command("DELE " + encoded(name, config.encoding));
        if (!removed.ok()) {
            throw std::runtime_error("FTP write succeeded, but the server could not remove probe " + name +
                                     " (reply " + std::to_string(removed.code) + ").");
        }
    }
};
} // namespace

ArchiveCheck save_image_ftp_password(std::string_view ref, std::string_view password)
{
    try {
        if (password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE || !clean(password)) {
            return issue("Invalid FTP password.");
        }
        auto target = credential_target(ref);
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = target.data();
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(password.data()));
        cred.CredentialBlobSize = static_cast<DWORD>(password.size());
        if (!CredWriteW(&cred, 0)) {
            return issue("Could not store FTP credentials for the current Windows user.");
        }
        return std::monostate{};
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
ArchiveCheck remove_image_ftp_password(std::string_view ref)
{
    try {
        if (!CredDeleteW(credential_target(ref).c_str(), CRED_TYPE_GENERIC, 0) &&
            GetLastError() != ERROR_NOT_FOUND) {
            return issue("Could not remove FTP credentials.");
        }
        return std::monostate{};
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
ArchiveCheck archive_detail::validate_ftp_settings(const ImageFtpSettings& s)
{
    if (s.host.empty() || !s.port || !clean(s.host) || !clean(s.user) || !clean(s.path) || !clean(s.credential) ||
        !(s.encoding == "UTF-8" || s.encoding == "utf-8" || s.encoding == "utf8" || s.encoding == "GBK" ||
          s.encoding == "gbk" || s.encoding == "GB2312" || s.encoding == "ANSI")) {
        return issue("Invalid FTP host, port, encoding or path.", core::ErrorCode::InvalidValue);
    }
    if (s.path.find('\\') != s.path.npos) {
        return issue("FTP directory must use forward slashes.");
    }
    for (const auto& p : path_from(s.path).relative_path()) {
        if (p == ".." || p == ".") {
            return issue("Invalid FTP directory.");
        }
    }
    return std::monostate{};
}
ArchiveCheck check_image_ftp(const ImageFtpSettings& s)
{
    try {
        auto checked = validate_ftp_settings(s);
        if (std::holds_alternative<core::Diagnostic>(checked)) {
            return checked;
        }
        FtpConnection connection(s);
        connection.probe();
        return std::monostate{};
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
core::Result<std::string> archive_detail::upload_image(const ImageFtpSettings& s, std::span<const std::byte> bytes,
                                                       const std::string& name, std::stop_token stop)
{
    try {
        FtpConnection ftp(s);
        return ftp.upload(bytes, name, stop);
    } catch (const std::exception& e) {
        return issue(e.what());
    }
}
} // namespace visionpro::infrastructure
