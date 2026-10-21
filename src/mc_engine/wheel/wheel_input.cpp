// See wheel_input.h. The pieces:
//
//   Wheel thread    owns this exe's own copy of SDL (the SDK's is a different instance inside
//                   rexruntime.dll, so its joystick layer is invisible from here). Starts SDL's
//                   joystick subsystem, looks for wheels once a second, opens / closes them and
//                   samples the open one every few milliseconds into a snapshot. FFB (item 5) will
//                   run on this same thread: it is the only thread that calls SDL.
//   Snapshot        steer / throttle / brake in 0..1 and the X360 button bits, plus a timestamp.
//   Policy          wheel_mode: which of the wheel and the gamepad drives this poll. Runs on the
//                   guest's input thread, on the latest snapshot.
//   ApplyToState()  writes the result over the pad state the guest is about to read.
//
// Everything about a particular wheel that is a guess lives in a profile
// (wheel_profiles.ini, or the generic defaults below), never in the code: the axis
// order of a wheel is not something SDL can tell us, and nobody who can test this has
// every model. Each wheel that gets opened writes wheel_report.txt with what SDL saw, so
// a tester's first run is enough to write its profile.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "wheel_input.h"
#include "wheel_ffb.h"

#include <rex/cvar.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_joystick.h>

#include <algorithm>
#include <climits>
#include <filesystem>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <set>
#include <string>
#include <vector>

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
#include "wheel_dinput.h"

