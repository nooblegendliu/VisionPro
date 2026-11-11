#pragma once

#include <visionpro/core/value.hpp>
#include <visionpro/core/resource.hpp>
#include <visionpro/core/state.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace visionpro::core {

enum class BindingSourceKind { Constant, RunOutput, PublicInput, Resource, GlobalRead };
enum class BindingCardinality { SingleSource, MultipleSources, CollectionSource };
enum class BindingTime { SnapshotBuild, BeforeInvocation };
enum class ConversionKind { Exact, WrapOptional, Int64ToFloat64Explicit };

struct BindingPolicy {
    std::vector<BindingSourceKind> allowed_sources{BindingSourceKind::Constant};
    BindingCardinality cardinality{BindingCardinality::SingleSource};
    BindingTime time{BindingTime::BeforeInvocation};
    std::vector<ConversionKind> allowed_conversions{ConversionKind::Exact, ConversionKind::WrapOptional};
};

struct IntegerRange { std::int64_t minimum; std::int64_t maximum; };
struct RealRange { double minimum; double maximum; };

struct ValueConstraints {
    std::optional<IntegerRange> integer_range;
    std::optional<RealRange> real_range;
    std::vector<ConfigValue> allowed_values;
    // String: UTF-8 bytes; Binary: bytes; List: number of elements. Never truncates.
    std::optional<std::size_t> max_length;
};


struct ParameterDescriptor {
    ParameterId id;
    std::string display_name;
    DataType type;
    bool required{true};
    std::optional<ConfigValue> default_value;
    ValueConstraints constraints;
    BindingPolicy binding;
    ValueSemantics semantics;
    bool subscribable{false};
    bool structural{false};
};

struct InputDescriptor {
    InputId id;
    std::string display_name;
    DataType type;
    bool required{true};
    std::optional<ConfigValue> default_value;
    ValueConstraints constraints;
    BindingPolicy binding;
    ValueSemantics semantics;
};

// Direction and data/control identity are encoded by these distinct structures.
struct OutputDescriptor {
    OutputId id;
    std::string display_name;
    DataType type;
    ValueSemantics semantics;
};

struct ControlOutputDescriptor { ControlOutputId id; std::string display_name; };

struct ToolSchema {
    SchemaId id;
    std::uint32_t version;
    std::vector<CapabilityId> required_capabilities;
    bool complete{true};
};

enum class ToolExecutionMode { Synchronous, Asynchronous };
enum class ToolInstanceLifetime { Invocation, WorkflowRun, SolutionSession };
enum class ToolReentrancy { Serialized, Reentrant };
enum class ToolEffects { None, State, External };
enum class ToolCancellation { Unsupported, Cooperative };
enum class ToolRetry { Never, Idempotent };
struct ToolExecutionPolicy {
    ToolExecutionMode mode{ToolExecutionMode::Synchronous};
    ToolInstanceLifetime lifetime{ToolInstanceLifetime::Invocation};
    ToolReentrancy reentrancy{ToolReentrancy::Serialized};
    ToolEffects effects{ToolEffects::None};
    ToolCancellation cancellation{ToolCancellation::Unsupported};
    ToolRetry retry{ToolRetry::Never};
    bool operator==(const ToolExecutionPolicy&) const = default;
};

struct ToolDescriptor {
    ToolTypeId id;
    std::string display_name;
    std::uint32_t config_version;
    ToolSchema schema;
    std::vector<ParameterDescriptor> parameters;
    std::vector<InputDescriptor> inputs;
    std::vector<OutputDescriptor> outputs;
    std::vector<ControlOutputDescriptor> control_outputs;
    std::optional<NodeStateDefinition> state;
    std::vector<ResourceRequirement> resources;
    std::optional<RegionCapabilities> region_capabilities;
    std::vector<PixelFormat> image_formats;
    ToolExecutionPolicy execution;
};

struct SchemaSupport {
    SchemaId id;
    std::uint32_t minimum_version;
    std::uint32_t maximum_version;
    std::vector<CapabilityId> capabilities;
};

[[nodiscard]] Result<std::monostate> validate_descriptor(const ToolDescriptor& descriptor);
[[nodiscard]] Result<std::monostate> validate_input_descriptor(const InputDescriptor& descriptor);
[[nodiscard]] Result<std::monostate> validate_output_descriptor(const OutputDescriptor& descriptor);
[[nodiscard]] Result<std::monostate> validate_image_support(const ToolDescriptor& descriptor, const ImageHandle& image);
[[nodiscard]] Result<std::monostate> validate_schema_support(
    const ToolDescriptor& descriptor, const SchemaSupport& support);

[[nodiscard]] Result<ConfigValue> validate_parameter_value(
    const ParameterDescriptor& descriptor, const std::optional<ConfigValue>& configured);
[[nodiscard]] Result<ConfigValue> validate_input_value(
    const InputDescriptor& descriptor, const std::optional<ConfigValue>& configured);

// nullopt means an UNCONFIGURED source, not Pending or a missing runtime result.
// Only an unconfigured source may use a declared default. A failed source never does.
[[nodiscard]] Result<ConfigValue> resolve_parameter_value(
    const ParameterDescriptor& descriptor, const std::optional<Result<ConfigValue>>& source);
[[nodiscard]] Result<ConfigValue> resolve_input_value(
    const InputDescriptor& descriptor, const std::optional<Result<ConfigValue>>& source);

// Metadata checks only. MultipleSources checks one source against the list element;
// scheduling, aggregation, runtime resource resolution and conversion execution are later work.
[[nodiscard]] Result<std::monostate> validate_binding(const ParameterDescriptor& target,
    BindingSourceKind source, const DataType& source_type, ConversionKind conversion);
[[nodiscard]] Result<std::monostate> validate_binding(const InputDescriptor& target,
    BindingSourceKind source, const DataType& source_type, ConversionKind conversion);

} // namespace visionpro::core
