// A steering wheel the game can be shown without owning one (wheel_fake).
//
// MCLA already ships a wheel path built for the Xbox 360 racing wheel
// (sections 10-11). It is selected by the pad's
// capabilities, not by anything the player configures:
//
//   sub_82614D30 asks XamInputGetCapabilities(user, 0) every frame and rewrites
//   the pad type from it (SubType 2 -> type 4, pad+52). sub_822A4ED0 then picks
//   the Steering_Wheel control map for a type-4 pad, and sub_82466330/470 build
//   a force-feedback device when Type == 1, SubType == 2 and Flags & 1.
//
// So a wheel is a pad that reports SubType 2 + FFB and then looks like the
// retail wheel: LX steers, RT is the throttle, LT the brake, RB/LB shift (the
// _Manual map only), LY/RX/RY stay centred.
//
// This file is the guest-side half of that. The source is either a real wheel
// (wheel_input.cpp: SDL joystick -> the state above) or a fake one, so the whole
// chain can be exercised without hardware:
//
//   wheel_fake = off   nothing here does anything; every hook calls the original.
//   wheel_fake = pad   the real pad / mnk state, presented as a wheel (stick
//                      LX / triggers / bumpers kept, LY / RX / RY centred).
//   wheel_fake = keys  A/D steer, W throttle, S brake, Q/E = LB/RB, read from
//                      the keyboard while the game window has focus. Buttons
//                      the keys do not own (Start, A, B, D-pad...) still come
//                      from the real state, so menus keep working.
//
// With the fake on, the force-feedback wrappers are answered here instead of by
// the SDK, whose XInputdFF* exports are stubs: the game would read an
// uninitialised effect count from GetDeviceInfo. Every command is decoded into
// the log (wheel_ffb_log) so the game's own use of the effects can be read back.
//
// Hooked guest functions (all one-shot wrappers around kernel imports):
//   sub_8275CB70  XamInputGetCapabilities(user, flags, caps)
//   sub_8275CB78  XamInputGetState(user, 1, state)
//   sub_8275C980  GetDeviceInfo(user, info)           info[0] = max effects
//   sub_8275CB08  DeviceControl(user, op)             op 4 = reset all effects
//   sub_8275C9E0  SetEffect(user, effect, 1, 0)       effect = 116-byte struct
//   sub_8275CA40  UpdateEffect(user, effect, 0x700, 0)
//   sub_8275CAA0  EffectOperation(user, {id, op}, 1, 1, 0)   op 2 PLAY, 1 STOP (see sub_82467A18: set/update an effect, then op 2)

#ifndef REXGLUE_HAS_XEO3_TARGET
#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "larecomp_log.h"
#include "wheel_ffb.h"
#include "wheel_input.h"

