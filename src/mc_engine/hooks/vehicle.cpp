// Vehicles: the shop's wheel-fit limits, pairwise collision, the chassis depth
// filter above 30 FPS, the Audi R8 DLC flags, and traffic (va_) cars driven as
// player cars.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(break_pairwise_collision, false, "MCLA/Patches", "Disables pairwise collision resolution.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(unlock_ride_height, false, "MCLA/Patches", "Allow the full stock ride-height table in the shop (down to rh_800 / -8).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(unlock_wheel_fit, false, "MCLA/Patches", "Allow every stock rim size, tire profile, tire width and ride height regardless of the car's clearance metrics.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Vehicle chassis suspension damping & ground depth continuous filter
REXCVAR_DEFINE_BOOL(smooth_chassis_depth, true, "MCLA/Physics",
    "Fix: Smooth vehicle chassis suspension and ground depth damping at 60 FPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// BadassBaboon's Recomp Adjustments: Continuous-time vehicle physics filters at 60+ FPS
REXCVAR_DEFINE_BOOL(smooth_vehicle_physics, true, "MCLA/Physics",
    "Fix: Continuous-time vehicle physics filters (angular velocity, rotational G-forces, tire slip) at 60+ FPS.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Traffic (va_) vehicles turned into player cars: chassis-bound substitution.
//
// A vp_ vehicle's physics bound is a phBoundComposite (type 12) whose five children are
// the chassis phBoundGeometry plus the four wheels. A va_ vehicle's bound is a bare
// phBoundGeometry (type 4) with no composite around it -- the split is total: none of
// the 41 va_ cars has a composite, all 65 vp_/vpd_ cars do. The bound lives in the
// car's .xtl/.xtp, not the .xct.
//
// The player-vehicle code reads *(root + 0x80) as the composite's child array without
// checking the type. On a geometry, +0x80 is m_Vertices, so the first float4 of vertex
// data is used as a phBound*; the resulting garbage object reports a polygon count > 0
// with a NULL polygon array and sub_8259FF88 faults at 0x8.
//
// Substituting the root itself when it is not a composite makes the deform pass work on
// the traffic car's own 16-vertex hull. It is a no-op for every shipped player car,
// which is why it is unconditional rather than cvar-gated.
static void SubstituteChassisBound(uint32_t root, PPCRegister& child) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base || root == 0) return;
    if (base[root + 4] != 12) child.u64 = root;
}

// 0x8232D048 in sub_8232CFF0, before `addi r6, r11, 0x10`. r9 holds root + 0x80.
void MCLA_TrafficChassisBound_8232D048(PPCRegister& r9, PPCRegister& r11) {
    SubstituteChassisBound(static_cast<uint32_t>(r9.u64) - 0x80u, r11);
}

// 0x8232D900 in sub_8232D8C8 (hull-index search), after `lwz r11, 0(r11)`. r3 is
// still the root bound returned by sub_8255B9A8.
void MCLA_TrafficChassisBound_8232D900(PPCRegister& r3, PPCRegister& r11) {
    SubstituteChassisBound(static_cast<uint32_t>(r3.u64), r11);
}

// 0x8232E274 in sub_8232E238 (suspension deform), after `lwz r31, 0(r10)`. r3 is
// still the root bound. This is the site that actually crashed.
void MCLA_TrafficChassisBound_8232E274(PPCRegister& r3, PPCRegister& r31) {
    SubstituteChassisBound(static_cast<uint32_t>(r3.u64), r31);
}

// 0x8259AA40, entry of sub_8259AA28 = phBoundComposite::ReleaseChildren, before
// `lhz r11, 0x92(r30)`. mcCarSim's destructor (sub_8232CDA8) calls it on the bound
// unconditionally, and sub_8232CEB0 does the same before re-attaching one. On a
// traffic car's bare geometry, +146 lands in the middle of the quantum-offset float
// and +128 is m_Vertices, so it walks vertex data as a child pointer array. Returning
// true jumps to the epilogue at 0x8259AA90, which is exactly right: a non-composite
// has no children to release.
bool MCLA_TrafficBoundRelease_8259AA40(PPCRegister& r30) {
    const auto* base = rex::Runtime::instance()->virtual_membase();
    const auto bound = static_cast<uint32_t>(r30.u64);
    if (!base) return false;
    return bound == 0 || base[bound + 4] != 12;
}

