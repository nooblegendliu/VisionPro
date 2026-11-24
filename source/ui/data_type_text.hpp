#pragma once

#include <visionpro/core/value.hpp>
#include <QString>

namespace visionpro::ui {

inline QString type_text(const core::DataType& type)
{
    using core::TypeKind;
    switch (type.kind()) {
    case TypeKind::Bool: return QStringLiteral("Bool");
    case TypeKind::Int64: return QStringLiteral("Int64");
    case TypeKind::Float64: return QStringLiteral("Float64");
    case TypeKind::String: return QStringLiteral("String");
    case TypeKind::Binary: return QStringLiteral("Binary");
    case TypeKind::Enum:
    case TypeKind::Record: return type.schema_id() ? QString::fromUtf8(type.schema_id()->text()) : QStringLiteral("Schema");
    case TypeKind::List: return QStringLiteral("List<%1>").arg(type_text(*type.element_type()));
    case TypeKind::Optional: return QStringLiteral("Optional<%1>").arg(type_text(*type.element_type()));
    case TypeKind::Image: return QStringLiteral("Image");
    case TypeKind::Point2D: return QStringLiteral("Point2D");
    case TypeKind::Line2D: return QStringLiteral("Line2D");
    case TypeKind::Circle2D: return QStringLiteral("Circle2D");
    case TypeKind::Pose2D: return QStringLiteral("Pose2D");
    case TypeKind::Transform2D: return QStringLiteral("Transform2D");
    case TypeKind::Region2D: return QStringLiteral("Region2D");
    case TypeKind::RasterMask: return QStringLiteral("RasterMask");
    case TypeKind::ResourceReference: return QStringLiteral("Resource reference");
    }
    return {};
}

} // namespace visionpro::ui