REXCVAR_DEFINE_STRING(wheel_fake, "off", "MCLA/Wheel",
    "Present the game with a steering wheel without owning one. off: nothing changes. pad: the "
    "real pad or keyboard-and-mouse state is shown as a wheel (stick X steers, triggers are the "
    "pedals, bumpers shift). keys: A/D steer, W throttle, S brake, Q/E shift down/up. Takes "
    "effect on the next input poll.")
    .allowed({"off", "pad", "keys"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_log, true, "MCLA/Wheel",
    "Log every force-feedback command the game sends while wheel_fake is on: effect id, type, "
    "duration and parameters. Capped so a long race cannot fill the log.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__rex_sub_8275CB70);
REX_EXTERN(__imp__rex_sub_8275CB78);
REX_EXTERN(__imp__rex_sub_8275C980);
REX_EXTERN(__imp__rex_sub_8275CB08);
REX_EXTERN(__imp__rex_sub_8275C9E0);
REX_EXTERN(__imp__rex_sub_8275CA40);
REX_EXTERN(__imp__rex_sub_8275CAA0);
REX_EXTERN(__imp__rex_sub_8260C4D0);

namespace {

enum class FakeMode { kOff, kPad, kKeys };

FakeMode CurrentMode() {
    const std::string& v = REXCVAR_GET(wheel_fake);
    if (v == "pad") return FakeMode::kPad;
    if (v == "keys") return FakeMode::kKeys;
    return FakeMode::kOff;
}

// The guest sees a wheel when one is faked or a real one is connected and allowed.
bool WheelOn() {
    return CurrentMode() != FakeMode::kOff || larecomp::wheel::WheelPresent();
}

// A real (or virtual SDL) wheel, as opposed to the keyboard / pad fakes: its effects go to the FFB engine.
bool RealWheel() {
    return CurrentMode() == FakeMode::kOff && larecomp::wheel::WheelPresent();
}

// Guest memory is big-endian.
uint16_t RdU16(const uint8_t* base, uint32_t a) {
    return uint16_t((uint16_t(base[a]) << 8) | base[a + 1]);
}
uint32_t RdU32(const uint8_t* base, uint32_t a) {
    return (uint32_t(base[a]) << 24) | (uint32_t(base[a + 1]) << 16) |
           (uint32_t(base[a + 2]) << 8) | uint32_t(base[a + 3]);
}
void WrU16(uint8_t* base, uint32_t a, uint16_t v) {
    base[a] = uint8_t(v >> 8);
    base[a + 1] = uint8_t(v);
}
void WrU32(uint8_t* base, uint32_t a, uint32_t v) {
    base[a] = uint8_t(v >> 24);
    base[a + 1] = uint8_t(v >> 16);
    base[a + 2] = uint8_t(v >> 8);
    base[a + 3] = uint8_t(v);
}

// The control map player 0 is using, and whether it is a Manual one. sub_822A4ED0 keeps the choice in
// dword_8287438C[player]; a map is 608 bytes with its Transmission at +600 (1 = Manual) and its name
// at the pointer in +596. A pointer that is not in guest RAM reads as Automatic.
bool SelectedMapIsManual(const uint8_t* base) {
    const uint32_t map = RdU32(base, 0x8287438C);
    if (map < 0x10000) return false;
    return RdU32(base, map + 600) == 1;
}

// The game polls user 0 for its pad; 0xFF means "any user" and is pinned to 0
// by the kernel. Only that user is turned into a wheel.
bool IsWheelUser(uint32_t user) {
    const uint32_t u = user & 0xFF;
    return u == 0 || u == 0xFF;
}

// X_INPUT_CAPABILITIES: u8 type, u8 sub_type, be16 flags, gamepad (12), vibration (4).
constexpr uint8_t kTypeGamepad = 1;
constexpr uint8_t kSubTypeWheel = 2;
constexpr uint16_t kCapsFfb = 0x0001;

// X_INPUT_STATE: be32 packet, then the gamepad: be16 buttons, u8 LT, u8 RT,
// be16 LX, LY, RX, RY.
constexpr uint32_t kStButtons = 4;
constexpr uint32_t kStLt = 6;
constexpr uint32_t kStRt = 7;
constexpr uint32_t kStLx = 8;
constexpr uint32_t kStLy = 10;
constexpr uint32_t kStRx = 12;
constexpr uint32_t kStRy = 14;
constexpr uint16_t kBtnLb = 0x0100;
constexpr uint16_t kBtnRb = 0x0200;

#ifdef _WIN32
bool GameWindowHasFocus() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
#else
bool GameWindowHasFocus() { return false; }
bool KeyDown(int) { return false; }
#endif

// Steering from the keyboard: a rate limit toward the target, so a key tap is a
// small correction and holding it sweeps to full lock, as with a wheel.
float StepKeySteer(float target) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    static float steer = 0.0f;
    const Clock::time_point now = Clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (dt > 0.1f) dt = 0.1f;  // a stall must not become a jump
    const bool returning = target == 0.0f || (target * steer < 0.0f);
    const float rate = returning ? 4.0f : 2.5f;  // full lock per second
    const float step = rate * dt;
    if (steer < target) steer = std::fmin(steer + step, target);
    else if (steer > target) steer = std::fmax(steer - step, target);
    return steer;
}

const char* EffectTypeName(uint32_t t) {
    switch (t) {
        case 1: return "constant";
        case 2: return "ramp";
        case 3: return "square";
        case 4: return "sine";
        case 5: return "triangle";
        case 6: return "saw-up";
        case 7: return "saw-down";
        case 8: return "spring";
        case 9: return "damper";
        default: return "?";
    }
}

std::atomic<int> g_ffb_log_budget{4000};

bool FfbLogOn() {
    return REXCVAR_GET(wheel_ffb_log) && g_ffb_log_budget.load(std::memory_order_relaxed) > 0;
}

void FfbLog(const char* what, uint32_t user, const uint8_t* base, uint32_t effect_ptr,
            uint32_t words) {
    if (!FfbLogOn()) return;
    if (g_ffb_log_budget.fetch_sub(1, std::memory_order_relaxed) == 1) {
        LARECOMP_APP_WARN("[wheel] FFB log budget exhausted, further commands are not logged");
    }
    if (words == 0) {
        LARECOMP_APP_INFO("[wheel] ffb {} user={}", what, user);
        return;
    }
    // Effect layout (sub_82465F70): [0] id, [1] PID type, [4] duration ms (-1 = infinite),
    // [5] delay, [7..10] envelope, [11..14] magnitude / offset / phase / period or the
    // first condition axis; the rest is the second axis.
    int32_t w[29] = {};
    for (uint32_t i = 0; i < words && i < 29; ++i) w[i] = int32_t(RdU32(base, effect_ptr + 4 * i));
    LARECOMP_APP_INFO(
        "[wheel] ffb {} user={} id={} type={}({}) dur={} delay={} env=[{},{},{},{}] "
        "p=[{},{},{},{}] p2=[{},{},{},{},{},{}]",
        what, user, w[0], w[1], EffectTypeName(uint32_t(w[1])), w[4], w[5], w[7], w[8], w[9], w[10],
        w[11], w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19], w[20]);
}

}  // namespace

