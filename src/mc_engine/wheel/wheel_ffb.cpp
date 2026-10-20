// See wheel_ffb.h.
//
// What can go wrong with force feedback is a wheel that fights its driver, and a bug here does
// that at several kilos of torque, so the safety rules come first and the physics second:
//
//   * One thread (the wheel thread) makes every device call. Commands arrive through a queue.
//   * Every command carries a generation. A stop, a reset, a device change or an emergency
//     bumps it, and queued commands from before are dropped: nothing waiting can restart a force.
//   * Forces are rate-limited and clamped, scaled by wheel_ffb_strength, and start at zero.
//   * Normal stop (the game window loses focus, the gamepad drives, the game stops sending
//     input frames): every effect to zero and stopped, nothing destroyed, resumes by itself.
//   * Emergency stop (panic cvar, repeated device errors, a call that hangs, device gone): gain to
//     zero, stop all, destroy. The engine stays disarmed until wheel_ffb_rearm is switched on.
//     There is no software guarantee against a driver that has stopped answering.
//   * The direction of positive force differs between wheels and drivers. The one force that can
//     do harm if that is wrong is the centring spring, so centring never depends on it: it is a
//     DirectInput spring condition, which by definition pulls towards the middle, and runs in the
//     wheel's firmware. Only the game's constant forces and vibrations use the measured sign
//     (wheel_ffb_sign=auto: a gentle push, once per device, disarmed if the wheel does not move).
//
// Three DirectInput effects on one axis:
//   constant   the game's constant forces and vibrations (and everything, as a fallback, when the
//              wheel will not make the other two), summed in software;
//   spring     centring: the game's own spring when it runs one, else ours (wheel_ffb_centering);
//   damper     the game's damper, else a light one of ours.
// The conditions are updated a few times a second and only when they change.
//
// The device calls are the part that cannot be tried without a wheel. A virtual joystick has no
// force-feedback device, so there the engine runs "log only": the whole path up to the force is
// exercised and the force is written to the log instead of the device. wheel_ffb_selftest plays a
// staged test and logs every device call's result and the wheel's position, so a report from a real
// wheel says by itself whether force reached the rim.
//
// Why DirectInput and not SDL's haptic API: the SDL3 this exe links (the SDK's build) is built
// without haptic support. On Windows SDL's haptic is DirectInput underneath anyway, and works the
// same way this does: the joystick (read through SDL, in wheel_input.cpp) and the force-feedback
// device (here) are two separate DirectInput handles on the same wheel, the second one exclusive
// and in the background, which force feedback requires.

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "wheel_ffb.h"

#include <rex/cvar.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <initguid.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "larecomp_log.h"

