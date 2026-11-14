// DEBUG ONLY, a probe the owner flips live in F4: can the game's own code draw the cars into the wet-road
// reflection? (GFX-UPGRADES-DESIGN.md section 13, research G5b-R; the test patch of 13.6 as a live switch.)
//
// mcla_gpu_road_reflection_cars_test: 0 = off (the default; nothing here changes anything then), 1 = the
// player's car, 2 = the player's car and the other vehicles (traffic). Never saved (.debug_only()).
//
// What the guest does (default.xex; the addresses were re-read in the recompiled sub_82360518 and its callees):
//   Vehicle::Draw (sub_82360518, r30 = the vehicle, r28 = the phase) has a branch for the road-reflection
//   phase 0x2 at 0x82360694. It draws the car through sub_8235FB78 with a camera swapped to mirrored matrices
//   (sub_8215E000 builds them, sub_8217CFD0 sets the camera and, after the draw, puts the saved one back), but
//   only if the vehicle's byte +593 is not 0 (0x823606A0) and the global byte 0x82908D5D is not 0 (loaded at
//   0x823606B0, tested at 0x823606B4). That byte is a "secondary view being drawn" mode flag (25 other
//   functions read it), 0 while driving, so the branch never runs on the street. The mirror plane is read
//   from renderer+332 at 0x8236076C (`lfs f0,332(r9)`, the constructor leaves 0.0: a plane through y = 0).
//   The street's own mirror plane is the player's ground height (what sub_822F3918 puts in
//   gReflectionParams.z: y at +52 of the matrix sub_822A8108(sub_822A39C8(*0x82874374)) returns).
//
// The three hooks (larecomp_config.toml) change REGISTERS only, never guest memory:
//   Hook_RoadReflCarsEnter  0x823606A0 (before `lbz r11,593(r30)`): counts, and decides for this vehicle whether
//                           the gate is opened and what the plane is. Opens only when ALL of this holds: the
//                           switch is on, the phase is exactly 0x2 and the renderer's current phase (+356) says
//                           0x2 too, the vehicle's +593 is set (the game's own condition), the global byte is 0
//                           (a game that has opened it itself, e.g. its own secondary view, is left alone), the
//                           vehicle is the player's (mode 1), a car is being driven (the race HUD is updating
//                           and the game is not paused: no pause screen, no map view, no menu, no garage), the
//                           lighting manager's garage flag (+14546) is off, the player's vehicle exists, and
//                           the player's ground height could be read through valid pointers.
//   Hook_RoadReflCarsGate   0x823606B4 (before `cmplwi cr6,r10,0`): r10 = 1 for a vehicle opened above.
//   Hook_RoadReflCarsPlane  0x8236076C (after the `lfs f0,332(r9)`): f0 = -(the player's ground height), so the
//                           mirror's y translation (-2 * f0 at 0x82360770..0x82360794) is 2h, a mirror about
//                           the street. The sign is the reading of those instructions: CHECK in the picture
//                           (a car mirrored too high or too low means the sign or the plane is wrong).
// A vehicle that is not opened goes through the unchanged guest code.
//
// What it can and cannot do (13.5 of the design; the one-run probe of 2026-11-13, 13.6, saw this): the game's
// car materials may have no technique for phase 2, then the branch draws nothing, and the camera swap may
// leave the reflection's other draws culled, so the lights of the shops on the wet road can vanish while it is
// on. That is all it can cost: the main view's camera is restored by the guest's own code.
//
// Log, tag "[road refl cars]": one line at each change of the switch (what is opened), and while it is on one
// line per 600 frames with the counts. TickRoadReflCars runs once a frame (Patch_DeltaTimePre).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"
#include "mc_engine/adaptive_triggers.h"

