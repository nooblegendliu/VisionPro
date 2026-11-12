#pragma once

#include <visionpro/workflow/model.hpp>

#include <memory>
#include <span>
#include <utility>

namespace visionpro::workflow {

struct SnapshotIdentity {
    core::SolutionSessionId session;
    core::ConfigSnapshotId snapshot;
    auto operator<=>(const SnapshotIdentity&) const = default;
};

// Owns a detached configuration revision and tool metadata. Capture requires
// global invariants, but retains local drafts for diagnostics and scoped planning.
class ConfigSnapshot final {
public:
    ConfigSnapshot(const ConfigSnapshot&) noexcept = default;
    ConfigSnapshot& operator=(const ConfigSnapshot&) noexcept = default;
    ConfigSnapshot(ConfigSnapshot&& other) noexcept : storage_(other.storage_) {}
    ConfigSnapshot& operator=(ConfigSnapshot&& other) noexcept { storage_ = other.storage_; return *this; }
    [[nodiscard]] static core::Result<ConfigSnapshot> capture(const Solution& solution,
        std::span<const core::ToolDescriptor> tools, SnapshotIdentity identity);
    [[nodiscard]] const SnapshotIdentity& identity() const noexcept;
    [[nodiscard]] const Solution& solution() const noexcept;
    [[nodiscard]] std::span<const core::ToolDescriptor> tools() const noexcept;
    [[nodiscard]] const core::ToolDescriptor* find_tool(const core::ToolTypeId& type) const noexcept;
private:
    struct Storage;
    explicit ConfigSnapshot(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
};

} // namespace visionpro::workflow
