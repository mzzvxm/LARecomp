// DualSense trigger effects on the PC.
//
// Two effects, each with a setting of its own (trigger_resistance, trigger_vibration):
//   Resistance, the feel of the PS5 build: R2 (the throttle) is a light spring that firms up
//     towards the end of its travel, L2 (the brake) is firmer, and both go slack whenever the car is
//     not being driven (menus, loading, the pause menu, a cutscene, the window not in front).
//   Vibration: R2 vibrates while the driven wheels spin under power, L2 while the brakes lock the
//     wheels, harder the more they slip, as most racing games do. The amounts are the game's own
//     tyre slip (wheel_slip.h), shared with the Xbox trigger rumble (impulse_triggers.cpp). A
//     trigger holds one effect at a time, so while it vibrates the spring or the brake gives way
//     to it and comes back when the slip is over.
// Nothing here reads the car except through those slip amounts: the HUD update (sub_822112A8)
// only runs while a car is driven, and the game's own paused flag says when the menu holds it.
//
// How it reaches the pad, on Windows. Not through the SDK's input driver, and not through SDL: the
// executable and rexruntime.dll each carry their own SDL, and the executable's has the HID
// joystick drivers off (wheel_input.cpp and ui/gamepad.cpp set the hints), so SDL_SendGamepadEffect
// has nothing to talk to there. The output report is written to the pad's HID interface with the
// Windows HID API. A report carries an enable bit per field and only the two trigger bits are
// set, so the rumble the SDK's driver sends keeps working. The layout is the DualSense's own, as
// SDL's PS5 driver writes it: report 0x02 of 48 bytes over USB, report 0x31 of 78 bytes with a
// CRC-32 over Bluetooth, 47 bytes of effect state in both. A trigger block is 11 bytes: a mode
// (0x05 off, 0x21 resistance per zone, 0x26 vibration per zone) and its parameters. A zone is a
// tenth of the travel and gets three bits of strength; a vibration also has a frequency in Hz.
//
// On Linux (and any other non-Windows build) there is one SDL in the process, the SDK's, with its
// HIDAPI drivers on (input_backend is "sdl" there), so the same 47 bytes of effect state go through
// SDL_SendGamepadEffect: SDL's PS5 driver adds the report id, the Bluetooth header and the CRC
// itself and queues the write on its own thread. The state keeps its two enable bits, and SDL's
// own rumble and LED reports never carry those, so neither side undoes the other. The pad is
// opened like any other (an SDL_Gamepad is reference counted, so the SDK's driver and this share
// it) and only a Sony DualSense or DualSense Edge is used. SDL reaches a DualSense for effects only
// through its HIDAPI driver, which opens /dev/hidraw*: a player without Steam's udev rules (or any
// rule that lets the logged-in user use the hidraw node) gets the pad through the kernel's evdev
// driver instead, where SDL has no effects. The same goes for a PlayStation pad that Steam Input
// has taken over (SDL shows Steam's virtual pad with the real pad's ids). A refused write says so
// in the log and the triggers stay as they are. The Steam Deck's own controls are not a DualSense.
//
// Threads. The game's thread does the per-frame tick, every 50 ms: it works out what the triggers
// should be (spring, brake, vibration, or slack) and does nothing else. Nothing that touches a
// device runs on it: no scan, no open, no write.
//   Windows: the tick hands the wish to a worker thread through a latest-wins mailbox
//     (trigger_mailbox.h), and the worker opens and closes the pads and writes the reports
//     (overlapped I/O, waiting up to 50 ms for a pad that is slow to answer: the wait is the
//     worker's own). It is started when the device watcher (device_watch.h) first says a DualSense
//     is plugged in, so a player without one has no extra thread. Which pads exist comes from the
//     watcher's arrival and removal notifications, not from a scan: the worker is woken when the
//     list of DualSense interfaces changes. A write happens only when something changed (a
//     vibration at most every 100 ms), and the newest wish always wins over an older one the
//     worker had not got to yet.
//   Elsewhere: the tick writes (SDL's HIDAPI driver queues the report on its own thread) and looks
//     for pads when the watcher says SDL added or removed a joystick, not on a timer.
// An effect stays in the pad until the next report, so the triggers are let go when both
// settings are off, when the car is not driven, when the window is not in front, and from
// Shutdown(). While a load stops the tick they stay as they were.
//
// The settings used to be one, adaptive_triggers. A settings file that still says
// adaptive_triggers = false is read as both new settings off (once, at the first tick).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "adaptive_triggers.h"

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winternl.h>  // NTSTATUS, which hidpi.h uses
#include <hidsdi.h>
#include <hidpi.h>
#pragma comment(lib, "hid.lib")
#else
#include <SDL3/SDL.h>
#include <rex/input/input_system.h>
#include <rex/ui/window.h>
#include "trigger_sdl.h"
#endif

