// Xbox One and Xbox Series trigger rumble on the PC (the impulse triggers).
//
// The effect, as most racing games do it: the right trigger shakes while the driven wheels spin
// under power, the left while the brakes lock the wheels, and each one harder the more the wheels
// slip. Nothing else moves them: no engine texture, no gear tops, no nitro. The amounts are the
// game's own tyre slip (wheel_slip.h), the same ones the DualSense's trigger vibration uses.
//
// How it reaches the pad, on Windows: Windows.Gaming.Input (WGI), from impulse_wgi.cpp. Not through
// SDL. The game's input is the SDK's (in rexruntime.dll), which opens an Xbox pad through SDL's
// XInput driver (the log of a launch says `SDL OnControllerDeviceAdded: "XInput Controller"`), and
// the XInput driver cannot drive the trigger motors. Of SDL's other Windows drivers (SDL 3.5),
// HIDAPI does not take an Xbox pad here, RawInput is off, SDL's own WGI driver leaves any pad that
// XInput handles to XInput, and the one that does set the trigger motors, GameInput
// (SDL_HINT_JOYSTICK_GAMEINPUT), cannot start in this build: SDL is compiled against the Windows
// SDK's gameinput.h, which is GameInput API version 0, and for that version SDL_InitGameInput
// answers E_NOINTERFACE on desktop Windows ("We require GameInput v1.1 or newer"), hint or no hint.
// An earlier version of this file set that hint in the executable's SDL; the play test's log
// shows the pad stayed on XInput (`trigger motors no`). Getting SDL's driver to work would need the
// Microsoft.GameInput headers (API v1 or later) in the SDK's SDL build (the game's own input
// shares it) and the GameInput redistributable on every player's PC (the GameInput.dll that
// Windows 11 ships in System32 is file version 0.2309). WGI is in every Windows 10 and 11, reaches Xbox
// 360, One and Series pads over USB and Bluetooth, and sets the two trigger motors together with
// the two main ones (GamepadVibration); it works while this process is the foreground application,
// which is when the worker rumbles. The game's input does not change: the SDK keeps setting the
// main motors through XInput, and the pad has two writers that the worker keeps in step, below.
//
// A pad's rumble is set as a whole by WGI (GamepadVibration takes the two motors and the two
// triggers), while the game sets its motors through the SDK's driver (XInputSetState). Setting
// the triggers alone would zero the game's motors, so every update also sends the motor speeds
// the game last asked for (VibrationDriven, the vibration hook's record): the motors keep their
// value and the triggers add theirs. A pad is not touched at all while no trigger rumbles. When
// the game changes a motor, XInput may set the triggers back to 0 behind the worker's back; the
// next update (25 ms) sets them again. Unlike SDL's, a WGI vibration has no duration: the triggers
// keep shaking until the next update, so the worker lets them go the moment the slip ends, the
// game stops driving or the window loses focus, and from Shutdown().
//
// On Linux (any non-Windows build) the process has ONE SDL, the SDK's, and the SDK's input driver
// (input_backend = "sdl") opens the same pads, so SDL_RumbleGamepadTriggers goes to the pad the game
// is already using. None of the Windows workarounds apply: SDL keeps the two motors and the two
// triggers as separate values and every driver that can rumble triggers sends all four from them,
// so the game's motors are left alone, nothing is mirrored and no update has to be made to differ.
// Which SDL drivers reach trigger motors there, with SDL 3.5 (the SDK's):
//   - Xbox One and Series pads over Bluetooth: SDL's Xbox One HIDAPI driver, through /dev/hidraw*
//     (every Microsoft pad counts as having trigger motors). The player's user must be allowed to use
//     the hidraw node: Steam's udev rules cover one Xbox pad (045e:0b22) only, so most players need
//     a rule of their own, e.g.  KERNEL=="hidraw*", KERNELS=="*045E:*", MODE="0660", TAG+="uaccess"
//   - Xbox One and Series pads over USB: SDL's GIP HIDAPI driver, which needs libusb. SDL keeps
//     libusb off for everything but a few Nintendo devices on Linux (SDL_HINT_HIDAPI_LIBUSB_WHITELIST,
//     on by default where a hidraw backend exists), because libusb takes the pad from the kernel's xpad
//     driver, and that driver is the game's input path for a wired Xbox pad today. LARecomp leaves
//     that alone: a wired pad stays on xpad (SDL reports no trigger motors for it, and the log says
//     so), unless the player starts the game with SDL_HIDAPI_LIBUSB_WHITELIST=0 in the environment
//     (SDL reads it as a hint) and /dev/bus/usb allows their user (a udev rule for 045e, uaccess).
//   - SDL's Linux evdev driver (the kernel's xpad or xpadneo/hid-microsoft device) and the Xbox 360
//     driver have no trigger rumble. Steam Input's virtual pad is an evdev device too; SDL types it
//     as the real pad behind it, so an Xbox One pad behind Steam Input is opened here, reports no
//     trigger motors and is closed again. The Steam Deck's own controls are not typed as an Xbox One
//     pad and are never opened.
// Only pads that SDL types as Xbox One are opened here (an Xbox 360 type pad has no trigger motors);
// every other pad is left to the SDK's driver.
//
// One worker thread does all of it, every 25 ms while a car is driven: the game's thread is only
// asked to start it. The thread reads the same "driving" answer the DualSense uses (the race HUD
// updated in the last 300 ms, the game not paused, the window in front), so a game thread that
// stalls (a load) lets the triggers go within a third of a second, and it lets them go when the
// option goes off, when the pad goes away and from Shutdown(). On Linux each update also carries
// a short duration, so SDL stops a rumble by itself if the worker were ever stuck.
//
// CHECK (Windows): never run against a pad. Test with an Xbox One or Series pad, over USB and over
// Bluetooth: the log says what was found (`[Impulse] Windows.Gaming.Input lists N gamepads`, then
// one line per pad with `trigger motors yes, driven through Windows.Gaming.Input`). With
// trigger_slip_log on, a burnout should shake the right trigger and a locked-wheel stop the left.
// A line saying WGI lists no gamepad, with an Xbox pad plugged in and the game in front, would mean
// WGI does not see the pad (then look at what else holds it) and a `would not set the vibration`
// line carries the HRESULT.
// CHECK (Windows): that the game's own rumble (a crash, a skid) keeps running while a trigger
// shakes, and that the triggers go still the moment the slip ends, the pause menu opens or the
// window loses focus. Also what a crash or a task kill does to a trigger that is shaking: a WGI
// vibration has no duration and only Shutdown() (a normal close) lets it go; if the pad keeps
// shaking after the process is gone, the worker needs a way to be told from the crash handler.
// CHECK (Linux): never run against a pad. With an Xbox One or Series pad over Bluetooth and
// access to its hidraw node the log should say `trigger motors yes` and a burnout should shake the
// right trigger (trigger_slip_log shows the amounts); with the pad over USB it says `no` and gives
// the reason, and `yes` with SDL_HIDAPI_LIBUSB_WHITELIST=0 and libusb in the SDL build (HAVE_LIBUSB;
// the build needs libusb-1.0 headers, the run needs libusb-1.0.so.0 and access to the pad's
// /dev/bus/usb node). Also that the game's rumble keeps running while a trigger shakes (the shared
// SDL keeps the motors and the triggers apart, read from SDL's drivers, not run), that the SDK's
// pad input is unchanged, and that a Steam Deck's own controls are never opened (no [Impulse] line).

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "impulse_triggers.h"

