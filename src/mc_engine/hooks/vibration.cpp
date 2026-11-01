// Controller vibration on or off.
//
// The game has no vibration setting (on the console it was the system's). It sets a
// pad's rumble in one place: sub_8275CB88(user, &{left, right}), called from the per-pad
// update (sub_82614D30) whenever a pad's rumble changed, copies the two 16-bit motor
// speeds to the stack and calls XamInputSetState(user, 0, speeds) at 0x8275CC34, the only
// call of it in the image. The SDK hands that to InputSystem::SetState, which sets every
// device of the user: SDL_RumbleGamepad for an SDL pad, XInputSetState for an XInput one.
// The game asks only when the rumble changes, so a motor keeps running until it says stop.
//
// With controller_vibration off the hook in front of that call zeroes the speeds: the
// drivers get a stop where the game asked for a rumble, and the game is told it worked.
// A motor that is already running when the option goes off has nothing to stop it until
// the game asks again, so the per-frame tick sends the stop itself.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "hooks.h"
#include "hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(controller_vibration, true, "MCLA/Input",
    "Controller vibration. Off keeps the game's rumble from reaching any controller and stops a "
    "motor that is running.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr uint32_t kUsers = 4;

// What each user's drivers were last told: the two motor speeds in one word, left in the high
// half. Nonzero = a motor may be running.
std::atomic<uint32_t> g_driven[kUsers];

}  // namespace

// 0x8275CC34 in sub_8275CB88: the `bl XamInputSetState`. r3 = the user, r5 = the speeds on the
// stack (two big-endian 16-bit values, left first).
void Hook_VibrationSet(PPCRegister& r3, PPCRegister& r5) {
    uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base || r5.u32 == 0) return;
    if (!REXCVAR_GET(controller_vibration)) {
        std::memset(base + r5.u32, 0, 4);
    }
    if (r3.u32 < kUsers) {
        g_driven[r3.u32].store(ReadGuestU32(base, r5.u32), std::memory_order_relaxed);
    }
}

// Per frame, on the game's thread (Patch_DeltaTimePre): the option was turned off while a
// motor was running.
void TickVibration() {
    if (REXCVAR_GET(controller_vibration)) return;
    auto* input = static_cast<rex::input::InputSystem*>(
        rex::Runtime::instance()->input_system());
    if (!input) return;
    for (uint32_t user = 0; user < kUsers; ++user) {
        if (g_driven[user].exchange(0, std::memory_order_relaxed) == 0) continue;
        rex::input::X_INPUT_VIBRATION stop;
        stop.left_motor_speed = 0;
        stop.right_motor_speed = 0;
        const uint32_t result = static_cast<uint32_t>(input->SetState(user, &stop));
        LARECOMP_APP_INFO("[Vibration] off: motors of user {} stopped (input layer answered 0x{:X})",
                          user, result);
    }
}
#endif  // REXGLUE_HAS_XEO3_TARGET
