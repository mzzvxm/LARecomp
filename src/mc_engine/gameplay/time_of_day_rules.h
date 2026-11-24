#pragma once

namespace larecomp::tod {

// Sample the winning source once per update. A negative clock hour means the
// real clock is disabled. The lower-priority sources are read only if needed.
template <typename ReadClock, typename ReadHeldHour>
constexpr float ResolvePin(int scene_milli, int shot_milli, bool hold,
                           ReadClock read_clock, ReadHeldHour read_held_hour) {
    const int requested = scene_milli >= 0 ? scene_milli : shot_milli;
    float hour;
    if (requested >= 0) {
        hour = static_cast<float>(requested) / 1000.0f;
    } else {
        hour = read_clock();
        if (hour < 0.0f) {
            if (!hold) return -1.0f;
            hour = read_held_hour();
        }
    }

    while (hour >= 24.0f) hour -= 24.0f;
    return hour < 0.0f ? 0.0f : hour;
}

}  // namespace larecomp::tod
