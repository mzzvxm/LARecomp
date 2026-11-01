// A dead zone for each stick, for a controller with stick drift.
//
// The game reads a pad through sub_8275CB78 (XamInputGetState); wheel_fake.cpp hooks it,
// and calls this for a pad that is not a wheel. A stick is taken as a point: inside the
// circle it reads as centred, and outside it the distance from the circle's edge is
// stretched back to the full range, so the stick moves on from 0 without a jump and still
// reaches full lock. 0, the default, leaves the values as they are. Everything that reads
// the game's pad follows this; the menus' own stick reads have their own thresholds.
//
// The chase camera already ignores the right stick up to about 0.3 of its range (seen by
// holding it at fixed values, parked), and these dead zones come on top of that: a small
// one is enough for a stick that drifts past it.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"

REXCVAR_DEFINE_DOUBLE(left_stick_deadzone, 0.0, "MCLA/Input",
    "Dead zone of the left stick, in percent of its range: a stick that rests off centre (drift) "
    "reads as centred inside it. 0 = none.")
    .range(0.0, 50.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(right_stick_deadzone, 0.0, "MCLA/Input",
    "Dead zone of the right stick, in percent of its range: a stick that rests off centre (drift) "
    "reads as centred inside it. 0 = none.")
    .range(0.0, 50.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// X_INPUT_STATE in guest memory: big-endian, the sticks from offset 8 as lx, ly, rx, ry.
constexpr uint32_t kLeftStick = 8;
constexpr uint32_t kRightStick = 12;

int16_t ReadAxis(const uint8_t* p) {
    return int16_t(uint16_t((p[0] << 8) | p[1]));
}

void WriteAxis(uint8_t* p, int16_t v) {
    p[0] = uint8_t(uint16_t(v) >> 8);
    p[1] = uint8_t(uint16_t(v));
}

int16_t ToAxis(double v) {
    return int16_t(std::clamp(std::lround(v * 32767.0), -32768L, 32767L));
}

void ApplyToStick(uint8_t* stick, double percent) {
    if (!(percent > 0.0)) return;
    const double zone = std::min(percent, 50.0) / 100.0;
    const double x = ReadAxis(stick) / 32767.0;
    const double y = ReadAxis(stick + 2) / 32767.0;
    const double length = std::sqrt(x * x + y * y);
    if (length <= zone) {
        WriteAxis(stick, 0);
        WriteAxis(stick + 2, 0);
        return;
    }
    const double scale = (length - zone) / (1.0 - zone) / length;
    WriteAxis(stick, ToAxis(x * scale));
    WriteAxis(stick + 2, ToAxis(y * scale));
}

}  // namespace

void ApplyStickDeadzones(uint8_t* base, uint32_t state) {
    ApplyToStick(base + state + kLeftStick, REXCVAR_GET(left_stick_deadzone));
    ApplyToStick(base + state + kRightStick, REXCVAR_GET(right_stick_deadzone));
}
#endif  // REXGLUE_HAS_XEO3_TARGET
