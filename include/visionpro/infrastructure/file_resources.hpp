#pragma once

#include <visionpro/workflow/tool_execution.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
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
    FileResourceLease(core::ResourceReference reference, std::filesystem::path path,
        std::shared_ptr<const std::vector<std::byte>> bytes);
    std::filesystem::path path_;
    std::shared_ptr<const std::vector<std::byte>> bytes_;
};

class FileResourceProvider final : public workflow::InvocationResourceProvider {
public:
    // Lock fixed file contents and record per-resource dependency failures.
    // The directory and supplied roots must be explicit absolute paths.
    [[nodiscard]] static core::Result<std::shared_ptr<FileResourceProvider>> create(
        const workflow::Solution& solution, const std::filesystem::path& solution_directory,
        std::span<const NamedResourceRoot> roots = {});
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept;
    [[nodiscard]] std::span<const core::ResourceKind> kinds() const noexcept override;
    [[nodiscard]] core::Result<std::monostate> preflight(const workflow::ExecutionPlan& plan) const override;
    [[nodiscard]] core::Result<std::shared_ptr<const core::ResourceLease>> acquire(
        const core::ToolExecutionConfiguration& configuration, const core::ToolResourceBinding& binding) override;
private:
    struct Storage;
    explicit FileResourceProvider(std::shared_ptr<const Storage> storage);
    std::shared_ptr<const Storage> storage_;
};

} // namespace visionpro::infrastructure
