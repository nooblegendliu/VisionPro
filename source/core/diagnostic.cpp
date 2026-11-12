#include <visionpro/core/diagnostic.hpp>
#include <visionpro/core/text.hpp>

namespace visionpro::core {

bool is_valid_diagnostic(const Diagnostic& diagnostic) noexcept
{
    const auto valid_text = [](std::string_view value) { return !value.empty() && is_valid_utf8(value); };
    if (diagnostic.severity < Severity::Info || diagnostic.severity > Severity::Error || !valid_text(diagnostic.message)) {
        return false;
    }
    if (!diagnostic.external) { return true; }
    const auto& external = *diagnostic.external;
    return external.domain >= ExternalStatusDomain::Unknown && external.domain <= ExternalStatusDomain::Judgement
        && (external.mapping == ExternalCodeMapping::Unmapped
            || (external.mapping == ExternalCodeMapping::Mapped && external.domain != ExternalStatusDomain::Unknown))
        && valid_text(external.operation)
        && (!external.description || valid_text(*external.description))
        && (!external.backend_version || valid_text(*external.backend_version));
}

ErrorCategory error_category(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::InvalidIdFormat:
    case ErrorCode::InvalidIdVersion:
    case ErrorCode::InvalidIdVariant:
    case ErrorCode::InvalidSchemaId:
    case ErrorCode::InvalidRuntimeId:
    case ErrorCode::InvalidCorrelationId:
    case ErrorCode::InvalidSchema:
    case ErrorCode::DuplicateSchemaMember:
    case ErrorCode::InvalidDescriptor:
    case ErrorCode::IncompatibleSchemaVersion:
    case ErrorCode::MissingCapability:
    case ErrorCode::IncompleteSchema:
    case ErrorCode::UnknownTarget:
    case ErrorCode::InvalidCoordinate:
    case ErrorCode::InvalidState:
    case ErrorCode::InvalidQueue:
    case ErrorCode::InvalidGlobalVariable:
    case ErrorCode::InvalidModel:
    case ErrorCode::DuplicateIdentity:
    case ErrorCode::InvalidContainment:
    case ErrorCode::InvalidBinding:
    case ErrorCode::InvalidSnapshot:
    case ErrorCode::ExternalReference:
    case ErrorCode::UnknownTool:
    case ErrorCode::InvalidControlLink:
    case ErrorCode::InvalidSelector:
    case ErrorCode::ScopeViolation:
    case ErrorCode::ControlCycle:
    case ErrorCode::DependencyCycle:
    case ErrorCode::ControlOrderConflict:
    case ErrorCode::IncompleteExecutionScope:
    case ErrorCode::InvalidSolutionManifest:
    case ErrorCode::UnsupportedSolutionFormat:
    case ErrorCode::UnsupportedSolutionContent:
        return ErrorCategory::Structure;
    case ErrorCode::IdentityExhausted:
    case ErrorCode::InvalidNodeResult:
    case ErrorCode::ResultIdentityMismatch:
    case ErrorCode::DuplicateResult:
    case ErrorCode::ResultRetired:
    case ErrorCode::InvalidResolutionContext:
    case ErrorCode::SourceFailed:
    case ErrorCode::SourceCancelled:
    case ErrorCode::SourceTimedOut:
    case ErrorCode::InputUnavailable:
    case ErrorCode::IndexOutOfRange:
    case ErrorCode::GlobalUnavailable:
    case ErrorCode::GlobalRevisionMismatch:
    case ErrorCode::InvalidRunHandle:
    case ErrorCode::ExecutionBoundaryFailure:
        return ErrorCategory::Runtime;
    case ErrorCode::InvalidUtf8:
    case ErrorCode::TypeMismatch:
    case ErrorCode::InvalidValue:
    case ErrorCode::MissingField:
    case ErrorCode::UnknownField:
    case ErrorCode::InvalidEnumValue:
    case ErrorCode::MissingRequiredValue:
    case ErrorCode::ConstraintViolation:
    case ErrorCode::DisallowedBindingSource:
    case ErrorCode::UnsupportedConversion:
    case ErrorCode::InvalidImage:
    case ErrorCode::UnsupportedPixelFormat:
    case ErrorCode::ImageBounds:
    case ErrorCode::SizeOverflow:
    case ErrorCode::InvalidGeometry:
    case ErrorCode::InvalidTransform:
    case ErrorCode::CoordinateMismatch:
    case ErrorCode::UnsupportedRegion:
    case ErrorCode::EmptyRegion:
    case ErrorCode::InvalidMask:
    case ErrorCode::InvalidRunRequest:
        return ErrorCategory::Parameter;
    case ErrorCode::IdGenerationFailed:
    case ErrorCode::InvalidResource:
    case ErrorCode::ResourceNotFound:
    case ErrorCode::ResourceKindMismatch:
    case ErrorCode::ResourceVersionMismatch:
    case ErrorCode::InvalidPath:
    case ErrorCode::SolutionIoFailure:
    case ErrorCode::SolutionAlreadyExists:
        return ErrorCategory::Resource;
    }
    return ErrorCategory::Unknown;
}