#include "device_watch.h"
#include "device_watch_core.h"
#include "mc_engine/hooks/hooks_internal.h"
#include "larecomp_log.h"
#include "trigger_mailbox.h"
#include "mc_engine/gameplay/wheel_slip.h"

REXCVAR_DEFINE_BOOL(trigger_resistance, true, "MCLA/Input",
    "DualSense trigger resistance: a throttle spring on R2 and brake resistance on L2 while a car "
    "is driven, slack everywhere else. Other controllers are not touched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(trigger_vibration, true, "MCLA/Input",
    "DualSense trigger vibration: R2 vibrates while the driven wheels spin under power and L2 while "
    "the brakes lock the wheels, harder the more they slip. Other controllers are not touched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The one setting both effects used to share. Only read for old settings files: false there turns
// trigger_resistance and trigger_vibration off at the first tick, and this goes back to true.
REXCVAR_DEFINE_BOOL(adaptive_triggers, true, "MCLA/Input",
    "Old setting, replaced by trigger_resistance and trigger_vibration. False in an older settings "
    "file turns both off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(trigger_throttle, 3, "MCLA/Input",
    "R2 resistance at the end of its travel, 1 (light) to 8 (firm). Used by trigger_resistance.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(trigger_brake, 6, "MCLA/Input",
    "L2 resistance at the end of its travel, 1 (light) to 8 (firm). Used by trigger_resistance.")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using Clock = std::chrono::steady_clock;

// The HUD updates every frame while a car is driven; this long without one is "not driving".
constexpr int64_t kHudFreshMs = 300;
constexpr int64_t kTickMs = 50;
// A vibration changes its strength at most this often, and holds its last strength this long
// after the slip it follows has gone, so a slip that comes and goes does not make it flicker.
constexpr int64_t kVibrationStepMs = 100;
constexpr int64_t kVibrationHoldMs = 150;
// The slip amount (0 to 1) from which a trigger vibrates, and the vibration's strength range
// (zone strengths are 1 to 8; the lightest are hard to feel).
constexpr float kVibrationFrom = 0.05f;
constexpr int kVibrationMin = 2;
constexpr int kVibrationMax = 7;
// Pointer to the pause manager, whose +4 byte is the game's paused flag and +10 the
// run-one-frame-while-paused bit (what Game_IsPaused, sub_822BF228, reads).
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

std::atomic<int64_t> g_hud_ms{0};  // when the HUD last updated

bool GamePaused() {
    rex::Runtime* rt = rex::Runtime::instance();
    const uint8_t* base = rt ? rt->virtual_membase() : nullptr;
    if (!base) return false;
    const uint32_t p = ReadGuestU32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;
}

bool Driving() {
    const int64_t hud = g_hud_ms.load(std::memory_order_relaxed);
    return hud != 0 && NowMs() - hud <= kHudFreshMs && !GamePaused();
}

bool WindowInFront() {
#if defined(_WIN32)
    HWND front = GetForegroundWindow();
    DWORD pid = 0;
    if (front) GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
#else
    // The SDK's window keeps its focus flag from SDL's focus events (the same flag the cursor hiding
    // reads). A compositor that is showing something over the game (Steam's overlay or its Quick
    // Access menu under gamescope, another window) takes the keyboard focus, so the triggers go
    // slack then. No window yet (or none to ask) counts as in front: nothing is driven without one.
    // CHECK: that the flag follows a click on another window under X11 and Wayland, and the Steam
    // overlay on a Steam Deck; a flag that stays false would keep the triggers slack for good.
    rex::Runtime* rt = rex::Runtime::instance();
    auto* input = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
    rex::ui::Window* window = input ? input->window() : nullptr;
    return window ? window->HasFocus() : true;
#endif
}

// --- The effects ---------------------------------------------------------------------------

struct Effect {
    uint8_t b[11] = {};
    bool operator==(const Effect& o) const { return std::memcmp(b, o.b, sizeof(b)) == 0; }
    bool operator!=(const Effect& o) const { return !(*this == o); }
};

Effect Off() {
    Effect e;
    e.b[0] = 0x05;
    return e;
}

// Resistance rising from s0 at zone `start` to s1 at zone `end` (zones 0 to 9, strengths 1 to
// 8), then s1 to the end of the travel; the zones before `start` stay free.
Effect Slope(int start, int end, int s0, int s1) {
    s0 = std::clamp(s0, 1, 8);
    s1 = std::clamp(s1, 1, 8);
    const float step = end > start ? float(s1 - s0) / float(end - start) : 0.0f;
    uint32_t force = 0;
    uint32_t active = 0;
    for (int zone = start; zone < 10; ++zone) {
        const int strength = zone > end ? s1 : int(std::lround(float(s0) + step * float(zone - start)));
        force |= uint32_t((strength - 1) & 7) << (3 * zone);
        active |= 1u << zone;
    }
    Effect e;
    e.b[0] = 0x21;
    e.b[1] = uint8_t(active & 0xFF);
    e.b[2] = uint8_t(active >> 8);
    for (int i = 0; i < 4; ++i) e.b[3 + i] = uint8_t(force >> (8 * i));
    return e;
}

// Vibration from zone `start` to the end of the travel, strength 1 to 8, `frequency` in Hz (mode
// 0x26: the zone bytes are the resistance's, the frequency is byte 9; the layout is the one the
// fork's pc_triggers.cpp writes).
// CHECK: never run against a DualSense from this code. In a burnout R2 should buzz and give the
// spring up, and under a locked-wheel stop L2 should; a trigger that stays on its spring or goes
// dead means these bytes or the zone start (1) need another value.
Effect Vibration(int start, int strength, int frequency) {
    strength = std::clamp(strength, 1, 8);
    uint32_t force = 0;
    uint32_t active = 0;
    for (int zone = std::clamp(start, 0, 9); zone < 10; ++zone) {
        force |= uint32_t((strength - 1) & 7) << (3 * zone);
        active |= 1u << zone;
    }
    Effect e;
    e.b[0] = 0x26;
    e.b[1] = uint8_t(active & 0xFF);
    e.b[2] = uint8_t(active >> 8);
    for (int i = 0; i < 4; ++i) e.b[3 + i] = uint8_t(force >> (8 * i));
    e.b[9] = uint8_t(std::clamp(frequency, 1, 255));
    return e;
}

const char* Describe(const Effect& e) {
    switch (e.b[0]) {
        case 0x05: return "free";
        case 0x21: return "resistance";
        case 0x26: return "vibration";
        default: return "?";
    }
}

// How strongly a trigger vibrates for a slip amount (0 = not at all). It follows the amount up
// and down while there is slip, and keeps its last strength for kVibrationHoldMs after it.
struct VibrationState {
    int strength = 0;
    int64_t until_ms = 0;
};

int VibrationStrength(VibrationState& v, float amount, int64_t now) {
    if (amount >= kVibrationFrom) {
        v.strength = kVibrationMin + int(std::lround(amount * float(kVibrationMax - kVibrationMin)));
        v.until_ms = now + kVibrationHoldMs;
        return v.strength;
    }
    if (now < v.until_ms) return v.strength;
    v.strength = 0;
    return 0;
}

// The 47 bytes of effect state that set both trigger blocks and nothing else (the enable bits of
// every other field stay 0). SDL's DS5EffectsState_t has the same layout: the enable bits first,
// the right trigger block at byte 10 and the left at byte 21.
void BuildState(const Effect& r2, const Effect& l2, uint8_t (&state)[47]) {
    std::memset(state, 0, sizeof(state));
    state[0] = 0x04 | 0x08;  // enable: right trigger, left trigger
    std::memcpy(state + 10, r2.b, sizeof(r2.b));
    std::memcpy(state + 21, l2.b, sizeof(l2.b));
}

#if defined(_WIN32)
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// The output report that sets both trigger blocks and nothing else; returns its size.
size_t BuildReport(bool bluetooth, const Effect& r2, const Effect& l2, uint8_t (&out)[78]) {
    uint8_t state[47];
    BuildState(r2, l2, state);
    std::memset(out, 0, sizeof(out));
    if (!bluetooth) {
        out[0] = 0x02;
        std::memcpy(out + 1, state, sizeof(state));
        return 48;
    }
    out[0] = 0x31;
    out[1] = 0x00;  // tag and sequence
    out[2] = 0x10;
    std::memcpy(out + 3, state, sizeof(state));
    const uint8_t seed = 0xA2;
    const uint32_t crc = ~Crc32(Crc32(0xFFFFFFFFu, &seed, 1), out, 74);
    for (int i = 0; i < 4; ++i) out[74 + i] = uint8_t(crc >> (8 * i));
    return 78;
}
#endif  // _WIN32

// --- The pads --------------------------------------------------------------------------------

struct Pad {
#if defined(_WIN32)
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;  // completion event of this pad's writes, made once and kept for as long as the pad is open
    bool bluetooth = false;
    size_t report_size = 0;  // what Windows wants written: the report padded to this
#else
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;  // a reference of its own to the pad the SDK's driver also has open
#endif
    bool sent = false;       // anything written yet
    Effect r2;               // what the triggers were last set to
    Effect l2;
    int64_t written_ms = 0;  // when
};

// Writes refused in a row after which no more pads are opened (the pads already open keep working).
constexpr int kGiveUpAfter = 5;

// Windows: touched by the effect worker only. Elsewhere: by the game's thread, under g_mutex.
int g_failures = 0;
int g_logged = 0;

std::atomic<bool> g_shut_down{false};
int64_t g_last_tick_ms = 0;  // game thread
bool g_migrated = false;     // game thread
VibrationState g_r2_vibration;  // game thread
VibrationState g_l2_vibration;

#if defined(_WIN32)
void ClosePad(Pad& pad) {
    if (pad.handle != INVALID_HANDLE_VALUE) CloseHandle(pad.handle);
    pad.handle = INVALID_HANDLE_VALUE;
    if (pad.event) CloseHandle(pad.event);
    pad.event = nullptr;
}

// Opens one DualSense interface for effect writes (the worker's thread: opening a HID device and
// asking for its report size are calls that can take a while). False if it could not be opened.
bool OpenPad(const std::wstring& path, Pad& pad) {
    pad.path = path;
    pad.handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pad.handle == INVALID_HANDLE_VALUE) {
        if (g_logged++ < 8) LARECOMP_APP_WARN("[Triggers] could not open the DualSense (error {})", GetLastError());
        return false;
    }
    pad.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!pad.event) {
        if (g_logged++ < 8) LARECOMP_APP_WARN("[Triggers] could not make an event for the DualSense (error {})", GetLastError());
        ClosePad(pad);
        return false;
    }
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    HIDP_CAPS caps = {};
    if (HidD_GetPreparsedData(pad.handle, &preparsed)) {
        HidP_GetCaps(preparsed, &caps);
        HidD_FreePreparsedData(preparsed);
    }
    pad.report_size = caps.OutputReportByteLength;
    // The Bluetooth interface is named for the HID service ({0000...}); its report is 78 bytes.
    pad.bluetooth = larecomp::devwatch::ClassifyLink(path).bluetooth || pad.report_size >= 78;
    LARECOMP_APP_INFO("[Triggers] DualSense found ({}, reports of {} bytes)",
                      pad.bluetooth ? "Bluetooth" : "USB", pad.report_size);
    return true;
}

