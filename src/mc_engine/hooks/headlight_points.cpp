//
// The game's own headlight points, for the renderer's headlight rig (src/gpu/headlight_rig.h, headlight_points.h).
//
// The vehicles' lights are submitted by sub_8237AC90 (r3 the light object, r4 a flag): per light kind a set of points, and for each point, at
// loc_8237AFB4 (one point, before the guest's intensity and cone tests; both ways in arrive there; the hook is one instruction on, at 0x8237AFB8, so that
// the night probe's own at 0x8237AFB4 can coexist: a hook is keyed by its address, and the instruction between them writes no general register), the registers
//
//   r26  the light object                         [r26] the owner (0: none; the points' world positions are then made by sub_8237A698, r30 points at them)
//   r31  the address of the set's pointer = object + 16 + 4 * kind      kind 3 is the Headlight (night_probe_rules.h LightKindName)
//   r29  4 times the point's index in the set
//   r17  the pointer to the kind's float, r20 the pointer to the object's float: their product is the intensity the guest tests
//
// [[r26] + 12] - 16 is the vehicle; the set (at [r31]) holds the array of point pointers at +20, and a point has its direction at +32 and its WORLD
// position at +48. All of this was read by the night probe (test/night-probe 3d3bb584, perf/out/NIGHT-PROBE.txt C12, C13) and every one of the
// pointers is one the guest dereferences itself at this very place, so the reads here are plain (the probe's Mapped() checks cost a VirtualQuery each).
// The player's and the racers' Headlight points read intensity 2.00 at night and 0.00 by day; their direction is the car's BACK axis; the traffic's have
// no owner and no direction (3.4 at night). Only the Headlight points are noted, and only while the renderer's rig wants them (mcla_gpu_headlights on):
// otherwise the hook is one relaxed load.
//
// Nothing is written into the guest. The table the points go to is gpu/headlight_points.h (a mutex, a few hundred bytes a frame).
//
#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>

#include <cstdint>

#include "gpu/headlight_points.h"
#include "hooks_internal.h"

namespace {

constexpr uint32_t kPlayerVehiclePtr = 0x8288DCF8u;  // the vehicle the local player drives
constexpr uint32_t kPointDirection = 32u;
constexpr uint32_t kPointPosition = 48u;
constexpr uint32_t kSetPoints = 20u;                 // the set's array of point pointers
constexpr uint32_t kObjectSets = 16u;                // the object's sets from here, a pointer each

const uint8_t* Base() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

bool PlausiblePtr(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

}  // namespace

// 0x8237AFB8, one light point of a vehicle (larecomp_config.toml). Registers as the comment at the top of the file says.
void MCLAHeadlightPoint(PPCRegister& r17, PPCRegister& r20, PPCRegister& r26, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {
    namespace hp = mcla::gpu::headlight_points;
    if (!hp::Wanted()) return;
    const uint32_t object = uint32_t(r26.u64), slot = uint32_t(r31.u64);
    if (slot < object + kObjectSets || (slot - object - kObjectSets) % 4 != 0) return;
    if ((slot - object - kObjectSets) / 4 != hp::kHeadlightKind) return;  // the other 24 points of a car end here
    const uint8_t* base = Base();
    if (!base || !PlausiblePtr(object) || !PlausiblePtr(slot)) return;

    hp::Point point;
    point.index = uint32_t(r29.u64) / 4u;
    const uint32_t owner = ReadGuestU32(base, object);
    if (owner != 0) {
        const uint32_t set = ReadGuestU32(base, slot);
        if (!PlausiblePtr(set) || !PlausiblePtr(owner)) return;
        const uint32_t link = ReadGuestU32(base, owner + 12);
        if (link != 0) point.vehicle = link - 16u;
        const uint32_t points = ReadGuestU32(base, set + kSetPoints);
        if (!PlausiblePtr(points)) return;
        const uint32_t p = ReadGuestU32(base, points + uint32_t(r29.u64));
        if (!PlausiblePtr(p)) return;
        for (uint32_t i = 0; i < 3; ++i) {
            point.dir[i] = ReadGuestF32(base, p + kPointDirection + 4u * i);
            point.pos[i] = ReadGuestF32(base, p + kPointPosition + 4u * i);
        }
        point.has_dir = true;
    } else {
        const uint32_t at = uint32_t(r30.u64);  // the position sub_8237A698 made
        if (!PlausiblePtr(at)) return;
        for (uint32_t i = 0; i < 3; ++i) point.pos[i] = ReadGuestF32(base, at + 4u * i);
    }
    const uint32_t table = uint32_t(r17.u64), value = uint32_t(r20.u64);
    if (!PlausiblePtr(table) || !PlausiblePtr(value)) return;
    point.intensity = ReadGuestF32(base, table) * ReadGuestF32(base, value);
    point.player = point.vehicle != 0 && point.vehicle == ReadGuestU32(base, kPlayerVehiclePtr);
    hp::Global().Note(point);
}

#else  // REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
void MCLAHeadlightPoint(PPCRegister& r17, PPCRegister& r20, PPCRegister& r26, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {}
#endif  // REXGLUE_HAS_XEO3_TARGET
