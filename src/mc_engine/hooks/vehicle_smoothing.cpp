// Motorcycle gyro above 30 FPS: mcBikeGyro::Update (sub_823688B8).
//
// sub_823688B8 is slot 2 of the mcBikeGyro vtable at 0x820488C0 (RTTI:
// mcBikeGyro : mcCarGyroBase : rage::vehGyro : rage::datBase). It only runs
// on a motorcycle; a car runs mcCarGyro::Update (0x823663D0) instead, which
// steps with the engine's dt and 1/dt and needs no fix.
//
// The bike gyro keeps a smoothed position at [this+64] and a smoothed velocity
// at [this+80], each stepped once per call with a fixed lerp factor:
//
//   82368940  cmpwi cr6, r11, 60       ; r11 = round(1/dt), the frame rate
//   8236894C  lfs   f16, 0xE94(r10)    ; 2.0
//   82368950  lfs   f13, 0x6740(r11)   ; position factor, 0.40
//   82368954  bge   cr6, loc_8236895C  ; >= 60 fps? leave it alone
//   82368958  fmuls f13, f13, f16      ; else 0.80
//   8236895C  <-- MCLABikeGyroPosSmoothing
//   ...
//   823689E8  bge   cr6, loc_823689F0  ; same test, velocity factor 0x6744:
//   823689EC  fmuls f0, f0, f16        ; 0.15 at >= 60 fps, 0.30 below
//   823689F0  <-- MCLABikeGyroVelSmoothing
//
// So the filter is tuned for exactly two rates, 30 and 60. At 120 or 144 FPS
// it takes the 60 FPS factor per call, two to almost five times as often as at
// 60, and the bike settles correspondingly faster. Same treatment as the chase
// camera: take the factor the engine uses below 60 as the 30 FPS console
// reference and step it in continuous time,
//
//   k(dt) = 1 - (1 - k30) ^ (30 * dt)
//
// which reproduces 30 FPS exactly and is rate-invariant everywhere else. The
// velocity is the change in smoothed position times 1/dt, so it follows once
// the position does.
//
// Not measured in game yet: whether this runs once per frame or once per
// physics substep. It differentiates with the frame-level 1/dt at [0x827D750C],
// which is only right once per frame, and mcCarGyro::Update does the same.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <cmath>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"

REXCVAR_DEFINE_BOOL(smooth_bike_gyro, true, "MCLA/Physics",
    "Fix: Step the motorcycle gyro's position and velocity filters in continuous time, so bikes "
    "lean and settle the same at any frame rate.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kBikeGyroUnder60Scale = 0x82040E94;  // 2.0

void ApplyBikeGyroSmoothing(PPCRegister& reg) {
    if (!REXCVAR_GET(smooth_bike_gyro)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    const double k = reg.f64;
    if (k <= 0.0 || k >= 1.0 || dt <= 0.0f) return;

    // Undo nothing the engine did not do: below 60 it has already scaled the
    // factor up to the 30 FPS value. fctiwz after the +/- 0.5 bias rounds half
    // away from zero.
    const float fps = ReadGuestF32(base, kGuestFrameRate);
    const int fps_i = static_cast<int>(fps >= 0.0f ? fps + 0.5f : fps - 0.5f);
    const double k30 = fps_i < 60 ? k : k * ReadGuestF32(base, kBikeGyroUnder60Scale);
    if (k30 <= 0.0 || k30 >= 1.0) return;

    reg.f64 = 1.0 - std::pow(1.0 - k30, static_cast<double>(dt) * 30.0);
}

}  // namespace

void MCLABikeGyroPosSmoothing(PPCRegister& f13) {
    ApplyBikeGyroSmoothing(f13);
}

void MCLABikeGyroVelSmoothing(PPCRegister& f0) {
    ApplyBikeGyroSmoothing(f0);
}
#else  // REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>

void MCLABikeGyroPosSmoothing(PPCRegister& f13) {}
void MCLABikeGyroVelSmoothing(PPCRegister& f0) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
