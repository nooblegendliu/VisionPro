#pragma once

#include <visionpro/workflow/run.hpp>

#include <map>
#include <set>

namespace visionpro::workflow {

// Session-scoped storage for terminal records. Owners serialize writes against
// readers and arbitrate admission and result acceptance outside this container.
class ResultStore final {
public:
    explicit ResultStore(core::SolutionSessionId session) : session_(session) {}
    [[nodiscard]] const core::SolutionSessionId& session() const noexcept { return session_; }
    [[nodiscard]] core::Result<std::monostate> insert(const ConfigSnapshot& snapshot, const NodeResult& result);
    [[nodiscard]] core::Result<std::optional<NodeResult>> lookup(const RunIdentity& run,
        const core::InvocationKey& invocation) const;
    // Returned records/values keep their ownership. Retired run IDs cannot be reused.
    [[nodiscard]] core::Result<std::monostate> release_run(const RunIdentity& run);
    [[nodiscard]] std::size_t size() const noexcept { return results_.size(); }
private:
    core::SolutionSessionId session_;
    std::map<core::WorkflowRunId, RunIdentity> runs_;
    std::map<core::InvocationKey, NodeResult> results_;
    std::set<core::WorkflowRunId> retired_;
};

} // namespace visionpro::workflow