REXCVAR_DEFINE_INT32(
    mcla_gpu_road_reflection_cars_test, 0, "MCLA/GPU",
    "Debug test, live, never saved: lets the game's own code draw cars into the wet-road reflection. 0 = off "
    "(the game as it is). 1 = the player's car. 2 = the player's car and the traffic. It opens the game's "
    "phase-2 vehicle draw (a camera mirrored about the player's ground height) only while a car is driven, "
    "not in a menu, the pause screen, the map or the garage. It may draw nothing (the car materials may "
    "have no reflection pass) and may thin out the other reflections while on; the log (\"[road refl cars]\") "
    "says how many vehicle draws went in.")
    .debug_only()
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kRendererPtr = 0x8287E064u;     // dword: the renderer (sub_822E6408's `this`)
constexpr uint32_t kRendererPhase = 356u;          // renderer+356: the phase being drawn
constexpr uint32_t kRendererView = 384u;           // renderer+384: the view index of this pass
constexpr uint32_t kRendererPlane = 332u;          // renderer+332: the plane the branch reads (0.0 stock)
constexpr uint32_t kPlayerVehicle = 0x8288DCF8u;   // dword: the vehicle the local player drives
constexpr uint32_t kSecondaryViewByte = 0x82908D5Du;  // the gate: non-zero = a secondary view mode
constexpr uint32_t kLightingMgrPtr = 0x8287E26Cu;  // dword: mcLightingManager
constexpr uint32_t kGarageLight = 14546u;          // mcLightingManager+14546: byte, the garage light sets
constexpr uint32_t kPlayerHolder = 0x82874374u;    // dword: what sub_822F3918 walks for the ground height
constexpr uint32_t kVehicleFlag = 593u;            // vehicle+593: byte, the phase-2 branch's first condition
constexpr uint32_t kReflectionPhase = 2u;
constexpr uint32_t kWindowFrames = 600u;

bool Plausible(uint32_t p) { return p >= 0x10000u && p < 0xE0000000u; }

// The counts of the last window (reset when it is logged). Written by the vehicle draws, read by the tick.
struct Counts {
    std::atomic<uint32_t> entered{0};         // vehicle draws that reached the phase-2 branch
    std::atomic<uint32_t> player{0};          //   of them the player's car
    std::atomic<uint32_t> flag_clear{0};      //   +593 clear: the game skips them itself
    std::atomic<uint32_t> player_flag_clear{0};
    std::atomic<uint32_t> at_gate{0};         // +593 set: they reach the gate
    std::atomic<uint32_t> native_open{0};     //   the game itself had the gate open: left alone
    std::atomic<uint32_t> skipped_traffic{0}; //   not the player's, switch 1
    std::atomic<uint32_t> refused_phase{0};   //   the renderer's phase is not 2
    std::atomic<uint32_t> refused_no_player{0};
    std::atomic<uint32_t> refused_not_driving{0};
    std::atomic<uint32_t> refused_garage{0};
    std::atomic<uint32_t> refused_no_ground{0};
    std::atomic<uint32_t> opened{0};          // gate opened + plane set: drawn with the mirrored camera
    std::atomic<uint32_t> opened_player{0};
    std::atomic<float> last_ground{0.0f};     // the last plane height used
    std::atomic<float> last_stock_plane{0.0f};  // what renderer+332 held at that moment
    std::atomic<int32_t> last_view{-1};
};
Counts g_counts;

// Per draw call, thread-local: set by Enter, used by Gate and Plane, which follow it in the same call.
thread_local bool t_open = false;
thread_local float t_plane = 0.0f;  // the value for f0 (-h)