// ---------------------------------------------------------------------------
// Capabilities and state
// ---------------------------------------------------------------------------

extern "C" REX_FUNC(rex_sub_8275CB70) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t caps = ctx.r5.u32;
    __imp__rex_sub_8275CB70(ctx, base);
    if (!WheelOn() || !IsWheelUser(user) || caps == 0) return;

    // Whatever the SDK answered, user 0 now has a wheel: success, gamepad type, wheel subtype,
    // force feedback. The rest of the structure (gamepad mask, vibration) is left as the
    // original wrote it, or cleared if it failed.
    if (ctx.r3.u32 != 0) {
        std::memset(base + caps, 0, 20);
        ctx.r3.u64 = 0;
    }
    base[caps + 0] = kTypeGamepad;
    base[caps + 1] = kSubTypeWheel;
    WrU16(base, caps + 2, uint16_t(RdU16(base, caps + 2) | kCapsFfb));

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        LARECOMP_APP_INFO("[wheel] user 0 reports a force-feedback wheel (type 1, subtype 2, "
                          "flags 0x{:04X}); source: {}",
                          RdU16(base, caps + 2),
                          CurrentMode() == FakeMode::kOff ? "a real wheel (SDL)"
                                                          : REXCVAR_GET(wheel_fake));
    }
}

