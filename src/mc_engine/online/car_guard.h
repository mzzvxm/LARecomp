#pragma once
//
// Rigged car configs: broken vinyl layers and tuning values the game cannot
// make (car_guard.cpp).
//

#include <cstdint>

namespace larecomp::car_guard {

// After the save is read into the garage (sub_822D2258): fixes every car the
// profile holds and keeps what was changed, so the player is told once they
// drive that car.
void AfterSaveLoad(uint8_t* base);

// Per-frame, game thread: shows a queued warning once the world is up (another
// player's car, or the garage car being driven), and puts the borrowed dialog
// text back after the dialog closes.
void Tick();

}  // namespace larecomp::car_guard