// Returns 'true' to inject a 'blr' (return from collision function)
bool Patch_PhysicsCollision() {
    return REXCVAR_GET(break_pairwise_collision);
}

// Ride height range. sub_82392F68 is the wheel-fit validator: with
//   f31 = TireRadius, f30 = RideHeight (negative = lowered)
// it rejects a setup when f31 + f30 < AxleToFloorboards ("ride too low
// (grinding floor)", result 1) or f31 - f30 > AxleToWheelwell ("tire too big
// (hitting wheel well)", result 2). Both AxleTo* values are per-car floats, so
// every car stops lowering at a different notch — typically -2 (rh_200), even
// though the stock table at off_820511C4/unk_820511F4 runs all the way to
// rh_800 (-0.15 m, i.e. -8) and the stepper sub_8269EED8 already clamps to
// 0..11. Zeroing f30 right after it is loaded (0x82392FF0) takes ride height
// out of both comparisons while leaving the rim/tire size checks - which only
// depend on f31 - exactly as shipped.
void Patch_RideHeightFit(PPCRegister& f30) {
    if (REXCVAR_GET(unlock_ride_height)) f30.f64 = 0.0;
}

// Wheel sizing range. The same validator gates all four wheel mods: the shop
// writes the picked byte (rim +2046, profile +2048, width +2044, ride +2042),
// calls sub_8269EFE0, and on a bad fit puts all four bytes back - which is why a
// car refuses larger rims or fatter tires long before the stock tables run out
// (RimSize 12..28, TireProfile 0..13, TireWidth 0..16, RideHeight 0..11). The
// three comparisons in sub_82392F68 start at 0x82392FFC; jumping straight to
// the "fits" tail at 0x823930FC reports success for every combination, so the
// menus expose their full stock lists. Cosmetic only - no geometry is created,
// the tires just clip the arches at the extremes.
bool Patch_WheelFitBypass() {
    return REXCVAR_GET(unlock_wheel_fit);
}

void Patch_BypassVehicleDLC(PPCRegister& r30) {
    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    uint32_t struct_addr = static_cast<uint32_t>(r30.u64);
    if (struct_addr == 0) return;

    uint8_t* ptr = base + struct_addr;

    // Preço em offset 0x2C, big-endian
    uint32_t price = (uint32_t(ptr[0x2C]) << 24) | (uint32_t(ptr[0x2D]) << 16) |
                     (uint32_t(ptr[0x2E]) << 8)  | uint32_t(ptr[0x2F]);

    if (price == 118000) {
        LARECOMP_APP_INFO("[Audi R8] Restaurando ContentFlags/PortalRewardIdx para valores padrão");

        // ContentDownloadFlags (0x30) = 1, ContentFlags (0x34) = 1 — big-endian
        ptr[0x30] = 0; ptr[0x31] = 0; ptr[0x32] = 0; ptr[0x33] = 1;
        ptr[0x34] = 0; ptr[0x35] = 0; ptr[0x36] = 0; ptr[0x37] = 1;
        // PortalRewardIdx (0x38) = -1 (0xFFFFFFFF)
        ptr[0x38] = 0xFF; ptr[0x39] = 0xFF; ptr[0x3A] = 0xFF; ptr[0x3B] = 0xFF;
    }
}