// Sends the report for the given state to the pad; false if Windows refused it. Waits for the pad
// (up to 50 ms): only ever called on the worker's thread.
bool Send(Pad& pad, const Effect& r2, const Effect& l2) {
    uint8_t report[78];
    const size_t size = BuildReport(pad.bluetooth, r2, l2, report);
    std::vector<uint8_t> buffer(std::max(pad.report_size, size), 0);
    std::memcpy(buffer.data(), report, size);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = pad.event;
    ResetEvent(pad.event);
    bool ok = WriteFile(pad.handle, buffer.data(), DWORD(buffer.size()), nullptr, &overlapped) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        // A pad that does not answer in 50 ms is left alone: it costs one dropped report, not a stall.
        DWORD written = 0;
        if (WaitForSingleObject(overlapped.hEvent, 50) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pad.handle, &overlapped, &written, FALSE) != 0;
        } else {
            CancelIoEx(pad.handle, &overlapped);
            GetOverlappedResult(pad.handle, &overlapped, &written, TRUE);
        }
    }
    return ok;
}
#else
// A Sony DualSense or DualSense Edge, by its ids, that SDL did not make up itself. Under Steam Input
// SDL reports the real pad's ids for Steam's virtual pad as well; that pad refuses effects (see Send).
bool IsDualSense(SDL_JoystickID id) {
    if (SDL_IsJoystickVirtual(id)) return false;
    const Uint16 product = SDL_GetGamepadProductForID(id);
    return SDL_GetGamepadVendorForID(id) == 0x054C && (product == 0x0CE6 || product == 0x0DF2);
}