#if !defined(_WIN32)
#include <SDL3/SDL.h>
#endif
#include <rex/cvar.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adaptive_triggers.h"
#include "hooks/hooks_internal.h"
#include "larecomp_log.h"
#if defined(_WIN32)
#include "impulse_wgi.h"
#else
#include "trigger_sdl.h"
#endif
#include "wheel_slip.h"

REXCVAR_DEFINE_BOOL(impulse_triggers, true, "MCLA/Input",
    "Xbox One and Xbox Series trigger rumble: the right trigger shakes while the driven wheels "
    "spin under power, the left while the brakes lock the wheels, harder the more they slip. "
    "Controllers without trigger motors, and a DualSense (which has its own settings), are not "
    "touched.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(impulse_trigger_strength, 50.0, "MCLA/Input",
    "How strong the Xbox trigger rumble is, in percent of what the trigger motors can do "
    "(10 to 100).")
    .range(10.0, 100.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, trigger_slip_log);  // wheel_slip.cpp

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kStepMs = 25;       // between two updates while a car is driven
constexpr int kIdleMs = 250;      // otherwise
constexpr int64_t kScanMs = 2000;
#if !defined(_WIN32)
// A trigger update's own duration: SDL ends it by itself if no newer one comes.
constexpr Uint32 kRumbleMs = 300;
#endif
// Below this slip amount a trigger is still. Above it the motor runs at kFloor of the strength
// setting, and at all of it for a full slip.
constexpr float kFrom = 0.05f;
constexpr float kFloor = 0.30f;
// How fast the motor follows the amount, per step: quickly up, slower down.
constexpr float kRise = 0.6f;
constexpr float kFall = 0.4f;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32)
// Off Windows the pads are SDL's (on Windows impulse_wgi.cpp keeps them).
struct Pad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;  // a reference of this SDL's own
    std::string name;
    bool shaking = false;  // the triggers were last set to something other than 0
};
#endif

