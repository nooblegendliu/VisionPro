#include "solution_storage.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace visionpro::infrastructure::detail {
namespace {
namespace fs = std::filesystem;
using namespace core;
using Check = Result<std::monostate>;

constexpr std::size_t io_chunk_size = 1 << 20;

Diagnostic error(ErrorCode code, std::string message)
{ return {code, Severity::Error, {}, std::move(message)}; }

Diagnostic io_error(std::string_view action, DWORD native_error)
{
    return error(ErrorCode::SolutionIoFailure,
        std::string(action) + " (Win32 error " + std::to_string(native_error) + ").");
}

bool absolute_path(const fs::path& path)
{ return path.is_absolute() && path.native().find(L'\0') == std::wstring::npos; }

bool regular_attributes(DWORD attributes) noexcept
{
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

class NativeFile final {
public:
    explicit NativeFile(HANDLE handle) noexcept : handle_(handle) {}
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;
    ~NativeFile() { if (valid()) { CloseHandle(handle_); } }

    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

private:
    HANDLE handle_;
};

Check check_local_ntfs(const fs::path& directory)
{
    if (!absolute_path(directory)) {
        return error(ErrorCode::InvalidPath, "Save requires an absolute package directory.");
    }
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) { return io_error("Could not inspect the package directory", GetLastError()); }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return error(ErrorCode::InvalidPath, "The package path must be a directory.");
    }
    std::wstring volume(32768, L'\0');
    if (!GetVolumePathNameW(directory.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) {
        return io_error("Could not determine the package volume", GetLastError());
    }
    if (GetDriveTypeW(volume.c_str()) != DRIVE_FIXED) {
        return error(ErrorCode::MissingCapability, "Transactional saves require a local NTFS volume.");
    }
    std::array<wchar_t, MAX_PATH + 1> file_system{};
    if (!GetVolumeInformationW(volume.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
        file_system.data(), static_cast<DWORD>(file_system.size()))) {
        return io_error("Could not determine the package filesystem", GetLastError());
    }
    if (std::wstring_view(file_system.data()) != L"NTFS") {
        return error(ErrorCode::MissingCapability, "Transactional saves require a local NTFS volume.");
    }
    return std::monostate{};
}

struct Sha256Handles {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};

    ~Sha256Handles()
    {
        if (hash) { BCryptDestroyHash(hash); }
        if (algorithm) { BCryptCloseAlgorithmProvider(algorithm, 0); }
    }
};

// A failed write disposes of the file represented by the opened handle.
// A failed CREATE_NEW never acquires ownership of the existing file.
struct DiscardPartialFile {
    HANDLE handle;
    bool complete{false};

