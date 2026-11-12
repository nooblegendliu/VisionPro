#pragma once

#include <visionpro/core/result.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>

namespace visionpro::infrastructure::detail {

inline constexpr auto package_lock_name = L".visionpro-save.lock";

// The sidecar stays in the package so every cooperative writer locks the
// same file, including writers using different spellings of its directory.
class PackageWriteLock final {
public:
    PackageWriteLock(const PackageWriteLock&) = delete;
    PackageWriteLock& operator=(const PackageWriteLock&) = delete;
    PackageWriteLock(PackageWriteLock&& other) noexcept;
    PackageWriteLock& operator=(PackageWriteLock&& other) noexcept;
    ~PackageWriteLock();

    [[nodiscard]] static core::Result<PackageWriteLock> acquire(const std::filesystem::path& directory);

private:
    explicit PackageWriteLock(void* handle) noexcept : handle_(handle) {}
    void release() noexcept;
    void* handle_;
};

[[nodiscard]] core::Result<std::string> read_manifest(const std::filesystem::path& manifest);
[[nodiscard]] core::Result<std::string> sha256(std::span<const std::byte> bytes);
[[nodiscard]] core::Result<std::monostate> write_staged_file(
    const std::filesystem::path& path, std::span<const std::byte> bytes);
[[nodiscard]] core::Result<std::monostate> publish_file(
    const std::filesystem::path& from, const std::filesystem::path& to, bool replace);

} // namespace visionpro::infrastructure::detail