std::mutex g_mutex;  // everything below, against Shutdown() on the window's thread
std::condition_variable g_cv;
#if !defined(_WIN32)
std::vector<Pad> g_pads;
std::vector<SDL_JoystickID> g_skipped;  // pads that cannot (or must not) be driven
#endif
std::thread* g_thread = nullptr;        // never destroyed: the process ends with _Exit
std::atomic<bool> g_started{false};
std::atomic<bool> g_done{false};
bool g_stop = false;
bool g_shut_down = false;
#if !defined(_WIN32)
bool g_sdl_up = false;
#endif
int64_t g_next_scan_ms = 0;
float g_level[2] = {0.0f, 0.0f};  // the motors' smoothed power, 0 to 1: left, right

// The motor power for a slip amount.
float Motor(float amount) {
    if (amount < kFrom) return 0.0f;
    const float strength = std::clamp(float(REXCVAR_GET(impulse_trigger_strength)) / 100.0f, 0.1f, 1.0f);
    return strength * (kFloor + (1.0f - kFloor) * std::clamp(amount, 0.0f, 1.0f));
}

void Smooth(float& level, float target) {
    level += (target - level) * (target > level ? kRise : kFall);
    if (target == 0.0f && level < 0.02f) level = 0.0f;
}

uint16_t ToMotor(float level) {
    return uint16_t(std::clamp(std::lround(level * 65535.0f), 0L, 65535L));
}

#if defined(_WIN32)
// Lets both triggers of every pad go, and forgets the pads.
void CloseAll() {
    larecomp::impulse::wgi::CloseAll(VibrationDriven(0));
    g_level[0] = g_level[1] = 0.0f;
    g_next_scan_ms = 0;
}
#else
// Lets both triggers of a pad go.
void Release(Pad& pad) {
    if (!pad.shaking) return;
    pad.shaking = false;
    SDL_RumbleGamepadTriggers(pad.gamepad, 0, 0, 0);
}

void CloseAll() {
    for (Pad& pad : g_pads) {
        Release(pad);
        SDL_CloseGamepad(pad.gamepad);
    }
    g_pads.clear();
    g_skipped.clear();
    g_level[0] = g_level[1] = 0.0f;
    g_next_scan_ms = 0;
}

