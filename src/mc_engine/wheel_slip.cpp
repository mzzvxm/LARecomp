// Wheelspin and brake lock-up of the player's car, from the game's own tyre model.
//
// The numbers come from the wheel update, sub_825666A0 (one call per wheel and physics step; r31
// is the wheel object, everything below is a big-endian float in guest memory):
//   +296  the speed of the wheel's contact point over the ground along the rolling direction
//   +304  that speed plus the tyre's surface speed: the update stores `+296 + (+552) * (+420)` at
//         0x82566904, so it is the longitudinal slip speed, 0 while the wheel rolls freely
//   +420  the tyre radius, +552 the wheel's angular speed (the update advances the rotation angle at
//         +476 by +552 times the step)
//   +529  1 while the wheel touches the ground, 0 in the air
//   +8    the car the wheel belongs to; [car+128] is its physics body, the same object the HUD
//         update reaches through [[player]+8]+128 (the speedometer reads the body's velocity)
// The update has one exit, the epilogue at 0x82567154, where r31 is still the wheel: the hook sits
// there, so the fields are the ones the step has just used.
//
// The sign of +296 follows the game's wheel axes and is not known (a car rolling forward may have
// it negative), so nothing here depends on it. The slip speed is the surface speed plus +296: it
// has the SAME sign as +296 when the tyre turns slower than the ground goes (braking, a locked
// wheel) and the OPPOSITE sign when the tyre turns faster (wheelspin). That is the whole rule:
//   wheelspin: the tyre is faster than the ground. Size: slip speed over the larger of the ground
//     speed and 5 m/s, so a burnout from a standstill counts in full and a small slip at speed
//     does not.
//   lock-up: the tyre is slower than the ground, at 6 m/s (about 22 km/h) or more. Size: slip
//     speed over ground speed, which is 1 for a wheel that has stopped turning.
// Only longitudinal slip counts. +520 (a 0 to 1 slip amount the tyre smoke reads) also rises in a
// sideways slide, which is neither of the two.
//
// The wheels of the player's car are told apart from every other car's by their owner: the HUD
// hook gives the car (Hook_TriggersHud -> NotePlayer) and a wheel counts when [wheel+8] is that
// car, or any object with the same physics body.
//
// CHECK: all of the above is read from the recompiled code and has never run against a car. Turn
// on trigger_slip_log and drive: a burnout from a standstill should give wheelspin near 1; hard
// braking from speed should show lock-up rising; plain driving, cornering and drifting without
// wheelspin should stay near 0. The thresholds below are guesses to tune against that log.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "wheel_slip.h"

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "hooks/hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(trigger_slip_log, false, "MCLA/Input",
    "Development: once a second while a car is driven, write each wheel's ground speed, slip speed "
    "and the wheelspin and lock-up amounts the trigger effects use to the log.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kOwner = 8;
constexpr uint32_t kGroundSpeed = 296;
constexpr uint32_t kSlipSpeed = 304;
constexpr uint32_t kOnGround = 529;
constexpr uint32_t kCarBody = 128;

// Wheelspin: slip speed over max(ground speed, kSpinRefSpeed), from kSpinDead (nothing) to
// kSpinFull (everything); and at least kSpinMinSlip m/s of slip.
constexpr float kSpinRefSpeed = 5.0f;
constexpr float kSpinMinSlip = 1.5f;
constexpr float kSpinDead = 0.15f;
constexpr float kSpinFull = 0.75f;
// Lock-up: slip speed over ground speed, from kLockDead to kLockFull, above kLockMinSpeed m/s.
constexpr float kLockMinSpeed = 6.0f;
constexpr float kLockDead = 0.35f;
constexpr float kLockFull = 0.95f;

// A wheel that has not been updated for this long is not counted (the physics step is a few
// milliseconds to a frame; a car that was replaced leaves its wheels behind).
constexpr int64_t kWheelFreshMs = 150;
// No HUD update for this long: no car is driven.
constexpr int64_t kPlayerFreshMs = 300;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

float Ramp(float v, float lo, float hi) {
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
}

bool Plausible(uint32_t pointer) {
    return pointer >= 0x40000000u && pointer < 0xC0000000u && (pointer & 3) == 0;
}

std::atomic<uint32_t> g_car{0};   // the car the HUD shows
std::atomic<uint32_t> g_body{0};  // its physics body
std::atomic<int64_t> g_player_ms{0};

