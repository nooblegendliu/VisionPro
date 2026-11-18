#pragma once

#include <visionpro/workflow/run.hpp>

namespace visionpro::workflow::detail {

[[nodiscard]] core::Result<core::DataValue> project_region_export(const NodeResult& record, const OutputReference& source);

// One running loop. Bounds and entry inputs stay frozen; each body frame gets
// its own index and output accumulators refer only to completed iterations.
class LoopExecution final {
public:
    [[nodiscard]] static core::Result<LoopExecution> create(const GroupConfiguration& group,
        const core::InvocationKey& call, std::vector<RunInput> inputs);
    [[nodiscard]] bool in_range() const noexcept;
    [[nodiscard]] core::Result<bool> advance();
    [[nodiscard]] std::vector<RunInput> body_inputs() const;
    [[nodiscard]] core::Result<std::monostate> collect(const LoopExport& spec, const core::DataValue& value);
    [[nodiscard]] core::Result<std::vector<OutputSlot>> outputs() const;

    const GroupConfiguration* group;
    core::InvocationKey call;
    std::int64_t index;
    std::optional<std::size_t> frame{};
    std::chrono::steady_clock::time_point due{};
private:
    struct Accumulator {
        core::OutputId id;
        std::optional<core::DataValue> single;
        std::vector<core::DataValue> values;
    };
    LoopExecution(const GroupConfiguration& configuration, core::InvocationKey identity,
        std::int64_t start, std::int64_t end, core::InputId index_id, std::vector<RunInput> inputs);
    std::int64_t end_;
    core::InputId index_id_;
    std::vector<RunInput> inputs_;
    std::vector<Accumulator> accumulators_;
};

} // namespace visionpro::workflow::detail