// Sets the triggers of a pad to the two motor powers (0 to 65535), left and right.
void Apply(Pad& pad, Uint16 left, Uint16 right) {
    if (left == 0 && right == 0) {
        Release(pad);
        return;
    }
    // An update that repeats the last values only extends the duration, which is what is wanted:
    // SDL re-sends a held trigger rumble by itself every 2 s, and ends it kRumbleMs after the last
    // update, so a stuck worker lets the triggers go.
    if (SDL_RumbleGamepadTriggers(pad.gamepad, left, right, kRumbleMs)) {
        pad.shaking = true;
    }
}
#endif

#if defined(_WIN32)
// Finds the pads that appeared (the ones with trigger motors) and forgets the ones that went.
void Scan() { larecomp::impulse::wgi::Scan(); }
#else
// Opens the pads that appeared (the ones with trigger motors) and closes the ones that went.
// The SDK's driver pumps SDL and keeps the list of pads current; this stays out of it.
void Scan() {
    int count = 0;
    SDL_JoystickID* found = SDL_GetGamepads(&count);
    std::vector<SDL_JoystickID> present;
    if (found) {
        present.assign(found, found + count);
        SDL_free(found);
    }
    for (size_t i = 0; i < g_pads.size();) {
        Pad& pad = g_pads[i];
        if (std::find(present.begin(), present.end(), pad.id) == present.end() ||
            !SDL_GamepadConnected(pad.gamepad)) {
            LARECOMP_APP_INFO("[Impulse] {} (id {}) is gone", pad.name, uint32_t(pad.id));
            SDL_CloseGamepad(pad.gamepad);
            g_pads.erase(g_pads.begin() + long(i));
        } else {
            ++i;
        }
    }
    g_skipped.erase(std::remove_if(g_skipped.begin(), g_skipped.end(),
                                   [&](SDL_JoystickID id) {
                                       return std::find(present.begin(), present.end(), id) == present.end();
                                   }),
                    g_skipped.end());
    for (SDL_JoystickID id : present) {
        if (std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad& p) { return p.id == id; }) ||
            std::find(g_skipped.begin(), g_skipped.end(), id) != g_skipped.end()) {
            continue;
        }
        // A DualSense has its own trigger effects, the other PlayStation pads have no trigger
        // motors, and the wheel code's virtual device is no pad.
        const SDL_GamepadType type = SDL_GetGamepadTypeForID(id);
        if (SDL_IsJoystickVirtual(id) || type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
            type == SDL_GAMEPAD_TYPE_PS5) {
            g_skipped.push_back(id);
            continue;
        }
        // The SDK's driver has every pad open here, so only a pad that can have impulse triggers is
        // opened a second time: an Xbox One or Series pad. The Xbox 360 type has no trigger motors.
        if (type != SDL_GAMEPAD_TYPE_XBOXONE) {
            g_skipped.push_back(id);
            continue;
        }
        SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
        if (!gamepad) {
            g_skipped.push_back(id);
            continue;
        }
        const char* name = SDL_GetGamepadName(gamepad);
        const bool motors = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
                                                   SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
        const SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(gamepad);
        LARECOMP_APP_INFO("[Impulse] {} (id {}, SDL type {}, vendor 0x{:04X}, product 0x{:04X}, {}): "
                          "trigger motors {}",
                          name ? name : "?", uint32_t(id), int(type), SDL_GetGamepadVendor(gamepad),
                          SDL_GetGamepadProduct(gamepad),
                          link == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless"
                          : link == SDL_JOYSTICK_CONNECTION_WIRED  ? "wired"
                                                                   : "link unknown",
                          motors ? "yes, driven through SDL" : "no");
        if (!motors) {
            LARECOMP_APP_INFO("[Impulse] {} has no trigger motors through SDL. Over USB that is the case "
                              "while the kernel's xpad driver has the pad (SDL_HIDAPI_LIBUSB_WHITELIST=0 "
                              "hands it to SDL); over Bluetooth, without access to /dev/hidraw*",
                              name ? name : "?");
            SDL_CloseGamepad(gamepad);
            g_skipped.push_back(id);
            continue;
        }
        Pad pad;
        pad.id = id;
        pad.gamepad = gamepad;
        pad.name = name ? name : "Xbox controller";
        g_pads.push_back(std::move(pad));
    }
}
#endif

