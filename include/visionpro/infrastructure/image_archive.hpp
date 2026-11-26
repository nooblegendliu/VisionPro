#pragma once

#include <visionpro/core/image_archive_writer.hpp>
#include <filesystem>
#include <functional>
#include <mutex>

namespace visionpro::infrastructure
{

using core::ArchiveImage;
using core::ArchiveKind;
using core::ArchiveSuffix;
using core::ImageFtpSettings;
using core::ImageSaveOptions;
using core::ImageSaveReceipt;
using core::ImageSaveReport;
using ArchiveCheck = core::Result<std::monostate>;
[[nodiscard]] ArchiveCheck save_image_ftp_password(std::string_view credential, std::string_view password);
[[nodiscard]] ArchiveCheck remove_image_ftp_password(std::string_view credential);
[[nodiscard]] ArchiveCheck check_image_ftp(const ImageFtpSettings& settings);

struct ArchivePackagePolicy {
    std::filesystem::path directory;
    std::uint32_t images_per_file{64};
    bool operator==(const ArchivePackagePolicy&) const = default;
};
struct ImageRetentionLocation {
    core::NodeId node;
    std::filesystem::path directory;
};
class ImageArchive final : public core::ImageArchiveWriter
{
  public:
    explicit ImageArchive(std::optional<ArchivePackagePolicy> policy = {});
    ~ImageArchive();
    ImageArchive(const ImageArchive&) = delete;
    ImageArchive& operator=(const ImageArchive&) = delete;
    [[nodiscard]] ImageSaveReport save(const core::ToolInvocationIdentity& identity,
                                       const ImageSaveOptions& options, std::vector<ArchiveImage> images,
                                       const std::function<bool()>& cancelled = {}) override;
    [[nodiscard]] ArchiveCheck flush();
    // Locations from the solution resource catalog are eligible for recovery of its durable FTP queues.
    void register_output_directory(const std::filesystem::path& directory);
    void register_output_node(const core::NodeId& node, std::optional<unsigned> retention_days = {});
    void set_retention_locations(std::span<const ImageRetentionLocation> locations);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
class ImageArchivePool final
{
  public:
    [[nodiscard]] std::shared_ptr<ImageArchive> writer(const std::optional<ArchivePackagePolicy>& policy);
    [[nodiscard]] ArchiveCheck flush();

  private:
    std::mutex mutex_;
    std::vector<std::pair<std::optional<ArchivePackagePolicy>, std::shared_ptr<ImageArchive>>> writers_;
};
class ImageOutputLease final : public core::ResourceLease
{
  public:
    ImageOutputLease(core::ResourceReference reference, std::filesystem::path directory,
                     std::shared_ptr<ImageArchive> archive, std::uint64_t capacity_bytes = 0)
        : ResourceLease(std::move(reference)), directory_(std::move(directory)), archive_(std::move(archive)),
          capacity_(capacity_bytes)
    {
    }
    [[nodiscard]] const std::filesystem::path& directory() const noexcept
    {
        return directory_;
    }
    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept
    {
        return capacity_;
    }
    [[nodiscard]] ImageArchive& archive() const noexcept
    {
        return *archive_;
    }

  private:
    std::filesystem::path directory_;
    std::shared_ptr<ImageArchive> archive_;
    std::uint64_t capacity_{};
};
struct ImagePackageEntry {
    std::string name;
    std::string origin;
    std::uint64_t size{};
};
[[nodiscard]] core::Result<std::vector<ImagePackageEntry>> list_image_package(const std::filesystem::path& file);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> read_image_package_entry(const std::filesystem::path& file,
                                                                               std::size_t index);
// Accepts one package or a directory containing packages. Never overwrites an existing extracted file.
[[nodiscard]] core::Result<std::vector<std::filesystem::path>>
extract_image_packages(const std::filesystem::path& file_or_directory, bool same_name_folder);

} // namespace visionpro::infrastructure