REXCVAR_DEFINE_STRING(wheel_mode, "auto", "MCLA/Wheel",
    "Who drives when a wheel is connected. auto: the wheel until the gamepad is moved, the gamepad "
    "until the wheel is. wheel: always the wheel. gamepad: the wheel is ignored and the game sees "
    "the pad. Only the driving controls switch; Start, Back, A, B and the D-pad work from both.")
    .allowed({"auto", "wheel", "gamepad"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(wheel_device, "auto", "MCLA/Wheel",
    "Which joystick is the wheel. auto: the first one SDL calls a wheel or a known wheel model. off: "
    "none. Anything else is matched against the device name (part of it, any case) or written as "
    "vendor:product in hex (046d:c24f) and opens that device even if SDL does not call it a wheel. "
    "Takes effect when the device is next scanned (about a second).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_device_range, 900, "MCLA/Wheel",
    "The lock-to-lock rotation the wheel is set to in its own driver, in degrees. The wheel reports "
    "-1..1 over that range; this and wheel_game_range turn it into the game's range.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_game_range, 540, "MCLA/Wheel",
    "The lock-to-lock rotation that reaches full steering lock in the game, in degrees. The game "
    "reads steering as 8 bits (255 steps), so a smaller range steers finer: 270 deg = 1.06 deg per "
    "step, 540 = 2.1, 900 = 3.5. Full lock is reached before the wheel's own end stop when this is "
    "smaller than wheel_device_range.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(wheel_steer_deadzone, 0.01, "MCLA/Wheel",
    "Fraction of the wheel's range around centre that reads as straight ahead (0.01 = 1%).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(wheel_steer_gamma, 1.0, "MCLA/Wheel",
    "Shapes steering after the range scaling: 1 is linear, above 1 softens the centre. The game "
    "applies its own curve afterwards, so leave this at 1 unless the car feels twitchy.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(wheel_virtual, "off", "MCLA/Wheel",
    "Test aid: create a virtual SDL wheel (type wheel, 4 axes, 20 buttons, a hat) driven from the "
    "keyboard (A/D steer, W throttle, S brake, Q/E paddles) so the whole SDL path can be run without "
    "hardware. off, low (pedals rest at -32768) or high (pedals rest at +32767).")
    .allowed({"off", "low", "high"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_learn, false, "MCLA/Wheel",
    "Run the wheel setup: a console window walks through the steering, the pedals, the paddles and "
    "the H-shifter, watches which axes and buttons move, and writes the wheel's section of "
    "wheel_profiles.ini. Switches itself off when it is done. Start the game with --wheel_learn=true "
    "before playing.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_auto_setup, true, "MCLA/Wheel",
    "The first time a wheel is found that has no section in wheel_profiles.ini, ask whether to run the setup "
    "now (a yes/no box, shown once the game window is up). Answering no writes a marker so it is not asked "
    "again; the setup can always be started with wheel_learn. Off: never ask.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(wheel_cancel_game_deadzone, 0.05, "MCLA/Wheel",
    "The game puts a dead zone of about 5% on its stick and pedal axes (measured: steering comes out as "
    "(x - 0.05) / 0.95), written for a thumb stick. A wheel is precise, so the first few degrees would do nothing. "
    "This lifts the wheel's output by that amount, so the game's own dead zone cancels and the response is "
    "linear. 0 turns it off. Set it to what you measure if a wheel still feels dead around centre.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_trace, false, "MCLA/Wheel",
    "Log, once a second, every axis and pressed button SDL reads from the wheel and what the game is given "
    "(who drives, steering, pedals, buttons). For finding out why a wheel behaves oddly: start the game with it, "
    "reproduce the problem, send the log.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_pedal_autorange, true, "MCLA/Wheel",
    "Pedals that never reach the end of their travel read full once they have been pressed to where "
    "they do reach (after the first firm press). Off: the pedals use their full nominal range. A "
    "pedal with throttle_full / brake_full in the profile is never auto-ranged.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp::wheel {

namespace {

using Clock = std::chrono::steady_clock;

// X_INPUT_GAMEPAD bits.
constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080;
constexpr uint16_t kLb = 0x0100, kRb = 0x0200, kBtnA = 0x1000, kBtnB = 0x2000, kBtnX = 0x4000,
                   kBtnY = 0x8000;
// What works from either device whoever is driving: menus and navigation.
constexpr uint16_t kMenuMask = kStart | kBack | kBtnA | kBtnB | kDpadUp | kDpadDown | kDpadLeft |
                               kDpadRight;

constexpr int kMaxAxes = 16;
constexpr int kNoGear = 100;
constexpr int kMaxButtons = 32;

struct Profile {
    std::string name = "generic";
    std::string origin = "built-in defaults";
    int steer_axis = 0;
    int throttle_axis = -1;  // -1: found by the rest-position heuristic
    int brake_axis = -1;
    int clutch_axis = -1;
    bool invert_steer = false;
    bool invert_throttle = false;
    bool invert_brake = false;
    bool combined_pedals = false;  // one axis, centred: throttle one way, brake the other
    std::array<uint16_t, kMaxButtons> button{};
    bool hat_dpad = true;
    // Optional per-model calibration. Unset values are measured: a pedal's rest is read when the wheel
    // is opened, its full travel is the far end of the axis (or what auto-range has seen).
    int device_range = 0;        // degrees lock to lock the wheel is set to; 0 = the wheel_device_range cvar
    int steer_center = 0;        // raw steering value that means straight ahead
    int pedal_rest[3] = {INT_MIN, INT_MIN, INT_MIN};   // throttle, brake, clutch
    int pedal_full[3] = {INT_MIN, INT_MIN, INT_MIN};
    // H-shifter: the button each lever position presses, -1 where the wheel has none. [0] reverse,
    // [1]..[7] first to seventh. Nothing is emulated unless at least one is set.
    std::array<int, 8> gear_button = {-1, -1, -1, -1, -1, -1, -1, -1};
};

// Buttons guessed for a wheel nobody has described: face buttons, then the two paddles. SDL's
// button order for a wheel is the device's own, so this is a starting point for the report and
// the profile, not a promise.
Profile GenericProfile() {
    Profile p;
    p.button[0] = kBtnA;
    p.button[1] = kBtnB;
    p.button[2] = kBtnX;
    p.button[3] = kBtnY;
    p.button[4] = kRb;  // right paddle
    p.button[5] = kLb;  // left paddle
    p.button[8] = kBack;
    p.button[9] = kStart;
    return p;
}

struct Device {
    SDL_JoystickID id = 0;
    SDL_Joystick* js = nullptr;
    bool virt = false;
    std::string name;
    uint16_t vid = 0, pid = 0;
    int naxes = 0, nbuttons = 0, nhats = 0;
    std::array<int16_t, kMaxAxes> rest{};
    Profile prof;
    int throttle = -1, brake = -1, clutch = -1;  // resolved
    std::array<int, kMaxAxes> seen{};            // farthest pedal travel seen (auto-range); INT_MIN = none yet
    dinput::Pad* di = nullptr;  // when set, the state is read through DirectInput instead of SDL
    bool di_tried = false;
    Device() { seen.fill(INT_MIN); }
};

int Ax(const Device& d, int i) { return d.di ? dinput::Axis(d.di, i) : SDL_GetJoystickAxis(d.js, i); }
bool Bt(const Device& d, int i) { return d.di ? dinput::Button(d.di, i) : SDL_GetJoystickButton(d.js, i); }
int Ht(const Device& d, int i) { return d.di ? dinput::Hat(d.di, i) : int(SDL_GetJoystickHat(d.js, i)); }

// ---------------------------------------------------------------------------
// State. Thread-owned unless said otherwise; heap-allocated and never freed, because the
// wheel thread can outlive static destruction at exit.
// ---------------------------------------------------------------------------

struct WheelFrame {
    float steer = 0.0f;     // -1 left .. 1 right, already in the game's range
    float throttle = 0.0f;  // 0..1
    float brake = 0.0f;
    uint16_t buttons = 0;   // X_INPUT_GAMEPAD bits
    float pos = 0.0f;       // steering position over the game's lock-to-lock range, no deadzone (for FFB)
    float pos_raw = 0.0f;   // the same over the wheel's own range
    int shifter = kNoGear;  // H-shifter lever: -1 reverse, 1..7, kNoGear in neutral or without one
};

struct Shared {
    // Wheel thread only.
    Device dev;
    bool have_dev = false;
    Clock::time_point last_scan{};
    SDL_JoystickID virtual_id = 0;
    SDL_Joystick* virtual_js = nullptr;
    bool sdl_up = false;
    std::filesystem::file_time_type ini_time{};
    std::string inventory;
    float key_steer = 0.0f;
    Clock::time_point key_last{};

    // Published by the wheel thread, read by the guest's.
    std::mutex snap_mutex;
    bool snap_present = false;
    WheelFrame snap_frame;
    Clock::time_point snap_stamp{};

    // Guest-side policy, behind its own lock.
    std::mutex policy_mutex;
    bool wheel_active = true;
    Clock::time_point trace_stamp{};
    Clock::time_point zero_since{};   // when every axis and button of the wheel started reading exactly 0
    std::set<SDL_JoystickID> dead_ids;  // joysticks that read nothing; skipped when a twin can be tried instead
    Clock::time_point dead_clear{};
    bool input_dead = false;          // ... for long enough that the wheel is taken for silent
    bool ffb_released = false;        // the force-feedback handle was closed to see if it was starving the input
    bool was_present = false;
    Clock::time_point last_switch{};
    float last_steer = 0.0f;

    // The car, from the HUD hook.
    std::mutex tele_mutex;
    int tele_gear = 0;
    float tele_speed = 0.0f;
    Clock::time_point tele_stamp{};

    // H-shifter emulation (guest input thread, under policy_mutex).
    int sh_request = kNoGear;          // the lever position being waited on
    Clock::time_point sh_request_since{};
    int sh_pulse = 0;                  // 0 idle, 1 pressed, 2 released
    int sh_dir = 0;                    // +1 up (RB), -1 down (LB)
    Clock::time_point sh_phase_since{};
    int sh_gear_at_press = 0;
    int sh_pulses_sent = 0;
    bool sh_warned = false;

    std::atomic<bool> driving{true};    // the wheel (not the pad) is driving; read by the FFB engine
    std::atomic<uint64_t> frame{0};     // guest input frames, from NoteInputFrame()
    uint64_t sh_press_frame = 0;        // the frame a shift press began on
    uint64_t sh_release_frame = 0;      // the frame it was let go on

    std::string offered;                 // vid:pid already asked about this session (wheel thread)
    std::atomic<bool> any_wheel{false};  // a wheel is open (wheel thread); read by the HUD hook
    std::atomic<bool> started{false};
};

Shared& S() {
    static Shared* s = new Shared();
    return *s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Hex4(uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%04x", v);
    return b;
}

// Models SDL does not always call a wheel. Classification only: nothing here says how the axes
// are laid out. Logitech: Driving Force/MOMO/DFGT/G25/G27/G29. Thrustmaster: T150, T300RS, TMX,
// T-GT, TX, T500RS.
bool KnownWheel(uint16_t vid, uint16_t pid) {
    if (vid == 0x046d) {
        switch (pid) {
            case 0xc294: case 0xc295: case 0xc298: case 0xc299: case 0xc29a: case 0xc29b:
            case 0xc24f:
                return true;
        }
    }
    if (vid == 0x044f) {
        switch (pid) {
            case 0xb65d: case 0xb65e: case 0xb66d: case 0xb66e: case 0xb66f: case 0xb677:
            case 0xb67e: case 0xb689: case 0xb669: case 0xb664:
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Profiles (wheel_profiles.ini)
//
//   [046d:c24f]            or [default]
//   name = Logitech G29
//   steer_axis = 0
//   throttle_axis = 2
//   brake_axis = 3
//   clutch_axis = 1
//   combined_pedals = 0
//   invert_steer = 0
//   button.4 = RB          A B X Y LB RB START BACK L3 R3 UP DOWN LEFT RIGHT NONE
// ---------------------------------------------------------------------------

uint16_t ButtonByName(const std::string& n) {
    static const std::map<std::string, uint16_t> m = {
        {"a", kBtnA}, {"b", kBtnB}, {"x", kBtnX}, {"y", kBtnY}, {"lb", kLb}, {"rb", kRb},
        {"start", kStart}, {"back", kBack}, {"l3", kL3}, {"r3", kR3}, {"up", kDpadUp},
        {"down", kDpadDown}, {"left", kDpadLeft}, {"right", kDpadRight}, {"none", 0}};
    auto it = m.find(Lower(n));
    return it == m.end() ? 0 : it->second;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool ParseBool(const std::string& v) {
    const std::string l = Lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

void ApplyKey(Profile& p, const std::string& key, const std::string& val) {
    if (key == "name") p.name = val;
    else if (key == "steer_axis") p.steer_axis = std::atoi(val.c_str());
    else if (key == "throttle_axis") p.throttle_axis = std::atoi(val.c_str());
    else if (key == "brake_axis") p.brake_axis = std::atoi(val.c_str());
    else if (key == "clutch_axis") p.clutch_axis = std::atoi(val.c_str());
    else if (key == "invert_steer") p.invert_steer = ParseBool(val);
    else if (key == "invert_throttle") p.invert_throttle = ParseBool(val);
    else if (key == "invert_brake") p.invert_brake = ParseBool(val);
    else if (key == "combined_pedals") p.combined_pedals = ParseBool(val);
    else if (key == "hat_dpad") p.hat_dpad = ParseBool(val);
    else if (key == "device_range") p.device_range = std::atoi(val.c_str());
    else if (key == "steer_center") p.steer_center = std::atoi(val.c_str());
    else if (key == "throttle_rest") p.pedal_rest[0] = std::atoi(val.c_str());
    else if (key == "brake_rest") p.pedal_rest[1] = std::atoi(val.c_str());
    else if (key == "clutch_rest") p.pedal_rest[2] = std::atoi(val.c_str());
    else if (key == "throttle_full") p.pedal_full[0] = std::atoi(val.c_str());
    else if (key == "brake_full") p.pedal_full[1] = std::atoi(val.c_str());
    else if (key == "clutch_full") p.pedal_full[2] = std::atoi(val.c_str());
    else if (key.rfind("gear.", 0) == 0) {
        const std::string g = key.substr(5);
        const int idx = (g == "r") ? 0 : std::atoi(g.c_str());
        if (idx >= 0 && idx < 8) p.gear_button[size_t(idx)] = std::atoi(val.c_str());
    }
    else if (key.rfind("button.", 0) == 0) {
        const int n = std::atoi(key.c_str() + 7);
        if (n >= 0 && n < kMaxButtons) p.button[size_t(n)] = ButtonByName(val);
    }
}

// Reads the section for vid:pid, falling back to [default]; both on top of the generic profile.
// The exact section is applied after [default] whatever order they sit in the file.
Profile LoadProfile(uint16_t vid, uint16_t pid) {
    Profile p = GenericProfile();
    std::ifstream in("wheel_profiles.ini");
    if (!in) return p;
    const std::string want = Hex4(vid) + ":" + Hex4(pid);
    std::vector<std::pair<std::string, std::string>> def_keys, exact_keys;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Lower(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string val = Trim(line.substr(eq + 1));
        if (section == "default") def_keys.emplace_back(key, val);
        else if (section == want) exact_keys.emplace_back(key, val);
    }
    for (const auto& kv : def_keys) ApplyKey(p, kv.first, kv.second);
    for (const auto& kv : exact_keys) ApplyKey(p, kv.first, kv.second);
    if (!exact_keys.empty()) p.origin = "wheel_profiles.ini [" + want + "]";
    else if (!def_keys.empty()) p.origin = "wheel_profiles.ini [default]";
    return p;
}

// ---------------------------------------------------------------------------
// Opening a wheel (wheel thread)
// ---------------------------------------------------------------------------

bool MatchesWheelDevice(SDL_JoystickID id, const std::string& want, uint16_t vid, uint16_t pid,
                        const char* name) {
    const std::string w = Lower(want);
    if (w == "off") return false;
    if (w == "auto") {
        return SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || KnownWheel(vid, pid);
    }
    if (w.size() == 9 && w[4] == ':') return w == Hex4(vid) + ":" + Hex4(pid);
    return name && Lower(name).find(w) != std::string::npos;
}

void WriteReport(const Device& d, SDL_JoystickID id) {
    char guid[64] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    std::ofstream out("wheel_report.txt", std::ios::app);
    if (!out) return;
    out << "=== wheel opened ===\n";
    out << "name: " << d.name << "\n";
    out << "vid:pid " << Hex4(d.vid) << ":" << Hex4(d.pid) << "   guid " << guid << "\n";
    out << "sdl type: " << int(SDL_GetJoystickTypeForID(id)) << " (wheel = " << int(SDL_JOYSTICK_TYPE_WHEEL)
        << ")   virtual: " << (d.virt ? "yes" : "no") << "\n";
    out << "axes " << d.naxes << "  buttons " << d.nbuttons << "  hats " << d.nhats << "\n";
    out << "axis rest values (as read when the device was opened -- keep the pedals released):\n";
    for (int i = 0; i < d.naxes && i < kMaxAxes; ++i) {
        out << "  axis " << i << ": " << d.rest[size_t(i)] << "\n";
    }
    out << "profile: " << d.prof.name << " (" << d.prof.origin << ")\n";
    out << "  steer " << d.prof.steer_axis << "  throttle " << d.throttle << "  brake " << d.brake
        << "  clutch " << d.clutch << "  combined_pedals " << d.prof.combined_pedals << "\n";
    out << "  device_range " << (d.prof.device_range > 0 ? d.prof.device_range : REXCVAR_GET(wheel_device_range))
        << "  steer_center " << d.prof.steer_center << "\n";
    for (int i = 0; i < 3; ++i) {
        static const char* names[3] = {"throttle", "brake", "clutch"};
        out << "  " << names[i] << " rest "
            << (d.prof.pedal_rest[i] != INT_MIN ? std::to_string(d.prof.pedal_rest[i]) : "(measured)") << "  full "
            << (d.prof.pedal_full[i] != INT_MIN ? std::to_string(d.prof.pedal_full[i]) : "(far end / auto-range)")
            << "\n";
    }
    out << "  h-shifter: ";
    bool any_gear = false;
    for (int g = 0; g < 8; ++g) {
        if (d.prof.gear_button[size_t(g)] >= 0) {
            out << (g == 0 ? std::string("R") : std::to_string(g)) << "=button "
                << d.prof.gear_button[size_t(g)] << " ";
            any_gear = true;
        }
    }
    out << (any_gear ? "\n" : "none (set gear.1 = N ... gear.7 = N, gear.r = N in the profile)\n");
    out << "To change any of this, add a section to wheel_profiles.ini next to the exe:\n"
        << "  [" << Hex4(d.vid) << ":" << Hex4(d.pid) << "]\n"
        << "  steer_axis = N\n  throttle_axis = N\n  brake_axis = N\n  invert_throttle = 0\n"
        << "  button.4 = RB\n\n";
}

// Pedal axes are the ones that rest at an end of their range. That is the one thing every
// wheel agrees on, so it picks candidates without knowing the model; their order is a guess
// (lowest index = throttle) that the profile overrides.
void ResolveAxes(Device& d) {
    d.throttle = d.prof.throttle_axis;
    d.brake = d.prof.brake_axis;
    d.clutch = d.prof.clutch_axis;
    std::vector<int> cand;
    for (int i = 0; i < d.naxes && i < kMaxAxes; ++i) {
        if (i == d.prof.steer_axis) continue;
        if (std::abs(int(d.rest[size_t(i)])) >= 26000) cand.push_back(i);
    }
    auto taken = [&](int i) {
        return i == d.throttle || i == d.brake || i == d.clutch || i == d.prof.steer_axis;
    };
    auto next = [&]() {
        for (int i : cand) {
            if (!taken(i)) return i;
        }
        return -1;
    };
    if (d.throttle < 0) d.throttle = next();
    if (d.brake < 0) d.brake = next();
    if (d.clutch < 0) d.clutch = next();
}

// Some drivers (a Thrustmaster TMX, a Fanatec) report every axis as 0 until the first real poll has come in, which
// would make a pedal at rest (32767) look like a pedal that moved. Polls until some axis reads non-zero, for at
// most `ms`; a wheel that really is all zero just costs the wait.
void SettleAxes(SDL_Joystick* js, int naxes, int ms) {
    const auto t0 = Clock::now();
    do {
        SDL_UpdateJoysticks();
        for (int i = 0; i < naxes; ++i) {
            if (SDL_GetJoystickAxis(js, i) != 0) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms);
}

void OpenDevice(Shared& sh, SDL_JoystickID id, bool virt) {
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
        LARECOMP_APP_WARN("[wheel] could not open joystick {}: {}", id, SDL_GetError());
        return;
    }
    Device d;
    d.id = id;
    d.js = js;
    d.virt = virt;
    const char* nm = SDL_GetJoystickNameForID(id);
    d.name = nm ? nm : "(unnamed)";
    d.vid = SDL_GetJoystickVendorForID(id);
    d.pid = SDL_GetJoystickProductForID(id);
    d.naxes = std::min(SDL_GetNumJoystickAxes(js), kMaxAxes);
    d.nbuttons = std::min(SDL_GetNumJoystickButtons(js), kMaxButtons);
    d.nhats = SDL_GetNumJoystickHats(js);
    if (!virt) SettleAxes(js, d.naxes, 800);
    for (int i = 0; i < d.naxes; ++i) {
        // The state at open, but some drivers report 0 there until the first real poll: then the
        // current reading is the better rest position (pedals released, as the report asks).
        Sint16 v = 0;
        const Sint16 cur = SDL_GetJoystickAxis(js, i);
        if (!SDL_GetJoystickAxisInitialState(js, i, &v) || (v == 0 && cur != 0)) v = cur;
        d.rest[size_t(i)] = v;
    }
    d.prof = LoadProfile(d.vid, d.pid);
    ResolveAxes(d);
    WriteReport(d, id);
    LARECOMP_APP_INFO(
        "[wheel] opened '{}' {}:{} axes={} buttons={} hats={} profile={} ({}) steer={} throttle={} "
        "brake={} clutch={}",
        d.name, Hex4(d.vid), Hex4(d.pid), d.naxes, d.nbuttons, d.nhats, d.prof.name, d.prof.origin,
        d.prof.steer_axis, d.throttle, d.brake, d.clutch);
    if (d.throttle < 0 || d.brake < 0) {
        LARECOMP_APP_WARN(
            "[wheel] '{}': could not tell the pedal axes apart from their rest positions (throttle={} "
            "brake={}). See wheel_report.txt and set throttle_axis / brake_axis in wheel_profiles.ini.",
            d.name, d.throttle, d.brake);
    }
    sh.dev = std::move(d);
    sh.have_dev = true;
    {
        std::error_code ec;
        sh.ini_time = std::filesystem::last_write_time("wheel_profiles.ini", ec);
    }
    {
        const int dev = sh.dev.prof.device_range > 0 ? sh.dev.prof.device_range : REXCVAR_GET(wheel_device_range);
        const int game = REXCVAR_GET(wheel_game_range);
        LARECOMP_APP_INFO("[wheel] steering scale: the wheel is taken to be set to {} deg lock to lock, the game reaches "
                          "full lock at {} deg (x{:.2f}); if that is not what the wheel's driver is set to, set "
                          "device_range in wheel_profiles.ini", dev, game, double(dev) / double(std::max(1, game)));
    }
    ffb::DeviceOpened(sh.dev.virt, sh.dev.vid, sh.dev.pid);
}

void CloseDevice(Shared& sh) {
    if (sh.have_dev && sh.dev.js) {
        ffb::DeviceClosing();  // the force-feedback handle goes before the joystick it belongs to
        dinput::Close(sh.dev.di);
        sh.dev.di = nullptr;
        SDL_CloseJoystick(sh.dev.js);
        LARECOMP_APP_INFO("[wheel] closed '{}'", sh.dev.name);
    }
    sh.dev = Device{};
    sh.have_dev = false;
}

// True while the setup runs: its console may sit in a terminal host whose window is not ours, so focus
// is not asked of the virtual test wheel then.
std::atomic<bool> g_wizard_active{false};

#ifdef _WIN32
bool WindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    // The setup's own console is ours too, though a console window belongs to its host process.
    if (fg == GetConsoleWindow()) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
#else
constexpr int VK_NUMPAD0 = 0x60;
bool WindowFocused() { return false; }
bool KeyDown(int) { return false; }
#endif

// The virtual wheel: a test source that goes through SDL like a real one.
void FeedVirtual(Shared& sh) {
    if (!sh.virtual_js) return;
    const std::string rest_mode = REXCVAR_GET(wheel_virtual);
    const int16_t rest = rest_mode == "high" ? 32767 : -32768;
    const int16_t pressed = rest_mode == "high" ? -32768 : 32767;
    float target = 0.0f;
    bool throttle = false, brake = false, lb = false, rb = false;
    bool gear_keys[7] = {};  // [0] reverse, [1]..[6]
    if (WindowFocused() || g_wizard_active.load(std::memory_order_relaxed)) {
        if (KeyDown('A')) target -= 1.0f;
        if (KeyDown('D')) target += 1.0f;
        throttle = KeyDown('W');
        brake = KeyDown('S');
        lb = KeyDown('Q');
        rb = KeyDown('E');
        // Numpad 1-6 and 0 stand for the H-shifter's lever: first to sixth, reverse (buttons 12..17
        // and 18). Needs NumLock on; with it off Windows reads these keys as the arrows.
        for (int g = 1; g < 7; ++g) gear_keys[g] = KeyDown(VK_NUMPAD0 + g);
        gear_keys[0] = KeyDown(VK_NUMPAD0);
    }
    static float last_target = 0.0f;
    if (target != last_target) {
        last_target = target;
        LARECOMP_APP_INFO("[wheel] virtual wheel: steering key target {:+.0f}", target);
    }
    const Clock::time_point now = Clock::now();
    if (sh.key_last.time_since_epoch().count() == 0) sh.key_last = now;
    float dt = std::chrono::duration<float>(now - sh.key_last).count();
    sh.key_last = now;
    dt = std::min(dt, 0.1f);
    const bool returning = target == 0.0f || target * sh.key_steer < 0.0f;
    const float step = (returning ? 4.0f : 2.5f) * dt;
    if (sh.key_steer < target) sh.key_steer = std::min(sh.key_steer + step, target);
    else if (sh.key_steer > target) sh.key_steer = std::max(sh.key_steer - step, target);
    SDL_SetJoystickVirtualAxis(sh.virtual_js, 0, int16_t(std::lround(sh.key_steer * 32767.0f)));
    SDL_SetJoystickVirtualAxis(sh.virtual_js, 1, rest);  // clutch
    SDL_SetJoystickVirtualAxis(sh.virtual_js, 2, throttle ? pressed : rest);
    SDL_SetJoystickVirtualAxis(sh.virtual_js, 3, brake ? pressed : rest);
    SDL_SetJoystickVirtualButton(sh.virtual_js, 4, rb);
    SDL_SetJoystickVirtualButton(sh.virtual_js, 5, lb);
    for (int g = 1; g < 7; ++g) SDL_SetJoystickVirtualButton(sh.virtual_js, 11 + g, gear_keys[g]);
    SDL_SetJoystickVirtualButton(sh.virtual_js, 18, gear_keys[0]);
}

void EnsureVirtual(Shared& sh) {
    const bool want = REXCVAR_GET(wheel_virtual) != "off";
    if (want && !sh.virtual_js) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_WHEEL;
        desc.vendor_id = 0x1209;   // pid.codes test range; not a real product
        desc.product_id = 0x0001;
        desc.naxes = 4;
        desc.nbuttons = 20;
        desc.nhats = 1;
        desc.name = "larecomp virtual wheel";
        sh.virtual_id = SDL_AttachVirtualJoystick(&desc);
        if (sh.virtual_id == 0) {
            LARECOMP_APP_WARN("[wheel] could not attach the virtual wheel: {}", SDL_GetError());
        } else {
            sh.virtual_js = SDL_OpenJoystick(sh.virtual_id);
            // Put the pedals at rest before anyone reads the device: a fresh virtual joystick
            // reads 0 on every axis.
            FeedVirtual(sh);
            SDL_UpdateJoysticks();
            LARECOMP_APP_INFO("[wheel] wheel_virtual={}: virtual wheel attached (id {})",
                              REXCVAR_GET(wheel_virtual), sh.virtual_id);
        }
    } else if (!want && sh.virtual_js) {
        if (sh.have_dev && sh.dev.js == sh.virtual_js) CloseDevice(sh);
        SDL_CloseJoystick(sh.virtual_js);
        SDL_DetachVirtualJoystick(sh.virtual_id);
        sh.virtual_js = nullptr;
        sh.virtual_id = 0;
    }
}

// Re-reads the profile when wheel_profiles.ini changes under an open wheel (a tester editing it, or the
// setup writing it), keeping the rest positions read when the wheel was opened.
void MaybeReloadProfile(Shared& sh) {
    if (!sh.have_dev) return;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time("wheel_profiles.ini", ec);
    if (ec || t == sh.ini_time) return;
    sh.ini_time = t;
    sh.dev.prof = LoadProfile(sh.dev.vid, sh.dev.pid);
    sh.dev.seen.fill(INT_MIN);
    ResolveAxes(sh.dev);
    WriteReport(sh.dev, sh.dev.id);
    LARECOMP_APP_INFO("[wheel] wheel_profiles.ini changed: profile reloaded ({}) steer={} throttle={} brake={} "
                      "clutch={}", sh.dev.prof.origin, sh.dev.prof.steer_axis, sh.dev.throttle, sh.dev.brake,
                      sh.dev.clutch);
}

// Every joystick SDL sees, for the tester: written to wheel_devices.txt when the set changes. A joystick that
// is not taken for a wheel is the usual reason nothing happens, and the way out (wheel_device or the setup)
// is named in the log.
void Inventory(Shared& sh, SDL_JoystickID* ids, int n) {
    std::string text;
    int candidates = 0;
    for (int i = 0; i < n; ++i) {
        const SDL_JoystickID id = ids[i];
        const char* nm = SDL_GetJoystickNameForID(id);
        const uint16_t vid = SDL_GetJoystickVendorForID(id);
        const uint16_t pid = SDL_GetJoystickProductForID(id);
        const bool virt = SDL_IsJoystickVirtual(id);
        char line[256];
        std::snprintf(line, sizeof(line), "%s  %s:%s  sdl type %d  %s%s\n", nm ? nm : "(unnamed)", Hex4(vid).c_str(),
                      Hex4(pid).c_str(), int(SDL_GetJoystickTypeForID(id)), virt ? "virtual " : "",
                      (SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || KnownWheel(vid, pid)) ? "[wheel]" : "");
        text += line;
        if (!virt && !(SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL || KnownWheel(vid, pid))) ++candidates;
    }
    if (text == sh.inventory) return;
    sh.inventory = text;
    std::ofstream out("wheel_devices.txt");
    if (out) {
        out << "Joysticks SDL sees (one per line: name, vendor:product, SDL type, [wheel] when taken for a wheel).\n"
               "To use one that is not marked, set wheel_device to part of its name or to vendor:product, or run\n"
               "the setup (--wheel_learn=true).\n\n"
            << text;
    }
    LARECOMP_APP_INFO("[wheel] joysticks seen: {} (see wheel_devices.txt)", n);
    if (n > 0 && candidates == n && !sh.have_dev) {  // joysticks, none of them taken for a wheel
        LARECOMP_APP_INFO("[wheel] none of them is taken for a wheel: if one is, set wheel_device to part of its name "
                          "or to vendor:product, or run the setup with --wheel_learn=true");
    }
}

// A wheel can be listed as several joysticks with the same ids (a Fanatec shows up twice) and only one of them
// is fed. The paths differ, so they are logged; the one that reads something is the one to use.
bool JoystickLive(SDL_JoystickID id) {
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) return false;
    const int na = SDL_GetNumJoystickAxes(js), nb = SDL_GetNumJoystickButtons(js), nh = SDL_GetNumJoystickHats(js);
    SettleAxes(js, na, 500);
    bool live = false;
    for (int i = 0; i < na && !live; ++i) live = SDL_GetJoystickAxis(js, i) != 0;
    for (int i = 0; i < nb && !live; ++i) live = SDL_GetJoystickButton(js, i);
    for (int i = 0; i < nh && !live; ++i) live = SDL_GetJoystickHat(js, i) != 0;
    SDL_CloseJoystick(js);
    return live;
}

SDL_JoystickID PickTwin(Shared& sh, const SDL_JoystickID* ids, int n, SDL_JoystickID first) {
    const uint16_t vid = SDL_GetJoystickVendorForID(first), pid = SDL_GetJoystickProductForID(first);
    std::vector<SDL_JoystickID> twins;
    for (int i = 0; i < n; ++i) {
        if (SDL_IsJoystickVirtual(ids[i]) || sh.dead_ids.count(ids[i])) continue;
        if (SDL_GetJoystickVendorForID(ids[i]) == vid && SDL_GetJoystickProductForID(ids[i]) == pid) twins.push_back(ids[i]);
    }
    if (twins.size() < 2) return first;
    SDL_JoystickID pick = 0;
    for (SDL_JoystickID t : twins) {
        const char* path = SDL_GetJoystickPathForID(t);
        const bool live = JoystickLive(t);
        LARECOMP_APP_INFO("[wheel] {}:{} joystick {} path '{}': {}", Hex4(vid), Hex4(pid), t, path ? path : "?",
                          live ? "reads data" : "reads nothing");
        if (live && !pick) pick = t;
    }
    return pick ? pick : first;
}

// Scans about once a second; opens the wanted wheel, drops a gone one.
void Scan(Shared& sh) {
    EnsureVirtual(sh);
    MaybeReloadProfile(sh);
    if (sh.have_dev && !SDL_JoystickConnected(sh.dev.js)) CloseDevice(sh);
    const std::string want = REXCVAR_GET(wheel_device);
    if (sh.have_dev && Lower(want) == "off") CloseDevice(sh);

    int n = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&n);
    if (!ids) return;
    Inventory(sh, ids, n);
    if (!sh.dead_ids.empty() && Clock::now() - sh.dead_clear > std::chrono::seconds(60)) sh.dead_ids.clear();
    // Some wheels show up as several joysticks with the same ids (a Fanatec listed twice) and only one of them
    // is fed: when the one in use reads nothing, try the other.
    if (sh.have_dev && sh.input_dead && !sh.dev.virt) {
        bool twin = false;
        for (int i = 0; i < n; ++i) {
            const SDL_JoystickID id = ids[i];
            if (id == sh.dev.id || sh.dead_ids.count(id)) continue;
            if (SDL_GetJoystickVendorForID(id) != sh.dev.vid || SDL_GetJoystickProductForID(id) != sh.dev.pid) continue;
            LARECOMP_APP_WARN("[wheel] '{}' (joystick {}) reads nothing and another joystick has the same ids: "
                              "trying joystick {} instead", sh.dev.name, sh.dev.id, id);
            sh.dead_ids.insert(sh.dev.id);
            sh.dead_clear = Clock::now();
            CloseDevice(sh);
            twin = true;
            break;
        }
    }
    if (!sh.have_dev && Lower(want) != "off") {
        for (int i = 0; i < n && !sh.have_dev; ++i) {
            const SDL_JoystickID id = ids[i];
            const char* nm = SDL_GetJoystickNameForID(id);
            const uint16_t vid = SDL_GetJoystickVendorForID(id);
            const uint16_t pid = SDL_GetJoystickProductForID(id);
            const bool virt = SDL_IsJoystickVirtual(id);
            // A virtual wheel is only ours when asked for; a virtual pad some other tool made is
            // not a wheel.
            if (virt && id != sh.virtual_id) continue;
            if (sh.dead_ids.count(id)) continue;
            if (id == sh.virtual_id || MatchesWheelDevice(id, want, vid, pid, nm)) {
                OpenDevice(sh, virt ? id : PickTwin(sh, ids, n, id), virt);
            }
        }
    }
    SDL_free(ids);
}

// ---------------------------------------------------------------------------
// Reading the wheel
// ---------------------------------------------------------------------------

// A pedal from 0 (released) to 1 (pressed). Rest is the profile's or what the axis read when the wheel was
// opened; full is the profile's or the far end of the axis. Without a profile value for full, auto-range
// (wheel_pedal_autorange) pulls it in to the farthest travel actually seen, once that is more than 40% of
// the way, so a pedal that stops short of the end still reads 1 when it is pressed hard.
float Pedal(Device& d, int slot, int axis, bool invert) {
    if (axis < 0 || axis >= d.naxes) return 0.0f;
    const Profile& p = d.prof;
    int v = Ax(d, axis);
    int r = p.pedal_rest[slot] != INT_MIN ? p.pedal_rest[slot] : d.rest[size_t(axis)];
    const bool explicit_full = p.pedal_full[slot] != INT_MIN;
    int f;
    if (explicit_full) f = p.pedal_full[slot];
    else if (r >= 16384) f = -32768;
    else if (r <= -16384) f = 32767;
    else f = v >= r ? 32767 : -32768;
    if (invert) {
        v = -v - 1;
        r = -r - 1;
        f = -f - 1;
    }
    float span = float(f - r);
    if (span == 0.0f) return 0.0f;
    if (!explicit_full && REXCVAR_GET(wheel_pedal_autorange) && (r >= 16384 || r <= -16384)) {
        int& seen = d.seen[size_t(axis)];
        if (seen == INT_MIN) seen = r;
        seen = span > 0 ? std::max(seen, v) : std::min(seen, v);
        const float eff = float(seen - r);
        if (std::fabs(eff) >= 0.4f * std::fabs(span) && std::fabs(eff) < std::fabs(span)) span = eff;
    }
    return std::clamp(float(v - r) / span, 0.0f, 1.0f);
}

WheelFrame ReadFrame(Device& d) {
    WheelFrame f;
    // Steering: raw -1..1 over the wheel's own range, rescaled to the game's.
    if (d.prof.steer_axis >= 0 && d.prof.steer_axis < d.naxes) {
        float s = float(Ax(d, d.prof.steer_axis) - d.prof.steer_center) / 32767.0f;
        if (d.prof.invert_steer) s = -s;
        const double dev = std::max(30, d.prof.device_range > 0 ? d.prof.device_range : REXCVAR_GET(wheel_device_range));
        const double game = std::max(30, REXCVAR_GET(wheel_game_range));
        f.pos_raw = std::clamp(s, -1.0f, 1.0f);
        s = float(std::clamp(double(s) * dev / game, -1.0, 1.0));
        f.pos = s;
        const float dz = float(std::clamp(REXCVAR_GET(wheel_steer_deadzone), 0.0, 0.3));
        const float a = std::fabs(s);
        float m = a <= dz ? 0.0f : (a - dz) / (1.0f - dz);
        const float gamma = float(std::clamp(REXCVAR_GET(wheel_steer_gamma), 0.2, 4.0));
        if (gamma != 1.0f) m = std::pow(m, gamma);
        f.steer = s < 0 ? -m : m;
    }
    if (d.prof.combined_pedals && d.throttle >= 0) {
        const int v = Ax(d, d.throttle);
        const int r = d.prof.pedal_rest[0] != INT_MIN ? d.prof.pedal_rest[0] : d.rest[size_t(d.throttle)];
        f.throttle = v > r ? std::clamp(float(v - r) / float(32767 - r), 0.0f, 1.0f) : 0.0f;
        f.brake = v < r ? std::clamp(float(r - v) / float(r + 32768), 0.0f, 1.0f) : 0.0f;
        if (d.prof.invert_throttle) std::swap(f.throttle, f.brake);
    } else {
        f.throttle = Pedal(d, 0, d.throttle, d.prof.invert_throttle);
        f.brake = Pedal(d, 1, d.brake, d.prof.invert_brake);
    }
    for (int i = 0; i < d.nbuttons; ++i) {
        if (Bt(d, i)) f.buttons |= d.prof.button[size_t(i)];
    }
    for (int g = 0; g < 8; ++g) {
        const int b = d.prof.gear_button[size_t(g)];
        if (b >= 0 && b < d.nbuttons && Bt(d, b)) {
            f.shifter = g == 0 ? -1 : g;
            break;
        }
    }
    if (d.prof.hat_dpad && d.nhats > 0) {
        const Uint8 h = Ht(d, 0);
        if (h & SDL_HAT_UP) f.buttons |= kDpadUp;
        if (h & SDL_HAT_DOWN) f.buttons |= kDpadDown;
        if (h & SDL_HAT_LEFT) f.buttons |= kDpadLeft;
        if (h & SDL_HAT_RIGHT) f.buttons |= kDpadRight;
    }
    return f;
}

// ---------------------------------------------------------------------------
// Setup (wheel_learn): finds the wheel's axes and buttons by watching them move
// ---------------------------------------------------------------------------

#ifdef _WIN32
HANDLE g_console = INVALID_HANDLE_VALUE;

// Its own console window, written to through its own handle: redirecting stdout would take the game's log
// (which goes to the process's stdout) into the window.
void ConsoleUp() {
    if (!GetConsoleWindow()) AllocConsole();
    g_console = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    SetConsoleTitleW(L"larecomp wheel setup - do not close this window");
}
void ConsoleDown() {
    if (g_console != INVALID_HANDLE_VALUE) CloseHandle(g_console);
    g_console = INVALID_HANDLE_VALUE;
    FreeConsole();
}
void ConsoleWrite(const std::string& line) {
    if (g_console == INVALID_HANDLE_VALUE) return;
    const std::string out = line + "\r\n";
    DWORD written = 0;
    WriteFile(g_console, out.data(), DWORD(out.size()), &written, nullptr);
}
#else
void ConsoleUp() {}
void ConsoleDown() {}
void ConsoleWrite(const std::string&) {}
#endif

void Say(const std::string& line) {
    ConsoleWrite(line);
    LARECOMP_APP_INFO("[wheel-setup] {}", line);
}

// A line typed into the setup console, or "" after ms. Reads key events one by one so a mouse move or a
// focus change cannot leave it blocked waiting for Enter.
std::string ReadLine(int ms) {
#ifdef _WIN32
    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return "";
    FlushConsoleInputBuffer(in);
    std::string line;
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms) {
        DWORD n = 0;
        if (!GetNumberOfConsoleInputEvents(in, &n) || n == 0) {
            Sleep(20);
            continue;
        }
        INPUT_RECORD rec;
        DWORD got = 0;
        if (!ReadConsoleInputW(in, &rec, 1, &got) || got == 0) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        const wchar_t c = rec.Event.KeyEvent.uChar.UnicodeChar;
        if (c == L'\r') break;
        if (c == L'\b') {
            if (!line.empty()) line.pop_back();
        } else if (c >= L'0' && c <= L'9') {
            line.push_back(char(c));
        }
    }
    CloseHandle(in);
    return line;
#else
    (void)ms;
    return "";
#endif
}

// Also keeps the virtual wheel fed: the setup waits in loops that only poll, and the virtual wheel is driven
// from the keyboard by the same thread.
void Sleep_ms(int ms) {
    Shared& sh = S();
    if (sh.virtual_js) FeedVirtual(sh);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

struct Peak {
    int base = 0, extreme = 0, delta = 0;
};

// Watches every axis for ms and returns, per axis, how far it got from where it started (or from `base`).
// With only_sign set, `only_axis` counts only what lies on that side of its starting value (the second half of
// the steering test: the wheel is still coming back from the left stop when the window opens).
std::array<Peak, kMaxAxes> WatchAxes(Device& d, int ms, const std::array<int, kMaxAxes>* base_override = nullptr,
                                     int only_axis = -1, int only_sign = 0) {
    std::array<Peak, kMaxAxes> pk{};
    SDL_UpdateJoysticks();
    for (int a = 0; a < d.naxes; ++a) {
        const int v = Ax(d, a);
        pk[size_t(a)].base = base_override ? (*base_override)[size_t(a)] : v;
        pk[size_t(a)].extreme = v;
        pk[size_t(a)].delta = std::abs(v - pk[size_t(a)].base);
        if (a == only_axis && only_sign != 0) {
            pk[size_t(a)].delta = 0;
            pk[size_t(a)].extreme = pk[size_t(a)].base;
        }
    }
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms) {
        SDL_UpdateJoysticks();
        for (int a = 0; a < d.naxes; ++a) {
            const int v = Ax(d, a);
            if (a == only_axis && only_sign != 0 && (v - pk[size_t(a)].base) * only_sign <= 0) continue;
            const int dl = std::abs(v - pk[size_t(a)].base);
            if (dl > pk[size_t(a)].delta) {
                pk[size_t(a)].delta = dl;
                pk[size_t(a)].extreme = v;
            }
        }
        Sleep_ms(5);
    }
    // What was seen, so a step that fails says why.
    std::string seen = "watched:";
    for (int a = 0; a < d.naxes; ++a) {
        seen += " axis" + std::to_string(a) + " moved " + std::to_string(pk[size_t(a)].delta);
    }
    LARECOMP_APP_INFO("[wheel-setup] {}", seen);
    return pk;
}

// The axis that moved most, if it clearly beat the others (not in `exclude`).
int BestAxis(const Device& d, const std::array<Peak, kMaxAxes>& pk, const std::vector<int>& exclude, int min_delta) {
    int best = -1, second = 0;
    for (int a = 0; a < d.naxes; ++a) {
        if (std::find(exclude.begin(), exclude.end(), a) != exclude.end()) continue;
        if (best < 0 || pk[size_t(a)].delta > pk[size_t(best)].delta) {
            if (best >= 0) second = std::max(second, pk[size_t(best)].delta);
            best = a;
        } else {
            second = std::max(second, pk[size_t(a)].delta);
        }
    }
    if (best < 0 || pk[size_t(best)].delta < min_delta) return -1;
    // Two axes that moved about as much (a combined pedal, a wheel that drives two axes) cannot be told apart.
    if (second * 10 > pk[size_t(best)].delta * 7) return -2;
    return best;
}

// Waits up to ms for a button that was up to go down; waits for it to come up again. -1 on timeout.
int WaitButton(Device& d, int ms, const std::vector<int>& taken) {
    SDL_UpdateJoysticks();
    std::array<bool, kMaxButtons> was{};
    for (int i = 0; i < d.nbuttons; ++i) was[size_t(i)] = Bt(d, i);
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms) {
        SDL_UpdateJoysticks();
        for (int i = 0; i < d.nbuttons; ++i) {
            if (was[size_t(i)] || !Bt(d, i)) continue;
            if (std::find(taken.begin(), taken.end(), i) != taken.end()) continue;
            const auto t1 = Clock::now();
            while (Bt(d, i) &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t1).count() < 2500) {
                SDL_UpdateJoysticks();
                Sleep_ms(5);
            }
            return i;
        }
        Sleep_ms(5);
    }
    return -1;
}

// Writes [vid:pid] into wheel_profiles.ini, replacing the old section and keeping everything else (a
// copy of the old file goes to wheel_profiles.ini.bak first).
void SaveProfileSection(uint16_t vid, uint16_t pid, const std::string& body) {
    const std::string header = "[" + Hex4(vid) + ":" + Hex4(pid) + "]";
    std::vector<std::string> keep;
    {
        std::ifstream in("wheel_profiles.ini");
        if (in) {
            std::ofstream bak("wheel_profiles.ini.bak", std::ios::trunc);
            std::string line;
            bool skipping = false;
            while (std::getline(in, line)) {
                bak << line << "\n";
                const std::string t = Trim(line);
                if (!t.empty() && t.front() == '[' && t.back() == ']') skipping = Lower(t) == header;
                if (!skipping) keep.push_back(line);
            }
        }
    }
    std::ofstream out("wheel_profiles.ini", std::ios::trunc);
    for (const std::string& l : keep) out << l << "\n";
    if (!keep.empty() && !Trim(keep.back()).empty()) out << "\n";
    out << header << "\n" << body;
}

#ifdef _WIN32
// The wheel thread starts long before the game's window exists. The setup waits for it (at most a minute) so
// the console does not appear in the middle of the boot, and the window it needs for focus is there.
void WaitForGameWindow() {
    struct Ctx { bool found = false; } ctx;
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count() < 60) {
        ctx.found = false;
        EnumWindows(
            [](HWND h, LPARAM lp) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(h, &pid);
                if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
                wchar_t cls[64] = {};
                GetClassNameW(h, cls, 64);
                if (std::wcscmp(cls, L"ConsoleWindowClass") == 0) return TRUE;
                reinterpret_cast<Ctx*>(lp)->found = true;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        if (ctx.found) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (ctx.found) std::this_thread::sleep_for(std::chrono::seconds(2));
}
#else
void WaitForGameWindow() {}
#endif

void RunWizard(Shared& sh) {
    WaitForGameWindow();
    ConsoleUp();
    g_wizard_active.store(true);
    Say("=== larecomp wheel setup ===");
    Say("Do not close this window. Hands off the wheel and pedals unless a step asks for them.");
    // The normal path lets go of the wheel first (this closes its force-feedback handle too).
    if (sh.have_dev) CloseDevice(sh);
    {
        std::lock_guard<std::mutex> l(sh.snap_mutex);
        sh.snap_present = false;
    }

    // Which joystick: the only candidate, or the one that moves.
    std::vector<SDL_JoystickID> cand;
    {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&n);
        if (ids) {
            for (int i = 0; i < n; ++i) {
                if (SDL_IsJoystickVirtual(ids[i]) && ids[i] != sh.virtual_id) continue;
                cand.push_back(ids[i]);
            }
            SDL_free(ids);
        }
    }
    if (cand.empty()) {
        Say("No joystick found. Connect the wheel (and its driver), then start the game with --wheel_learn=true.");
        rex::cvar::SetFlagByName("wheel_learn", "false");
        Sleep_ms(6000);
        ConsoleDown();
        return;
    }
    Device d;
    SDL_JoystickID chosen = cand[0];
    if (cand.size() > 1) {
        Say("More than one joystick is connected. TURN THE WHEEL a little so it can be told from the others (20 s).");
        std::vector<SDL_Joystick*> js;
        std::vector<std::array<int, kMaxAxes>> base(cand.size());
        for (size_t i = 0; i < cand.size(); ++i) {
            js.push_back(SDL_OpenJoystick(cand[i]));
            for (int a = 0; a < kMaxAxes; ++a) base[i][size_t(a)] = js[i] && a < SDL_GetNumJoystickAxes(js[i]) ? SDL_GetJoystickAxis(js[i], a) : 0;
        }
        int found = -1;
        const auto t0 = Clock::now();
        while (found < 0 && std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count() < 20) {
            SDL_UpdateJoysticks();
            for (size_t i = 0; i < cand.size() && found < 0; ++i) {
                if (!js[i]) continue;
                for (int a = 0; a < SDL_GetNumJoystickAxes(js[i]) && a < kMaxAxes; ++a) {
                    if (std::abs(SDL_GetJoystickAxis(js[i], a) - base[i][size_t(a)]) > 9000) {
                        found = int(i);
                        break;
                    }
                }
            }
            Sleep_ms(10);
        }
        for (SDL_Joystick* j : js) {
            if (j) SDL_CloseJoystick(j);
        }
        if (found < 0) {
            Say("Nothing moved. Setup cancelled.");
            rex::cvar::SetFlagByName("wheel_learn", "false");
            Sleep_ms(4000);
            ConsoleDown();
            return;
        }
        chosen = cand[size_t(found)];
    }
    d.id = chosen;
    d.js = SDL_OpenJoystick(chosen);
    if (!d.js) {
        Say(std::string("Could not open the joystick: ") + SDL_GetError());
        rex::cvar::SetFlagByName("wheel_learn", "false");
        Sleep_ms(4000);
        ConsoleDown();
        return;
    }
    d.virt = SDL_IsJoystickVirtual(chosen);
    const char* nm = SDL_GetJoystickNameForID(chosen);
    d.name = nm ? nm : "(unnamed)";
    d.vid = SDL_GetJoystickVendorForID(chosen);
    d.pid = SDL_GetJoystickProductForID(chosen);
    d.naxes = std::min(SDL_GetNumJoystickAxes(d.js), kMaxAxes);
    d.nbuttons = std::min(SDL_GetNumJoystickButtons(d.js), kMaxButtons);
    d.nhats = SDL_GetNumJoystickHats(d.js);
    Say("Setting up: " + d.name + " (" + Hex4(d.vid) + ":" + Hex4(d.pid) + "), " + std::to_string(d.naxes) +
        " axes, " + std::to_string(d.nbuttons) + " buttons.");

    Profile prof = GenericProfile();
    prof.name = d.name;
    prof.button.fill(0);
    prof.gear_button = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool ok = true;
    std::vector<int> axes_taken;

    auto countdown = [&](const char* what) {
        Say(std::string("Get ready: ") + what);
        Sleep_ms(2500);
    };

    // 1. Steering
    countdown("steering. Leave the wheel centred, then turn it all the way LEFT and hold it there.");
    std::array<int, kMaxAxes> centre{};
    SettleAxes(d.js, d.naxes, 1500);
    SDL_UpdateJoysticks();
    for (int a = 0; a < d.naxes; ++a) centre[size_t(a)] = Ax(d, a);
    Say("GO: turn the wheel fully LEFT and hold (5 s)");
    auto left = WatchAxes(d, 5000, &centre);
    const int steer = BestAxis(d, left, {}, 12000);
    int steer_left = 0, steer_right = 0;
    if (steer < 0) {
        Say(steer == -2 ? "Two axes moved about the same: the wheel cannot be told apart. Setup stopped."
                        : "The wheel did not move enough. Setup stopped.");
        ok = false;
    } else {
        steer_left = left[size_t(steer)].extreme;
        Say("GO: now turn the wheel fully RIGHT and hold (5 s)");
        const int left_sign = steer_left > centre[size_t(steer)] ? 1 : -1;
        auto right = WatchAxes(d, 5000, &centre, steer, -left_sign);
        steer_right = right[size_t(steer)].extreme;
        if (std::abs(steer_right - steer_left) < 20000) {
            Say("The left and right positions are too close. Setup stopped.");
            ok = false;
        } else {
            prof.steer_axis = steer;
            prof.invert_steer = steer_left > steer_right;
            prof.steer_center = std::abs(centre[size_t(steer)]) > 300 ? centre[size_t(steer)] : 0;
            axes_taken.push_back(steer);
            Say("Steering is axis " + std::to_string(steer) + (prof.invert_steer ? " (inverted)" : "") + ".");
        }
    }

    // 1b. The wheel's rotation angle: SDL cannot read it, and the game's steering scale depends on it.
    if (ok) {
        Say("GO: type the rotation angle your wheel is set to in its driver, in degrees (for example 900), then press "
            "Enter. Wait 15 s to keep the default of " + std::to_string(REXCVAR_GET(wheel_device_range)) + ".");
        const std::string typed = ReadLine(15000);
        const int deg = typed.empty() ? 0 : std::atoi(typed.c_str());
        if (deg >= 90 && deg <= 3600) {
            prof.device_range = deg;
            Say("Rotation angle: " + std::to_string(deg) + " degrees.");
        } else if (!typed.empty()) {
            Say("That is not a usable angle (90-3600); keeping the default.");
        } else {
            Say("Keeping the default angle.");
        }
    }

    // 2. Pedals
    struct PedalStep { const char* name; int slot; bool optional; };
    const PedalStep steps[3] = {{"ACCELERATOR", 0, false}, {"BRAKE", 1, false}, {"CLUTCH", 2, true}};
    int pedal_axis[3] = {-1, -1, -1};
    for (const PedalStep& st : steps) {
        if (!ok) break;
        std::string msg = std::string("Press the ") + st.name + " pedal all the way down and hold it (5 s)";
        if (st.optional) msg += ". No clutch? Just wait.";
        Say("Release every pedal. " + std::string(st.optional ? "(optional)" : ""));
        Sleep_ms(2000);
        Say("GO: " + msg);
        auto pk = WatchAxes(d, 5000);
        const int ax = BestAxis(d, pk, axes_taken, 12000);
        if (ax == -1 && st.optional) {
            Say("No clutch.");
            continue;
        }
        if (ax < 0) {
            Say(ax == -2 ? "Two axes moved about the same, so this pedal cannot be told from another."
                         : "That pedal did not move enough.");
            ok = false;
            break;
        }
        pedal_axis[st.slot] = ax;
        axes_taken.push_back(ax);
        prof.pedal_rest[st.slot] = pk[size_t(ax)].base;
        prof.pedal_full[st.slot] = pk[size_t(ax)].extreme;
        Say(std::string(st.name) + " is axis " + std::to_string(ax) + " (rest " + std::to_string(pk[size_t(ax)].base) +
            ", pressed " + std::to_string(pk[size_t(ax)].extreme) + ").");
    }
    prof.throttle_axis = pedal_axis[0];
    prof.brake_axis = pedal_axis[1];
    prof.clutch_axis = pedal_axis[2];

    // 3. Buttons
    std::vector<int> taken_buttons;
    struct ButtonStep { const char* what; uint16_t bit; bool required; };
    const ButtonStep bsteps[] = {
        {"the RIGHT paddle (shift up)", kRb, true},   {"the LEFT paddle (shift down)", kLb, true},
        {"the button you want as A / OK", kBtnA, false}, {"the button you want as B / back", kBtnB, false},
        {"START (pause)", kStart, false},             {"BACK / SELECT (map)", kBack, false},
        {"the button you want as X", kBtnX, false},   {"the button you want as Y (camera)", kBtnY, false},
    };
    if (ok) {
        Say("Buttons: press each one when asked. Wait 7 s to skip one that this wheel does not have.");
        for (const ButtonStep& bs : bsteps) {
            Sleep_ms(800);
            Say(std::string("GO: press ") + bs.what);
            const int b = WaitButton(d, 7000, taken_buttons);
            if (b < 0) {
                Say(bs.required ? "Skipped (the wheel will not shift with paddles)." : "Skipped.");
                continue;
            }
            prof.button[size_t(b)] = bs.bit;
            taken_buttons.push_back(b);
            Say("Button " + std::to_string(b) + " set.");
        }

        // 4. H-shifter (optional, all or nothing: wait on the first to skip)
        Sleep_ms(800);
        Say("GO: H-shifter (optional): move it into 1st gear and hold. Wait 8 s if there is none.");
        int first = WaitButton(d, 8000, taken_buttons);
        if (first >= 0) {
            prof.gear_button[1] = first;
            taken_buttons.push_back(first);
            Say("1st = button " + std::to_string(first));
            for (int g = 2; g <= 7; ++g) {
                Sleep_ms(500);
                Say("GO: move the shifter into gear " + std::to_string(g) + " (wait to stop here)");
                const int b = WaitButton(d, 6000, taken_buttons);
                if (b < 0) break;
                prof.gear_button[size_t(g)] = b;
                taken_buttons.push_back(b);
                Say("Gear " + std::to_string(g) + " = button " + std::to_string(b));
            }
            Say("GO: move the shifter into REVERSE (wait to skip)");
            const int rv = WaitButton(d, 6000, taken_buttons);
            if (rv >= 0) {
                prof.gear_button[0] = rv;
                Say("Reverse = button " + std::to_string(rv));
            }
        } else {
            Say("No H-shifter.");
        }
    }

    if (ok) {
        std::ostringstream body;
        body << "name = " << d.name << "\n";
        body << "steer_axis = " << prof.steer_axis << "\n";
        body << "invert_steer = " << (prof.invert_steer ? 1 : 0) << "\n";
        if (prof.steer_center != 0) body << "steer_center = " << prof.steer_center << "\n";
        const char* pn[3] = {"throttle", "brake", "clutch"};
        const int pa[3] = {prof.throttle_axis, prof.brake_axis, prof.clutch_axis};
        for (int i = 0; i < 3; ++i) {
            if (pa[i] < 0) continue;
            body << pn[i] << "_axis = " << pa[i] << "\n";
            body << pn[i] << "_rest = " << prof.pedal_rest[i] << "\n";
            body << pn[i] << "_full = " << prof.pedal_full[i] << "\n";
        }
        static const struct { uint16_t bit; const char* name; } names[] = {
            {kBtnA, "A"}, {kBtnB, "B"}, {kBtnX, "X"}, {kBtnY, "Y"}, {kLb, "LB"}, {kRb, "RB"},
            {kStart, "START"}, {kBack, "BACK"}};
        for (int b = 0; b < kMaxButtons; ++b) {
            if (!prof.button[size_t(b)]) continue;
            for (const auto& n : names) {
                if (n.bit == prof.button[size_t(b)]) body << "button." << b << " = " << n.name << "\n";
            }
        }
        for (int g = 0; g < 8; ++g) {
            if (prof.gear_button[size_t(g)] < 0) continue;
            body << "gear." << (g == 0 ? std::string("r") : std::to_string(g)) << " = " << prof.gear_button[size_t(g)] << "\n";
        }
        if (prof.device_range > 0) body << "device_range = " << prof.device_range << "\n";
        else body << "# device_range = 900   (the lock-to-lock rotation the wheel is set to, in degrees)\n";
        SaveProfileSection(d.vid, d.pid, body.str());
        Say("Done. Saved [" + Hex4(d.vid) + ":" + Hex4(d.pid) + "] to wheel_profiles.ini (the old file is wheel_profiles.ini.bak).");
        Say("If the wheel is set to something other than 900 degrees in its driver, add device_range = <degrees> to that section.");
    } else {
        Say("Setup did not finish; nothing was saved.");
    }
    SDL_CloseJoystick(d.js);
    rex::cvar::SetFlagByName("wheel_learn", "false");
    Sleep_ms(ok ? 5000 : 7000);
    g_wizard_active.store(false);
    ConsoleDown();
    sh.last_scan = Clock::time_point{};
}

// ---------------------------------------------------------------------------
// First run for a wheel
// ---------------------------------------------------------------------------

bool ProfileSectionExists(uint16_t vid, uint16_t pid) {
    std::ifstream in("wheel_profiles.ini");
    if (!in) return false;
    const std::string want = "[" + Hex4(vid) + ":" + Hex4(pid) + "]";
    std::string line;
    while (std::getline(in, line)) {
        if (Lower(Trim(line)) == want) return true;
    }
    return false;
}

#ifdef _WIN32
std::wstring Widen(const std::string& u8) {
    if (u8.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u8.data(), int(u8.size()), w.data(), n);
    return w;
}
#endif

// A wheel with no profile gets one question, once: set it up now? The answer yes starts the setup; no leaves
// a marker section so it is not asked again (the section holds no values, so the generic profile still applies).
void OfferSetup(Shared& sh) {
    if (!sh.have_dev || !REXCVAR_GET(wheel_auto_setup) || REXCVAR_GET(wheel_learn)) return;
    const std::string key = Hex4(sh.dev.vid) + ":" + Hex4(sh.dev.pid);
    if (sh.offered == key) return;
    if (ProfileSectionExists(sh.dev.vid, sh.dev.pid)) {
        sh.offered = key;
        return;
    }
    sh.offered = key;
    LARECOMP_APP_INFO("[wheel] first run for '{}' ({}): no profile yet, offering the setup", sh.dev.name, key);
#ifdef _WIN32
    WaitForGameWindow();
    const std::wstring text =
        L"Wheel found: " + Widen(sh.dev.name) + L" (" + Widen(key) + L")\n\n"
        L"There is no profile for it yet, so its axes and buttons are guessed.\n"
        L"Set it up now? It takes 2-3 minutes and opens a console window.";
    const int answer = MessageBoxW(nullptr, text.c_str(), L"larecomp wheel",
                                   MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
    if (answer == IDYES) {
        LARECOMP_APP_INFO("[wheel] setup accepted");
        rex::cvar::SetFlagByName("wheel_learn", "true");
    } else {
        LARECOMP_APP_INFO("[wheel] setup declined; it will not be asked again for {} (wheel_learn=true runs it)", key);
        SaveProfileSection(sh.dev.vid, sh.dev.pid, "name = " + sh.dev.name + "\nsetup = declined\n");
    }
#endif
}

// ---------------------------------------------------------------------------
// The wheel thread
// ---------------------------------------------------------------------------

bool WantSdl() {
    return Lower(REXCVAR_GET(wheel_device)) != "off" || REXCVAR_GET(wheel_virtual) != "off" ||
           REXCVAR_GET(wheel_learn);
}

void WheelThread() {
    Shared& sh = S();
    // This SDL instance has to stay away from every pad: the SDK's own driver owns those. Only
    // DirectInput devices are enumerated, and the HID drivers that would init a pad are off.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    while (true) {
        if (!WantSdl()) {
            if (sh.sdl_up) {
                if (sh.have_dev) CloseDevice(sh);
                EnsureVirtual(sh);
                SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
                sh.sdl_up = false;
                std::lock_guard<std::mutex> l(sh.snap_mutex);
                sh.snap_present = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (!sh.sdl_up) {
            if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) {
                LARECOMP_APP_WARN("[wheel] SDL joystick init failed: {}", SDL_GetError());
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            sh.sdl_up = true;
            sh.last_scan = Clock::time_point{};
            LARECOMP_APP_INFO("[wheel] SDL joystick layer up on the wheel thread");
        }

        if (REXCVAR_GET(wheel_learn)) {
            EnsureVirtual(sh);
            if (sh.virtual_js) FeedVirtual(sh);
            RunWizard(sh);
            continue;
        }
        const Clock::time_point now = Clock::now();
        if (sh.virtual_js) FeedVirtual(sh);
        SDL_UpdateJoysticks();
        if (sh.last_scan.time_since_epoch().count() == 0 ||
            now - sh.last_scan >= std::chrono::seconds(1)) {
            sh.last_scan = now;
            Scan(sh);
        }
        if (sh.have_dev && sh.dev.di) dinput::Poll(sh.dev.di);
        if (sh.have_dev) OfferSetup(sh);
        // A wheel whose every axis, button and hat reads exactly 0 is not being read: pedals at rest sit at an
        // end of their range, never at 0. Seen on a Fanatec where SDL returned nothing for minutes, which left the
        // game with both pedals half down and the wheel claiming the controls from the gamepad.
        if (sh.have_dev) {
            bool all_zero = true;
            for (int i = 0; i < sh.dev.naxes && all_zero; ++i) all_zero = Ax(sh.dev, i) == 0;
            for (int i = 0; i < sh.dev.nbuttons && all_zero; ++i) all_zero = !Bt(sh.dev, i);
            for (int i = 0; i < sh.dev.nhats && all_zero; ++i) all_zero = Ht(sh.dev, i) == 0;
            if (!all_zero) {
                sh.zero_since = {};
                if (sh.input_dead) {
                    sh.input_dead = false;
                    LARECOMP_APP_INFO("[wheel] the wheel's input is alive again");
                }
            } else {
                if (sh.zero_since.time_since_epoch().count() == 0) sh.zero_since = now;
                if (!sh.input_dead && !sh.dev.virt && !sh.dev.di_tried && now - sh.zero_since > std::chrono::seconds(2)) {
                    // SDL's DirectInput driver gets nothing: read the wheel ourselves, the same way, and log
                    // what each step returned.
                    sh.dev.di_tried = true;
                    bool live = false;
                    sh.dev.di = dinput::Open(sh.dev.vid, sh.dev.pid, &live);
                    if (sh.dev.di && live) {
                        LARECOMP_APP_WARN("[wheel] SDL reads nothing from '{}' but DirectInput does: reading the "
                                          "wheel through DirectInput from now on", sh.dev.name);
                        sh.zero_since = {};
                    } else if (sh.dev.di) {
                        dinput::Close(sh.dev.di);
                        sh.dev.di = nullptr;
                    }
                }
                if (!sh.input_dead && sh.zero_since.time_since_epoch().count() != 0 &&
                    now - sh.zero_since > std::chrono::seconds(2)) {
                    sh.input_dead = true;
                    LARECOMP_APP_WARN(
                        "[wheel] every axis and button of '{}' has read exactly 0 for 2 s: SDL is not getting "
                        "the wheel's state, so the wheel is ignored and the gamepad drives until it is. If it does not "
                        "come back, start with --wheel_ffb=false (force feedback holds the wheel exclusively) and "
                        "send the log.", sh.dev.name);
                    if (!sh.dev.virt && !sh.ffb_released) {
                        sh.ffb_released = true;
                        LARECOMP_APP_WARN("[wheel] ffb: releasing the force-feedback handle to see if it was starving "
                                          "the input; force feedback stays off for this session");
                        ffb::DeviceClosing();
                    }
                }
            }
        } else {
            sh.zero_since = {};
            sh.input_dead = false;
            sh.ffb_released = false;
        }
        WheelFrame frame_now;
        {
            std::lock_guard<std::mutex> l(sh.snap_mutex);
            sh.snap_present = sh.have_dev && !sh.input_dead;
            sh.any_wheel.store(sh.have_dev && !sh.input_dead, std::memory_order_relaxed);
            if (sh.have_dev) {
                sh.snap_frame = ReadFrame(sh.dev);
                frame_now = sh.snap_frame;
            }
            sh.snap_stamp = now;
        }
        if (sh.have_dev && REXCVAR_GET(wheel_trace) && now - sh.trace_stamp > std::chrono::seconds(1)) {
            sh.trace_stamp = now;
            std::ostringstream o;
            o << "axes:";
            for (int i = 0; i < sh.dev.naxes; ++i) o << " " << Ax(sh.dev, i);
            o << " | buttons down:";
            for (int i = 0; i < sh.dev.nbuttons; ++i) {
                if (Bt(sh.dev, i)) o << " " << i;
            }
            o << " | hat0: " << (sh.dev.nhats > 0 ? int(Ht(sh.dev, 0)) : -1);
            char fb[160];
            std::snprintf(fb, sizeof fb, " | frame: steer %+.3f throttle %.3f brake %.3f buttons 0x%04x",
                          frame_now.steer, frame_now.throttle, frame_now.brake, unsigned(frame_now.buttons));
            LARECOMP_APP_INFO("[wheel] trace {}{} | wheel drives: {}", o.str(), fb,
                              sh.wheel_active ? "yes" : "no");
        }
        if (sh.have_dev && !sh.ffb_released) {
            ffb::TickIn in;
            in.now = now;
            in.pos = frame_now.pos;
            in.pos_raw = frame_now.pos_raw;
            {
                // Debounced: an overlay or a system dialog takes the foreground for a moment, and a pause that
                // long would drop the forces for seconds. Only a window that stays away counts as lost.
                static Clock::time_point last_focus{};
                if (WindowFocused()) last_focus = now;
                in.focused = now - last_focus < std::chrono::milliseconds(1500);
            }
            in.driving = sh.driving.load(std::memory_order_relaxed);
            in.frame = sh.frame.load(std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> l(sh.tele_mutex);
                in.speed_ms = now - sh.tele_stamp < std::chrono::milliseconds(400) ? sh.tele_speed : 0.0f;
            }
            ffb::Tick(in);
        }
        // 250 Hz with a wheel to read; with none, only the once-a-second scan has anything to do.
        std::this_thread::sleep_for(std::chrono::milliseconds(sh.have_dev || sh.virtual_js ? 4 : 50));
    }
}

void EnsureThread() {
    Shared& sh = S();
    if (!sh.started.exchange(true)) {
        std::thread(WheelThread).detach();
    }
}

// ---------------------------------------------------------------------------
// Who drives (guest side)
// ---------------------------------------------------------------------------

bool PadMoved(const PadIn& p) {
    const int stick = std::max({std::abs(int(p.lx)), std::abs(int(p.ly)), std::abs(int(p.rx)),
                                std::abs(int(p.ry))});
    return stick > 11500 || p.lt > 90 || p.rt > 90 || (p.buttons & (kLb | kRb | kBtnX | kBtnY)) != 0;
}

bool WheelMoved(const WheelFrame& f, float last_steer) {
    return std::fabs(f.steer - last_steer) > 0.03f || std::fabs(f.steer) > 0.25f ||
           f.throttle > 0.2f || f.brake > 0.2f || (f.buttons & ~kMenuMask) != 0;
}

// The latest snapshot, or false if there is no wheel or the wheel thread has gone quiet (a stale
// snapshot would hold the last pedal position forever).
bool Snapshot(WheelFrame& out) {
    Shared& sh = S();
    std::lock_guard<std::mutex> l(sh.snap_mutex);
    if (!sh.snap_present) return false;
    if (Clock::now() - sh.snap_stamp > std::chrono::milliseconds(500)) return false;
    out = sh.snap_frame;
    return true;
}

}  // namespace

void EarlyInit() {
    // Wheels SDL is likely to also map as a gamepad would otherwise be opened twice: by the SDK's
    // SDL input driver as a stick on user 0, and by us. Known models are hidden from the gamepad
    // layer only; they stay visible as joysticks. The SDK's SDL is another instance (inside
    // rexruntime.dll), so a hint set here would not reach it: SDL also reads hints from the
    // environment, which both see.
    std::string list;
    auto add = [&](uint16_t v, uint16_t p) {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%04x/0x%04x,", v, p);
        list += b;
    };
    for (uint16_t p : {0xc294, 0xc295, 0xc298, 0xc299, 0xc29a, 0xc29b, 0xc24f}) add(0x046d, p);
    for (uint16_t p : {0xb65d, 0xb65e, 0xb66d, 0xb66e, 0xb66f, 0xb677, 0xb67e, 0xb689, 0xb669, 0xb664}) {
        add(0x044f, p);
    }
    // A wheel the user named by vendor:product is theirs too.
    const std::string want = Lower(REXCVAR_GET(wheel_device));
    if (want.size() == 9 && want[4] == ':') {
        list += "0x" + want.substr(0, 4) + "/0x" + want.substr(5) + ",";
    }
    if (!list.empty()) list.pop_back();
#ifdef _WIN32
    // Keep whatever the user already put there.
    char existing[1024] = {};
    const DWORD n = GetEnvironmentVariableA("SDL_GAMECONTROLLER_IGNORE_DEVICES", existing, sizeof(existing));
    if (n > 0 && n < sizeof(existing)) list = std::string(existing) + "," + list;
    SetEnvironmentVariableA("SDL_GAMECONTROLLER_IGNORE_DEVICES", list.c_str());
#endif
    EnsureThread();
}

bool WheelPresent() {
    EnsureThread();
    if (Lower(REXCVAR_GET(wheel_mode)) == "gamepad") return false;
    WheelFrame f;
    return Snapshot(f);
}

void NoteInputFrame() {
    S().frame.fetch_add(1, std::memory_order_relaxed);
}

void NoteHudGear(int gear, float speed_ms) {
    Shared& sh = S();
    std::lock_guard<std::mutex> l(sh.tele_mutex);
    // Quiet unless there is a wheel: everyone's HUD calls this, and a gear change is not news without one.
    if (gear != sh.tele_gear && sh.any_wheel.load(std::memory_order_relaxed)) {
        LARECOMP_APP_INFO("[wheel] car gear {} -> {} ({:.1f} m/s)", sh.tele_gear, gear, speed_ms);
    }
    sh.tele_gear = gear;
    sh.tele_speed = speed_ms;
    sh.tele_stamp = Clock::now();
}

namespace {

// One poll of the H-shifter emulation. The game shifts one gear per RB (up) or LB (down) press and
// only in a Manual map, so a lever position becomes a short run of presses: the car's gear, as the
// HUD reports it, says how far there is to go, and each press is checked against it. Returns the
// button bits to add. The lever has to rest in a position for 80 ms first, so passing through
// neutral or through the gears on the way does not shift anything.
uint16_t ShifterBits(Shared& sh, const WheelFrame& f, bool manual, Clock::time_point now) {
    constexpr auto kSettle = std::chrono::milliseconds(80);
    constexpr auto kGap = std::chrono::milliseconds(60);
    constexpr auto kShiftWait = std::chrono::milliseconds(700);

    auto idle = [&]() {
        sh.sh_pulse = 0;
        sh.sh_pulses_sent = 0;
    };
    if (f.shifter == kNoGear && sh.sh_request == kNoGear && sh.sh_pulse == 0) return 0;
    int gear;
    float speed;
    bool fresh;
    {
        std::lock_guard<std::mutex> l(sh.tele_mutex);
        gear = sh.tele_gear;
        speed = sh.tele_speed;
        fresh = now - sh.tele_stamp < std::chrono::milliseconds(400);
    }
    if (f.shifter != sh.sh_request) {
        sh.sh_request = f.shifter;
        sh.sh_request_since = now;
        if (sh.sh_pulse != 1) idle();  // a press in progress is let go of below
        LARECOMP_APP_INFO("[wheel] shifter lever -> {} (car gear {}, {} transmission)",
                          f.shifter == kNoGear ? std::string("neutral")
                                               : (f.shifter < 0 ? std::string("R") : std::to_string(f.shifter)),
                          gear, manual ? "manual" : "automatic");
    }
    if (!fresh || !manual) {
        if (!sh.sh_warned && sh.sh_request != kNoGear) {
            sh.sh_warned = true;
            LARECOMP_APP_INFO("[wheel] shifter lever moved but {}; nothing is shifted",
                              !manual ? "the transmission is Automatic (pick a Manual control map)"
                                      : "the car's gear is not known (the HUD is not up)");
        }
        idle();
        return 0;
    }
    sh.sh_warned = false;

    // The game registers one shift per frame the button is down (its own debounce only applies when
    // the direction flips), and a frame at 120 fps is 8 ms: the press is exactly one input frame
    // long, then released for at least kGap and two frames.
    const uint64_t frame = sh.frame.load(std::memory_order_relaxed);
    if (sh.sh_pulse == 1) {
        // The time limit is a safety net: if frames stop counting, a press must not stay down.
        if (frame == sh.sh_press_frame && now - sh.sh_phase_since < std::chrono::milliseconds(100)) {
            return sh.sh_dir > 0 ? kRb : kLb;
        }
        sh.sh_pulse = 2;
        sh.sh_phase_since = now;
        sh.sh_release_frame = frame;
        return 0;
    }
    if (sh.sh_pulse == 2) {
        if (now - sh.sh_phase_since < kGap || frame < sh.sh_release_frame + 2) return 0;
        // Wait for the game to take the shift (the HUD gear changes), or give up and look again.
        if (gear == sh.sh_gear_at_press && now - sh.sh_phase_since < kShiftWait) return 0;
        sh.sh_pulse = 0;
    }

    if (sh.sh_request == kNoGear) {
        idle();
        return 0;
    }
    if (now - sh.sh_request_since < kSettle) return 0;
    const int want = sh.sh_request;
    if (want == gear) {
        idle();
        return 0;
    }
    // Reverse from speed would be a mistake the lever makes easy; the game does not forbid it.
    if (want < 0 && gear >= 0 && speed > 3.0f) return 0;
    if (sh.sh_pulses_sent >= 9) return 0;  // something refuses the shift; wait for the lever to move
    sh.sh_dir = want > gear ? 1 : -1;
    sh.sh_gear_at_press = gear;
    sh.sh_pulse = 1;
    sh.sh_press_frame = frame;
    sh.sh_phase_since = now;
    ++sh.sh_pulses_sent;
    LARECOMP_APP_INFO("[wheel] shifter: {} (gear {} -> {}), press {}", sh.sh_dir > 0 ? "up" : "down",
                      gear, want, sh.sh_pulses_sent);
    return sh.sh_dir > 0 ? kRb : kLb;
}

}  // namespace

bool ApplyToState(PadIn& pad, bool manual_transmission) {
    EnsureThread();
    Shared& sh = S();
    if (Lower(REXCVAR_GET(wheel_mode)) == "gamepad") {
        sh.driving.store(false, std::memory_order_relaxed);
        return false;
    }
    WheelFrame f;
    if (!Snapshot(f)) return false;

    std::lock_guard<std::mutex> lock(sh.policy_mutex);
    const std::string mode = Lower(REXCVAR_GET(wheel_mode));
    const Clock::time_point now = Clock::now();
    if (!sh.was_present) {
        // A wheel just appeared (or came back): it drives until the gamepad is moved.
        sh.was_present = true;
        sh.wheel_active = true;
        sh.last_switch = now;
        sh.last_steer = f.steer;
    }

    if (mode == "wheel") {
        sh.wheel_active = true;
    } else if (now - sh.last_switch > std::chrono::milliseconds(1500)) {
        if (sh.wheel_active && PadMoved(pad) && !WheelMoved(f, sh.last_steer)) {
            sh.wheel_active = false;
            sh.last_switch = now;
            LARECOMP_APP_INFO("[wheel] gamepad moved: the gamepad drives");
        } else if (!sh.wheel_active && WheelMoved(f, sh.last_steer)) {
            sh.wheel_active = true;
            sh.last_switch = now;
            LARECOMP_APP_INFO("[wheel] wheel moved: the wheel drives");
        }
    }
    sh.last_steer = f.steer;
    sh.driving.store(sh.wheel_active, std::memory_order_relaxed);

    // The game takes a dead zone off every analog axis it reads; give it back (wheel_cancel_game_deadzone).
    const float dz = float(std::clamp(REXCVAR_GET(wheel_cancel_game_deadzone), 0.0, 0.3));
    auto lift = [&](float v, float noise) {
        const float a = std::fabs(v);
        if (a <= noise) return 0.0f;
        if (dz <= 0.0f) return v;
        const float o = dz + (1.0f - dz) * a;
        return v < 0 ? -o : o;
    };
    f.steer = lift(f.steer, 0.0005f);
    f.throttle = lift(f.throttle, 0.004f);
    f.brake = lift(f.brake, 0.004f);

    // A wheel shows no left stick Y or right stick.
    pad.ly = pad.rx = pad.ry = 0;
    uint16_t shift_bits = 0;
    if (sh.wheel_active) shift_bits = ShifterBits(sh, f, manual_transmission, now);
    if (sh.wheel_active) {
        pad.lx = int16_t(std::lround(f.steer * 32767.0f));
        pad.rt = uint8_t(std::lround(f.throttle * 255.0f));
        pad.lt = uint8_t(std::lround(f.brake * 255.0f));
        pad.buttons = uint16_t(f.buttons | shift_bits | (pad.buttons & kMenuMask));
    } else {
        pad.buttons = uint16_t(pad.buttons | (f.buttons & kMenuMask));
    }
    return true;
}

}  // namespace larecomp::wheel

#endif  // REXGLUE_HAS_XEO3_TARGET
