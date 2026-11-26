#pragma once

#include <visionpro/infrastructure/image_archive.hpp>
#include "solution_storage.hpp"

namespace visionpro::infrastructure::archive_detail
{
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
inline core::Diagnostic issue(std::string message, core::ErrorCode code = core::ErrorCode::SolutionIoFailure)
{
    return {code, core::Severity::Error, {}, std::move(message)};
}
std::string uuid();
std::string utf8(const fs::path& path);
fs::path path_from(std::string_view name);
bool safe_name(std::string_view name);
fs::path unique_path(const fs::path& desired);
ArchiveCheck atomic_file(const fs::path& path, std::span<const std::byte> bytes, bool replace = false);
struct PackageImage {
    std::string name, origin;
    Bytes bytes;
};
ArchiveCheck write_package(const fs::path& target, std::span<const PackageImage> images);
ArchiveCheck validate_ftp_settings(const ImageFtpSettings& settings);
core::Result<std::string> upload_image(const ImageFtpSettings& settings, std::span<const std::byte> encoded_image,
                                       const std::string& name, std::stop_token stop = {});
} // namespace visionpro::infrastructure::archive_detail
