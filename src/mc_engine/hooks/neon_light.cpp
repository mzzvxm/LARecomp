//
// The car's neon underglow as a light of the renderer's deferred local light pass (src/gpu/neon_light.h has the story, the layout of what is read and the CHECKs; src/gpu/local_lights_sources.h
// makes the lights).
//
// What the game does (feature-parity/NEON-PULSE.md section 3, native\tree-k1\generated larecomp_recomp.44.cpp): sub_823553F8, the car effect object's light update, runs once an update and ends by
// storing the exterior neon's colour (ExtNeonColor x ExtNeonIntensity x the pulse x the fade) and its enable byte in the light object at this + 560, whose geometry (four points of a tube, a matrix)
// sub_823511F8 filled. This overrides that function exactly as hooks/neon_pulse.cpp overrides the pulse's (sub_82353E38): the game's own update runs first, untouched, and then the object is READ,
// so the colour the light pass uses is the game's own, with the pulse as the game made it (the console's 30 Hz hold included: mcla_fix_neon_30hz), the day and night constants and the fade.
//
// NOTHING IS WRITTEN INTO THE GUEST. While nothing wants it (NEON LIGHT off, or no pass) the cost is one relaxed load; while it is wanted, a few dozen loads of guest memory a car and an update, and
// a mutex. The symbol is the only one this file defines; neon_pulse.cpp's is another function's, and both are in the [functions] table of larecomp_config.toml, so there is no codegen.
//
// CHECK (nothing here could be run): the offsets are read from the recompiled code (neon_light.h's list); the first log line of a session says what the first lit car read, how the tube's four
// points were taken (world or car space: N1) and whether the colour is the game's arithmetic of the config and the pulse.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>
#include <rex/ppc/context.h>
#include <rex/kernel/title_runtime.h>

#include <atomic>
#include <cstdint>

#include "gpu/light_points.h"
#include "gpu/neon_light.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REX_EXTERN(__imp__rex_sub_823553F8);  // the car effect object's light update (the neon's colour is its last step)

namespace {

namespace nl = larecomp::gpu::neon_light;

// Guest memory as neon_light.h ReadNeonCar reads it.
struct GuestMemory {
    const uint8_t* base;
    uint8_t U8(uint32_t addr) const { return base[addr]; }
    uint32_t U32(uint32_t addr) const { return ReadGuestU32(base, addr); }
    float F32(uint32_t addr) const { return ReadGuestF32(base, addr); }
};

std::atomic<bool> g_logged_first{false};
std::atomic<bool> g_logged_unplaced{false};

}  // namespace

// Runs the game's update, then reads the car's exterior neon light while the pass wants it.
extern "C" REX_FUNC(rex_sub_823553F8) {
    const uint32_t self = ctx.r3.u32;
    __imp__rex_sub_823553F8(ctx, base);

    if (!nl::Wanted()) return;
    const GuestMemory mem{base};
    nl::NeonCar car;
    if (!nl::ReadNeonCar(mem, self, ReadGuestU32(base, larecomp::gpu::light_points::kPlayerVehiclePtr), &car)) return;
    nl::Global().Note(car);

    // The first car whose neon is lit, and the first whose tube the four points do not place: one line each (the log says N1 at once).
    if (car.on && nl::Brightest(car.colour) > 0.0f) {
        nl::Tube tube;
        const bool placed = nl::ResolveTube(car, &tube);
        if (placed ? !g_logged_first.exchange(true, std::memory_order_relaxed) : !g_logged_unplaced.exchange(true, std::memory_order_relaxed)) {
            LARECOMP_APP_INFO("{}", nl::FormatLine(car, tube, placed));
        }
    }
}
#endif  // REXGLUE_HAS_XEO3_TARGET
