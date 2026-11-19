#pragma once

#include <visionpro/core/shared_data.hpp>
#include <visionpro/workflow/snapshot.hpp>

namespace visionpro::workflow {

class SharedDataService final : public core::SharedData {
public:
    // A publication shares compatible cells with its predecessor. In-flight calls
    // retain their prior view when a definition is replaced. Omit previous on activation.
    [[nodiscard]] static core::Result<std::shared_ptr<SharedDataService>> create(const ConfigSnapshot& snapshot,
        std::shared_ptr<const SharedDataService> previous = {});
    ~SharedDataService() override;
    void stop_waits() noexcept;
    [[nodiscard]] const core::GlobalVariableDefinition* variable_definition(core::VariableId id) const noexcept override;
    [[nodiscard]] const core::DataQueueDefinition* queue_definition(core::QueueId id) const noexcept override;
    [[nodiscard]] core::Result<core::GlobalVariableEntry> read(core::VariableId id) const override;
    [[nodiscard]] core::Result<std::vector<core::GlobalVariableEntry>> read_many(std::span<const core::VariableId> ids) const override;
    [[nodiscard]] core::Result<std::vector<core::GlobalVariableEntry>> read_all() const override;
    [[nodiscard]] core::Result<core::GlobalVariableEntry> write(core::VariableId id, const core::DataValue& value,
        const core::UpdateOrigin& origin) override;
    [[nodiscard]] core::Result<std::vector<core::GlobalVariableEntry>> write_many(std::span<const core::GlobalWrite> updates,
        const core::UpdateOrigin& origin) override;
    [[nodiscard]] core::Result<core::QueueMessage> send(core::QueueId id, const core::DataValue& payload,
        const core::CorrelationId& correlation, const core::UpdateOrigin& producer, std::stop_token stop = {}) override;
    [[nodiscard]] core::Result<std::vector<core::QueueMessage>> receive_many(std::span<const core::QueueRead> requests,
        const core::UpdateOrigin& consumer, std::stop_token stop = {}) override;
    [[nodiscard]] core::Result<core::QueuePreview> preview(core::QueueId id) const override;
private:
    struct Impl;
    explicit SharedDataService(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace visionpro::workflow
