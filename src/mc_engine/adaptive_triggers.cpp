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
// No thread of its own and nothing registered with Windows: the work is done by a per-frame
// tick on the game's thread, every 50 ms, and writes only when something changed (a vibration
// at most every 100 ms). The pads are looked for every few seconds while no car is driven (a
// scan is a few milliseconds), so a pad plugged in during a race is picked up at the next menu.
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
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
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
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#else
#include <SDL3/SDL.h>
#include <rex/input/input_system.h>
#include <rex/ui/window.h>
#include "trigger_sdl.h"
#endif

#include "hooks/hooks_internal.h"
#include "larecomp_log.h"
#include "wheel_slip.h"

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
constexpr int64_t kScanMs = 5000;
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

std::mutex g_mutex;  // the tick (game thread) against Shutdown (the window's thread)
std::vector<Pad> g_pads;
int64_t g_last_tick_ms = 0;
int64_t g_next_scan_ms = 0;
int g_failures = 0;
int g_logged = 0;
bool g_shut_down = false;
bool g_migrated = false;
VibrationState g_r2_vibration;
VibrationState g_l2_vibration;

#if defined(_WIN32)
// Paths of the connected DualSenses: the HID interfaces whose path names Sony's vendor id and the
// DualSense or DualSense Edge product id. Nothing is opened here.
std::vector<std::wstring> FindPads() {
    std::vector<std::wstring> found;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA item = {};
    item.cbSize = sizeof(item);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &item); ++i) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
        if (size == 0) continue;
        std::vector<uint8_t> raw(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, nullptr)) continue;
        std::wstring path = detail->DevicePath;
        for (wchar_t& c : path) c = wchar_t(std::towlower(c));
        if (path.find(L"054c") != std::wstring::npos &&
            (path.find(L"0ce6") != std::wstring::npos || path.find(L"0df2") != std::wstring::npos)) {
            found.push_back(path);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

void ClosePad(Pad& pad) {
    if (pad.handle != INVALID_HANDLE_VALUE) CloseHandle(pad.handle);
    pad.handle = INVALID_HANDLE_VALUE;
}

// Sends the report for the given state to the pad; false if Windows refused it.
bool Send(Pad& pad, const Effect& r2, const Effect& l2) {
    uint8_t report[78];
    const size_t size = BuildReport(pad.bluetooth, r2, l2, report);
    std::vector<uint8_t> buffer(std::max(pad.report_size, size), 0);
    std::memcpy(buffer.data(), report, size);
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool ok = WriteFile(pad.handle, buffer.data(), DWORD(buffer.size()), nullptr, &overlapped) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        // A pad that does not answer in 50 ms is left alone: it costs one dropped frame, not a stall.
        DWORD written = 0;
        if (WaitForSingleObject(overlapped.hEvent, 50) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(pad.handle, &overlapped, &written, FALSE) != 0;
        } else {
            CancelIoEx(pad.handle, &overlapped);
            GetOverlappedResult(pad.handle, &overlapped, &written, TRUE);
        }
    }
    CloseHandle(overlapped.hEvent);
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
bool Send(Pad& pad, const Effect& r2, const Effect& l2) {
    uint8_t state[47];
    BuildState(r2, l2, state);
    if (SDL_SendGamepadEffect(pad.gamepad, state, int(sizeof(state)))) return true;
    // CHECK: never run against a DualSense on Linux. "not supported" here means SDL has the pad
    // through the kernel's evdev driver (no access to /dev/hidraw*) or as Steam Input's virtual pad,
    // not through its HIDAPI driver. The caller drops the pad, and the scan finds it again; after 5
    // refusals the effects give up.
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

#if !defined(_WIN32)
// Opens the DualSenses that appeared and closes the ones that went. SDL's own list of pads is
// current without any call here: the SDK's driver pumps SDL's events, which updates the joysticks.
void Scan() {
    if (g_failures >= 5) return;
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
#else
// Opens the pads that appeared and closes the ones that went.
void Scan() {
    if (g_failures >= 5) return;
    const std::vector<std::wstring> present = FindPads();
    for (const std::wstring& path : present) {
        if (std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad& p) { return p.path == path; })) continue;
        Pad pad;
        pad.path = path;
        pad.handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pad.handle == INVALID_HANDLE_VALUE) {
            if (g_logged++ < 8) LARECOMP_APP_WARN("[Triggers] could not open the DualSense (error {})", GetLastError());
            continue;
        }
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps = {};
        if (HidD_GetPreparsedData(pad.handle, &preparsed)) {
            HidP_GetCaps(preparsed, &caps);
            HidD_FreePreparsedData(preparsed);
        }
        pad.report_size = caps.OutputReportByteLength;
        // The Bluetooth interface is named for the HID service ({0000...}); its report is 78 bytes.
        pad.bluetooth = path.find(L"#{0000") != std::wstring::npos || pad.report_size >= 78;
        LARECOMP_APP_INFO("[Triggers] DualSense found ({}, reports of {} bytes)",
                          pad.bluetooth ? "Bluetooth" : "USB", pad.report_size);
        g_pads.push_back(std::move(pad));
    }
    for (size_t i = 0; i < g_pads.size();) {
        if (std::find(present.begin(), present.end(), g_pads[i].path) == present.end()) {
            ClosePad(g_pads[i]);
            g_pads.erase(g_pads.begin() + long(i));
        } else {
            ++i;
        }
    }
}
#endif

}  // namespace

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre); works at most every 50 ms.
void Tick() {
    const int64_t now = NowMs();
    if (now - g_last_tick_ms < kTickMs) return;
    g_last_tick_ms = now;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shut_down) return;
    ReadOldSetting();
    const bool resistance = REXCVAR_GET(trigger_resistance);
    const bool vibration = REXCVAR_GET(trigger_vibration);
    if (!resistance && !vibration) {
        // Both off: let go of what was set and give the pads back.
        for (Pad& pad : g_pads) {
            Release(pad);
            ClosePad(pad);
        }
        g_pads.clear();
        g_next_scan_ms = 0;
        return;
    }
    const bool driving = Driving();
    if (!driving && now >= g_next_scan_ms) {
        g_next_scan_ms = now + kScanMs;
        Scan();
    }
    const bool engaged = driving && WindowInFront();
    Effect r2 = Off();
    Effect l2 = Off();
    bool vibrating = false;
    if (engaged) {
        if (resistance) {
            r2 = Slope(1, 9, 1, REXCVAR_GET(trigger_throttle));
            l2 = Slope(2, 8, 2, REXCVAR_GET(trigger_brake));
        }
        if (vibration) {
            // A trigger holds one effect: the vibration replaces the spring or the brake while
            // there is slip.
            const larecomp::slip::Amounts slip = larecomp::slip::Current();
            if (const int strength = VibrationStrength(g_r2_vibration, slip.wheelspin, now)) {
                r2 = Vibration(1, strength, 30);
                vibrating = true;
            }
            if (const int strength = VibrationStrength(g_l2_vibration, slip.lockup, now)) {
                l2 = Vibration(1, strength, 25);
                vibrating = true;
            }
        }
    } else {
        g_r2_vibration = {};
        g_l2_vibration = {};
    }
    for (size_t i = 0; i < g_pads.size();) {
        Pad& pad = g_pads[i];
        const bool changed = !pad.sent || pad.r2 != r2 || pad.l2 != l2;
        // A vibration's strength follows the slip at most every kVibrationStepMs; ending it, or
        // any other change, is not held back.
        const bool paced = vibrating && pad.sent && now - pad.written_ms < kVibrationStepMs;
        if (changed && !paced && !Write(pad, r2, l2)) {
            // Refused: the pad is gone or not ours to write to. It is looked for again at the next scan.
            ClosePad(pad);
            g_pads.erase(g_pads.begin() + long(i));
            if (++g_failures >= 5) LARECOMP_APP_WARN("[Triggers] writing to the DualSense failed 5 times; giving up");
            continue;
        }
        ++i;
    }
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shut_down) return;
    g_shut_down = true;
    for (Pad& pad : g_pads) {
        Release(pad);
        ClosePad(pad);
    }
    g_pads.clear();
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
