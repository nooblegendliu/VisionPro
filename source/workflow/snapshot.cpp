#include <visionpro/workflow/snapshot.hpp>

#include <algorithm>
#include <set>

namespace visionpro::workflow {

struct ConfigSnapshot::Storage {
    Solution solution;
    std::vector<core::ToolDescriptor> tools;
    SnapshotIdentity identity;
};

core::Result<ConfigSnapshot> ConfigSnapshot::capture(const Solution& solution,
    std::span<const core::ToolDescriptor> tools, SnapshotIdentity identity)
{
    if (const auto checked = validate_model_shape(solution);
        const auto* failure = std::get_if<core::Diagnostic>(&checked)) { return *failure; }
    std::set<core::ToolTypeId> types;
    for (const auto& tool : tools) {
        if (!types.insert(tool.id).second) {
            core::Diagnostic result{core::ErrorCode::InvalidSnapshot, core::Severity::Error, {},
                "Snapshot tool metadata contains duplicate type identities."};
            result.location.solution = solution.id;
            result.location.tool = tool.id;
            return result;
        }
        if (const auto checked = core::validate_descriptor(tool);
            const auto* failure = std::get_if<core::Diagnostic>(&checked)) { return *failure; }
    }
    // Copy mutable containers even for rvalue arguments. Shared ConfigValue nodes
    // already enforce read-only ownership, so their payloads need no further copy.
    return ConfigSnapshot{std::make_shared<const Storage>(Storage{
        solution, std::vector<core::ToolDescriptor>(tools.begin(), tools.end()), identity})};
}

const SnapshotIdentity& ConfigSnapshot::identity() const noexcept { return storage_->identity; }
const Solution& ConfigSnapshot::solution() const noexcept { return storage_->solution; }
std::span<const core::ToolDescriptor> ConfigSnapshot::tools() const noexcept { return storage_->tools; }
const core::ToolDescriptor* ConfigSnapshot::find_tool(const core::ToolTypeId& type) const noexcept
{
    const auto found = std::ranges::find(storage_->tools, type, &core::ToolDescriptor::id);
    return found == storage_->tools.end() ? nullptr : &*found;
}

} // namespace visionpro::workflow
