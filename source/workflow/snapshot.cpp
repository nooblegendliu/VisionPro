#include <visionpro/workflow/snapshot.hpp>

#include <algorithm>
#include <set>

namespace visionpro::workflow {

struct ConfigSnapshot::Storage {
    Solution solution;
    std::vector<core::ToolDescriptor> tools;
    SnapshotIdentity identity;
    std::map<core::NodeId, core::ToolDescriptor> specializations;
};

core::Result<ConfigSnapshot> ConfigSnapshot::capture(const Solution& solution,
    std::span<const core::ToolDescriptor> tools, SnapshotIdentity identity)
{
    if (const auto checked = validate_global_invariants(solution);
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
    Storage storage{solution, std::vector<core::ToolDescriptor>(tools.begin(), tools.end()), identity, {}};
    for (const auto& flow : storage.solution.workflows) {
        for (const auto& node : flow.nodes) {
            const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
            if (!config) { continue; }
            auto tool = std::ranges::find(tools, config->type, &core::ToolDescriptor::id);
            if (tool != tools.end() && tool->specialize) {
                storage.specializations.emplace(node.id, specialize_tool(*tool, node));
            }
        }
    }
    return ConfigSnapshot{std::make_shared<const Storage>(std::move(storage))};
}

const SnapshotIdentity& ConfigSnapshot::identity() const noexcept { return storage_->identity; }
const Solution& ConfigSnapshot::solution() const noexcept { return storage_->solution; }
std::span<const core::ToolDescriptor> ConfigSnapshot::tools() const noexcept { return storage_->tools; }
const core::ToolDescriptor* ConfigSnapshot::find_tool(const core::ToolTypeId& type) const noexcept
{
    const auto found = std::ranges::find(storage_->tools, type, &core::ToolDescriptor::id);
    return found == storage_->tools.end() ? nullptr : &*found;
}

const core::ToolDescriptor* ConfigSnapshot::find_tool(const Node& node) const noexcept
{
    if (const auto found = storage_->specializations.find(node.id); found != storage_->specializations.end()) {
        return &found->second;
    }
    const auto* config = std::get_if<ToolConfiguration>(&node.configuration);
    return config ? find_tool(config->type) : nullptr;
}

} // namespace visionpro::workflow