extern "C" REX_FUNC(rex_sub_8275CB78) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t state = ctx.r4.u32;
    __imp__rex_sub_8275CB78(ctx, base);
    const FakeMode mode = CurrentMode();
    if (!IsWheelUser(user) || state == 0) return;
    if (mode == FakeMode::kOff && !larecomp::wheel::WheelPresent()) return;

    if (ctx.r3.u32 != 0) {
        // Nothing connected underneath: still a wheel, at rest.
        std::memset(base + state, 0, 16);
        ctx.r3.u64 = 0;
    }

    if (mode == FakeMode::kOff) {
        // A real wheel: SDL joystick -> the state a 360 wheel reports (wheel_input.cpp).
        larecomp::wheel::PadIn pad;
        pad.buttons = RdU16(base, state + kStButtons);
        pad.lt = base[state + kStLt];
        pad.rt = base[state + kStRt];
        pad.lx = int16_t(RdU16(base, state + kStLx));
        pad.ly = int16_t(RdU16(base, state + kStLy));
        pad.rx = int16_t(RdU16(base, state + kStRx));
        pad.ry = int16_t(RdU16(base, state + kStRy));
        if (larecomp::wheel::ApplyToState(pad, SelectedMapIsManual(base))) {
            WrU16(base, state + kStButtons, pad.buttons);
            base[state + kStLt] = pad.lt;
            base[state + kStRt] = pad.rt;
            WrU16(base, state + kStLx, uint16_t(pad.lx));
            WrU16(base, state + kStLy, uint16_t(pad.ly));
            WrU16(base, state + kStRx, uint16_t(pad.rx));
            WrU16(base, state + kStRy, uint16_t(pad.ry));
        }
        return;
    }

    // A wheel owns the left stick's X axis and the two pedals; LY and the right stick stay centred.
    WrU16(base, state + kStLy, 0);
    WrU16(base, state + kStRx, 0);
    WrU16(base, state + kStRy, 0);

    if (mode != FakeMode::kKeys) return;

    float target = 0.0f;
    uint16_t buttons = RdU16(base, state + kStButtons);
    uint8_t lt = base[state + kStLt];
    uint8_t rt = base[state + kStRt];
    if (GameWindowHasFocus()) {
        if (KeyDown('A')) target -= 1.0f;
        if (KeyDown('D')) target += 1.0f;
        rt = KeyDown('W') ? 255 : 0;
        lt = KeyDown('S') ? 255 : 0;
        buttons = uint16_t(buttons & ~(kBtnLb | kBtnRb));
        if (KeyDown('Q')) buttons |= kBtnLb;
        if (KeyDown('E')) buttons |= kBtnRb;
    } else {
        rt = lt = 0;
    }
    const float steer = StepKeySteer(target);
    WrU16(base, state + kStLx, uint16_t(int16_t(std::lround(steer * 32767.0f))));
    WrU16(base, state + kStButtons, buttons);
    base[state + kStLt] = lt;
    base[state + kStRt] = rt;
}

// ---------------------------------------------------------------------------
// Force feedback
// ---------------------------------------------------------------------------