// sub_822F3918's source of gReflectionParams.z: sub_822A8108(sub_822A39C8(holder)) + 52, behind the check of
// sub_822A39C0 (the holder has a first entry). Pointer walks, each checked before it is followed.
bool PlayerGroundY(const uint8_t* base, uint32_t r13, float* out) {
    const uint32_t holder = ReadGuestU32(base, kPlayerHolder);
    if (!Plausible(holder) || !Plausible(r13)) return false;
    const uint32_t tls = ReadGuestU32(base, r13);  // lwz r11,0(r13): the thread's block
    if (!Plausible(tls)) return false;
    const bool alt = base[tls + 72u] != 0;  // lbzx r9,72,r11: the "alt thread" index
    const int32_t count = int32_t(ReadGuestU32(base, holder + (alt ? 260u : 132u)));
    const uint32_t entry = ReadGuestU32(base, holder + (alt ? 136u : 8u));  // sub_822A39C8
    if (count <= 0 || !Plausible(entry)) return false;
    const uint32_t r11 = ReadGuestU32(base, entry + 48u);  // sub_822A8108
    uint32_t m;
    if (r11 == 0) {
        m = entry + 288u;
    } else {
        if (!Plausible(r11)) return false;
        m = alt ? r11 + 272u : ReadGuestU32(base, r11 + 28u) + 16u;
    }
    if (!Plausible(m)) return false;
    const float y = ReadGuestF32(base, m + 52u);
    if (!std::isfinite(y) || std::fabs(y) > 10000.0f) return false;
    *out = y;
    return true;
}

void LogChange(int from, int to) {
    if (to == 0) {
        LARECOMP_APP_INFO("[road refl cars] switch {} -> 0: off, the game's own vehicle draw runs unchanged", from);
    } else {
        LARECOMP_APP_INFO(
            "[road refl cars] switch {} -> {}: in the road reflection phase (0x2) the game's own mirrored-camera "
            "car draw is opened for {} while a car is driven (not paused, no garage, no menu); the mirror plane "
            "is the player's ground height. The cars may still not show: their materials may have no reflection pass",
            from, to, to == 1 ? "the player's car only" : "the player's car and the traffic");
    }
}

void LogWindow(int mode, uint32_t frames) {
    Counts& c = g_counts;
    const uint32_t entered = c.entered.exchange(0), player = c.player.exchange(0),
                   flag_clear = c.flag_clear.exchange(0), player_flag_clear = c.player_flag_clear.exchange(0),
                   at_gate = c.at_gate.exchange(0), native_open = c.native_open.exchange(0),
                   skipped_traffic = c.skipped_traffic.exchange(0), r_phase = c.refused_phase.exchange(0),
                   r_no_player = c.refused_no_player.exchange(0), r_not_driving = c.refused_not_driving.exchange(0),
                   r_garage = c.refused_garage.exchange(0), r_no_ground = c.refused_no_ground.exchange(0),
                   opened = c.opened.exchange(0), opened_player = c.opened_player.exchange(0);
    LARECOMP_APP_INFO(
        "[road refl cars] switch {}, last {} frames: {} vehicle draws went into phase 2 (opened: the game's "
        "mirrored-camera draw ran; the player's {}); {} reached the phase-2 branch ({} the player's; flag +593 "
        "clear {}, the player's {}); at the gate {}: already open by the game {}, traffic left out {}, refused: "
        "not driving {}, garage {}, no player car {}, no ground height {}, phase mismatch {}; plane height {:.2f} "
        "(stock plane {:.2f}), view {}{}",
        mode, frames, opened, opened_player, entered, player, flag_clear, player_flag_clear, at_gate,
        native_open, skipped_traffic, r_not_driving, r_garage, r_no_player, r_no_ground, r_phase,
        c.last_ground.load(), c.last_stock_plane.load(), c.last_view.load(),
        entered == 0 ? "; no vehicle draw reached the reflection phase (not drawn, or no car in it)" : "");
}

void ResetCounts() {
    Counts& c = g_counts;
    for (std::atomic<uint32_t>* a :
         {&c.entered, &c.player, &c.flag_clear, &c.player_flag_clear, &c.at_gate, &c.native_open,
          &c.skipped_traffic, &c.refused_phase, &c.refused_no_player, &c.refused_not_driving, &c.refused_garage,
          &c.refused_no_ground, &c.opened, &c.opened_player})
        a->store(0);
}

}  // namespace

