#pragma once

#include <visionpro/core/id.hpp>
#include <visionpro/core/schema_id.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <cstddef>
#include <variant>
#include <vector>

namespace visionpro::core {

// Explicit values are stable; new codes must not renumber existing ones.
enum class ErrorCode : std::uint32_t {
    InvalidIdFormat = 1001,
    InvalidIdVersion = 1002,
    InvalidIdVariant = 1003,
    InvalidSchemaId = 1004,
    InvalidRuntimeId = 1005,
    IdentityExhausted = 1006,
    InvalidCorrelationId = 1007,
    InvalidSchema = 1101,
    DuplicateSchemaMember = 1102,
    InvalidUtf8 = 1103,
    TypeMismatch = 1104,
    InvalidValue = 1105,
    MissingField = 1106,
    UnknownField = 1107,
    InvalidEnumValue = 1108,
    InvalidDescriptor = 1201,
    MissingRequiredValue = 1202,
    ConstraintViolation = 1203,
    DisallowedBindingSource = 1204,
    UnsupportedConversion = 1205,
    IncompatibleSchemaVersion = 1206,
    MissingCapability = 1207,
    IncompleteSchema = 1208,
    UnknownTarget = 1209,
    InvalidImage = 1301,
    UnsupportedPixelFormat = 1302,
    ImageBounds = 1303,
    SizeOverflow = 1304,
    InvalidCoordinate = 1401,
    InvalidGeometry = 1402,
    InvalidTransform = 1403,
    CoordinateMismatch = 1404,
    UnsupportedRegion = 1405,
    EmptyRegion = 1406,
    InvalidMask = 1407,
    InvalidResource = 1501,
    ResourceNotFound = 1502,
    ResourceKindMismatch = 1503,
    ResourceVersionMismatch = 1504,
    InvalidPath = 1505,
    InvalidState = 1601,
    InvalidQueue = 1602,
    InvalidGlobalVariable = 1603,
    InvalidModel = 1701,
    DuplicateIdentity = 1702,
    InvalidContainment = 1703,
    InvalidBinding = 1704,
    InvalidSnapshot = 1705,
    InvalidRunRequest = 1706,
    InvalidNodeResult = 1707,
    ExternalReference = 1708,
    UnknownTool = 1709,
    InvalidControlLink = 1710,
    InvalidSelector = 1711,
    ScopeViolation = 1712,
    ControlCycle = 1713,
    DependencyCycle = 1714,
    ControlOrderConflict = 1715,
    IncompleteExecutionScope = 1716,
    ResultIdentityMismatch = 1801,
    DuplicateResult = 1802,
    ResultRetired = 1803,
    InvalidResolutionContext = 1804,
    SourceFailed = 1805,
    SourceCancelled = 1806,
    SourceTimedOut = 1807,
    InputUnavailable = 1808,
    IndexOutOfRange = 1809,
    GlobalUnavailable = 1810,
    GlobalRevisionMismatch = 1811,
    InvalidRunHandle = 1901,
    ExecutionBoundaryFailure = 1902,
    IdGenerationFailed = 2001,
};

enum class ErrorCategory { Unknown, Structure, Parameter, Resource, Runtime };
enum class Severity { Info, Warning, Error };

[[nodiscard]] ErrorCategory error_category(ErrorCode code) noexcept;
[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

struct DiagnosticElementIndex {
    std::size_t value;
    bool operator==(const DiagnosticElementIndex&) const = default;
};
using DiagnosticPathStep = std::variant<FieldId, DiagnosticElementIndex>;

struct DiagnosticLocation {
    std::optional<SolutionId> solution;
    std::optional<WorkflowId> workflow;
    std::optional<NodeId> node;
    std::optional<ToolTypeId> tool;
    std::optional<SchemaId> schema;
    std::optional<InputId> input;
    std::optional<OutputId> output;
    std::optional<ParameterId> parameter;
    std::vector<DiagnosticPathStep> path;
    std::optional<ResourceId> resource;
    std::optional<VariableId> variable;
    std::optional<QueueId> queue;
    std::optional<RegionId> region;
    std::optional<ControlLinkId> control_link;
};

enum class ExternalStatusDomain { Unknown, Call, Algorithm, Output, Resource, Judgement };
enum class ExternalCodeMapping { Unmapped, Mapped };

struct ExternalDiagnostic {
    SchemaId provider;
    std::string operation;
    ExternalStatusDomain domain;
    std::int64_t raw_code;
    ExternalCodeMapping mapping{ExternalCodeMapping::Unmapped};
    std::optional<SchemaId> provider_domain{};
    std::optional<std::string> description{};
    std::optional<std::string> backend_version{};
};

struct Diagnostic {
    ErrorCode code;
    Severity severity;
    DiagnosticLocation location;
    std::string message;
    std::optional<ExternalDiagnostic> external{};

    [[nodiscard]] ErrorCategory category() const noexcept { return error_category(code); }
};

[[nodiscard]] bool is_valid_diagnostic(const Diagnostic& diagnostic) noexcept;

} // namespace visionpro::core