void ClosePad(Pad& pad) {
    if (pad.gamepad) SDL_CloseGamepad(pad.gamepad);
    pad.gamepad = nullptr;
}

// Sends the effect state for the given triggers to the pad through SDL; false if SDL refused it.
// SDL's HIDAPI driver queues the report for its own rumble thread and returns, so this does not wait
// for the pad. CHECK: never run against a DualSense on Linux.
bool Send(Pad& pad, const Effect& r2, const Effect& l2) {
    uint8_t state[47];
    BuildState(r2, l2, state);
    if (SDL_SendGamepadEffect(pad.gamepad, state, int(sizeof(state)))) return true;
    // CHECK: never run against a DualSense on Linux. "not supported" here means SDL has the pad
    // through the kernel's evdev driver (no access to /dev/hidraw*) or as Steam Input's virtual pad,
    // not through its HIDAPI driver. The caller drops the pad, and a later look finds it again; after
    // 5 refusals the effects give up.
    if (g_logged++ < 8) {
        LARECOMP_APP_WARN("[Triggers] SDL would not send the effect to the DualSense: {}. SDL can only do "
                          "that with the pad open through its HIDAPI driver (/dev/hidraw*, which the "
                          "player needs access to; Steam's udev rules give it), not through the kernel's "
                          "evdev device or Steam Input's virtual pad", SDL_GetError());
    }
    return false;
}
#endif