// One pass of the worker, with g_mutex held; returns how long to wait before the next.
int Step(int64_t now) {
    if (!REXCVAR_GET(impulse_triggers)) {
        // Off: let go of what was set and give the pads back.
        CloseAll();
        return kIdleMs;
    }
#if !defined(_WIN32)
    if (!g_sdl_up) {
        // This is the SDK's SDL: its input driver starts the gamepad layer, and this waits for it
        // (see trigger_sdl.h).
        if (!larecomp::trigger_sdl::GamepadLayerUp()) return 1000;
        g_sdl_up = true;
    }
#endif
    if (now >= g_next_scan_ms) {
        g_next_scan_ms = now + kScanMs;
        Scan();
    }
#if defined(_WIN32)
    const bool no_pads = !larecomp::impulse::wgi::HasPads();
#else
    const bool no_pads = g_pads.empty();
#endif
    if (no_pads) {
        // The slip log (trigger_slip_log, a test aid) also runs with no pad, e.g. on a scripted drive.
        if (REXCVAR_GET(trigger_slip_log) && larecomp::triggers::IsDriving()) {
            larecomp::slip::Current();
            return kStepMs;
        }
        return kIdleMs;
    }
    const bool driving = larecomp::triggers::IsDriving() && larecomp::triggers::IsWindowInFront();
    if (driving) {
        const larecomp::slip::Amounts slip = larecomp::slip::Current();
        Smooth(g_level[0], Motor(slip.lockup));
        Smooth(g_level[1], Motor(slip.wheelspin));
    } else {
        g_level[0] = g_level[1] = 0.0f;
    }
    const uint16_t left = ToMotor(g_level[0]);
    const uint16_t right = ToMotor(g_level[1]);
#if defined(_WIN32)
    larecomp::impulse::wgi::Apply(left, right, VibrationDriven(0));
#else
    for (Pad& pad : g_pads) Apply(pad, left, right);
#endif
    return driving ? kStepMs : kIdleMs;
}

void Run() {
    for (;;) {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (g_stop) break;
        const int wait_ms = Step(NowMs());
        g_cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [] { return g_stop; });
        if (g_stop) break;
    }
    g_done.store(true, std::memory_order_release);
}

}  // namespace

namespace larecomp::impulse {

void EarlyInit() {
    // Nothing to set any more. On Windows the trigger motors are reached through Windows.Gaming.Input,
    // which needs no SDL hint (the GameInput hint this used to set never took effect: see the top of
    // the file). Off Windows no hint is needed either: SDL's HIDAPI drivers are on by default there,
    // the one SDL is the SDK's, and the one hint that would reach wired Xbox pads
    // (SDL_HINT_HIDAPI_LIBUSB_WHITELIST) changes how the game itself gets those pads, so it is left
    // to the player's environment (see the top of the file). The call stays so that the start-up
    // code need not change.
}

void Tick() {
    if (g_started.load(std::memory_order_relaxed) || !REXCVAR_GET(impulse_triggers)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_started.load(std::memory_order_relaxed) || g_shut_down) return;
    g_started.store(true, std::memory_order_relaxed);
    g_thread = new std::thread(Run);
}

void Shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_shut_down) return;
        g_shut_down = true;
        g_stop = true;
    }
    g_cv.notify_all();
    if (!g_started.load(std::memory_order_relaxed)) return;
    for (int i = 0; i < 100 && !g_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool done = g_done.load(std::memory_order_acquire);
    if (!done) {
        LARECOMP_APP_WARN("[Impulse] the worker did not stop within 500 ms; letting the triggers go from here");
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        CloseAll();
    }
    if (g_thread && g_thread->joinable()) {
        if (done) {
            g_thread->join();
        } else {
            g_thread->detach();
        }
    }
}

}  // namespace larecomp::impulse
#endif  // REXGLUE_HAS_XEO3_TARGET
