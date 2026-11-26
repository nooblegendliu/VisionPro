#pragma once

#include <visionpro/core/tool_execution.hpp>
#include <filesystem>
#include <functional>

namespace visionpro::core
{

struct ImageFtpSettings {
    std::string host;
    std::uint16_t port{21};
    std::string user;
    std::string credential; // Windows Credential Manager reference.
    std::string encoding{"UTF-8"};
    std::string path{"/"};
};
enum class ArchiveSuffix { Sequence, Time, None };
enum class ArchiveKind { Original, Rendered };
struct ArchiveImage {
    ArchiveKind kind{};
    std::filesystem::path directory;
    std::string prefix;
    ArchiveSuffix suffix{ArchiveSuffix::Sequence};
    std::string extension{"png"}; // png, jpg/jpeg or bmp, bytes already encoded.
    std::vector<std::uint8_t> bytes;
    std::size_t cache_limit{20};
    std::uint64_t capacity_bytes{};
};
struct ImageSaveOptions {
    bool date_directories{};
    bool sync_storage{true};
    std::string verdict;     // Empty, OK or NG, used in names.
    std::size_t interval{};  // Triggered batch size, zero/one commits immediately.
    std::size_t max_files{}; // Zero means unlimited.
    std::uint64_t minimum_free_bytes{};
    unsigned retention_days{}; // Zero disables expiry.
    bool overwrite_oldest{};
    std::optional<ImageFtpSettings> ftp;
};
struct ImageSaveReceipt {
    ArchiveKind kind{};
    std::string name;
    std::optional<std::filesystem::path> local_file;
    std::optional<std::filesystem::path> package;
};
struct ImageSaveReport {
    std::vector<ImageSaveReceipt> images;
    std::optional<core::Diagnostic> failure; // Previously committed receipts survive partial failure.
};
class ImageArchiveWriter
{
  public:
    virtual ~ImageArchiveWriter() = default;
    [[nodiscard]] virtual ImageSaveReport save(const ToolInvocationIdentity& identity,
                                               const ImageSaveOptions& options, std::vector<ArchiveImage> images,
                                               const std::function<bool()>& cancelled = {}) = 0;
};
} // namespace visionpro::core
