#pragma once

#include <visionpro/workflow/tool_execution.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace visionpro::infrastructure {

class ImageArchivePool;

struct NamedResourceRoot {
    core::SchemaId id;
    std::filesystem::path directory;
};

class FileResourceLease final : public core::ResourceLease {
public:
    // A provenance path. Consumers use the owned bytes for this invocation.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return *bytes_; }
private:
    friend class FileResourceProvider;
    struct FilePin;
    FileResourceLease(core::ResourceReference reference, std::filesystem::path path,
        std::shared_ptr<const std::vector<std::byte>> bytes, bool managed);
    std::filesystem::path path_;
    std::shared_ptr<const std::vector<std::byte>> bytes_;
    std::shared_ptr<const FilePin> pin_;
};

class FileSequenceLease final : public core::ResourceLease {
public:
    FileSequenceLease(core::ResourceReference reference, std::vector<std::filesystem::path> files, std::size_t initial,
        std::vector<std::string> digests)
        : ResourceLease(std::move(reference)), files_(std::move(files)), initial_(initial), digests_(std::move(digests)) {}
    [[nodiscard]] std::span<const std::filesystem::path> files() const noexcept { return files_; }
    [[nodiscard]] std::size_t initial_index() const noexcept { return initial_; }
    [[nodiscard]] std::optional<std::string_view> digest(std::size_t index) const {
        return digests_.empty() ? std::nullopt : std::optional<std::string_view>{digests_.at(index)};
    }
private:
    std::vector<std::filesystem::path> files_;
    std::size_t initial_;
    std::vector<std::string> digests_;
};

// Resolve an explicitly bound directory using the same wildcard and ordering as catalog sequences.
[[nodiscard]] core::Result<std::vector<std::filesystem::path>> image_directory_files(const std::filesystem::path& directory);

class FileResourceProvider final : public workflow::InvocationResourceProvider {
public:
    // Lock fixed file contents and record per-resource dependency failures.
    // Supplied directories must be absolute. An unsaved draft can resolve
    // absolute and named-root resources without a solution directory.
    [[nodiscard]] static core::Result<std::shared_ptr<FileResourceProvider>> create(
        const workflow::Solution& solution, const std::optional<std::filesystem::path>& solution_directory,
        std::span<const NamedResourceRoot> roots = {}, std::shared_ptr<ImageArchivePool> archives = {});
    // Read one catalog file with the same path, revision and content checks as execution.
    [[nodiscard]] static core::Result<std::shared_ptr<const FileResourceLease>> read_resource(
        const workflow::Solution& solution, const core::ResourceReference& reference,
        const std::optional<std::filesystem::path>& solution_directory,
        std::span<const NamedResourceRoot> roots = {});
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept;
    [[nodiscard]] std::span<const core::ResourceKind> kinds() const noexcept override;
    [[nodiscard]] std::shared_ptr<core::ImageArchiveWriter> image_archive_writer() const override;
    [[nodiscard]] std::shared_ptr<core::ImageArchiveWriter> image_storage_writer() const override;
    [[nodiscard]] core::Result<std::monostate> flush_image_archive() const;
    [[nodiscard]] core::Result<std::monostate> preflight(const workflow::ExecutionPlan& plan) const override;
    [[nodiscard]] core::Result<std::monostate> validate_selected_value(
        const workflow::Solution& snapshot, const core::DataValue& value) const override;
    [[nodiscard]] core::Result<std::shared_ptr<const core::ResourceLease>> acquire(
        const core::ToolExecutionConfiguration& configuration, const core::ToolResourceBinding& binding) override;
private:
    struct Storage;
    explicit FileResourceProvider(std::shared_ptr<const Storage> storage);
    std::shared_ptr<const Storage> storage_;
};

[[nodiscard]] core::Result<std::shared_ptr<const std::vector<std::byte>>> read_file_bytes(
    const std::filesystem::path& absolute_file, std::optional<std::string_view> digest = {});

} // namespace visionpro::infrastructure