// GetDeviceInfo(user, info): info[0] = the number of effects the device can hold. The game
// clamps it to 64 (sub_82466470) and uses 9 slots per wheel; the rest is headroom.
extern "C" REX_FUNC(rex_sub_8275C980) {
    if (!WheelOn() || !IsWheelUser(ctx.r3.u32) || ctx.r4.u32 == 0) {
        __imp__rex_sub_8275C980(ctx, base);
        return;
    }
    const uint32_t info = ctx.r4.u32;
    std::memset(base + info, 0, 16);
    WrU32(base, info, 16);
    LARECOMP_APP_INFO("[wheel] ffb GetDeviceInfo user={} -> max effects 16", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

// DeviceControl(user, op): op 4 is the game's "reset the device, drop every effect"; it is called
// when the wheel is set up and when the FF objects are torn down.
extern "C" REX_FUNC(rex_sub_8275CB08) {
    if (!WheelOn() || !IsWheelUser(ctx.r3.u32)) {
        __imp__rex_sub_8275CB08(ctx, base);
        return;
    }
    if (FfbLogOn()) {
        LARECOMP_APP_INFO("[wheel] ffb DeviceControl user={} arg=0x{:X} (reset: all effects dropped)",
                          ctx.r3.u32, ctx.r4.u32);
    }
    if (RealWheel()) larecomp::wheel::ffb::PostReset();
    ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(rex_sub_8275C9E0) {
    if (!WheelOn() || !IsWheelUser(ctx.r3.u32) || ctx.r4.u32 == 0) {
        __imp__rex_sub_8275C9E0(ctx, base);
        return;
    }
    FfbLog("SetEffect", ctx.r3.u32, base, ctx.r4.u32, 21);
    if (RealWheel()) {
        int32_t w[larecomp::wheel::ffb::kEffectWords];
        for (int i = 0; i < larecomp::wheel::ffb::kEffectWords; ++i) {
            w[i] = int32_t(RdU32(base, ctx.r4.u32 + 4 * i));
        }
        larecomp::wheel::ffb::PostEffect(false, w);
    }
    ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(rex_sub_8275CA40) {
    if (!WheelOn() || !IsWheelUser(ctx.r3.u32) || ctx.r4.u32 == 0) {
        __imp__rex_sub_8275CA40(ctx, base);
        return;
    }
    FfbLog("UpdateEffect", ctx.r3.u32, base, ctx.r4.u32, 21);
    if (RealWheel()) {
        int32_t w[larecomp::wheel::ffb::kEffectWords];
        for (int i = 0; i < larecomp::wheel::ffb::kEffectWords; ++i) {
            w[i] = int32_t(RdU32(base, ctx.r4.u32 + 4 * i));
        }
        larecomp::wheel::ffb::PostEffect(true, w);
    }
    ctx.r3.u64 = 0;
}

// EffectOperation(user, {id, op}, ...): op 2 plays the effect, op 1 stops it. (From the game's own use: after
// creating or updating an effect sub_82467A18 sends op 2; destroying one sends op 1 first; pausing sends op 1 to
// every playing effect and resuming op 2. This was backwards for a while and no effect ever counted as playing.)
extern "C" REX_FUNC(rex_sub_8275CAA0) {
    if (!WheelOn() || !IsWheelUser(ctx.r3.u32) || ctx.r4.u32 == 0) {
        __imp__rex_sub_8275CAA0(ctx, base);
        return;
    }
    if (RealWheel()) {
        larecomp::wheel::ffb::PostOperation(int32_t(RdU32(base, ctx.r4.u32)),
                                            RdU32(base, ctx.r4.u32 + 4) == 2);
    }
    if (FfbLogOn() && g_ffb_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const uint32_t id = RdU32(base, ctx.r4.u32);
        const uint32_t op = RdU32(base, ctx.r4.u32 + 4);
        LARECOMP_APP_INFO("[wheel] ffb EffectOperation user={} id={} op={}", ctx.r3.u32, id,
                          op == 2 ? "play" : (op == 1 ? "stop" : "?"));
    }
    ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Input frame counter
// ---------------------------------------------------------------------------

// sub_8260C4D0 is the game's per-frame input update: keyboard, then the pad poll for every pad
// (sub_826150F0 -> sub_82614D30 -> the state hook above), then its callbacks.
extern "C" REX_FUNC(rex_sub_8260C4D0) {
    larecomp::wheel::NoteInputFrame();
    __imp__rex_sub_8260C4D0(ctx, base);
}

// ---------------------------------------------------------------------------
// The car, from the HUD
// ---------------------------------------------------------------------------

// Midasm hook at 0x822118D4 (the bl to sub_8223FF00 in the HUD update, sub_822112A8): r4 = the
// gear (-1 reverse, 0 neutral, 1..N), r5 = the vehicle the HUD shows. Speed is the length of the
// velocity vector at *(vehicle+0x80)+0x110. Read-only.
void Hook_WheelHudGear(PPCRegister& r4, PPCRegister& r5) {
    const uint8_t* base = rex::Runtime::instance()->virtual_membase();
    if (!base) return;
    float speed = 0.0f;
    const uint32_t veh = r5.u32;
    if (veh > 0x10000) {
        const uint32_t phys = RdU32(base, veh + 0x80);
        if (phys > 0x10000) {
            const uint32_t raw[3] = {RdU32(base, phys + 0x110), RdU32(base, phys + 0x114),
                                     RdU32(base, phys + 0x118)};
            float v[3];
            std::memcpy(v, raw, sizeof(v));
            const float m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (std::isfinite(m)) speed = m;
        }
    }
    larecomp::wheel::NoteHudGear(int(r4.s32), speed);
}

#endif  // REXGLUE_HAS_XEO3_TARGET