// Once a frame: the log of a change of the switch, and the 600-frame counts while it is on.
void TickRoadReflCars() {
    const int mode = std::clamp<int>(REXCVAR_GET(mcla_gpu_road_reflection_cars_test), 0, 2);
    static int s_mode = 0;
    static uint32_t s_frames = 0;
    if (mode != s_mode) {
        LogChange(s_mode, mode);
        s_mode = mode;
        s_frames = 0;
        ResetCounts();
    }
    if (mode == 0) return;
    if (++s_frames >= kWindowFrames) {
        LogWindow(mode, s_frames);
        s_frames = 0;
    }
}

// 0x823606A0 in sub_82360518: the `lbz r11,593(r30)` of the phase-2 branch (reached only with bit 2 of the phase
// set). r30 = the vehicle, r28 = the phase, r13 = the guest's thread register.
void Hook_RoadReflCarsEnter(PPCRegister& r30, PPCRegister& r28, PPCRegister& r13) {
    t_open = false;
    if (REXCVAR_GET(mcla_gpu_road_reflection_cars_test) <= 0) return;
    if (r28.u32 != kReflectionPhase) return;
    auto* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    const uint32_t vehicle = r30.u32;
    if (!base || !Plausible(vehicle)) return;

    Counts& c = g_counts;
    c.entered.fetch_add(1, std::memory_order_relaxed);
    const uint32_t player = ReadGuestU32(base, kPlayerVehicle);
    const bool is_player = vehicle == player;
    if (is_player) c.player.fetch_add(1, std::memory_order_relaxed);
    if (base[vehicle + kVehicleFlag] == 0) {  // the game's own first condition: it skips this vehicle
        c.flag_clear.fetch_add(1, std::memory_order_relaxed);
        if (is_player) c.player_flag_clear.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    c.at_gate.fetch_add(1, std::memory_order_relaxed);
    if (base[kSecondaryViewByte] != 0) {  // the game has the gate open itself: its mode, not ours
        c.native_open.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (REXCVAR_GET(mcla_gpu_road_reflection_cars_test) == 1 && !is_player) {
        c.skipped_traffic.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Only a street frame: the reflection phase is being drawn, a car is driven and no garage is up.
    const uint32_t renderer = ReadGuestU32(base, kRendererPtr);
    if (!Plausible(renderer) || ReadGuestU32(base, renderer + kRendererPhase) != kReflectionPhase) {
        c.refused_phase.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Plausible(player)) {
        c.refused_no_player.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!larecomp::triggers::IsDriving()) {  // the race HUD is updating and the game is not paused
        c.refused_not_driving.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t mgr = ReadGuestU32(base, kLightingMgrPtr);
    if (Plausible(mgr) && base[mgr + kGarageLight] != 0) {
        c.refused_garage.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float ground = 0.0f;
    if (!PlayerGroundY(base, r13.u32, &ground)) {
        c.refused_no_ground.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    t_plane = -ground;
    t_open = true;
    c.opened.fetch_add(1, std::memory_order_relaxed);
    if (is_player) c.opened_player.fetch_add(1, std::memory_order_relaxed);
    c.last_ground.store(ground, std::memory_order_relaxed);
    c.last_stock_plane.store(ReadGuestF32(base, renderer + kRendererPlane), std::memory_order_relaxed);
    c.last_view.store(int32_t(ReadGuestU32(base, renderer + kRendererView)), std::memory_order_relaxed);
}

// 0x823606B4 in sub_82360518: the `cmplwi cr6,r10,0` right after `lbz r10,-29347(r11)` loaded the gate byte.
void Hook_RoadReflCarsGate(PPCRegister& r10) {
    if (t_open) r10.u64 = 1;
}

// 0x8236076C in sub_82360518, after the `lfs f0,332(r9)`: the plane the mirror is built about. Straight-line
// code from the gate to here, so a vehicle opened above always arrives; the flag is used up here.
void Hook_RoadReflCarsPlane(PPCRegister& f0) {
    if (!t_open) return;
    t_open = false;
    f0.f64 = static_cast<double>(t_plane);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
