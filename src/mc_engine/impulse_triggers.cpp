// Xbox One and Xbox Series trigger rumble on the PC (the impulse triggers).
//
// The effect, as most racing games do it: the right trigger shakes while the driven wheels spin
// under power, the left while the brakes lock the wheels, and each one harder the more the wheels
// slip. Nothing else moves them: no engine texture, no gear tops, no nitro. The amounts are the
// game's own tyre slip (wheel_slip.h), the same ones the DualSense's trigger vibration uses.
//
// How it reaches the pad. The executable and rexruntime.dll each carry their own SDL. The game's
// input is the SDK's (in rexruntime.dll), which on Windows opens an Xbox pad through SDL's
// XInput driver (the log of a launch says `SDL OnControllerDeviceAdded: "XInput Controller"`),
// and the XInput driver cannot drive the trigger motors: SDL_RumbleGamepadTriggers answers
// "unsupported" and the pad's SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN is false. Of SDL's other
// Windows drivers (SDL 3.5), HIDAPI does not take an Xbox pad here, RawInput and Windows.Gaming.
// Input are off by default and WGI leaves any pad that XInput handles to XInput, and only GameInput
// (SDL_HINT_JOYSTICK_GAMEINPUT, off by default outside the GDK) sets the trigger motors, over USB
// and Bluetooth alike. So this file uses the executable's own SDL, a different instance from the
// SDK's, and asks it for GameInput in EarlyInit(): the hint is read when the joystick layer
// starts, and LARecomp starts that layer itself (SetupEnvironment calls EarlyInit before the
// wheel thread and the menu pad layer do). GameInput handles gamepads only: wheels stay with
// DirectInput, and a DualSense with HIDAPI. The SDK's SDL is left alone: the game's input
// does not change.
// What the SDK would need to do the same itself: SDL_HINT_JOYSTICK_GAMEINPUT set for its SDL
// (the environment variable of that name reaches both instances, so it would change the game's
// input path for every Xbox pad) and an InputSystem call that ends in SDL_RumbleGamepadTriggers.
//
// A pad's rumble is set as a whole by GameInput (SetRumbleState takes the two motors and the two
// triggers), while the game sets its motors through the SDK's driver (XInputSetState). Setting
// the triggers alone would zero the game's motors, so every update also sends the motor speeds
// the game last asked for (VibrationDriven, the vibration hook's record): the motors keep their
// value and the triggers add theirs. A pad is not touched at all while no trigger rumbles.
//
// One worker thread does all of it, every 25 ms while a car is driven: the game's thread is only
// asked to start it. The thread reads the same "driving" answer the DualSense uses (the race HUD
// updated in the last 300 ms, the game not paused, the window in front), so a game thread that
// stalls (a load) lets the triggers go within a third of a second, and it lets them go when the
// option goes off, when the pad goes away and from Shutdown(). Each update also carries a short
// duration, so SDL stops a rumble by itself if the worker were ever stuck.
//
// CHECK: never run against a pad. Test with an Xbox One or Series pad, over USB and over
// Bluetooth: the log says what was found (`[Impulse] ...: trigger motors yes/no`). `no` for
// a pad named "XInput Controller" means GameInput did not take it: the hint did not reach the
// executable's SDL before its joystick layer started, GameInput is not in this SDL build, or
// gameinput.dll is missing on the machine (GameInput's runtime ships with Windows 11 and as a
// redistributable for Windows 10). With trigger_slip_log on, a burnout should shake the right
// trigger and a locked-wheel stop the left.
// CHECK: with GameInput on, the wheel thread and the menu pad layer (the same SDL) must still find
// their devices: GameInput takes gamepads only and leaves wheels to DirectInput, so a wheel
// should still be found, and the pad should still move the menus.
// CHECK: that the game's own rumble (a crash, a skid) keeps running while a trigger shakes, and
// that the triggers go still the moment the slip ends, the pause menu opens or the window loses
// focus.

#ifndef REXGLUE_HAS_XEO3_TARGET
#if defined(_WIN32)
#include "impulse_triggers.h"

