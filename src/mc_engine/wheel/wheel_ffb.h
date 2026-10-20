#pragma once
//
// Force feedback for the steering wheel: the game's effects in, one force out.
//
// The game drives a wheel the way the Xbox 360 had it: a handful of long-running effects
// (spring, constant, damper, square) that it creates once, starts and stops, and rewrites
// dozens of times a second. Wheels on a PC differ in which of those they can play, how many at
// once, and whether rewriting one restarts it. So this does not forward them: it keeps a mirror
// of the game's effects, adds them up in software (spring and damper from the wheel's own
// position and speed) and plays the sum as a single constant force, which every force-feedback
// wheel has. See, sections 6-9 and 16.
//
// Threads: the guest's FFB thread posts commands (Post*, any thread, never blocks); the wheel
// thread owns the wheel and calls everything else. Nothing else touches the force-feedback device.
// The device is DirectInput, not SDL: the SDK's SDL3 is built without haptic support.
//

#include <chrono>
#include <cstdint>

namespace larecomp::wheel::ffb {

using Clock = std::chrono::steady_clock;

// The effect struct the game hands XInputdFFSetEffect / UpdateEffect (sub_82465F70): 29 words
// are kept. [0] id, [1] PID type (1 constant, 2 ramp, 3 square, 4 sine, 5 triangle, 6/7 saw,
// 8 spring, 9 damper), [4] duration ms (-1 infinite), [5] delay ms, [11..] the parameters.
constexpr int kEffectWords = 29;

// Guest side. A command carries the generation it was posted in; anything older than the
// engine's current generation is dropped, so nothing queued before a stop can restart a force.
void PostEffect(bool update, const int32_t* words);
void PostOperation(int32_t id, bool start);
void PostReset();  // DeviceControl: drop every effect

// Wheel thread.
void DeviceOpened(bool virtual_device, uint16_t vid, uint16_t pid);
void DeviceClosing();  // stops, destroys and closes the force-feedback device

struct TickIn {
    Clock::time_point now;
    float pos;          // wheel position, -1..1 over the game's lock-to-lock range
    float pos_raw;      // the same over the wheel's own range (for the sign check)
    bool focused;       // the game window has focus
    bool driving;       // the wheel (not the gamepad) is driving
    uint64_t frame;     // the guest's input frame counter (its heartbeat)
    float speed_ms;     // the car's speed (the HUD hook); 0 when unknown
};
void Tick(const TickIn& in);

}  // namespace larecomp::wheel::ffb
