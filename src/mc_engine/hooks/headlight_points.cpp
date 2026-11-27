//
// The game's own light points, for the renderer's headlight rig (src/gpu/headlight_rig.h, headlight_points.h) and the light manager's car lights (src/gpu/light_points.h, car_lights.h).
//
// The vehicles' lights are submitted by sub_8237AC90 (r3 the light object, r4 a flag): per light kind a set of points, and for each point, at
// loc_8237AFB4 (one point, before the guest's intensity and cone tests; both ways in arrive there; the hook is one instruction on, at 0x8237AFB8, so that
// the night probe's own at 0x8237AFB4 can coexist: a hook is keyed by its address, and the instruction between them writes no general register), the registers
//
//   r26  the light object                         [r26] the owner (0: none; the points' world positions are then made by sub_8237A698, r30 points at them)
//   r31  the address of the set's pointer = object + 16 + 4 * kind      kind 3 is the Headlight; the object has 19 sets, kinds 0 to 18 (night_probe_rules.h LightKindName)
//   r29  4 times the point's index in the set
//   r17  the pointer to the first float, r20 the pointer to the object's float of the kind: their product is the intensity the guest tests
//
// [[r26] + 12] - 16 is the vehicle; the set (at [r31]) holds the array of point pointers at +20, and a point is a world matrix: rows right +0, up +16, direction +32 and
// POSITION +48 (a car with no owner: the same matrix in the buffer sub_8237A698 filled, whose +48 is r30). All of this was read by the night probe (test/night-probe 3d3bb584,
// perf/out/NIGHT-PROBE.txt C12, C13) and every one of the pointers is one the guest dereferences itself at this very place (rows 0 and 1 are in the same 64 bytes as the two it
// reads), so the reads here are plain (the probe's Mapped() checks cost a VirtualQuery each). The player's and the racers' Headlight points read intensity 2.00 at night and 0.00 by
// day; their direction is the car's BACK axis; the traffic's have no owner (3.4 to 3.8 at night).
//
// WIDENED (LIGHT-UNIFY U7): the registers, the address and the function's name are what they were, so this is a rebuild and no codegen. Two consumers, each asked for only what it wants,
// each one relaxed load when nobody does:
//   - the headlight rig (headlight_points.h Wanted: mcla_gpu_headlights on): the Headlight points only, as before, what the rig has always read;
//   - the light manager (light_points.h Want: car lights, RT lens emissives, the debug census): every kind it asked for, with the light object, the whole matrix and the peak
//     intensity, into light_points.h's table.
//
// Nothing is written into the guest. The tables are a mutex and a few hundred bytes a frame (a few thousand for every kind of every car in sight).
//
#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>

#include <cstdint>

#include "gpu/headlight_points.h"
#include "gpu/light_points.h"
#include "hooks_internal.h"
#include "guest_timing.h"
#include "run_ready.h"
#include "title_puddle_probe.h"
#include "larecomp_log.h"

namespace {

const uint8_t* Base() {
    auto* rt = REX_RUNTIME();
    return rt ? rt->GetMembase().GetBase() : nullptr;
}

// Guest memory as light_points.h ReadPoint reads it.
struct GuestMemory {
    const uint8_t* base;
    uint32_t U32(uint32_t addr) const { return ReadGuestU32(base, addr); }
    float F32(uint32_t addr) const { return ReadGuestF32(base, addr); }
};

}  // namespace

// 0x8237AFB8, one light point of a vehicle (larecomp_config.toml). Registers as the comment at the top of the file says.
void MCLAHeadlightPoint(PPCRegister& r17, PPCRegister& r20, PPCRegister& r26, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {
    const mc::guest_timing::Scope timing(mc::guest_timing::Section::kLights);
    namespace hp = mcla::gpu::headlight_points;
    namespace lp = larecomp::gpu::light_points;
    const bool rig = hp::Wanted();
    if (!rig && !lp::AnyWanted()) return;
    const uint32_t object = uint32_t(r26.u64), slot = uint32_t(r31.u64);
    uint32_t kind = 0;
    if (!lp::KindOfSlot(object, slot, &kind)) return;  // not one of the object's 19 sets
    const bool for_rig = rig && kind == hp::kHeadlightKind;  // the other 24 points of a car end here for the rig
    const bool for_manager = lp::Wanted(kind);
    if (!for_rig && !for_manager) return;
    const uint8_t* base = Base();
    if (!base) return;

    lp::Regs regs;
    regs.object = object;
    regs.slot = slot;
    regs.index_x4 = uint32_t(r29.u64);
    regs.pos_ptr = uint32_t(r30.u64);
    regs.table_ptr = uint32_t(r17.u64);
    regs.value_ptr = uint32_t(r20.u64);
    const GuestMemory mem{base};
    lp::Point point;
    if (!lp::ReadPoint(mem, regs, ReadGuestU32(base, lp::kPlayerVehiclePtr), &point)) return;
    if (for_manager) lp::Global().Note(point);
    // The rig's table keeps its own contract: an owned point has its direction, one with no owner only its place.
    if (for_rig) hp::Global().Note(lp::ToHeadlightPoint<hp::Point>(point));
}

// CHECK: Verify each arm skips its branch in the title on both renderers.
// 0x8237B0C0 is immediately before the branch that selects AddGlow or the
// call to sub_822FB508. 0x8237B170 advances the point index on both routes.
bool Hook_ProbeTitleVehicleLight(PPCRegister& r14) {
    namespace probe = mc::title_puddles;
    if (!probe::SkipVehiclePoint(probe::ConfiguredArm(), r14.u32) ||
        mc::run_ready::Entered() || !mc::run_ready::Pending(mc::run_ready::Event::Roam)) return false;
    static std::atomic_flag logged;
    if (!logged.test_and_set(std::memory_order_relaxed)) {
        LARECOMP_APP_INFO("[title puddles] skipping guest vehicle {}", r14.u32 ? "projected light" : "glow");
    }
    return true;
}

#else  // REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/context.h>
void MCLAHeadlightPoint(PPCRegister& r17, PPCRegister& r20, PPCRegister& r26, PPCRegister& r29, PPCRegister& r30, PPCRegister& r31) {}
bool Hook_ProbeTitleVehicleLight(PPCRegister&) { return false; }
#endif  // REXGLUE_HAS_XEO3_TARGET