// Writes the state for the given triggers to the pad; false if it was refused.
bool Write(Pad& pad, const Effect& r2, const Effect& l2) {
    if (!Send(pad, r2, l2)) return false;
    g_failures = 0;
    if ((pad.r2.b[0] != r2.b[0] || pad.l2.b[0] != l2.b[0]) && g_logged++ < 12) {
        LARECOMP_APP_INFO("[Triggers] R2 {}, L2 {}", Describe(r2), Describe(l2));
    }
    pad.sent = true;
    pad.r2 = r2;
    pad.l2 = l2;
    pad.written_ms = NowMs();
    return true;
}

// Lets both triggers go, if the pad may hold an effect.
void Release(Pad& pad) {
    if (pad.sent && (pad.r2 != Off() || pad.l2 != Off())) Write(pad, Off(), Off());
}

// An older settings file has the one setting both effects used to share. False there means the
// player had the triggers off: both new settings follow, and the old one goes back to its default
// so this does not happen again.
void ReadOldSetting() {
    if (g_migrated) return;
    g_migrated = true;
    if (REXCVAR_GET(adaptive_triggers)) return;
    // CHECK: the write goes through the cvar layer from the game's thread; after it larecomp.toml
    // should lose adaptive_triggers = false and gain the two new keys at the next settings save.
    LARECOMP_APP_INFO("[Triggers] adaptive_triggers is false in the settings: trigger_resistance "
                      "and trigger_vibration are set to false");
    rex::cvar::SetFlagByName("trigger_resistance", "false");
    rex::cvar::SetFlagByName("trigger_vibration", "false");
    rex::cvar::SetFlagByName("adaptive_triggers", "true");
}

// What the tick wants the triggers to be, worked out from the settings, the car and the window.
// `vibrating` says one of the effects is a vibration (written at most every kVibrationStepMs).
struct Wish {
    bool enabled = false;  // at least one of the two settings is on; false = let the triggers go and give the pads back
    bool vibrating = false;
    Effect r2 = Off();
    Effect l2 = Off();
    bool operator==(const Wish& o) const {
        return enabled == o.enabled && vibrating == o.vibrating && r2 == o.r2 && l2 == o.l2;
    }
    bool operator!=(const Wish& o) const { return !(*this == o); }
};

// Reads the settings and the game and works out the wish. Game thread, every kTickMs.
Wish WorkOutWish(int64_t now) {
    Wish wish;
    const bool resistance = REXCVAR_GET(trigger_resistance);
    const bool vibration = REXCVAR_GET(trigger_vibration);
    if (!resistance && !vibration) return wish;
    wish.enabled = true;
    const bool driving = Driving();
    const bool engaged = driving && WindowInFront();
    if (engaged) {
        if (resistance) {
            wish.r2 = Slope(1, 9, 1, REXCVAR_GET(trigger_throttle));
            wish.l2 = Slope(2, 8, 2, REXCVAR_GET(trigger_brake));
        }
        if (vibration) {
            // A trigger holds one effect: the vibration replaces the spring or the brake while
            // there is slip.
            const larecomp::slip::Amounts slip = larecomp::slip::Current();
            if (const int strength = VibrationStrength(g_r2_vibration, slip.wheelspin, now)) {
                wish.r2 = Vibration(1, strength, 30);
                wish.vibrating = true;
            }
            if (const int strength = VibrationStrength(g_l2_vibration, slip.lockup, now)) {
                wish.l2 = Vibration(1, strength, 25);
                wish.vibrating = true;
            }
        }
    } else {
        g_r2_vibration = {};
        g_l2_vibration = {};
    }
    return wish;
}

