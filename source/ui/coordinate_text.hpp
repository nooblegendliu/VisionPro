#pragma once

#include <visionpro/core/coordinate.hpp>
#include <QString>

namespace visionpro::ui {

inline QString unit_symbol(core::MeasurementUnit unit)
{
    using enum core::MeasurementUnit;
    switch (unit) {
    case Unspecified: return QStringLiteral("unspecified");
    case Dimensionless: return QStringLiteral("dimensionless");
    case Pixel: return QStringLiteral("px");
    case Millimetre: return QStringLiteral("mm");
    case Second: return QStringLiteral("s");
    case Radian: return QStringLiteral("rad");
    case Degree: return QStringLiteral("°");
    }
    return QStringLiteral("unknown");
}

} // namespace visionpro::ui