#include <SDL3/SDL.h>
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

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kStepMs = 25;       // between two updates while a car is driven
constexpr int kIdleMs = 250;      // otherwise
constexpr int64_t kScanMs = 2000;
// A trigger update's own duration: SDL ends it by itself if no newer one comes.
constexpr Uint32 kRumbleMs = 300;
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

struct Pad {
    SDL_JoystickID id = 0;
    SDL_Gamepad* gamepad = nullptr;  // a reference of this SDL's own
    std::string name;
    bool shaking = false;  // the triggers were last set to something other than 0
};

std::mutex g_mutex;  // everything below, against Shutdown() on the window's thread
std::condition_variable g_cv;
std::vector<Pad> g_pads;
std::vector<SDL_JoystickID> g_skipped;  // pads that cannot (or must not) be driven
std::thread* g_thread = nullptr;        // never destroyed: the process ends with _Exit
std::atomic<bool> g_started{false};
std::atomic<bool> g_done{false};
bool g_stop = false;
bool g_shut_down = false;
bool g_sdl_up = false;
int64_t g_next_scan_ms = 0;
float g_level[2] = {0.0f, 0.0f};  // the motors' smoothed power, 0 to 1: left, right
bool g_flip = false;              // see Apply()

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

Uint16 ToMotor(float level) {
    return Uint16(std::clamp(std::lround(level * 65535.0f), 0L, 65535L));
}

// Sets the pad's own two motors to what the game last asked for, so that an update of the
// triggers (which sets all four) leaves them as they are.
void MirrorMotors(Pad& pad) {
    const uint32_t speeds = VibrationDriven(0);
    SDL_RumbleGamepad(pad.gamepad, Uint16(speeds >> 16), Uint16(speeds & 0xFFFF), 0);
}

// Lets both triggers of a pad go.
void Release(Pad& pad) {
    if (!pad.shaking) return;
    pad.shaking = false;
    MirrorMotors(pad);
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
    // The game's own motors go along: one update sets all four (see the top of the file).
    MirrorMotors(pad);
    // SDL does not send an update that repeats the last values, and the SDK's driver may have
    // set the triggers back to 0 behind its back when the game changed a motor. The last bit
    // alternates, so every update goes out (and carries the duration along).
    g_flip = !g_flip;
    if (left != 0) left = Uint16(std::min<int>(65535, left + (g_flip ? 1 : 0)));
    if (right != 0) right = Uint16(std::min<int>(65535, right + (g_flip ? 1 : 0)));
    if (SDL_RumbleGamepadTriggers(pad.gamepad, left, right, kRumbleMs)) {
        pad.shaking = true;
    }
}

// Opens the pads that appeared (the ones with trigger motors) and closes the ones that went.
void Scan() {
    SDL_UpdateJoysticks();
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
                          motors ? "yes" : "no");
        if (!motors) {
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

// One pass of the worker, with g_mutex held; returns how long to wait before the next.
int Step(int64_t now) {
    if (!REXCVAR_GET(impulse_triggers)) {
        // Off: let go of what was set and give the pads back.
        CloseAll();
        return kIdleMs;
    }
    if (!g_sdl_up) {
        // Not before here: the joystick layer comes up with the first pad scan.
        g_sdl_up = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!g_sdl_up) {
            LARECOMP_APP_WARN("[Impulse] SDL's gamepad layer would not start: {}", SDL_GetError());
            return 5000;
        }
    }
    if (now >= g_next_scan_ms) {
        g_next_scan_ms = now + kScanMs;
        Scan();
    }
    if (g_pads.empty()) return kIdleMs;
    const bool driving = larecomp::triggers::IsDriving() && larecomp::triggers::IsWindowInFront();
    if (driving) {
        const larecomp::slip::Amounts slip = larecomp::slip::Current();
        Smooth(g_level[0], Motor(slip.lockup));
        Smooth(g_level[1], Motor(slip.wheelspin));
    } else {
        g_level[0] = g_level[1] = 0.0f;
    }
    const Uint16 left = ToMotor(g_level[0]);
    const Uint16 right = ToMotor(g_level[1]);
    for (Pad& pad : g_pads) Apply(pad, left, right);
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
    // This executable's SDL, not the SDK's (see the top of the file). Before its joystick layer starts.
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "1");
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
#endif  // _WIN32
#endif  // REXGLUE_HAS_XEO3_TARGET