std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::InvalidIdFormat: return "InvalidIdFormat";
    case ErrorCode::InvalidIdVersion: return "InvalidIdVersion";
    case ErrorCode::InvalidIdVariant: return "InvalidIdVariant";
    case ErrorCode::InvalidSchemaId: return "InvalidSchemaId";
    case ErrorCode::InvalidRuntimeId: return "InvalidRuntimeId";
    case ErrorCode::IdentityExhausted: return "IdentityExhausted";
    case ErrorCode::InvalidCorrelationId: return "InvalidCorrelationId";
    case ErrorCode::InvalidSchema: return "InvalidSchema";
    case ErrorCode::DuplicateSchemaMember: return "DuplicateSchemaMember";
    case ErrorCode::InvalidUtf8: return "InvalidUtf8";
    case ErrorCode::TypeMismatch: return "TypeMismatch";
    case ErrorCode::InvalidValue: return "InvalidValue";
    case ErrorCode::MissingField: return "MissingField";
    case ErrorCode::UnknownField: return "UnknownField";
    case ErrorCode::InvalidEnumValue: return "InvalidEnumValue";
    case ErrorCode::InvalidDescriptor: return "InvalidDescriptor";
    case ErrorCode::MissingRequiredValue: return "MissingRequiredValue";
    case ErrorCode::ConstraintViolation: return "ConstraintViolation";
    case ErrorCode::DisallowedBindingSource: return "DisallowedBindingSource";
    case ErrorCode::UnsupportedConversion: return "UnsupportedConversion";
    case ErrorCode::IncompatibleSchemaVersion: return "IncompatibleSchemaVersion";
    case ErrorCode::MissingCapability: return "MissingCapability";
    case ErrorCode::IncompleteSchema: return "IncompleteSchema";
    case ErrorCode::UnknownTarget: return "UnknownTarget";
    case ErrorCode::InvalidImage: return "InvalidImage";
    case ErrorCode::UnsupportedPixelFormat: return "UnsupportedPixelFormat";
    case ErrorCode::ImageBounds: return "ImageBounds";
    case ErrorCode::SizeOverflow: return "SizeOverflow";
    case ErrorCode::InvalidCoordinate: return "InvalidCoordinate";
    case ErrorCode::InvalidGeometry: return "InvalidGeometry";
    case ErrorCode::InvalidTransform: return "InvalidTransform";
    case ErrorCode::CoordinateMismatch: return "CoordinateMismatch";
    case ErrorCode::UnsupportedRegion: return "UnsupportedRegion";
    case ErrorCode::EmptyRegion: return "EmptyRegion";
    case ErrorCode::InvalidMask: return "InvalidMask";
    case ErrorCode::InvalidResource: return "InvalidResource";
    case ErrorCode::ResourceNotFound: return "ResourceNotFound";
    case ErrorCode::ResourceKindMismatch: return "ResourceKindMismatch";
    case ErrorCode::ResourceVersionMismatch: return "ResourceVersionMismatch";
    case ErrorCode::InvalidPath: return "InvalidPath";
    case ErrorCode::InvalidState: return "InvalidState";
    case ErrorCode::InvalidQueue: return "InvalidQueue";
    case ErrorCode::InvalidGlobalVariable: return "InvalidGlobalVariable";
    case ErrorCode::InvalidModel: return "InvalidModel";
    case ErrorCode::DuplicateIdentity: return "DuplicateIdentity";
    case ErrorCode::InvalidContainment: return "InvalidContainment";
    case ErrorCode::InvalidBinding: return "InvalidBinding";
    case ErrorCode::InvalidSnapshot: return "InvalidSnapshot";
    case ErrorCode::InvalidRunRequest: return "InvalidRunRequest";
    case ErrorCode::InvalidNodeResult: return "InvalidNodeResult";
    case ErrorCode::ExternalReference: return "ExternalReference";
    case ErrorCode::UnknownTool: return "UnknownTool";
    case ErrorCode::InvalidControlLink: return "InvalidControlLink";
    case ErrorCode::InvalidSelector: return "InvalidSelector";
    case ErrorCode::ScopeViolation: return "ScopeViolation";
    case ErrorCode::ControlCycle: return "ControlCycle";
    case ErrorCode::DependencyCycle: return "DependencyCycle";
    case ErrorCode::ControlOrderConflict: return "ControlOrderConflict";
    case ErrorCode::IncompleteExecutionScope: return "IncompleteExecutionScope";
    case ErrorCode::ResultIdentityMismatch: return "ResultIdentityMismatch";
    case ErrorCode::DuplicateResult: return "DuplicateResult";
    case ErrorCode::ResultRetired: return "ResultRetired";
    case ErrorCode::InvalidResolutionContext: return "InvalidResolutionContext";
    case ErrorCode::SourceFailed: return "SourceFailed";
    case ErrorCode::SourceCancelled: return "SourceCancelled";
    case ErrorCode::SourceTimedOut: return "SourceTimedOut";
    case ErrorCode::InputUnavailable: return "InputUnavailable";
    case ErrorCode::IndexOutOfRange: return "IndexOutOfRange";
    case ErrorCode::GlobalUnavailable: return "GlobalUnavailable";
    case ErrorCode::GlobalRevisionMismatch: return "GlobalRevisionMismatch";
    case ErrorCode::InvalidRunHandle: return "InvalidRunHandle";
    case ErrorCode::ExecutionBoundaryFailure: return "ExecutionBoundaryFailure";
    case ErrorCode::IdGenerationFailed: return "IdGenerationFailed";
    case ErrorCode::SolutionIoFailure: return "SolutionIoFailure";
    case ErrorCode::InvalidSolutionManifest: return "InvalidSolutionManifest";
    case ErrorCode::UnsupportedSolutionFormat: return "UnsupportedSolutionFormat";
    case ErrorCode::UnsupportedSolutionContent: return "UnsupportedSolutionContent";
    case ErrorCode::SolutionAlreadyExists: return "SolutionAlreadyExists";
    }
    return "UnknownErrorCode";
}

} // namespace visionpro::core