#if defined(_WIN32)

// --- The effect worker (Windows) -------------------------------------------------------------
// CHECK: never run against a DualSense from this code. Opening the interface, the report size, the
// report bytes (BuildReport is unchanged) and the overlapped write with its 50 ms wait are the old
// code moved to the worker's thread; what is new is the mailbox, the pacing decision (DecideWrite,
// tested) and the retry after a refused open or write.

// A pad that would not open is tried again after this, a write that was refused after that.
constexpr int64_t kOpenRetryMs = 2000;
constexpr int64_t kWriteRetryMs = 5000;
// Shutdown waits this long for the worker to let the triggers go (a write waits up to 50 ms a pad).
constexpr int kShutdownWaitMs = 600;

struct Worker {
    larecomp::triggers::LatestMailbox<Wish> mailbox;  // game thread -> worker, latest wins
    std::atomic<bool> started{false};
    std::mutex mutex;  // guards thread and done, and the start against Shutdown
    std::condition_variable done_cv;
    bool done = false;
    std::thread* thread = nullptr;
};

Worker& W() {
    // Never destroyed: the worker may still be running when the statics go (a process that ends
    // through exit() without Shutdown), and must not find its mailbox gone.
    static Worker* worker = new Worker();
    return *worker;
}

Wish g_posted;  // game thread: the last wish handed to the worker

// Opens the pads of DualSenses that appeared and closes the ones that went, from the watcher's
// list (worker's thread). A pad that would not open is tried again a little later, a few times.
void SyncPads(std::vector<Pad>& pads, larecomp::devwatch::RetryBackoff& retry, int64_t now) {
    const std::vector<std::wstring> present = larecomp::devwatch::DualSensePaths();
    retry.Retain(present);
    for (size_t i = 0; i < pads.size();) {
        if (std::find(present.begin(), present.end(), pads[i].path) == present.end()) {
            ClosePad(pads[i]);
            pads.erase(pads.begin() + long(i));
        } else {
            ++i;
        }
    }
    if (g_failures >= kGiveUpAfter) return;
    for (const std::wstring& path : present) {
        if (std::any_of(pads.begin(), pads.end(), [&](const Pad& p) { return p.path == path; })) continue;
        if (!retry.Allowed(path, now)) continue;
        Pad pad;
        if (OpenPad(path, pad)) {
            retry.Clear(path);
            pads.push_back(std::move(pad));
        } else {
            retry.Failed(path, now, kOpenRetryMs);
        }
    }
}