    ~DiscardPartialFile()
    {
        if (!complete) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
} // namespace

PackageWriteLock::PackageWriteLock(PackageWriteLock&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

PackageWriteLock& PackageWriteLock::operator=(PackageWriteLock&& other) noexcept
{
    if (this != &other) {
        release();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

PackageWriteLock::~PackageWriteLock() { release(); }

void PackageWriteLock::release() noexcept
{
    if (!handle_) { return; }
    OVERLAPPED range{};
    UnlockFileEx(handle_, 0, 1, 0, &range);
    CloseHandle(handle_);
    handle_ = nullptr;
}

Result<PackageWriteLock> PackageWriteLock::acquire(const fs::path& directory)
{
    auto checked = check_local_ntfs(directory);
    if (auto* failure = std::get_if<Diagnostic>(&checked)) { return std::move(*failure); }
    const auto sidecar = directory / package_lock_name;
    NativeFile file(CreateFileW(sidecar.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file.valid()) { return io_error("Could not open the package save lock", GetLastError()); }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info)) {
        return io_error("Could not inspect the package save lock", GetLastError());
    }
    if (GetFileType(file.get()) != FILE_TYPE_DISK || !regular_attributes(info.dwFileAttributes)) {
        return error(ErrorCode::InvalidPath, "The package save lock must be a regular file.");
    }
    OVERLAPPED range{};
    if (!LockFileEx(file.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &range)) {
        return io_error("Could not acquire the package save lock", GetLastError());
    }
    return PackageWriteLock{file.release()};
}

Result<std::string> read_manifest(const fs::path& manifest)
{
    if (!absolute_path(manifest)) { return error(ErrorCode::InvalidPath, "The manifest path must be absolute."); }
    NativeFile file(CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) { return io_error("Could not open the solution manifest", GetLastError()); }
    if (GetFileType(file.get()) != FILE_TYPE_DISK) {
        return error(ErrorCode::SolutionIoFailure, "The solution manifest must be a regular file.");
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length)) { return io_error("Could not read the manifest size", GetLastError()); }
    std::string text;
    if (length.QuadPart < 0 || static_cast<std::uintmax_t>(length.QuadPart) > text.max_size()) {
        return error(ErrorCode::SizeOverflow, "The solution manifest exceeds the supported file size.");
    }
    text.resize(static_cast<std::size_t>(length.QuadPart));
    std::size_t offset = 0;
    while (offset != text.size()) {
        const auto count = static_cast<DWORD>(std::min(text.size() - offset, io_chunk_size));
        DWORD read = 0;
        if (!ReadFile(file.get(), text.data() + offset, count, &read, nullptr)) {
            return io_error("Could not read the solution manifest", GetLastError());
        }
        if (read == 0) { return error(ErrorCode::SolutionIoFailure, "The solution manifest ended before its recorded size."); }
        offset += read;
    }
    char extra{};
    DWORD read = 0;
    if (!ReadFile(file.get(), &extra, 1, &read, nullptr)) {
        const auto native_error = GetLastError();
        if (native_error != ERROR_HANDLE_EOF) { return io_error("Could not check the manifest end", native_error); }
    } else if (read != 0) {
        return error(ErrorCode::SolutionIoFailure, "The solution manifest changed size during reading.");
    }
    return text;
}

Result<std::string> sha256(std::span<const std::byte> bytes)
{
    const auto failure = [] {
        return error(ErrorCode::ExecutionBoundaryFailure, "Windows CNG could not compute the SHA-256 digest.");
    };
    Sha256Handles handles;
    if (BCryptOpenAlgorithmProvider(&handles.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0
        || BCryptCreateHash(handles.algorithm, &handles.hash, nullptr, 0, nullptr, 0, 0) != 0) { return failure(); }
    while (!bytes.empty()) {
        const auto count = std::min<std::size_t>(bytes.size(), std::numeric_limits<ULONG>::max());
        // BCryptHashData reads the caller's buffer despite the mutable parameter type.
        auto* input = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data()));
        if (BCryptHashData(handles.hash, input, static_cast<ULONG>(count), 0) != 0) { return failure(); }
        bytes = bytes.subspan(count);
    }
    std::array<UCHAR, 32> hash{};
    if (BCryptFinishHash(handles.hash, hash.data(), static_cast<ULONG>(hash.size()), 0) != 0) { return failure(); }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string digest(64, '\0');
    for (std::size_t index = 0; index < hash.size(); ++index) {
        digest[2 * index] = hex[hash[index] >> 4];
        digest[2 * index + 1] = hex[hash[index] & 0xf];
    }
    return digest;
}

Check write_staged_file(const fs::path& path, std::span<const std::byte> bytes)
{
    if (!absolute_path(path)) { return error(ErrorCode::InvalidPath, "The staged file path must be absolute."); }
    NativeFile file(CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const auto native_error = GetLastError();
        if (native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS) {
            return error(ErrorCode::SolutionAlreadyExists, "The staged file path already exists.");
        }
        return io_error("Could not create the staged file", native_error);
    }
    DiscardPartialFile discard{file.get()};
    while (!bytes.empty()) {
        const auto count = static_cast<DWORD>(std::min(bytes.size(), io_chunk_size));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data(), count, &written, nullptr)) {
            return io_error("Could not write the staged file", GetLastError());
        }
        if (written == 0) { return error(ErrorCode::SolutionIoFailure, "The staged file write made no progress."); }
        bytes = bytes.subspan(written);
    }
    if (!FlushFileBuffers(file.get())) { return io_error("Could not flush the staged file", GetLastError()); }
    discard.complete = true;
    return std::monostate{};
}

Check publish_file(const fs::path& from, const fs::path& to, bool replace)
{
    if (!absolute_path(from) || !absolute_path(to) || from == to) {
        return error(ErrorCode::InvalidPath, "Publication requires distinct absolute file paths.");
    }
    const auto source = GetFileAttributesW(from.c_str());
    if (source == INVALID_FILE_ATTRIBUTES) { return io_error("Could not inspect the staged file", GetLastError()); }
    if (!regular_attributes(source)) { return error(ErrorCode::InvalidPath, "The staged file must be a regular file."); }

    const auto target = GetFileAttributesW(to.c_str());
    if (target != INVALID_FILE_ATTRIBUTES) {
        if (!replace) { return error(ErrorCode::SolutionAlreadyExists, "The destination already exists."); }
        if (!regular_attributes(target)) {
            return error(ErrorCode::InvalidPath, "The replacement target must be a regular file.");
        }
    } else {
        const auto native_error = GetLastError();
        if (replace || native_error != ERROR_FILE_NOT_FOUND) {
            return io_error("Could not inspect the publication target", native_error);
        }
    }
    // Copy is disabled so publication is a same-volume rename.
    if (!MoveFileExW(from.c_str(), to.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) {
        const auto native_error = GetLastError();
        if (!replace && (native_error == ERROR_FILE_EXISTS || native_error == ERROR_ALREADY_EXISTS)) {
            return error(ErrorCode::SolutionAlreadyExists, "The destination already exists.");
        }
        return io_error("Could not publish the staged file", native_error);
    }
    return std::monostate{};
}

} // namespace visionpro::infrastructure::detail
