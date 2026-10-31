// DualSense adaptive triggers on the PC.
//
// The feel is the PS5 build's: R2 (the throttle) is a light spring that firms up towards the
// end of its travel, L2 (the brake) is firmer, and both go slack whenever the car is not being
// driven (menus, loading, the pause menu, a cutscene, the window not in front). Nothing here
// reads the car: the HUD update (sub_822112A8) only runs while a car is driven, and the game's
// own paused flag says when the menu holds it.
//
// How it reaches the pad. Not through the SDK's input driver, and not through SDL: the
// executable and rexruntime.dll each carry their own SDL, and the executable's has the HID
// joystick drivers off (wheel_input.cpp and ui/gamepad.cpp set the hints), so SDL_SendGamepadEffect
// has nothing to talk to there. The output report is written to the pad's HID interface with the
// Windows HID API. A report carries an enable bit per field and only the two trigger bits are
// set, so the rumble the SDK's driver sends keeps working. The layout is the DualSense's own, as
// SDL's PS5 driver writes it: report 0x02 of 48 bytes over USB, report 0x31 of 78 bytes with a
// CRC-32 over Bluetooth, 47 bytes of effect state in both. A trigger block is 11 bytes: a mode
// (0x05 off, 0x21 resistance per zone) and its parameters. A zone is a tenth of the travel and
// gets three bits of strength.
//
// No thread of its own and nothing registered with Windows: the work is done by a per-frame
// tick on the game's thread, every 50 ms, and writes only when something changed. The pads are
// looked for every few seconds while no car is driven (a scan is a few milliseconds), so a pad
// plugged in during a race is picked up at the next menu. An effect stays in the pad until the
// next report, so the triggers are let go when the option goes off, when the car is not driven,
// when the window is not in front, and from Shutdown(). While a load stops the tick they stay as
// they were.

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
#endif

#include "hooks/hooks_internal.h"
#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(adaptive_triggers, true, "MCLA/Input",
    "DualSense adaptive triggers: a throttle spring on R2 and brake resistance on L2 while a car "
    "is driven, slack everywhere else. Other controllers are not touched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(trigger_throttle, 3, "MCLA/Input",
    "R2 resistance at the end of its travel, 1 (light) to 8 (firm).")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(trigger_brake, 6, "MCLA/Input",
    "L2 resistance at the end of its travel, 1 (light) to 8 (firm).")
    .range(1, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using Clock = std::chrono::steady_clock;

// The HUD updates every frame while a car is driven; this long without one is "not driving".
constexpr int64_t kHudFreshMs = 300;
constexpr int64_t kTickMs = 50;
constexpr int64_t kScanMs = 5000;
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
    return true;
#endif
}

// --- The effects ---------------------------------------------------------------------------

struct Effect {
    uint8_t b[11] = {};
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

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// The output report that sets both trigger blocks and nothing else; returns its size.
size_t BuildReport(bool bluetooth, const Effect& r2, const Effect& l2, uint8_t (&out)[78]) {
    uint8_t state[47] = {};
    state[0] = 0x04 | 0x08;  // enable: right trigger, left trigger
    std::memcpy(state + 10, r2.b, sizeof(r2.b));
    std::memcpy(state + 21, l2.b, sizeof(l2.b));
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

// --- The pads --------------------------------------------------------------------------------

struct Pad {
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool bluetooth = false;
    size_t report_size = 0;  // what Windows wants written: the report padded to this
    bool sent = false;       // anything written yet
    bool engaged = false;    // the triggers were last set to the spring and the brake
    int throttle = 0;
    int brake = 0;
};

std::mutex g_mutex;  // the tick (game thread) against Shutdown (the window's thread)
std::vector<Pad> g_pads;
int64_t g_last_tick_ms = 0;
int64_t g_next_scan_ms = 0;
int g_failures = 0;
int g_logged = 0;
bool g_shut_down = false;

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

// Writes the report for the given state to the pad; false if Windows refused it.
bool Write(Pad& pad, bool engaged, int throttle, int brake) {
    const Effect r2 = engaged ? Slope(1, 9, 1, throttle) : Off();
    const Effect l2 = engaged ? Slope(2, 8, 2, brake) : Off();
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
    if (!ok) return false;
    g_failures = 0;
    if (pad.engaged != engaged && g_logged++ < 8) {
        LARECOMP_APP_INFO("[Triggers] {}", engaged ? "driving: spring and brake set" : "released");
    }
    pad.sent = true;
    pad.engaged = engaged;
    pad.throttle = throttle;
    pad.brake = brake;
    return true;
}

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

}  // namespace

namespace larecomp::triggers {

// Every frame, on the game's thread (Patch_DeltaTimePre); works at most every 50 ms.
void Tick() {
    const int64_t now = NowMs();
    if (now - g_last_tick_ms < kTickMs) return;
    g_last_tick_ms = now;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_shut_down) return;
    if (!REXCVAR_GET(adaptive_triggers)) {
        // Off: let go of what was set and give the pads back.
        for (Pad& pad : g_pads) {
            if (pad.engaged) Write(pad, false, 0, 0);
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
    const int throttle = REXCVAR_GET(trigger_throttle);
    const int brake = REXCVAR_GET(trigger_brake);
    for (size_t i = 0; i < g_pads.size();) {
        Pad& pad = g_pads[i];
        const bool changed = !pad.sent || pad.engaged != engaged ||
                             (engaged && (pad.throttle != throttle || pad.brake != brake));
        if (changed && !Write(pad, engaged, throttle, brake)) {
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
        if (pad.engaged) Write(pad, false, 0, 0);
        ClosePad(pad);
    }
    g_pads.clear();
}

}  // namespace larecomp::triggers

// 0x822117C0 in sub_822112A8, the race HUD update: it runs once a frame while a car is driven.
void Hook_TriggersHud() {
    g_hud_ms.store(NowMs(), std::memory_order_relaxed);
}
#endif  // REXGLUE_HAS_XEO3_TARGET