void WorkerMain() {
    Worker& worker = W();
    std::vector<Pad> pads;
    larecomp::devwatch::RetryBackoff retry(kGiveUpAfter);
    Wish wish;
    uint32_t seen_generation = 0xFFFFFFFFu;  // the watcher's list as it was when the pads were last synced
    int64_t wake_at_ms = -1;                 // a paced write or a retry is due then (-1: nothing waits)
    for (;;) {
        std::chrono::milliseconds timeout{-1};
        if (wake_at_ms >= 0) timeout = std::chrono::milliseconds(std::max<int64_t>(0, wake_at_ms - NowMs()));
        const auto wake = worker.mailbox.Wait(wish, timeout);
        if (wake.closed) break;
        const int64_t now = NowMs();
        wake_at_ms = -1;
        const auto wake_by = [&](int64_t at) {
            if (at >= 0 && (wake_at_ms < 0 || at < wake_at_ms)) wake_at_ms = at;
        };

        if (!wish.enabled) {
            // Both settings off (or the first wish has not come): let go of what was set and give
            // the pads back. They are opened again when the settings come on.
            for (Pad& pad : pads) {
                Release(pad);
                ClosePad(pad);
            }
            pads.clear();
            retry.ClearAll();
            seen_generation = 0xFFFFFFFFu;
            continue;
        }

        const uint32_t generation = larecomp::devwatch::Generation();
        if (generation != seen_generation || retry.NextDueMs(now) == 0) {
            seen_generation = generation;
            SyncPads(pads, retry, now);
        }
        if (g_failures < kGiveUpAfter) {
            const int64_t retry_in = retry.NextDueMs(now);
            if (retry_in >= 0) wake_by(now + std::max<int64_t>(retry_in, 1));
        }

        for (size_t i = 0; i < pads.size();) {
            Pad& pad = pads[i];
            const larecomp::triggers::WriteDecision decision = larecomp::triggers::DecideWrite(
                pad.sent, pad.r2 == wish.r2 && pad.l2 == wish.l2, wish.vibrating, pad.written_ms, now, kVibrationStepMs);
            if (decision.action == larecomp::triggers::WriteAction::kLater) wake_by(now + decision.wait_ms);
            if (decision.action == larecomp::triggers::WriteAction::kNow && !Write(pad, wish.r2, wish.l2)) {
                // Refused: the pad is gone or not ours to write to. It is opened again a little
                // later, unless the watcher says it left (then its entry goes with the pad).
                retry.Failed(pad.path, now, kWriteRetryMs);
                ClosePad(pad);
                pads.erase(pads.begin() + long(i));
                if (++g_failures >= kGiveUpAfter) {
                    LARECOMP_APP_WARN("[Triggers] writing to the DualSense failed {} times; giving up", kGiveUpAfter);
                }
                continue;
            }
            ++i;
        }
    }
    // Closed (Shutdown): the triggers back to slack, the pads back to Windows.
    for (Pad& pad : pads) {
        Release(pad);
        ClosePad(pad);
    }
    {
        std::lock_guard<std::mutex> lock(worker.mutex);
        worker.done = true;
    }
    worker.done_cv.notify_all();
}

// The watcher's call when a controller came or went (a pool thread of the OS): wake the worker so it
// looks at the list of DualSenses. Nothing else happens here.
void OnDevicesChanged(void*) { W().mailbox.Wake(); }

// Game thread. The worker starts once the watcher has seen a DualSense and lives until Shutdown.
void StartWorker() {
    Worker& worker = W();
    std::lock_guard<std::mutex> lock(worker.mutex);
    if (worker.started.load(std::memory_order_relaxed) || g_shut_down.load(std::memory_order_relaxed)) return;
    larecomp::devwatch::AddChangeListener(&OnDevicesChanged, nullptr);
    worker.thread = new std::thread(WorkerMain);
    worker.started.store(true, std::memory_order_release);
    LARECOMP_APP_INFO("[Triggers] a DualSense is plugged in: the effect worker starts");
}

#else  // !_WIN32

constexpr int64_t kScanMs = 5000;  // a dropped pad is looked for again after this

std::mutex g_mutex;  // the tick (game thread) against Shutdown (the window's thread)
std::vector<Pad> g_pads;
uint32_t g_seen_generation = 0xFFFFFFFFu;  // the watcher's generation when SDL's list was last read
int64_t g_rescan_ms = 0;                   // a pad was dropped: look again after this (0 = no)

// Opens the DualSenses that appeared and closes the ones that went. SDL's own list of pads is
// current without any call here: the SDK's driver pumps SDL's events, which updates the joysticks.
void Scan() {
    if (g_failures >= kGiveUpAfter) return;
    if (!larecomp::trigger_sdl::GamepadLayerUp()) return;
    std::vector<SDL_JoystickID> present;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (IsDualSense(ids[i])) present.push_back(ids[i]);
        }
        SDL_free(ids);
    }
    for (SDL_JoystickID id : present) {
        if (std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad& p) { return p.id == id; })) continue;
        SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
        if (!gamepad) {
            if (g_logged++ < 8) LARECOMP_APP_WARN("[Triggers] could not open the DualSense: {}", SDL_GetError());
            continue;
        }
        Pad pad;
        pad.id = id;
        pad.gamepad = gamepad;
        const SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(gamepad);
        LARECOMP_APP_INFO("[Triggers] DualSense found ({})",
                          link == SDL_JOYSTICK_CONNECTION_WIRELESS ? "Bluetooth"
                          : link == SDL_JOYSTICK_CONNECTION_WIRED  ? "USB"
                                                                   : "link unknown");
        g_pads.push_back(std::move(pad));
    }
    for (size_t i = 0; i < g_pads.size();) {
        if (std::find(present.begin(), present.end(), g_pads[i].id) == present.end() ||
            !SDL_GamepadConnected(g_pads[i].gamepad)) {
            ClosePad(g_pads[i]);
            g_pads.erase(g_pads.begin() + long(i));
        } else {
            ++i;
        }
    }
}

