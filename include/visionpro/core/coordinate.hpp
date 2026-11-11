#pragma once

#include <visionpro/core/result.hpp>

#include <optional>

namespace visionpro::core {

enum class MeasurementUnit { Unspecified, Dimensionless, Pixel, Millimetre, Second, Radian, Degree };
struct ValueSemantics {
    MeasurementUnit unit{MeasurementUnit::Unspecified};
    std::optional<CoordinateSystemId> coordinate_system;
    bool operator==(const ValueSemantics&) const = default;
};

struct Vector2D {
    double x;
    double y;
    bool operator==(const Vector2D&) const = default;
};
enum class CoordinateKind { Image, Physical, Local };
enum class Handedness { RightHanded, LeftHanded };

// Axes are unit vectors in a declared reference convention: right is +X, up is +Y.
// Image coordinates therefore use (1,0), (0,-1), left-handed, pixel-center origin.
struct CoordinateFrameDefinition {
    CoordinateSystemId id;
    CoordinateKind kind;
    MeasurementUnit unit;
    Vector2D origin;
    Vector2D x_axis;
    Vector2D y_axis;
    Handedness handedness;
    bool operator==(const CoordinateFrameDefinition&) const = default;
};

[[nodiscard]] Result<std::monostate> validate_coordinate_frame(const CoordinateFrameDefinition& frame);

} // namespace visionpro::core