// BadassBaboon's Recomp Adjustments: Vehicle chassis suspension damping & ground depth filter continuous-time scaling
// 0x82563720: lis r11, flt_82001D14@ha in sub_82563298.
// f0 is the chassis ground depth filter coefficient alpha (0.10 at 30 FPS, 0.05 at 60 FPS).
void MCLAChassisDepthSmoothing(PPCRegister& f0) {
    if (!REXCVAR_GET(smooth_chassis_depth)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    if (dt > 0.0f && dt <= 0.25f) {
        f0.f64 = 1.0 - std::pow(0.90, static_cast<double>(dt) * 30.0);
    }
}

// BadassBaboon's Recomp Adjustments: Suspension ground pushback damper velocity authority expansion
// 0x8256384C: before bl sub_825A3D90 in sub_82563298.
// f0 holds the true unclamped penetration velocity (x - x_prev) / dt.
// f3 holds the velocity clamped to [-2.5, +2.5] by the original code.
// Clamping to 2.5 m/s clips damping authority during high-G compression/rebound (sharp turns, burnouts),
// starving the critically-damped PD solver of velocity damping and causing limit-cycle hopping.
void MCLAChassisGroundPushbackDamper(PPCRegister& f0, PPCRegister& f3) {
    if (!REXCVAR_GET(smooth_chassis_depth)) return;

    const float unclamped_v = static_cast<float>(f0.f64);
    f3.f64 = std::clamp(unclamped_v, -12.0f, 12.0f);
}

// BadassBaboon's Recomp Adjustments: Suspension ground pushback acceleration limiter and high-FPS impulse scaling
// 0x82563850: entry after bl sub_825A3D90 in sub_82563298.
// f1 is the acceleration returned by sub_825A3D90.
// Limiting excessive pushback acceleration prevents single-frame spring spikes from kicking the chassis upward.
// At > 60 FPS (e.g. 100, 120, 144, 240+ FPS uncapped), sub_82563298 executes 1/dt times per second instead of 60.
// Scaling by min(1.0f, dt * 60.0f) ensures that the cumulative pushback impulse per second matches the 60 Hz reference
// and prevents suspension over-stiffening or hopping at high frame rates.
void MCLAChassisPushbackAccel(PPCRegister& f1) {
    if (!REXCVAR_GET(smooth_chassis_depth)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    float dt_scale = 1.0f;
    if (base) {
        const float dt = ReadGuestF32(base, kGuestFrameDelta);
        if (dt > 0.0f && dt < (1.0f / 60.0f)) {
            dt_scale = dt * 60.0f;
        }
    }

    const float clamped_a = std::clamp(static_cast<float>(f1.f64), -100.0f, 120.0f);
    f1.f64 = static_cast<double>(clamped_a * dt_scale);
}


// BadassBaboon's Recomp Adjustments: Lateral tire grip & slip velocity continuous-time smoothing
// 0x82369E40: loc_82369E40 in sub_823688B8.
// f0 is the tire slip filter coefficient alpha (0.30 at < 60 FPS, 0.15 at >= 60 FPS).
void MCLATireSlipSmoothing(PPCRegister& f0) {
    if (!REXCVAR_GET(smooth_vehicle_physics)) return;

    auto* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;

    const float dt = ReadGuestF32(base, kGuestFrameDelta);
    if (dt > 0.0f && dt <= 0.25f) {
        // Console reference: retention at 30 FPS = (1.0 - 0.30) = 0.70 per step.
        f0.f64 = 1.0 - std::pow(0.70, static_cast<double>(dt) * 30.0);
    }
}
#else // REXGLUE_HAS_XEO3_TARGET
// XEO3 stubs: empty implementations so the linker resolves codegen calls.

#include <rex/ppc/context.h>

bool Patch_PhysicsCollision() { return false; }
void Patch_RideHeightFit(PPCRegister& f30) {}
bool Patch_WheelFitBypass() { return false; }
void Patch_BypassVehicleDLC(PPCRegister& r30) {}
void MCLAChassisDepthSmoothing(PPCRegister& f0) {}
void MCLAChassisGroundPushbackDamper(PPCRegister& f0, PPCRegister& f3) {}
void MCLAChassisPushbackAccel(PPCRegister& f1) {}
void MCLATireSlipSmoothing(PPCRegister& f0) {}
void MCLA_TrafficChassisBound_8232D048(PPCRegister& r9, PPCRegister& r11) {}
void MCLA_TrafficChassisBound_8232D900(PPCRegister& r3, PPCRegister& r11) {}
void MCLA_TrafficChassisBound_8232E274(PPCRegister& r3, PPCRegister& r31) {}
bool MCLA_TrafficBoundRelease_8259AA40(PPCRegister& r30) { return false; }
#endif // REXGLUE_HAS_XEO3_TARGET