#endif  // _WIN32

}  // namespace

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre); works at most every 50 ms.
void Tick() {
    const int64_t now = NowMs();
    if (now - g_last_tick_ms < kTickMs) return;
    g_last_tick_ms = now;
    if (g_shut_down.load(std::memory_order_acquire)) return;
    ReadOldSetting();
#if defined(_WIN32)
    Worker& worker = W();
    if (!worker.started.load(std::memory_order_acquire)) {
        // No worker until the watcher has seen a DualSense: a player without one pays one atomic read.
        if (larecomp::devwatch::DualSenseCount() == 0) return;
        if (!REXCVAR_GET(trigger_resistance) && !REXCVAR_GET(trigger_vibration)) return;
        StartWorker();
        if (!worker.started.load(std::memory_order_acquire)) return;
        g_posted = Wish();  // the first wish always goes
    }
    const Wish wish = WorkOutWish(now);
    if (wish != g_posted) {
        g_posted = wish;
        worker.mailbox.Post(wish);
    }
#else
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shut_down.load(std::memory_order_relaxed)) return;
    const Wish wish = WorkOutWish(now);
    if (!wish.enabled) {
        // Both off: let go of what was set and give the pads back.
        for (Pad& pad : g_pads) {
            Release(pad);
            ClosePad(pad);
        }
        g_pads.clear();
        g_seen_generation = 0xFFFFFFFFu;  // look again when they come on
        return;
    }
    // SDL added or removed a joystick since the list was last read (the watcher's generation), or a
    // dropped pad is due another try: nothing else makes this look. Only while no car is driven.
    const uint32_t generation = larecomp::devwatch::Generation();
    if (!Driving() && (generation != g_seen_generation || (g_rescan_ms != 0 && now >= g_rescan_ms))) {
        g_seen_generation = generation;
        g_rescan_ms = 0;
        Scan();
    }
    for (size_t i = 0; i < g_pads.size();) {
        Pad& pad = g_pads[i];
        const WriteDecision decision =
            DecideWrite(pad.sent, pad.r2 == wish.r2 && pad.l2 == wish.l2, wish.vibrating, pad.written_ms, now, kVibrationStepMs);
        if (decision.action == WriteAction::kNow && !Write(pad, wish.r2, wish.l2)) {
            // Refused: the pad is gone or not ours to write to. It is looked for again after kScanMs.
            ClosePad(pad);
            g_pads.erase(g_pads.begin() + long(i));
            g_rescan_ms = now + kScanMs;
            if (++g_failures >= kGiveUpAfter) {
                LARECOMP_APP_WARN("[Triggers] writing to the DualSense failed {} times; giving up", kGiveUpAfter);
            }
            continue;
        }
        ++i;
    }
#endif
}

void Shutdown() {
    if (g_shut_down.exchange(true, std::memory_order_acq_rel)) return;
#if defined(_WIN32)
    Worker& worker = W();
    std::unique_lock<std::mutex> lock(worker.mutex);
    if (!worker.started.load(std::memory_order_acquire)) return;
    worker.mailbox.Close();
    // The worker lets the triggers go and closes the pads (a write can wait up to 50 ms for a slow
    // pad); this waits for it, but not for ever: the process ends either way.
    if (worker.done_cv.wait_for(lock, std::chrono::milliseconds(kShutdownWaitMs), [&] { return worker.done; })) {
        worker.thread->join();
        delete worker.thread;
        worker.thread = nullptr;
    } else {
        LARECOMP_APP_WARN("[Triggers] the effect worker did not finish in {} ms; the triggers may stay as they are",
                          kShutdownWaitMs);
        worker.thread->detach();
    }
#else
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Pad& pad : g_pads) {
        Release(pad);
        ClosePad(pad);
    }
    g_pads.clear();
#endif
}

bool IsDriving() { return Driving(); }

bool IsGamePaused() { return GamePaused(); }

bool IsWindowInFront() { return WindowInFront(); }

}  // namespace larecomp::triggers

// 0x822117C0 in sub_822112A8, the race HUD update: it runs once a frame while a car is driven.
// r30 = the HUD's player object; the wheel slip hook (wheel_slip.cpp) finds the player's wheels
// through the car it holds at +8.
void Hook_TriggersHud(PPCRegister& r30) {
    g_hud_ms.store(NowMs(), std::memory_order_relaxed);
    larecomp::slip::NotePlayer(r30.u32);
}
#endif  // REXGLUE_HAS_XEO3_TARGET
