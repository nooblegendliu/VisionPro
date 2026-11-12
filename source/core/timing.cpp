#include <visionpro/core/timing.hpp>

#include <cmath>
#include <set>
#include <tuple>

namespace visionpro::core {

bool valid_execution_timing(std::optional<std::chrono::nanoseconds> elapsed,
    std::span<const TimingMeasurement> measurements)
{
    if (elapsed && elapsed->count() < 0) { return false; }
    std::set<std::tuple<TimingScope, TimingSource, std::optional<SchemaId>>> scopes;
    for (const auto& measurement : measurements) {
        if (measurement.scope < TimingScope::QueueWait || measurement.scope > TimingScope::Display
            || measurement.source < TimingSource::RuntimeClock || measurement.source > TimingSource::DisplayClock
            || measurement.unit < TimingUnit::Unknown || measurement.unit > TimingUnit::Seconds
            || (measurement.value && (!std::isfinite(*measurement.value) || *measurement.value < 0))
            || (measurement.scope == TimingScope::InvocationWallClock
                && measurement.source == TimingSource::RuntimeClock)
            || !scopes.emplace(measurement.scope, measurement.source, measurement.provider).second) {
            return false;
        }
    }
    return true;
}

} // namespace visionpro::core