struct WheelSlot {
    uint32_t wheel = 0;
    int64_t ms = 0;
    float ground = 0.0f;  // +296, for the log
    float slip = 0.0f;    // +304, for the log
    float spin = 0.0f;
    float lock = 0.0f;
};

std::mutex g_mutex;
WheelSlot g_slots[8];  // a car has four wheels, a bike two; the rest is room for a replaced car

}  // namespace

namespace larecomp::slip {

void NotePlayer(uint32_t player) {
    rex::Runtime* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base || !Plausible(player)) return;
    const uint32_t car = ReadGuestU32(base, player + 8);
    if (!Plausible(car)) return;
    g_car.store(car, std::memory_order_relaxed);
    g_body.store(ReadGuestU32(base, car + kCarBody), std::memory_order_relaxed);
    g_player_ms.store(NowMs(), std::memory_order_relaxed);
}

Amounts Current() {
    Amounts out;
    const int64_t now = NowMs();
    const int64_t player = g_player_ms.load(std::memory_order_relaxed);
    if (player == 0 || now - player > kPlayerFreshMs) return out;
    std::string line;
    const bool log = REXCVAR_GET(trigger_slip_log);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const WheelSlot& slot : g_slots) {
            if (slot.wheel == 0 || now - slot.ms > kWheelFreshMs) continue;
            out.wheelspin = std::max(out.wheelspin, slot.spin);
            out.lockup = std::max(out.lockup, slot.lock);
            if (log) {
                char text[96];
                std::snprintf(text, sizeof(text), " [%08X ground %.1f slip %.1f]", slot.wheel,
                              double(slot.ground), double(slot.slip));
                line += text;
            }
        }
    }
    if (log) {
        static std::atomic<int64_t> next_log_ms{0};
        if (now >= next_log_ms.load(std::memory_order_relaxed)) {
            next_log_ms.store(now + 1000, std::memory_order_relaxed);
            LARECOMP_APP_INFO("[Slip] wheelspin {:.2f} lock-up {:.2f}{}", double(out.wheelspin),
                              double(out.lockup), line.empty() ? " (no wheel updated)" : line);
        }
    }
    return out;
}

}  // namespace larecomp::slip

// 0x82567154 in sub_825666A0, the epilogue of the wheel update: `addi r1,r1,432`. r31 = the wheel.
// Runs for every wheel of every car; everything but the player's returns after one load.
void Hook_WheelSlip(PPCRegister& r31) {
    const uint32_t car = g_car.load(std::memory_order_relaxed);
    if (car == 0) return;
    rex::Runtime* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base) return;
    const uint32_t wheel = r31.u32;
    const uint32_t owner = ReadGuestU32(base, wheel + kOwner);
    if (owner != car) {
        // Another object of the same physics body counts as well.
        const uint32_t body = g_body.load(std::memory_order_relaxed);
        if (owner == 0 || body == 0 || ReadGuestU32(base, owner + kCarBody) != body) return;
    }

    float spin = 0.0f;
    float lock = 0.0f;
    const float ground = ReadGuestF32(base, wheel + kGroundSpeed);
    const float slip = ReadGuestF32(base, wheel + kSlipSpeed);
    if (base[wheel + kOnGround] != 0 && std::isfinite(ground) && std::isfinite(slip)) {
        const float ground_speed = std::fabs(ground);
        const float slip_speed = std::fabs(slip);
        if (slip * ground <= 0.0f) {
            // The tyre turns faster than the ground goes.
            if (slip_speed >= kSpinMinSlip) {
                spin = Ramp(slip_speed / std::max(ground_speed, kSpinRefSpeed), kSpinDead, kSpinFull);
            }
        } else if (ground_speed >= kLockMinSpeed) {
            // Slower than the ground: 1 when the wheel has stopped.
            lock = Ramp(slip_speed / ground_speed, kLockDead, kLockFull);
        }
    }

    const int64_t now = NowMs();
    std::lock_guard<std::mutex> guard(g_mutex);
    WheelSlot* slot = nullptr;
    for (WheelSlot& s : g_slots) {
        if (s.wheel == wheel) {
            slot = &s;
            break;
        }
        if (!slot || s.ms < slot->ms) slot = &s;  // else the oldest
    }
    slot->wheel = wheel;
    slot->ms = now;
    slot->ground = ground;
    slot->slip = slip;
    slot->spin = spin;
    slot->lock = lock;
}
#endif  // REXGLUE_HAS_XEO3_TARGET
