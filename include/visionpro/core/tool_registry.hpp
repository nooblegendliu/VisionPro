#pragma once

#include <visionpro/core/tool_execution.hpp>

namespace visionpro::core {

struct ToolRegistration {
    ToolDescriptor descriptor;
    SchemaSupport support;
    std::shared_ptr<ToolInstanceFactory> factory;
};

// Owns the static catalog and factory references. Building or validating the
// catalog does not create instances or access tool resources.
class ToolRegistry final {
public:
    ToolRegistry(const ToolRegistry&) noexcept = default;
    ToolRegistry& operator=(const ToolRegistry&) noexcept = default;
    ToolRegistry(ToolRegistry&& other) noexcept : storage_(other.storage_) {}
    ToolRegistry& operator=(ToolRegistry&& other) noexcept { storage_ = other.storage_; return *this; }
    [[nodiscard]] static Result<ToolRegistry> create(std::span<const ToolRegistration> registrations);
    [[nodiscard]] std::span<const ToolDescriptor> descriptors() const noexcept;
    [[nodiscard]] const ToolDescriptor* find_descriptor(const ToolTypeId& type) const noexcept;
    [[nodiscard]] const SchemaSupport* find_support(const ToolTypeId& type) const noexcept;
    // Check that the snapshot and registered tool agree, then check support
    // for this selected tool. Other registered types need not be executable.
    [[nodiscard]] Result<std::monostate> validate(const ToolDescriptor& descriptor) const;
    [[nodiscard]] Result<std::shared_ptr<ToolInstance>> create_instance(
        const ToolExecutionConfiguration& configuration) const;
private:
    struct Storage;
    explicit ToolRegistry(std::shared_ptr<const Storage> storage) : storage_(std::move(storage)) {}
    std::shared_ptr<const Storage> storage_;
};

} // namespace visionpro::core
