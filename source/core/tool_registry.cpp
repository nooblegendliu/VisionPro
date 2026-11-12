#include <visionpro/core/tool_registry.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace visionpro::core {
namespace {

Diagnostic registration_error(ErrorCode code, const ToolDescriptor& descriptor, std::string message)
{
    DiagnosticLocation where;
    where.tool = descriptor.id;
    where.schema = descriptor.schema.id;
    return {code, Severity::Error, std::move(where), std::move(message)};
}

template<class Range>
bool same_range(const std::optional<Range>& left, const std::optional<Range>& right)
{
    if (!left || !right) { return left.has_value() == right.has_value(); }
    return left->minimum == right->minimum && left->maximum == right->maximum;
}

bool same_constraints(const ValueConstraints& left, const ValueConstraints& right)
{
    return same_range(left.integer_range, right.integer_range)
        && same_range(left.real_range, right.real_range)
        && std::ranges::is_permutation(left.allowed_values, right.allowed_values)
        && left.max_length == right.max_length;
}

bool same_binding(const BindingPolicy& left, const BindingPolicy& right)
{
    return left.cardinality == right.cardinality && left.time == right.time
        && std::ranges::is_permutation(left.allowed_sources, right.allowed_sources)
        && std::ranges::is_permutation(left.allowed_conversions, right.allowed_conversions);
}

template<class Target>
bool same_target(const Target& left, const Target& right)
{
    return left.id == right.id && left.type == right.type && left.required == right.required
        && left.default_value == right.default_value && left.semantics == right.semantics
        && same_constraints(left.constraints, right.constraints) && same_binding(left.binding, right.binding);
}

bool same_output(const OutputDescriptor& left, const OutputDescriptor& right)
{
    return left.id == right.id && left.type == right.type && left.semantics == right.semantics
        && left.required == right.required
        && std::ranges::is_permutation(left.allowed_absence, right.allowed_absence);
}

bool same_state(const std::optional<NodeStateDefinition>& left, const std::optional<NodeStateDefinition>& right)
{
    if (!left || !right) { return left.has_value() == right.has_value(); }
    return left->schema == right->schema && left->initial_value == right->initial_value
        && left->scope == right->scope && left->reset == right->reset
        && left->compatibility == right->compatibility && left->mutation == right->mutation
        && left->concurrency == right->concurrency;
}

bool same_contract(const ToolDescriptor& left, const ToolDescriptor& right)
{
    const auto same_parameter = [](const ParameterDescriptor& a, const ParameterDescriptor& b) {
        return same_target(a, b) && a.subscribable == b.subscribable && a.structural == b.structural;
    };
    const auto same_resource = [](const ResourceRequirement& a, const ResourceRequirement& b) {
        return a.slot == b.slot && a.kind == b.kind && a.required == b.required;
    };
    return left.schema.complete == right.schema.complete
        && std::ranges::is_permutation(left.schema.required_capabilities, right.schema.required_capabilities)
        && std::ranges::equal(left.parameters, right.parameters, same_parameter)
        && std::ranges::equal(left.inputs, right.inputs, same_target<InputDescriptor>)
        && std::ranges::equal(left.outputs, right.outputs, same_output)
        && std::ranges::equal(left.control_outputs, right.control_outputs, std::ranges::equal_to{},
            &ControlOutputDescriptor::id, &ControlOutputDescriptor::id)
        && same_state(left.state, right.state) && left.state_parameters == right.state_parameters
        && std::ranges::equal(left.resources, right.resources, same_resource)
        && left.region_capabilities == right.region_capabilities
        && std::ranges::is_permutation(left.image_formats, right.image_formats)
        && left.execution == right.execution;
}

} // namespace

struct ToolRegistry::Storage {
    struct Entry {
        std::size_t descriptor_index;
        SchemaSupport support;
        std::shared_ptr<ToolInstanceFactory> factory;
    };
    std::vector<ToolDescriptor> descriptors;
    std::map<ToolTypeId, Entry> entries;
};

Result<ToolRegistry> ToolRegistry::create(std::span<const ToolRegistration> registrations)
{
    Storage storage;
    storage.descriptors.reserve(registrations.size());
    for (const auto& registration : registrations) {
        const auto& descriptor = registration.descriptor;
        const auto& support = registration.support;
        if (auto checked = validate_descriptor(descriptor);
            const auto* failure = std::get_if<Diagnostic>(&checked)) { return *failure; }
        if (!registration.factory) {
            return registration_error(ErrorCode::InvalidDescriptor, descriptor, "Tool registration requires an instance factory.");
        }
        if (support.minimum_version == 0 || support.minimum_version > support.maximum_version
            || std::set<CapabilityId>(support.capabilities.begin(), support.capabilities.end()).size() != support.capabilities.size()) {
            return registration_error(ErrorCode::InvalidDescriptor, descriptor,
                "Schema support requires a nonzero ordered version range and unique capabilities.");
        }
        const auto [_, inserted] = storage.entries.emplace(descriptor.id,
            Storage::Entry{storage.descriptors.size(), support, registration.factory});
        if (!inserted) {
            return registration_error(ErrorCode::DuplicateIdentity, descriptor, "Tool type is already registered.");
        }
        storage.descriptors.push_back(descriptor);
    }
    return ToolRegistry{std::make_shared<const Storage>(std::move(storage))};
}

std::span<const ToolDescriptor> ToolRegistry::descriptors() const noexcept { return storage_->descriptors; }
const ToolDescriptor* ToolRegistry::find_descriptor(const ToolTypeId& type) const noexcept
{
    const auto found = storage_->entries.find(type);
    return found == storage_->entries.end() ? nullptr : &storage_->descriptors[found->second.descriptor_index];
}

const SchemaSupport* ToolRegistry::find_support(const ToolTypeId& type) const noexcept
{
    const auto found = storage_->entries.find(type);
    return found == storage_->entries.end() ? nullptr : &found->second.support;
}

Result<std::monostate> ToolRegistry::validate(const ToolDescriptor& descriptor) const
{
    const auto found = storage_->entries.find(descriptor.id);
    if (found == storage_->entries.end()) {
        return registration_error(ErrorCode::UnknownTool, descriptor, "Tool type has no static factory registration.");
    }
    const auto& registered = storage_->descriptors[found->second.descriptor_index];
    if (registered.config_version != descriptor.config_version || registered.schema.id != descriptor.schema.id
        || registered.schema.version != descriptor.schema.version) {
        return registration_error(ErrorCode::IncompatibleSchemaVersion, descriptor,
            "Snapshot tool configuration and Schema versions must match the static registration.");
    }
    if (!same_contract(registered, descriptor)) {
        return registration_error(ErrorCode::InvalidDescriptor, descriptor,
            "Snapshot tool contract differs from the static registration at the same version.");
    }
    return validate_schema_support(descriptor, found->second.support);
}

Result<std::shared_ptr<ToolInstance>> ToolRegistry::create_instance(const ToolExecutionConfiguration& configuration) const
{
    const auto& data = configuration.data();
    if (auto checked = validate(data.tool); auto* failure = std::get_if<Diagnostic>(&checked)) {
        failure->location.workflow = data.workflow;
        failure->location.node = data.node;
        return std::move(*failure);
    }
    const auto factory = storage_->entries.at(data.tool.id).factory;
    return create_tool_instance(*factory, configuration);
}

} // namespace visionpro::core
