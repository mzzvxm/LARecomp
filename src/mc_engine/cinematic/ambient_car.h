#pragma once
//
// The guards of a scene's ambient car (`car <id> ambient <model> at <x,y,z> [heading <deg>]`; car_verbs.h). Pure: no game, no log; cars_test.cpp
// checks it alone. The state machine that uses it (AmbientCar) is declared in car_spawn.h with the other cars' machines and implemented in
// ambient_car.cpp; the game side is cars.cpp.
//
// WHAT AN AMBIENT CAR IS HERE. A car of the game's own traffic class (the pool of 98 that the traffic draw, the traffic shadows and the
// ambient manager's census count), made on purpose at a point, with no rail and no driver. The traffic probe (hooks/traffic_probe.cpp; run
// TRAFFICPROBE.txt, 2026-11-19) proved the way:
//   AmbientMgr_AddAmbientToLayout (0x826D6850; model name, a vector pointer, an angle in degrees) takes a slot from the layout pool (512 slots,
//   class 0) and returns its id. The layout item is a placement; the PARKED spawner (sub_826F23A0, run for every zone each update; it needs the
//   parked enable byte 0x8282A7CA and a layout byte at mgr + 0x642C7) turns it into a real car within a second, about 0.5 m from the point,
//   class 0 of the live pool, marked as placed (+40 non-zero: the unspawn pass leaves it alone). It stood still and did not go for 76 s.
//   The angle is the one Racer_Teleport takes (LayoutHeadingDeg in car_verbs.h): 272 gave the scene heading 92.
//   So a scene that closes the parked spawner (`parked off`, a parked scale of 0) gets no ambient car: the scene parser warns about it.
//
// THE REMOVAL. AmbientMgr_RemoveAmbientFromLayout (0x826D5CD8; the layout item's id) is what the game's own scripts use to take a placement back
// (read in the generated code, sub_826D5CD8 -> sub_826E1A20(layout, id, 15); CHECK: the probe never called it). It finds the item by its id in the
// layout pool's live and used lists, clears it and, when a car is bound to it (+76), sends the manager the despawn message for that car with
// reason 15 (sub_826D50E8: sub_82687930 + sub_826DF370 + sub_826ED540 on mgr + 8800); a placement whose car is not made yet (+76 clear, +80 >= 0)
// has its spawn ticket cancelled (sub_826D4B78); then the item goes back to the pool's free list (sub_826E0D08). An id that is not there is a
// logged no-op. So the clean-up of a scene is one native call for each car, and the slot comes back.
//
// THE GUARDS (the probe's, with the cap made a scene's): free roam only (the game's Cruising state), a car being driven (the game is not
// paused), the world up for 120 updates, a layout slot free with two to spare, and at most 8 placements in a scene. The model must be in
// the manager's type list and the point near the player: the game side checks those at the call (it has the lists).
//

#include <cstdint>

namespace mc::cinematic {

inline constexpr uint32_t kAmbientMinFreeLayoutSlots = 2;  // the last slots stay with the game (the probe's kMinFreeLayoutSlots)
inline constexpr uint32_t kAmbientMinWorldUpdates = 120;   // updates the world must have been up (the probe's kMinWorldUpdates)
inline constexpr uint32_t kAmbientMaxPerScene = 8;         // placements one scene may make
inline constexpr int64_t kAmbientGiveUpMs = 90000;         // a placement that has waited this long for the world fails
inline constexpr double kAmbientMaxFromPlayerM = 1000.0;   // the point is plausible: within this of the player's car (the probe's rule)

// What the game side saw this update.
struct AmbientGate {
    bool manager = false;         // the ambient manager exists
    uint32_t manager_addr = 0;    // ... and where (a layout item is only good in the manager that made it)
    bool player = false;          // the player's car exists
    bool free_roam = false;       // the game's state machine is in Cruising
    bool not_paused = false;      // not paused, not in a menu or a loading screen
    uint32_t world_updates = 0;   // updates the manager and the player's car have been there in a row
    bool layout_pool = false;     // the layout's pool was read and its counts are sane
    uint32_t layout_free = 0;     // class 0 free slots
    uint32_t placed_so_far = 0;   // placements this scene has made
};

enum class AmbientRefusal : uint8_t {
    kNone,
    kNoManager,
    kNoPlayer,
    kNotFreeRoam,
    kNotDriving,
    kTooSoon,
    kNoLayoutPool,
    kPoolFull,
    kCapReached,
};

const char* AmbientRefusalText(AmbientRefusal r);
// A refusal that a later update may lift (the world is not up yet, a menu is open): the placement waits. The others are final: it fails.
bool AmbientRefusalPasses(AmbientRefusal r);
// Every guard in one place, in the order a person would check them. kNone = the call may be made.
AmbientRefusal DecideAmbientAdd(const AmbientGate& gate);

}  // namespace mc::cinematic
