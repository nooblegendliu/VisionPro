#pragma once

#include <visionpro/workflow/tool_execution.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace visionpro::infrastructure {

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

class FileResourceProvider final : public workflow::InvocationResourceProvider {
public:
    // Lock fixed file contents and record per-resource dependency failures.
    // Supplied directories must be absolute. An unsaved draft can resolve
    // absolute and named-root resources without a solution directory.
    [[nodiscard]] static core::Result<std::shared_ptr<FileResourceProvider>> create(
        const workflow::Solution& solution, const std::optional<std::filesystem::path>& solution_directory,
        std::span<const NamedResourceRoot> roots = {});
    // Read one catalog file with the same path, revision and content checks as execution.
    [[nodiscard]] static core::Result<std::shared_ptr<const FileResourceLease>> read_resource(
        const workflow::Solution& solution, const core::ResourceReference& reference,
        const std::optional<std::filesystem::path>& solution_directory,
        std::span<const NamedResourceRoot> roots = {});
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept;
    [[nodiscard]] std::span<const core::ResourceKind> kinds() const noexcept override;
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
    const std::filesystem::path& absolute_file);

} // namespace visionpro::infrastructure