REXCVAR_DEFINE_BOOL(wheel_ffb, true, "MCLA/Wheel",
    "Force feedback on the steering wheel. Off: the wheel is never given a force.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_ffb_strength, 70, "MCLA/Wheel",
    "Force feedback strength, 0-100: the share of the wheel's full force that the game's strongest effect "
    "reaches. The game's own forces are mostly 10-40% of its scale (it was written for a light 360 wheel), so a "
    "direct-drive wheel needs this high to feel them; a belt or gear wheel may want it lower. Check "
    "wheel_ffb_trace for the peak force the game actually asks for.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_ffb_boost, 200, "MCLA/Wheel",
    "Multiplies the forces the GAME asks for (constant, vibration, its spring and damper), 100-500 %, before "
    "wheel_ffb_strength. The game's forces are mostly 10-40% of its scale, written for a light 360 wheel, so on a "
    "strong base they feel weak: 200 doubles them, with the result capped at the wheel's full force. Centring "
    "added by this program (wheel_ffb_centering) is not multiplied.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(wheel_ffb_sign, "auto", "MCLA/Wheel",
    "Which way a positive constant force turns the wheel (centring does not use it). auto: measured once per "
    "device with a gentle push (keep your hands off the wheel); normal / inverted: skip the test. If the game's "
    "constant forces push into the turn instead of against it, set the other one.")
    .allowed({"auto", "normal", "inverted"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_ffb_centering, 40, "MCLA/Wheel",
    "Centring added by this program while the game is not running its own spring (it does not while "
    "free-roaming), 0-100: the most force the centring spring can reach, before wheel_ffb_strength. It is "
    "about a third of that when parked and grows with the car's speed, like the self-aligning force of a real "
    "car, so the wheel returns to centre when let go. It is a native DirectInput spring (the wheel's firmware "
    "does the work) when the wheel allows one. 0 leaves the wheel slack between the game's effects.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_autocenter, true, "MCLA/Wheel",
    "Only used when wheel_ffb_centering is 0: leave the wheel driver's own centring spring on. With centring "
    "above 0 the driver's spring is switched off (two springs would fight). Takes effect when the wheel is "
    "opened.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_panic, false, "MCLA/Wheel",
    "Emergency stop for the wheel's force feedback: switching it on stops every force and "
    "disarms until wheel_ffb_rearm is switched on.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_rearm, false, "MCLA/Wheel",
    "Switch on to arm force feedback again after an emergency stop (it re-checks the wheel first). "
    "Switch it off again before the next time.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(wheel_ffb_watchdog_ms, 500, "MCLA/Wheel",
    "Force goes to zero when the game has not run an input frame for this long (a loading screen, "
    "a hang) and comes back by itself when it does.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_selftest, false, "MCLA/Wheel",
    "When force feedback arms, play a staged test and log it: 0.5 s push one way, 0.5 s the other, 2 s of "
    "centring spring (turn the wheel a little and let go), 1 s of damper. Every device call's result and the "
    "wheel's position (every 50 ms) go to the log, so a dead force path shows in the log whatever you feel. "
    "Needs the wheel to be free.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(wheel_ffb_trace, false, "MCLA/Wheel",
    "Log the force sent to the wheel about four times a second, every device call's result, and every state "
    "change.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace larecomp::wheel::ffb {

namespace {

// ---------------------------------------------------------------------------
// Commands (any thread -> wheel thread)
// ---------------------------------------------------------------------------

enum class Op : uint8_t { kSet, kUpdate, kStart, kStop, kReset };

struct Cmd {
    Op op = Op::kSet;
    uint32_t generation = 0;
    int32_t id = 0;
    int32_t w[kEffectWords] = {};
};

struct Queue {
    std::mutex m;
    std::vector<Cmd> items;
    std::atomic<uint32_t> generation{1};
};

Queue& Q() {
    static Queue* q = new Queue();
    return *q;
}

void Push(const Cmd& c) {
    Queue& q = Q();
    std::lock_guard<std::mutex> l(q.m);
    // A guest that keeps posting while nothing drains (no wheel thread tick) must not grow without bound.
    if (q.items.size() < 4096) q.items.push_back(c);
}

// ---------------------------------------------------------------------------
// The mirror of the game's effects (wheel thread only)
// ---------------------------------------------------------------------------

struct Fx {
    int32_t w[kEffectWords] = {};
    bool started = false;
    Clock::time_point start{};
};

enum class State { kNoDevice, kCalibrating, kArmed, kEmergency };

// A spring or damper to play: k is the stiffness (0..1 of the device's maximum), sat the most force it may
// reach (0..1 of the wheel's full force, before strength), off the centre of a spring (-1..1).
struct Cond {
    bool on = false;
    float k = 0.0f;
    float sat = 0.0f;
    float off = 0.0f;
    float soft_gain = 1.0f;  // stiffness per unit of lock when a spring has to be played in software
};

struct Mix {
    float constant = 0.0f;   // -1..1, the game's constant forces and vibrations
    bool periodic = false;
    Cond spring;
    Cond damper;
};

struct Engine {
    State state = State::kNoDevice;
    bool log_only = true;
    bool native = false;           // spring and damper effects exist on the device
    bool fx_running = false;       // the constant effect has been started
    bool nat_running = false;      // spring and damper started
    int sign = 1;
    uint32_t engine_gen = 1;
    std::unordered_map<int32_t, Fx> mirror;

    // Output
    float level = 0.0f;            // slewed constant force, -1..1, before sign and strength
    int last_sent = 0;
    int last_nat[6] = {0, 0, 0, 0, 0, 0};   // spring offset / coef / sat, damper coef / sat / unused (device units)
    Clock::time_point last_send{};
    Clock::time_point last_nat_send{};
    Clock::time_point last_trace{};
    Clock::time_point last_status{};
    Clock::time_point last_summary{};
    float peak_out = 0.0f;         // largest |force| in this summary interval, 0..1 of the wheel's full force
    uint32_t cmds_seen = 0;        // game commands applied in this interval
    int consecutive_failures = 0;
    bool paused = false;

    // Self-test
    Clock::time_point selftest_start{};   // 0 = not running
    bool selftest_done = false;
    Clock::time_point selftest_pos_log{};

    // Heartbeat
    uint64_t last_frame = 0;
    Clock::time_point last_frame_change{};

    // Position / speed
    float last_pos = 0.0f;
    float vel = 0.0f;
    Clock::time_point last_tick{};

    // Calibration of the constant force's sign: a ladder of nudges, the gentlest first
    Clock::time_point calib_start{};
    int calib_phase = 0;           // 0 settle, 1 pushing, 2 resting before the next, wheel read after each push
    int calib_try = 0;
    float calib_pos0 = 0.0f;

    uint16_t dev_vid = 0, dev_pid = 0;
    bool dev_virtual = false;
    bool rearm_seen = false;       // last value of wheel_ffb_rearm
    bool warned_unsupported = false;
    std::set<std::string> reported;  // device calls whose failure has been logged
};

Engine& E() {
    static Engine* e = new Engine();
    return *e;
}

const char* TypeName(int t) {
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

bool Trace() { return REXCVAR_GET(wheel_ffb_trace); }
bool Selftesting(const Engine& e) { return e.selftest_start.time_since_epoch().count() != 0; }

// ---------------------------------------------------------------------------
// The force-feedback device (wheel thread)
// ---------------------------------------------------------------------------

constexpr int kFull = 32767;
constexpr int kDi = 10000;  // DirectInput's nominal range

// Times a call: one that takes this long means the driver has stopped answering.
class CallTimer {
 public:
    CallTimer() : t0_(Clock::now()) {}
    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0_).count();
    }
 private:
    Clock::time_point t0_;
};

// Result of a device call: the first failure of each kind is always logged (with its HRESULT); successes only
// when the self-test or the trace is on.
bool Hr(Engine& e, const char* what, long hr) {
    const bool ok = hr >= 0;
    if (!ok) {
        if (e.reported.insert(what).second) {
            LARECOMP_APP_WARN("[wheel] ffb: {} failed: HRESULT 0x{:08X}", what, static_cast<uint32_t>(hr));
        }
    } else if (Selftesting(e) || Trace()) {
        LARECOMP_APP_INFO("[wheel] ffb: {} ok (0x{:08X})", what, static_cast<uint32_t>(hr));
    }
    return ok;
}

#ifdef _WIN32

// One DirectInput device with up to three effects on its first axis. DirectInput is loaded at run time
// (nothing to link), and the device gets a data format of a single axis because this handle never reads
// input: only an axis to attach the forces to.
struct Di {
    HMODULE lib = nullptr;
    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* dev = nullptr;
    IDirectInputEffect* effect = nullptr;   // constant force
    IDirectInputEffect* spring = nullptr;
    IDirectInputEffect* damper = nullptr;
    HWND helper = nullptr;
    DICONSTANTFORCE cf{};
    DICONDITION sc{};
    DICONDITION dc{};
    DIEFFECT eff{};
    DIEFFECT seff{};
    DIEFFECT deff{};
    LONG axis = 0;       // effect axis: object offset 0 in the data format below
    LONG dir = 0;
    DWORD ff_period_us = 0;
};

struct FindCtx {
    DWORD product = 0;  // MAKELONG(vendor, product), how DirectInput builds a product GUID
    GUID instance{};
    bool found = false;
    wchar_t name[MAX_PATH] = {};
};

BOOL CALLBACK FindDevice(LPCDIDEVICEINSTANCEW inst, LPVOID ref) {
    auto* ctx = static_cast<FindCtx*>(ref);
    if (inst->guidProduct.Data1 == ctx->product) {
        ctx->instance = inst->guidInstance;
        ctx->found = true;
        std::wcsncpy(ctx->name, inst->tszProductName, MAX_PATH - 1);
        return DIENUM_STOP;
    }
    return DIENUM_CONTINUE;
}

typedef HRESULT(WINAPI* DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

void ReleaseEffect(IDirectInputEffect*& fx) {
    if (!fx) return;
    fx->Stop();
    fx->Unload();
    fx->Release();
    fx = nullptr;
}

void DiRelease(Di& d) {
    ReleaseEffect(d.effect);
    ReleaseEffect(d.spring);
    ReleaseEffect(d.damper);
    if (d.dev) {
        d.dev->SendForceFeedbackCommand(DISFFC_STOPALL);
        d.dev->Unacquire();
        d.dev->Release();
        d.dev = nullptr;
    }
    if (d.di) {
        d.di->Release();
        d.di = nullptr;
    }
    if (d.helper) {
        DestroyWindow(d.helper);
        d.helper = nullptr;
    }
    if (d.lib) {
        FreeLibrary(d.lib);
        d.lib = nullptr;
    }
}

void FillEffect(Di& d, DIEFFECT& e, void* params, DWORD cb) {
    e = DIEFFECT{};
    e.dwSize = sizeof(DIEFFECT);
    e.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    e.dwDuration = INFINITE;
    e.dwSamplePeriod = 0;
    e.dwGain = DI_FFNOMINALMAX;
    e.dwTriggerButton = DIEB_NOTRIGGER;
    e.dwTriggerRepeatInterval = 0;
    e.cAxes = 1;
    e.rgdwAxes = reinterpret_cast<LPDWORD>(&d.axis);
    e.rglDirection = &d.dir;
    e.lpEnvelope = nullptr;
    e.cbTypeSpecificParams = cb;
    e.lpvTypeSpecificParams = params;
    e.dwStartDelay = 0;
}

// Finds the force-feedback device for vendor:product, takes it exclusively and creates the effects.
bool DiOpen(Engine& e, Di& d, uint16_t vid, uint16_t pid) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // already initialised on this thread is fine
    d.lib = LoadLibraryW(L"dinput8.dll");
    if (!d.lib) {
        LARECOMP_APP_WARN("[wheel] ffb: dinput8.dll is not available");
        return false;
    }
    auto create = reinterpret_cast<DirectInput8CreateFn>(GetProcAddress(d.lib, "DirectInput8Create"));
    if (!create || !Hr(e, "DirectInput8Create", create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                                       IID_IDirectInput8W, reinterpret_cast<void**>(&d.di), nullptr))) {
        return false;
    }
    FindCtx ctx;
    ctx.product = MAKELONG(vid, pid);
    d.di->EnumDevices(DI8DEVCLASS_GAMECTRL, FindDevice, &ctx, DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);
    if (!ctx.found) {
        LARECOMP_APP_INFO("[wheel] ffb: DirectInput lists no force-feedback device for {:04x}:{:04x}", vid, pid);
        return false;
    }
    if (!Hr(e, "CreateDevice", d.di->CreateDevice(ctx.instance, &d.dev, nullptr))) return false;

    // One axis, absolute, at offset 0.
    static DIOBJECTDATAFORMAT obj = {&GUID_XAxis, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0};
    DIDATAFORMAT fmt = {};
    fmt.dwSize = sizeof(fmt);
    fmt.dwObjSize = sizeof(DIOBJECTDATAFORMAT);
    fmt.dwFlags = DIDF_ABSAXIS;
    fmt.dwDataSize = 4;
    fmt.dwNumObjs = 1;
    fmt.rgodf = &obj;
    if (!Hr(e, "SetDataFormat", d.dev->SetDataFormat(&fmt))) return false;

    // Force feedback needs exclusive access, and a window; a hidden one of our own is enough.
    d.helper = CreateWindowExW(0, L"STATIC", L"larecomp wheel ffb", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!d.helper || !Hr(e, "SetCooperativeLevel(exclusive, background)",
                         d.dev->SetCooperativeLevel(d.helper, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) {
        LARECOMP_APP_WARN("[wheel] ffb: could not take the wheel exclusively (is another program using its "
                          "force feedback?)");
        return false;
    }

    // The driver's own centring spring: off whenever this program centres the wheel (two springs fight),
    // on only when centring is switched off and wheel_ffb_autocenter asks for it. Has to be set before the
    // device is acquired.
    const bool driver_spring = REXCVAR_GET(wheel_ffb_centering) <= 0 && REXCVAR_GET(wheel_ffb_autocenter);
    DIPROPDWORD ac = {};
    ac.diph.dwSize = sizeof(ac);
    ac.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    ac.diph.dwObj = 0;
    ac.diph.dwHow = DIPH_DEVICE;
    ac.dwData = driver_spring ? DIPROPAUTOCENTER_ON : DIPROPAUTOCENTER_OFF;
    Hr(e, "SetProperty(DIPROP_AUTOCENTER)", d.dev->SetProperty(DIPROP_AUTOCENTER, &ac.diph));
    DIPROPDWORD gain = ac;
    gain.dwData = kDi;
    Hr(e, "SetProperty(DIPROP_FFGAIN)", d.dev->SetProperty(DIPROP_FFGAIN, &gain.diph));

    if (!Hr(e, "Acquire", d.dev->Acquire())) {
        LARECOMP_APP_WARN("[wheel] ffb: could not acquire the wheel exclusively");
        return false;
    }

    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(d.dev->GetCapabilities(&caps))) {
        d.ff_period_us = caps.dwFFSamplePeriod;
        LARECOMP_APP_INFO("[wheel] ffb: DirectInput force-feedback device '{}' open: axes {}, buttons {}, "
                          "sample period {} us, min time resolution {} us, driver spring {}",
                          std::string(ctx.name, ctx.name + std::wcslen(ctx.name)), caps.dwAxes, caps.dwButtons,
                          caps.dwFFSamplePeriod, caps.dwFFMinTimeResolution, driver_spring ? "on" : "off");
    }

    d.axis = 0;  // DIJOFS_X: the offset of the one object in the data format
    d.dir = 0;
    d.cf.lMagnitude = 0;
    FillEffect(d, d.eff, &d.cf, sizeof(DICONSTANTFORCE));
    if (!Hr(e, "CreateEffect(constant force)", d.dev->CreateEffect(GUID_ConstantForce, &d.eff, &d.effect, nullptr)) ||
        !d.effect) {
        LARECOMP_APP_WARN("[wheel] ffb: the wheel cannot play a constant force, which is the least this needs; "
                          "force feedback is off for this device");
        d.effect = nullptr;
        return false;
    }

    // Centring and damping as native conditions, at zero until something asks for them. A wheel that refuses
    // them still works: the same forces are then summed into the constant one in software.
    d.sc = DICONDITION{};
    d.dc = DICONDITION{};
    FillEffect(d, d.seff, &d.sc, sizeof(DICONDITION));
    FillEffect(d, d.deff, &d.dc, sizeof(DICONDITION));
    const bool spring_ok = Hr(e, "CreateEffect(spring)", d.dev->CreateEffect(GUID_Spring, &d.seff, &d.spring, nullptr)) && d.spring;
    const bool damper_ok = Hr(e, "CreateEffect(damper)", d.dev->CreateEffect(GUID_Damper, &d.deff, &d.damper, nullptr)) && d.damper;
    if (!spring_ok) d.spring = nullptr;
    if (!damper_ok) d.damper = nullptr;
    e.native = spring_ok;   // centring is the one that matters; a missing damper is only a missing damper
    LARECOMP_APP_INFO("[wheel] ffb: effects: constant ok, spring {}, damper {} -> centring is {}",
                      spring_ok ? "ok" : "NOT AVAILABLE", damper_ok ? "ok" : "not available",
                      spring_ok ? "native (the wheel's firmware)" : "done in software through the constant force");
    return true;
}

// Sets the level (-kFull..kFull) on the constant effect. DirectInput's nominal range is +-10000.
long DiSetLevel(Di& d, int level) {
    if (!d.effect) return -1;
    d.cf.lMagnitude = static_cast<LONG>(std::clamp(level, -kFull, kFull)) * kDi / kFull;
    return d.effect->SetParameters(&d.eff, DIEP_TYPESPECIFICPARAMS);
}

// Sets a condition effect: offset, coefficient (one for both sides), saturation, all in DirectInput units.
long DiSetCondition(IDirectInputEffect* fx, DIEFFECT& eff, DICONDITION& c, int offset, int coef, int sat) {
    if (!fx) return -1;
    c.lOffset = offset;
    c.lPositiveCoefficient = coef;
    c.lNegativeCoefficient = coef;
    c.dwPositiveSaturation = static_cast<DWORD>(std::clamp(sat, 0, kDi));
    c.dwNegativeSaturation = static_cast<DWORD>(std::clamp(sat, 0, kDi));
    c.lDeadBand = 0;
    return fx->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
}

Di g_di;
#else
struct Di {};
bool DiOpen(Engine&, Di&, uint16_t, uint16_t) { return false; }
void DiRelease(Di&) {}
#endif

void EmergencyStop(Engine& e, const char* why);

void BumpGeneration(Engine& e) {
    e.engine_gen = Q().generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Every effect to zero and stopped, nothing destroyed.
void NormalStop(Engine& e) {
    e.level = 0.0f;
    e.last_sent = 0;
    std::memset(e.last_nat, 0, sizeof(e.last_nat));
#ifdef _WIN32
    if (!e.log_only) {
        if (g_di.effect && e.fx_running) {
            DiSetLevel(g_di, 0);
            Hr(e, "Stop(constant)", g_di.effect->Stop());
        }
        if (e.nat_running) {
            if (g_di.spring) {
                DiSetCondition(g_di.spring, g_di.seff, g_di.sc, 0, 0, 0);
                Hr(e, "Stop(spring)", g_di.spring->Stop());
            }
            if (g_di.damper) {
                DiSetCondition(g_di.damper, g_di.deff, g_di.dc, 0, 0, 0);
                Hr(e, "Stop(damper)", g_di.damper->Stop());
            }
        }
    }
#endif
    e.fx_running = false;
    e.nat_running = false;
}

bool EnsureRunning(Engine& e) {
#ifdef _WIN32
    if (e.log_only || !g_di.effect) return true;
    if (e.fx_running) return true;
    CallTimer t;
    const long hr = g_di.effect->Start(1, 0);
    const bool ok = Hr(e, "Start(constant)", hr);
    if (t.ms() > 100) {
        EmergencyStop(e, "starting the effect took too long: the device is not answering");
        return false;
    }
    if (!ok) {
        if (++e.consecutive_failures >= 3) EmergencyStop(e, "the force effect would not start");
        return false;
    }
    e.fx_running = true;
    e.consecutive_failures = 0;
#else
    (void)e;
#endif
    return true;
}

void SendLevel(Engine& e, int level, Clock::time_point now) {
    if (e.state != State::kArmed && e.state != State::kCalibrating) return;
#ifdef _WIN32
    if (!e.log_only && g_di.effect) {
        if (!EnsureRunning(e)) return;
        CallTimer t;
        const long hr = DiSetLevel(g_di, level);
        const bool ok = Hr(e, "SetParameters(constant)", hr);
        if (t.ms() > 100) {
            EmergencyStop(e, "updating the force took too long: the device is not answering");
            return;
        }
        if (!ok) {
            if (++e.consecutive_failures >= 3) EmergencyStop(e, "three force updates in a row failed");
            return;
        }
        e.consecutive_failures = 0;
        e.last_sent = level;
        e.last_send = now;
        if (Trace() && now - e.last_trace > std::chrono::milliseconds(250)) {
            e.last_trace = now;
            LARECOMP_APP_INFO("[wheel] ffb force {} ({:.0f}%)", level, 100.0f * level / kFull);
        }
        return;
    }
#endif
    // Log only: the force the wheel would have got.
    if (Trace() && std::abs(level - e.last_sent) > kFull / 50 &&
        now - e.last_trace > std::chrono::milliseconds(250)) {
        e.last_trace = now;
        LARECOMP_APP_INFO("[wheel] ffb force {} ({:.0f}%) (log only: no force-feedback device)", level,
                          100.0f * level / kFull);
    }
    e.last_sent = level;
}

// Plays the spring and the damper natively (device units). Updates only when something changed enough, and at
// most ten times a second: a condition effect is rewritten in the wheel's firmware, not streamed.
void SendConditions(Engine& e, const Mix& m, float strength, Clock::time_point now, bool force) {
    if (e.state != State::kArmed && e.state != State::kCalibrating) return;
    auto dev = [&](const Cond& c, int* coef, int* sat) {
        *coef = c.on ? static_cast<int>(std::lround(std::clamp(c.k, 0.0f, 1.0f) * kDi)) : 0;
        *sat = c.on ? static_cast<int>(std::lround(std::clamp(c.sat * strength, 0.0f, 1.0f) * kDi)) : 0;
    };
    int sc, ss, dc, ds;
    dev(m.spring, &sc, &ss);
    dev(m.damper, &dc, &ds);
    const int so = m.spring.on ? static_cast<int>(std::lround(std::clamp(m.spring.off, -1.0f, 1.0f) * kDi)) : 0;
    const int now_v[6] = {so, sc, ss, dc, ds, 0};
    bool changed = false;
    for (int i = 0; i < 5; ++i) changed = changed || std::abs(now_v[i] - e.last_nat[i]) >= 200;
    const bool turning_off = (ss == 0 && e.last_nat[2] != 0) || (ds == 0 && e.last_nat[4] != 0);
    if (!force && !turning_off && (!changed || now - e.last_nat_send < std::chrono::milliseconds(100))) return;

#ifdef _WIN32
    if (!e.log_only && e.native) {
        if (!e.nat_running) {
            if (g_di.spring) Hr(e, "Start(spring)", g_di.spring->Start(1, 0));
            if (g_di.damper) Hr(e, "Start(damper)", g_di.damper->Start(1, 0));
            e.nat_running = true;
        }
        CallTimer t;
        if (g_di.spring) Hr(e, "SetParameters(spring)", DiSetCondition(g_di.spring, g_di.seff, g_di.sc, so, sc, ss));
        if (g_di.damper) Hr(e, "SetParameters(damper)", DiSetCondition(g_di.damper, g_di.deff, g_di.dc, 0, dc, ds));
        if (t.ms() > 100) {
            EmergencyStop(e, "updating the spring took too long: the device is not answering");
            return;
        }
    }
#endif
    std::memcpy(e.last_nat, now_v, sizeof(e.last_nat));
    e.last_nat_send = now;
    if (Trace()) {
        LARECOMP_APP_INFO("[wheel] ffb conditions{}: spring off {:+d} coef {} sat {} | damper coef {} sat {}",
                          e.log_only ? " (log only)" : (e.native ? " (native)" : " (software)"), so, sc, ss, dc, ds);
    }
}

// Stop, destroy, gain to zero where the wheel has it, then stay disarmed.
void EmergencyStop(Engine& e, const char* why) {
    LARECOMP_APP_ERROR("[wheel] FFB EMERGENCY STOP: {}. Force feedback is disarmed; switch "
                       "wheel_ffb_rearm on to arm it again.", why);
    e.level = 0.0f;
    e.last_sent = 0;
    std::memset(e.last_nat, 0, sizeof(e.last_nat));
#ifdef _WIN32
    if (g_di.dev) {
        // Gain first: it silences every effect at once, whatever state the ones we made are in.
        DIPROPDWORD gain = {};
        gain.diph.dwSize = sizeof(gain);
        gain.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        gain.diph.dwHow = DIPH_DEVICE;
        gain.dwData = 0;
        g_di.dev->SetProperty(DIPROP_FFGAIN, &gain.diph);
        g_di.dev->SendForceFeedbackCommand(DISFFC_STOPALL);
        ReleaseEffect(g_di.effect);
        ReleaseEffect(g_di.spring);
        ReleaseEffect(g_di.damper);
    }
#endif
    e.fx_running = false;
    e.nat_running = false;
    e.native = false;
    e.state = State::kEmergency;
    e.mirror.clear();
    e.selftest_start = {};
    BumpGeneration(e);
    e.rearm_seen = REXCVAR_GET(wheel_ffb_rearm);
}

void CloseDevice(Engine& e) {
#ifdef _WIN32
    DiRelease(g_di);
    g_di = Di{};
#endif
    e.fx_running = false;
    e.nat_running = false;
    e.native = false;
    e.log_only = true;
}

void OpenDevice(Engine& e, bool virtual_device, uint16_t vid, uint16_t pid) {
    e.log_only = true;
    e.native = false;
    if (virtual_device) {
        LARECOMP_APP_INFO("[wheel] ffb: virtual wheel: force feedback output is log only");
        e.native = true;  // so the log-only run plays the conditions as the real path would
        return;
    }
#ifdef _WIN32
    g_di = Di{};
    if (DiOpen(e, g_di, vid, pid)) {
        e.log_only = false;
    } else {
        DiRelease(g_di);
        g_di = Di{};
        e.native = false;
        LARECOMP_APP_INFO("[wheel] ffb: no force-feedback device: force feedback output is log only");
    }
#else
    (void)vid; (void)pid;
    LARECOMP_APP_INFO("[wheel] ffb: force feedback is Windows only here: log only");
#endif
}

// ---------------------------------------------------------------------------
// Mirror and force
// ---------------------------------------------------------------------------

void Apply(Engine& e, const Cmd& c, Clock::time_point now) {
    switch (c.op) {
        case Op::kReset:
            e.mirror.clear();
            e.level = 0.0f;
            if (Trace()) LARECOMP_APP_INFO("[wheel] ffb reset: every effect dropped");
            break;
        case Op::kSet:
        case Op::kUpdate: {
            const int32_t id = c.w[0];
            auto it = e.mirror.find(id);
            const bool fresh = it == e.mirror.end();
            Fx& fx = fresh ? e.mirror[id] : it->second;
            std::memcpy(fx.w, c.w, sizeof(fx.w));
            // An update for an effect this side never saw (the engine was re-armed under a running
            // game) carries everything, and an infinite effect the game is still rewriting is running.
            if (fresh && c.op == Op::kUpdate && fx.w[4] < 0) {
                fx.started = true;
                fx.start = now;
            }
            if (Trace() && fresh) {
                LARECOMP_APP_INFO("[wheel] ffb effect {} = {} (dur {} ms)", id, TypeName(fx.w[1]), fx.w[4]);
            }
            break;
        }
        case Op::kStart: {
            auto it = e.mirror.find(c.id);
            if (it != e.mirror.end()) {
                it->second.started = true;
                it->second.start = now;
            }
            break;
        }
        case Op::kStop: {
            auto it = e.mirror.find(c.id);
            if (it != e.mirror.end()) it->second.started = false;
            break;
        }
    }
}

float Wave(int type, float ph) {
    switch (type) {
        case 3: return ph < 0.5f ? 1.0f : -1.0f;                          // square
        case 4: return std::sin(6.2831853f * ph);                         // sine
        case 5: return 1.0f - 4.0f * std::fabs(ph - 0.5f);                // triangle
        case 6: return 2.0f * ph - 1.0f;                                  // saw up
        case 7: return 1.0f - 2.0f * ph;                                  // saw down
        default: return 0.0f;
    }
}

// What the game has running, as a constant force plus the conditions to play, with centring added when the game
// runs no spring of its own.
Mix Gather(Engine& e, Clock::time_point now, float speed_ms) {
    Mix m;
    float total = 0.0f;
    const float boost = std::clamp(REXCVAR_GET(wheel_ffb_boost), 100, 500) / 100.0f;
    for (auto& kv : e.mirror) {
        Fx& fx = kv.second;
        if (!fx.started) continue;
        const int type = fx.w[1];
        const int dur = fx.w[4];
        const int delay = std::max(0, fx.w[5]);
        const long long since =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - fx.start).count();
        if (since < delay) continue;
        if (dur >= 0 && since >= static_cast<long long>(delay) + dur) {
            fx.started = false;  // ran its course
            continue;
        }
        switch (type) {
            case 1:  // constant: [11] magnitude, +-127 full scale; positive pushes towards positive steering
                total += static_cast<float>(fx.w[11]) / 127.0f;
                break;
            case 3: case 4: case 5: case 6: case 7: {  // periodic: [11] magnitude, [12] offset, [14] period ms
                const float period = fx.w[14] > 0 ? static_cast<float>(fx.w[14]) : 100.0f;
                const float t = static_cast<float>(since - delay);
                const float ph = std::fmod(t / period, 1.0f);
                total += (static_cast<float>(fx.w[11]) * Wave(type, ph) + static_cast<float>(fx.w[12])) / 127.0f;
                m.periodic = true;
                break;
            }
            case 8: {  // spring: [11] offset, [12] / [13] coefficients (right / left), [14] / [15] saturation
                const float k = static_cast<float>(std::max(fx.w[12], fx.w[13])) / 127.0f;
                const float sat = static_cast<float>(std::max(fx.w[14], fx.w[15])) / 255.0f;
                if (!m.spring.on || k * sat > m.spring.k * m.spring.sat) {
                    m.spring = Cond{true, k, sat, static_cast<float>(fx.w[11]) / 127.0f, 1.0f};
                }
                break;
            }
            case 9: {  // damper: same layout
                const float k = static_cast<float>(std::max(fx.w[12], fx.w[13])) / 127.0f;
                const float sat = static_cast<float>(std::max(fx.w[14], fx.w[15])) / 255.0f;
                if (!m.damper.on || k * sat > m.damper.k * m.damper.sat) m.damper = Cond{true, k, sat, 0.0f, 1.0f};
                break;
            }
            default:
                if (!e.warned_unsupported) {
                    e.warned_unsupported = true;
                    LARECOMP_APP_INFO("[wheel] ffb: effect type {} ({}) is not played", type, TypeName(type));
                }
                break;
        }
    }
    m.constant = std::clamp(total * boost, -1.0f, 1.0f);
    if (m.spring.on) m.spring.sat = std::min(1.0f, m.spring.sat * boost);
    if (m.damper.on) m.damper.k = std::min(1.0f, m.damper.k * boost);

    // Centring: the game only runs its own spring in some situations (not while free-roaming), and a wheel
    // that nothing centres stays wherever it is let go. Light when parked, full from about 20 m/s, like a car's
    // self-aligning force, with a light damper so the stiffness does not ring.
    const int centering = std::clamp(REXCVAR_GET(wheel_ffb_centering), 0, 100);
    if (centering > 0) {
        const float c = centering / 100.0f;
        const float sf = std::clamp(0.35f + 0.65f * speed_ms / 20.0f, 0.35f, 1.0f);
        if (!m.spring.on) {
            m.spring = Cond{true, 1.0f, c * sf, 0.0f, 6.5f};
        } else {
            // The game's spring saturates at 10-30% of its scale, too little for a strong base: ours is the floor.
            m.spring.k = std::max(m.spring.k, 1.0f);
            m.spring.sat = std::max(m.spring.sat, c * sf);
        }
        if (!m.damper.on) m.damper = Cond{true, 0.5f, 0.35f * c, 0.0f, 1.0f};
    }
    return m;
}

// The conditions as force, for a wheel that will not play them itself: summed into the constant force.
// (This one does use the measured sign, like everything that goes through the constant effect.)
float Fold(const Mix& m, float pos, float vel) {
    float f = 0.0f;
    if (m.spring.on) {
        const float x = pos - m.spring.off;
        f += std::clamp(-x * m.spring.k * m.spring.soft_gain, -m.spring.sat, m.spring.sat);
    }
    if (m.damper.on) {
        f += std::clamp(-std::clamp(vel / 3.0f, -1.0f, 1.0f) * m.damper.k, -m.damper.sat, m.damper.sat);
    }
    return f;
}

void Arm(Engine& e, Clock::time_point now) {
    e.state = State::kArmed;
    e.level = 0.0f;
    e.last_frame_change = now;
    LARECOMP_APP_INFO("[wheel] ffb armed (constant sign {}, strength {}%, centring {}%{})", e.sign > 0 ? "normal" : "inverted",
                      REXCVAR_GET(wheel_ffb_strength), REXCVAR_GET(wheel_ffb_centering), e.log_only ? ", log only" : "");
    if (REXCVAR_GET(wheel_ffb_selftest) && !e.selftest_done) {
        e.selftest_start = now;
        e.selftest_pos_log = {};
        LARECOMP_APP_INFO("[wheel] ffb SELF-TEST starts: a push one way (0.5 s), the other way (0.5 s), then 2 s of "
                          "centring spring (turn the wheel a little and let go), then 1 s of damper. Every device call "
                          "and the wheel's position are logged.");
    }
}

void StartCalibration(Engine& e, Clock::time_point now) {
    const std::string mode = REXCVAR_GET(wheel_ffb_sign);
    if (mode == "normal" || mode == "inverted" || e.log_only) {
        e.sign = mode == "inverted" ? -1 : 1;
        Arm(e, now);
        return;
    }
    e.state = State::kCalibrating;
    e.calib_start = now;
    e.calib_phase = 0;
    e.calib_try = 0;
    LARECOMP_APP_INFO("[wheel] ffb: checking which way a positive constant force turns the wheel with a few gentle "
                      "nudges; keep your hands off it for a couple of seconds");
}

// A ladder of nudges, the gentlest first: a strong direct-drive wheel moves a lot on 4%, a geared one may need
// more to overcome its friction. The first that moves the wheel decides the sign.
void CalibrationTick(Engine& e, const TickIn& in) {
    using ms = std::chrono::milliseconds;
    static const int kSteps[3] = {4, 10, 18};   // percent of full force
    const auto since = std::chrono::duration_cast<ms>(in.now - e.calib_start).count();
    if (e.calib_phase == 0 || e.calib_phase == 2) {
        if (since < (e.calib_phase == 0 ? 700 : 500)) return;  // let the wheel settle
        e.calib_pos0 = in.pos_raw;
        e.calib_phase = 1;
        e.calib_start = in.now;
        SendLevel(e, kFull * kSteps[e.calib_try] / 100, in.now);
        LARECOMP_APP_INFO("[wheel] ffb: nudge {} of {}: +{}% for 0.3 s (the wheel is at {:+.3f})", e.calib_try + 1, 3,
                          kSteps[e.calib_try], in.pos_raw);
        return;
    }
    if (since < 300) return;
    const float moved = in.pos_raw - e.calib_pos0;
    SendLevel(e, 0, in.now);
    NormalStop(e);
    if (e.state == State::kEmergency) return;
    if (std::fabs(moved) < 0.006f) {
        LARECOMP_APP_INFO("[wheel] ffb: that nudge moved the wheel {:+.4f}: not enough to tell", moved);
        if (++e.calib_try < 3) {
            e.calib_phase = 2;
            e.calib_start = in.now;
            return;
        }
        LARECOMP_APP_WARN(
            "[wheel] ffb: three nudges (up to 18%) did not move the wheel, so which way a positive constant force "
            "turns it is unknown. Force feedback stays off. Free the wheel and switch wheel_ffb_rearm on, or set "
            "wheel_ffb_sign to normal or inverted.");
        e.state = State::kEmergency;
        e.rearm_seen = REXCVAR_GET(wheel_ffb_rearm);
        return;
    }
    e.sign = moved > 0 ? 1 : -1;
    LARECOMP_APP_INFO("[wheel] ffb: a positive constant force turned the wheel {:+.3f}: sign {}", moved,
                      e.sign > 0 ? "normal" : "inverted");
    Arm(e, in.now);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public: guest side
// ---------------------------------------------------------------------------

void PostEffect(bool update, const int32_t* words) {
    Cmd c;
    c.op = update ? Op::kUpdate : Op::kSet;
    c.generation = Q().generation.load(std::memory_order_acquire);
    std::memcpy(c.w, words, sizeof(c.w));
    c.id = c.w[0];
    Push(c);
}

void PostOperation(int32_t id, bool start) {
    Cmd c;
    c.op = start ? Op::kStart : Op::kStop;
    c.generation = Q().generation.load(std::memory_order_acquire);
    c.id = id;
    Push(c);
}

void PostReset() {
    Cmd c;
    c.op = Op::kReset;
    c.generation = Q().generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    Push(c);
}

// ---------------------------------------------------------------------------
// Public: wheel thread
// ---------------------------------------------------------------------------

void DeviceOpened(bool virtual_device, uint16_t vid, uint16_t pid) {
    Engine& e = E();
    e.mirror.clear();
    BumpGeneration(e);
    e.last_tick = {};
    e.last_pos = 0.0f;
    e.vel = 0.0f;
    e.consecutive_failures = 0;
    e.warned_unsupported = false;
    e.reported.clear();
    e.rearm_seen = REXCVAR_GET(wheel_ffb_rearm);
    if (!REXCVAR_GET(wheel_ffb)) {
        e.state = State::kNoDevice;
        LARECOMP_APP_INFO("[wheel] ffb: off (wheel_ffb)");
        return;
    }
    OpenDevice(e, virtual_device, vid, pid);
    e.dev_vid = vid;
    e.dev_pid = pid;
    e.dev_virtual = virtual_device;
    e.state = State::kCalibrating;
    e.calib_start = Clock::now();
    StartCalibration(e, Clock::now());
}

void DeviceClosing() {
    Engine& e = E();
    if (e.state != State::kNoDevice) LARECOMP_APP_INFO("[wheel] ffb: device closing, force stopped");
    CloseDevice(e);
    e.mirror.clear();
    BumpGeneration(e);
    e.state = State::kNoDevice;
    e.level = 0.0f;
}

void Tick(const TickIn& in) {
    Engine& e = E();
    if (e.state == State::kNoDevice) return;

    // Panic first, before anything in the queue.
    if (REXCVAR_GET(wheel_ffb_panic) && e.state != State::kEmergency) {
        EmergencyStop(e, "wheel_ffb_panic");
    }
    if (!REXCVAR_GET(wheel_ffb) && e.state != State::kEmergency) {
        NormalStop(e);
        e.mirror.clear();
    }

    // Drain the queue, dropping what was posted before the last stop.
    std::vector<Cmd> batch;
    {
        Queue& q = Q();
        std::lock_guard<std::mutex> l(q.m);
        batch.swap(q.items);
    }
    e.cmds_seen += uint32_t(batch.size());
    for (const Cmd& c : batch) {
        if (c.op == Op::kReset) {
            if (c.generation >= e.engine_gen) {
                e.engine_gen = c.generation;
                Apply(e, c, in.now);
            }
            continue;
        }
        if (c.generation != e.engine_gen) continue;
        if (e.state == State::kEmergency || e.state == State::kCalibrating) continue;
        Apply(e, c, in.now);
    }

    if (e.state == State::kEmergency) {
        const bool rearm = REXCVAR_GET(wheel_ffb_rearm);
        if (rearm && !e.rearm_seen && !REXCVAR_GET(wheel_ffb_panic) && REXCVAR_GET(wheel_ffb)) {
            LARECOMP_APP_INFO("[wheel] ffb: re-arming");
            CloseDevice(e);
            e.reported.clear();
            OpenDevice(e, e.dev_virtual, e.dev_vid, e.dev_pid);
            e.mirror.clear();
            BumpGeneration(e);
            e.consecutive_failures = 0;
            StartCalibration(e, in.now);
        }
        e.rearm_seen = rearm;
        return;
    }
    if (e.state == State::kCalibrating) {
        CalibrationTick(e, in);
        return;
    }

    // Heartbeat: the guest's input frames. A game that is loading or hung stops them.
    if (in.frame != e.last_frame) {
        e.last_frame = in.frame;
        e.last_frame_change = in.now;
    }
    const auto stale = std::chrono::duration_cast<std::chrono::milliseconds>(in.now - e.last_frame_change).count();
    const bool alive = stale < std::max(100, REXCVAR_GET(wheel_ffb_watchdog_ms));
    // The self-test runs whatever the game is doing (its window may not be up yet), but never without the wheel
    // being the driver's.
    const bool allowed = Selftesting(e) ? in.driving : (alive && in.focused && in.driving);
    if (!allowed) {
        if (!e.paused) {
            e.paused = true;
            NormalStop(e);
            LARECOMP_APP_INFO("[wheel] ffb paused ({})",
                              !alive ? "the game stopped running input frames" : (!in.focused ? "window not focused"
                                                                                               : "the gamepad drives"));
        }
        return;
    }
    if (e.paused) {
        e.paused = false;
        e.level = 0.0f;  // back in from zero, through the slew limiter
        LARECOMP_APP_INFO("[wheel] ffb resumed");
    }

    // Position and speed, for the damper and for folding conditions into the constant force.
    float dt = 0.004f;
    if (e.last_tick.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(in.now - e.last_tick).count();
    }
    e.last_tick = in.now;
    dt = std::clamp(dt, 0.001f, 0.1f);
    const float raw_vel = (in.pos - e.last_pos) / dt;
    e.vel += (raw_vel - e.vel) * std::min(1.0f, dt / 0.03f);  // ~30 ms low-pass: position is 16-bit steps
    e.last_pos = in.pos;

    Mix mix = Gather(e, in.now, in.speed_ms);

    // The staged self-test replaces what the game asks for.
    float strength_scale = std::clamp(REXCVAR_GET(wheel_ffb_strength), 0, 100) / 100.0f;
    if (Selftesting(e)) {
        const long long t = std::chrono::duration_cast<std::chrono::milliseconds>(in.now - e.selftest_start).count();
        mix = Mix{};
        const char* step = "";
        if (t < 500) {
            mix.constant = 0.25f / strength_scale;     // a fixed 25% of full force whatever the strength cvar says
            step = "constant +25%";
        } else if (t < 1000) {
            mix.constant = -0.25f / strength_scale;
            step = "constant -25%";
        } else if (t < 3000) {
            mix.spring = Cond{true, 1.0f, 0.5f / strength_scale, 0.0f, 6.5f};
            step = "spring (centring)";
        } else if (t < 4000) {
            mix.damper = Cond{true, 1.0f, 0.5f / strength_scale, 0.0f, 1.0f};
            step = "damper";
        } else {
            e.selftest_start = {};
            e.selftest_done = true;
            LARECOMP_APP_INFO("[wheel] ffb self-test over");
        }
        if (Selftesting(e) && in.now - e.selftest_pos_log >= std::chrono::milliseconds(50)) {
            e.selftest_pos_log = in.now;
            LARECOMP_APP_INFO("[wheel] selftest t={:4d} ms  {:<18} wheel {:+.3f}", int(t), step, in.pos_raw);
        }
    }
    mix.constant = std::clamp(mix.constant, -1.0f, 1.0f);

    // Without native conditions they go into the constant force, through the measured sign.
    float target = mix.constant;
    if (!e.native) target = std::clamp(target + Fold(mix, in.pos, e.vel), -1.0f, 1.0f);

    // Slew: full scale in about 80 ms. A collision's jolt still lands; a bug's step does not.
    const float max_step = 12.5f * dt;
    e.level += std::clamp(target - e.level, -max_step, max_step);

    const int strength = std::clamp(REXCVAR_GET(wheel_ffb_strength), 0, 100);
    const int out = static_cast<int>(std::lround(e.level * (strength / 100.0f) * e.sign * kFull));
    float peak = std::fabs(e.level * (strength / 100.0f));
    if (e.native) {
        peak = std::max(peak, std::max(mix.spring.on ? mix.spring.sat * strength / 100.0f : 0.0f,
                                       mix.damper.on ? mix.damper.sat * strength / 100.0f : 0.0f));
    }
    e.peak_out = std::max(e.peak_out, peak);

    if (in.now - e.last_summary > std::chrono::seconds(10)) {
        if (e.last_summary.time_since_epoch().count() != 0) {
            int running = 0;
            for (const auto& kv : e.mirror) running += kv.second.started ? 1 : 0;
            std::string playing = "n/a";
#ifdef _WIN32
            if (!e.log_only) {
                auto st = [&](IDirectInputEffect* fx) {
                    DWORD flags = 0;
                    if (!fx || FAILED(fx->GetEffectStatus(&flags))) return "?";
                    return (flags & DIEGES_PLAYING) ? "playing" : "stopped";
                };
                playing = std::string("constant ") + st(g_di.effect) + ", spring " + st(g_di.spring) + ", damper " +
                          st(g_di.damper);
            }
#endif
            LARECOMP_APP_INFO("[wheel] ffb summary (10 s): {} game commands, {} effects known / {} running, peak force "
                              "{:.0f}% of the wheel's full force (strength {}%, centring {}%), speed {:.1f} m/s, "
                              "device: {}",
                              e.cmds_seen, e.mirror.size(), running, 100.0f * e.peak_out, strength,
                              std::clamp(REXCVAR_GET(wheel_ffb_centering), 0, 100), in.speed_ms, playing);
        }
        e.last_summary = in.now;
        e.peak_out = 0.0f;
        e.cmds_seen = 0;
    }

    if (Trace() && in.now - e.last_status > std::chrono::milliseconds(500)) {
        e.last_status = in.now;
        int running = 0;
        int constant = 0;
        for (const auto& kv : e.mirror) {
            running += kv.second.started ? 1 : 0;
            if (kv.second.started && kv.second.w[1] == 1) constant += kv.second.w[11];
        }
        LARECOMP_APP_INFO("[wheel] ffb status: {} effects known, {} running, pos {:+.2f}, speed {:+.2f}, "
                          "game constant {:+d}, target {:+.3f}, level {:+.3f}", e.mirror.size(), running,
                          in.pos, e.vel, constant, target, e.level);
    }

    // The conditions: a few times a second, when they change.
    if (e.native) SendConditions(e, mix, strength / 100.0f, in.now, false);

    // The constant force: at most ~125 Hz, and only when it changes enough to matter (or a periodic effect runs).
    const auto since_send = std::chrono::duration_cast<std::chrono::milliseconds>(in.now - e.last_send).count();
    if (since_send >= 8 && (std::abs(out - e.last_sent) > kFull / 200 || (mix.periodic && out != e.last_sent) ||
                            (out == 0 && e.last_sent != 0))) {
        SendLevel(e, out, in.now);
        e.last_send = in.now;
    }
}

}  // namespace larecomp::wheel::ffb

#endif  // REXGLUE_HAS_XEO3_TARGET
