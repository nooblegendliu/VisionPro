#pragma once

#include <visionpro/core/schema_id.hpp>

#include <chrono>
#include <optional>
#include <span>

namespace visionpro::core {

enum class TimingScope { QueueWait, InvocationWallClock, ToolModule, ToolAlgorithm, ResultMaterialization, Display };
enum class TimingSource { RuntimeClock, ToolReported, AdapterReported, DisplayClock };
enum class TimingUnit { Unknown, Nanoseconds, Microseconds, Milliseconds, Seconds };

struct TimingMeasurement {
    TimingScope scope;
    TimingSource source;
    TimingUnit unit;
    // Missing means unmeasured. A reported number may still have an unknown unit.
    std::optional<double> value{};
    std::optional<SchemaId> provider{};
};

// elapsed holds the native invocation wall clock in exact integer nanoseconds.
// Other sources may report their own invocation duration as separate measurements.
[[nodiscard]] bool valid_execution_timing(std::optional<std::chrono::nanoseconds> elapsed,
    std::span<const TimingMeasurement> measurements);

} // namespace visionpro::core
