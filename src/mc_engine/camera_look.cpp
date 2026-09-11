// Both branches below define the same symbols, so the declarations they are
// checked against have to be visible to both.
#include "camera_look.h"
#include "menu_items.h"
#include "map_mouse.h"

#ifndef REXGLUE_HAS_XEO3_TARGET
#include "logging.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>

#if defined(_WIN32)
#include <Windows.h>
#include "imgui.h"
#endif

REXCVAR_DEFINE_BOOL(cam_freelook, false, "MCLA/Camera",
    "Free look on the gameplay camera with the mouse. The shipped lookaround "
    "is four digital pan actions easing towards clamped limits, which is why "
    "the camera settles on a handful of angles instead of following you. While "
    "this is on the cursor is captured and the mouse steers the view directly.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_sens, 0.0035, "MCLA/Camera",
    "Radians of view movement per pixel of mouse movement.")
    .range(0.0001, 0.05)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_yaw_limit, 3.14159, "MCLA/Camera",
    "How far the view may turn left or right, in radians. Pi is all the way "
    "round. This is the cap on our own angle, not the game's -- the game's own "
    "limits are what this feature overwrites.")
    .range(0.1, 3.15)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_pitch_limit, 0.7, "MCLA/Camera",
    "How much tilt the mouse may accumulate, in radians. This caps the input, "
    "not the result -- where the camera ends up is decided by the two "
    "elevation limits below, which are what keep it out of the ground.")
    .range(0.05, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// A camera bolted to the car -- cockpit, bumper, bonnet -- has nothing to
// orbit: it turns where it sits, like a head. Swinging it round a centre seven
// metres away, which is right for a chase camera, walks it straight out
// through the bodywork.
//
// Which rigs those are cannot be worked out from here. The honest signal would
// be the camera's real distance to the car, and that distance is exactly what
// this feature never manages to find out -- it is why the orbit radius is a
// cvar in the first place. So it is a list, and the diagnostic prints the rig
// number every time it applies, which makes filling the list a matter of
// reading one line per view.
//
// r24 in sub_822C0320 is the rig the camera system settled on this frame, and
// Hook_CameraSystem has been recording it since the first probe. Per the note
// on Patch_DebugCam it is 1 for the cockpit view and 2 for a static one.
REXCVAR_DEFINE_STRING(cam_freelook_inplace_rigs, "0,1", "MCLA/Camera",
    "Comma-separated camera rig indices that turn in place instead of "
    "orbiting: the views mounted on the car. Switch to a view, read the rig "
    "number out of the cam_freelook_diag line, add it here. Empty orbits "
    "everything.\n"
    "\n"
    "The camera button cycles 3, 2, 1, 0 and wraps; 4 belongs to some other "
    "context and turns up on its own. 1 is the cockpit, and 0 is the bumper "
    "sitting next to it in the cycle -- which is a reading of the cycle order, "
    "not something measured, so if a view mounted on the car still swings out "
    "of the bodywork, its number belongs in this list.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Rotating the finished matrix means the game's camera collision never sees
// the result, so the floor has to be put back by hand. It states cleanly in
// geometry: pos = centre - forward * d with d negative, so the camera sits
// forward.y * |d| above the point it orbits and its elevation is
// asin(forward.y). Clamping that angle is exactly clamping how far under the
// car the view can get. Orbiting rigs only -- in place the position never
// moves, so there is nothing to keep out of the ground.
REXCVAR_DEFINE_DOUBLE(cam_freelook_min_elev, -0.08, "MCLA/Camera",
    "Lowest the camera may go, as an angle in radians above the horizontal "
    "through the point it orbits. 0 is dead level with the car; below that it "
    "goes under it and into the ground. Negative values are allowed for "
    "deliberately low shots, on the understanding that the ground is not "
    "consulted.")
    .range(-0.5, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_max_elev, 1.3, "MCLA/Camera",
    "Highest the camera may go, same units. Short of pi/2 on purpose: at dead "
    "overhead the forward vector is parallel to world up and the yaw stops "
    "meaning anything.")
    .range(0.2, 1.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Both default to 0 = leave the shipped value alone. Their units are not known
// -- "rate" reads as per-second and "factor" as a 0..1 blend, but neither is
// confirmed -- and a wrong value here is indistinguishable from the whole
// approach not working. Changing one thing at a time is the point.
REXCVAR_DEFINE_DOUBLE(cam_freelook_approach, 0.0, "MCLA/Camera",
    "LookaroundApproachRate while free look is on. 0 leaves the shipped value. "
    "The shipped one eases the view towards its target over a good fraction of "
    "a second, which reads as lag under a mouse.")
    .range(0.0, 200.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_lerp, 0.0, "MCLA/Camera",
    "LookaroundLerpFactor while free look is on. 0 leaves the shipped value.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The shipped camera recentres itself: LookaroundDelayTime then
// LookaroundDecayRate. Not reproducing that was the difference between a free
// look and a view that silently accumulates every stray twitch of the mouse
// until the car is sitting crooked and there is no way to put it back.
REXCVAR_DEFINE_DOUBLE(cam_freelook_return_delay, 0.35, "MCLA/Camera",
    "Seconds the mouse has to sit still before the view starts easing back "
    "behind the car. The game's own equivalent is LookaroundDelayTime. Set it "
    "very high to keep the view wherever it was left.")
    .range(0.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_return_rate, 4.0, "MCLA/Camera",
    "How quickly the view eases back once the delay is up, as a fraction of "
    "the remaining angle per second. The game's own equivalent is "
    "LookaroundDecayRate.")
    .range(0.1, 40.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_invert_x, false, "MCLA/Camera",
    "Mirror the horizontal axis. Needed if the tune's left/right limits turn "
    "out to be signed the other way round from what this assumes.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_invert_y, false, "MCLA/Camera",
    "Mirror the vertical axis. See cam_freelook_invert_x.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ── Driving the camera object the probe found ──────────────────────────
//
// The measurement landed on one object -- camsys+860, which came out as
// 0xBD910980 -- and on fourteen floats inside it, at +96 through +156. Three
// rows of unit-length vectors at +96/+112/+128 and a world position at +144:
// the camera's own matrix, rotating about Y and orbiting as the view swings.
//
// The two floats that are not part of it are the interesting ones. +140 and
// +156 are the w lanes of the third and fourth rows, which is where RAGE
// habitually parks a scalar it did not want to give a field of its own, and
// they went from about zero to -0.947685 and -7.34199 -- an angle in radians
// and, to within the accuracy of reading a matrix, the radius the position
// orbited on.
//
// Whether +140 is what the camera READS to build that matrix or what it WROTE
// afterwards is the only thing left to settle, and no amount of staring
// settles it. So the offsets are cvars, the write happens inside the camera
// tick before the update runs, and the value is read back on the next frame
// and compared against what was written. That answers it in one run and lets
// other candidates be tried without a rebuild.
REXCVAR_DEFINE_INT32(cam_look_obj, 860, "MCLA/Camera",
    "Byte offset into the camera system holding the pointer to the object the "
    "look lives in. 860 is what the field probe found.")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_freelook_orbit, 7.5, "MCLA/Camera",
    "Distance from the camera to the point it turns around, used whenever the "
    "object's own +156 is not usable.\n"
    "\n"
    "It usually is not. +156 was measured at -7.34 during a held look and that "
    "is where this default comes from, but it is the w lane of the position "
    "row and the game computes it the same way it computes the angle at +140 "
    "-- both read zero whenever the game's own look is idle, which is exactly "
    "when this feature is doing the looking instead. The field still wins when "
    "it holds something sane, so a camera that keeps a real distance there is "
    "followed rather than overridden.")
    .range(1.0, 200.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(cam_look_phase, 2, "MCLA/Camera",
    "Which matrix the look turns. 2 is the real one: the camera matrix "
    "sub_8220AA68 is handed at 0x8220AA88, before it has read a byte of "
    "it, from which the game publishes to dword_8286D804+4208 and +4272 "
    "and mirrors into the cine director. 0 and 1 turn that mirror "
    "instead, at 0x822C0790 and at the camera tick epilogue, and are "
    "kept only because they were tried first -- sub_823CC2E8 is a plain "
    "setter that copies the real matrix into it, so writing there is "
    "writing to a copy and nothing on screen ever changed. The camera "
    "collision lives on its own switch, cam_freelook_collide, and not on "
    "this one -- see there.")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_cs_invert, false, "MCLA/Camera",
    "Flip the sign used when feeding the camera state's own two angle channels "
    "(cs+776 yaw, cs+772 pitch). The offset and direction were derived from "
    "sub_82202E38 and sub_82227DE0 -- both store `pi/2 - angle`, so a turn is a "
    "subtraction -- but which way that reads on screen is a handedness question "
    "the maths does not settle. If the view turns opposite to the mouse with "
    "cam_freelook_collide on, this is the switch. Only affects the collide "
    "path.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_collide, false, "MCLA/Camera",
    "Turn the camera one stage earlier, at the camera state (0x8231CEB0), so "
    "the game's own camera collision resolves the rotated position and the "
    "view stops passing through walls and buildings. OFF by default because it "
    "is unproven: it hands the look to sub_8231CE30, and any rig whose camera "
    "state does not route through there gets no free look at all while this is "
    "on. Turn cam_freelook_diag on with it -- the log says whether the hook "
    "ran, which camera state it got, and what CollideType that state carries. "
    "With this off, cam_look_phase decides everything, exactly as before.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_tune, false, "MCLA/Camera",
    "Also pin the LookaroundLimit* tune fields, the first approach. It did not "
    "hold on its own and is kept only so the two can be tried together.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_freelook_diag, false, "MCLA/Camera",
    "Log the lookaround tune fields as they are found and the angles being "
    "written, once a second.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_probe_mark, false, "MCLA/Camera",
    "Button: snapshot the gameplay camera object. Flips back off. Do it with "
    "the car still and the view centred, then look around, then use "
    "cam_probe_dump.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cam_probe_dump, false, "MCLA/Camera",
    "Button: snapshot the camera again, diff it against cam_probe_mark and "
    "write every float that moved to <exe>/cam_fields.txt. Flips back off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The two buttons above cannot be used for the measurement they were written
// for: reaching them means opening the settings overlay, and opening it takes
// the game out of whatever camera state was being held. This runs the whole
// sequence on a timer instead, so the overlay is only ever open while the
// camera is idle and both samples are taken with it shut.
REXCVAR_DEFINE_BOOL(cam_probe_run, false, "MCLA/Camera",
    "Button: run the whole camera measurement on a timer. Close the overlay "
    "when it is armed; the baseline is taken after cam_probe_arm_delay with "
    "the camera left alone, and the diff cam_probe_hold_delay after that with "
    "the look held. Both samples happen with nothing on screen. Flips back "
    "off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_probe_arm_delay, 4.0, "MCLA/Camera",
    "Seconds between arming cam_probe_run and the baseline sample. Long "
    "enough to close the overlay and let the camera settle.")
    .range(1.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_DOUBLE(cam_probe_hold_delay, 6.0, "MCLA/Camera",
    "Seconds between the baseline sample and the diff. Hold the look for all "
    "of it.")
    .range(1.0, 30.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// One camera tune's lookaround block. The game builds several of these (the
// tune is per camera type), and every one that registers a full set is driven,
// because there is no telling from here which is behind the wheel right now.
struct LookTune {
    uint32_t owner = 0;
    uint32_t delay_time = 0;      // LookaroundDelayTime
    uint32_t lerp_factor = 0;     // LookaroundLerpFactor
    uint32_t approach_rate = 0;   // LookaroundApproachRate
    uint32_t decay_rate = 0;      // LookaroundDecayRate
    uint32_t limit_left = 0;
    uint32_t limit_right = 0;
    uint32_t limit_up = 0;
    uint32_t limit_down = 0;

    // What the tune held before we first wrote to it, so turning the feature
    // off puts the shipped camera back rather than leaving it pinned.
    float saved[8] = {};
    bool saved_valid = false;

    bool complete() const {
        return delay_time && lerp_factor && approach_rate && decay_rate &&
               limit_left && limit_right && limit_up && limit_down;
    }
};

std::mutex g_tunes_mutex;
std::vector<LookTune> g_tunes;

// Set once a complete tune is known, so the per-frame tick can skip the lock
// entirely until there is something to do.
std::atomic<bool> g_have_tune{false};

// The look angles the mouse has accumulated, and whether it is currently the
// thing steering. Both are produced once per frame inside Hook_CameraSystem --
// the mouse must be read in exactly one place, because reading it recentres
// the pointer and a second reader would only ever see a delta of zero.
float g_yaw = 0.0f;
float g_pitch = 0.0f;
std::atomic<bool> g_look_active{false};
bool g_applied = false;

// Set once a frame from the frame tick: something else on screen owns the
// pointer and free look has to keep its hands off it. Read by the mouse path
// so the decision is taken in one place and both the camera tick and the frame
// tick honour it.
std::atomic<bool> g_yield_pointer{false};

// Set by Hook_CameraStateUpdate on the frames it turned a camera state, and
// consumed by Hook_CameraMatrixPublish so the same rotation is never applied
// twice. A rig whose camera state is of another class never reaches the first
// hook, and the publish site still covers it.
std::atomic<uint32_t> g_cs_applied{0};

// Defined with the probe further down; the tick has to call it before any of
// its own early-outs.
void TickCameraProbe();

uint8_t* GetMembase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

float ReadGuestFloat(uint32_t ea) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return 0.0f;
    const uint8_t* p = base + ea;
    uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                    (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

void WriteGuestFloat(uint32_t ea, float f) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return;
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    uint8_t* p = base + ea;
    p[0] = uint8_t((bits >> 24) & 0xFF);
    p[1] = uint8_t((bits >> 16) & 0xFF);
    p[2] = uint8_t((bits >> 8) & 0xFF);
    p[3] = uint8_t(bits & 0xFF);
}

LookTune& TuneFor(uint32_t owner) {
    for (auto& t : g_tunes) {
        if (t.owner == owner) return t;
    }
    g_tunes.push_back(LookTune{});
    g_tunes.back().owner = owner;
    return g_tunes.back();
}

// ── Mouse capture ──────────────────────────────────────────────────────
//
// The same shape as the free-fly camera's (hooks.cpp): hide and confine the
// pointer, recenter it every frame and take the raw pixel delta. A view that
// can turn all the way round cannot be driven from an absolute position, and
// recentering is what keeps the pointer from walking off the window.
//
// Deliberately independent of mnk_mode. That maps the mouse onto the right
// stick, which on this camera means the digital pan actions -- the very thing
// being replaced -- so the two must not both be live. See the note in
// TickCameraLook.

#if defined(_WIN32)
bool g_captured = false;

bool MouseDelta(bool want, float& out_dx, float& out_dy) {
    out_dx = 0.0f;
    out_dy = 0.0f;

    auto* win = rex::Runtime::instance()->display_window();
    if (!win) return false;
    HWND hwnd = static_cast<HWND>(win->GetNativeWindowHandle());
    if (!hwnd) return false;

    // An overlay that wants the mouse gets it. Deterministic from the live
    // ImGui state rather than a toggle that can drift out of sync with it.
    if (want && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
        want = false;
    // Nothing to steer while the window is not the one being typed at.
    if (want && GetForegroundWindow() != hwnd) want = false;

    RECT rc;
    GetClientRect(hwnd, &rc);
    POINT center = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
    ClientToScreen(hwnd, &center);

    if (!want) {
        if (g_captured) {
            g_captured = false;
            win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
            win->ReleaseMouse();
        }
        return false;
    }

    if (!g_captured) {
        g_captured = true;
        win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
        win->CaptureMouse();
        // Seed on the centre so the first frame does not carry whatever
        // distance the pointer happened to be from it.
        SetCursorPos(center.x, center.y);
        return true;
    }

    POINT cur;
    GetCursorPos(&cur);
    out_dx = float(cur.x - center.x);
    out_dy = float(cur.y - center.y);
    SetCursorPos(center.x, center.y);
    return true;
}
#else
bool MouseDelta(bool, float& out_dx, float& out_dy) {
    out_dx = 0.0f;
    out_dy = 0.0f;
    return false;
}
#endif

void SaveOriginals(LookTune& t) {
    if (t.saved_valid) return;
    t.saved[0] = ReadGuestFloat(t.delay_time);
    t.saved[1] = ReadGuestFloat(t.lerp_factor);
    t.saved[2] = ReadGuestFloat(t.approach_rate);
    t.saved[3] = ReadGuestFloat(t.decay_rate);
    t.saved[4] = ReadGuestFloat(t.limit_left);
    t.saved[5] = ReadGuestFloat(t.limit_right);
    t.saved[6] = ReadGuestFloat(t.limit_up);
    t.saved[7] = ReadGuestFloat(t.limit_down);
    t.saved_valid = true;
}

void RestoreOriginals(LookTune& t) {
    if (!t.saved_valid) return;
    WriteGuestFloat(t.delay_time, t.saved[0]);
    WriteGuestFloat(t.lerp_factor, t.saved[1]);
    WriteGuestFloat(t.approach_rate, t.saved[2]);
    WriteGuestFloat(t.decay_rate, t.saved[3]);
    WriteGuestFloat(t.limit_left, t.saved[4]);
    WriteGuestFloat(t.limit_right, t.saved[5]);
    WriteGuestFloat(t.limit_up, t.saved[6]);
    WriteGuestFloat(t.limit_down, t.saved[7]);
}

void ApplyAngles(LookTune& t, float yaw, float pitch) {
    SaveOriginals(t);

    // Collapse both windows onto the angle we want. Whatever the clamp reads,
    // a window of zero width has one answer.
    WriteGuestFloat(t.limit_left, yaw);
    WriteGuestFloat(t.limit_right, yaw);
    WriteGuestFloat(t.limit_up, pitch);
    WriteGuestFloat(t.limit_down, pitch);

    // Nothing may pull the view back to centre while the player is holding it
    // somewhere. An hour of delay is as good as forever here and, unlike the
    // 1e9 this used to write, cannot turn into an infinity or a NaN if the
    // field is subtracted from or divided by somewhere downstream.
    WriteGuestFloat(t.decay_rate, 0.0f);
    WriteGuestFloat(t.delay_time, 3600.0f);

    float approach = float(REXCVAR_GET(cam_freelook_approach));
    if (approach > 0.0f) WriteGuestFloat(t.approach_rate, approach);
    float lerp = float(REXCVAR_GET(cam_freelook_lerp));
    if (lerp > 0.0f) WriteGuestFloat(t.lerp_factor, lerp);
}

}  // namespace

void CameraLookOnTuneField(const char* name, uint32_t owner, uint32_t field_addr) {
    if (!name || !field_addr) return;
    std::string_view n(name);
    if (n.substr(0, 10) != "Lookaround") return;

    std::lock_guard<std::mutex> lock(g_tunes_mutex);
    LookTune& t = TuneFor(owner);

    if (n == "LookaroundDelayTime") t.delay_time = field_addr;
    else if (n == "LookaroundLerpFactor") t.lerp_factor = field_addr;
    else if (n == "LookaroundApproachRate") t.approach_rate = field_addr;
    else if (n == "LookaroundDecayRate") t.decay_rate = field_addr;
    else if (n == "LookaroundLimitLeft") t.limit_left = field_addr;
    else if (n == "LookaroundLimitRight") t.limit_right = field_addr;
    else if (n == "LookaroundLimitUp") t.limit_up = field_addr;
    else if (n == "LookaroundLimitDown") t.limit_down = field_addr;
    else return;

    if (t.complete()) {
        g_have_tune.store(true, std::memory_order_relaxed);
        if (REXCVAR_GET(cam_freelook_diag)) {
            MC_INFO("[cam-look] tune 0x{:08X} complete: L=0x{:08X} R=0x{:08X} "
                    "U=0x{:08X} D=0x{:08X} decay=0x{:08X} delay=0x{:08X}",
                    t.owner, t.limit_left, t.limit_right, t.limit_up,
                    t.limit_down, t.decay_rate, t.delay_time);
        }
    }
}

void TickCameraLook() {
    // Ahead of every early-out below: the timed measurement has to keep
    // running whether or not free look is on or a tune has been found.
    TickCameraProbe();

    // ── Who owns the pointer ───────────────────────────────────────────
    //
    // The full map drives an absolute cursor from the same mouse this steers
    // with. Free look re-centres the pointer every time it reads a delta, so
    // with the map up the two together read as the cursor being yanked to the
    // middle of the map -- and the map never sees a clean motion, so it cannot
    // take the mouse back on its own either.
    //
    // Decided here rather than inside the mouse read because this runs every
    // frame from Patch_DeltaTimePre, and the camera tick does not: if the map
    // stops the camera from updating while free look still holds the capture,
    // the pointer would stay hidden and pinned with nothing left to release it.
    bool yield = MapMouseWantsPointer();
    bool was = g_yield_pointer.exchange(yield, std::memory_order_relaxed);
    if (yield) {
        // Idempotent -- returns the cursor and drops the capture the first time
        // and does nothing on every frame after.
        float dx = 0.0f, dy = 0.0f;
        MouseDelta(false, dx, dy);
        g_look_active.store(false, std::memory_order_relaxed);
        // Behind the diag switch: the map's gate drops and comes back every
        // time something opens over it, so this would otherwise be a running
        // commentary on the Active Missions pop-up.
        if (!was && REXCVAR_GET(cam_freelook_diag))
            MC_INFO("[cam-look] map has the pointer; free look standing down");
    } else if (was && REXCVAR_GET(cam_freelook_diag)) {
        MC_INFO("[cam-look] pointer back");
    }

    bool want = REXCVAR_GET(cam_freelook);

    // mnk_mode routes the mouse into the right stick, which on this camera is
    // the digital pan actions -- exactly what free look replaces. Running both
    // means every mouse movement both steers the view and fires a pan step, so
    // the two fight. Free look wins and says so once.
    if (want && rex::cvar::GetFlagByName("mnk_mouse") == "true") {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            MC_WARN("[cam-look] cam_freelook and mnk_mouse both drive the view "
                    "-- mnk_mouse feeds the mouse to the right stick, which is "
                    "the pan actions this replaces. Turn mnk_mouse off.");
    }

    if (!g_have_tune.load(std::memory_order_relaxed)) {
        if (want) {
            static std::atomic<bool> told{false};
            if (!told.exchange(true))
                MC_WARN("[cam-look] no camera lookaround tune has registered "
                        "yet -- nothing to drive. It arrives with the first "
                        "camera tune the game loads.");
        }
        return;
    }

    // The angles come from Hook_CameraSystem, which is where the mouse is
    // read. All this path does is the optional tune pinning on top.
    bool active = g_look_active.load(std::memory_order_relaxed) &&
                  REXCVAR_GET(cam_freelook_tune);

    std::lock_guard<std::mutex> lock(g_tunes_mutex);

    if (!active) {
        if (g_applied) {
            for (auto& t : g_tunes) {
                if (t.complete()) RestoreOriginals(t);
            }
            g_applied = false;
        }
        return;
    }

    for (auto& t : g_tunes) {
        if (t.complete()) ApplyAngles(t, g_yaw, g_pitch);
    }
    g_applied = true;
}

// ── Camera field probe ─────────────────────────────────────────────────
//
// Two hooks have now been aimed at what looked like the gameplay camera and
// neither fired once: sub_82320298 ("mcPlayerCamera::Update", whose only
// cross-reference is a vtable slot) and sub_82316148 (the preset switcher).
// Both were emitted into generated/ and both stayed silent, so that whole
// 0x8231xxxx family is not what runs here.
//
// This anchors on something that provably does. sub_822C0320 is the camera
// system tick -- Patch_DebugCamGate and Patch_DebugCam live inside it and the
// free-fly camera works, so it executes every frame -- and `mr r20, r3` in its
// prologue makes r20 the camera system for the whole function.
//
// Rather than guess a third time at which object below it holds the look, the
// snapshot takes the system itself plus one level of everything it points at.
// Whatever the angle lives in, it is in there.

namespace {

std::atomic<uint32_t> g_cam_sys{0};
std::atomic<int32_t> g_cam_rig{-1};

// A block of guest floats to watch.
struct ProbeRegion {
    uint32_t base = 0;
    uint32_t count = 0;
    uint32_t from_offset = 0;   // where in the camera system the pointer came from
    bool is_root = false;
};

std::vector<ProbeRegion> g_regions;
std::vector<float> g_baseline;
uint32_t g_probe_sys = 0;
int32_t g_probe_rig = -1;

// The first sweep went one level deep over 2 KB and found exactly one object
// whose matrix moved. Writing to that matrix works -- the position in the log
// follows the mouse -- and the screen still does not, so the camera the
// renderer reads is somewhere this did not reach. Hence deeper and wider.
constexpr uint32_t kRootFloats = 1024;   // 4 KB of the camera system itself
// 512 bytes per child. A camera's basis and position live in the first few
// dozen bytes of whatever holds them, so a narrow window buys three times as
// many objects for the same total, and coverage is what is missing.
constexpr uint32_t kChildFloats = 128;
constexpr uint32_t kRootScanBytes = 4096;
constexpr uint32_t kChildScanBytes = 512;
// Split, because the first attempt let level one eat the whole budget and
// level two never ran at all -- every region in that dump was `from camsys+N`.
constexpr size_t kMaxLevel1 = 96;
constexpr size_t kMaxRegions = 192;

// Reading a guest address with no page behind it faults the host process, and
// a pointer scan is guesswork by construction -- most of what looks like a
// pointer is not one. Structured exception handling turns that from a crash
// into a skipped region. No C++ objects in here on purpose: a function that
// needs unwinding cannot carry __try.
bool SafeReadGuest(uint32_t ea, uint32_t bytes, void* dst) {
    uint8_t* base = GetMembase();
    if (!base || !ea) return false;
#if defined(_WIN32)
    __try {
        std::memcpy(dst, base + ea, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, base + ea, bytes);
    return true;
#endif
}

// The image sits at 0x82xxxxxx and this game's heap has been seen well above it
// (the vinyl work areas land around 0xBD000000). Anything outside that range,
// unaligned, or pointing back at the object being scanned is not worth chasing.
bool PlausibleGuestPtr(uint32_t v, uint32_t self) {
    if (v & 3u) return false;
    if (v < 0x82000000u || v >= 0xD0000000u) return false;
    return v != self;
}

// Hunts for a camera basis anywhere in the first `bytes` of an object and
// returns the offset it starts at, or -1.
//
// The earlier version only looked at +96, which is where the one camera found
// so far keeps it. That was assuming the answer: a different camera class has
// no reason to use the same offset, and the whole point of this sweep is to
// find one that has not been seen yet. Three consecutive 16-byte-strided
// vectors of unit length is the signature, and it is specific enough that
// random floats do not trip it.
int32_t FindCameraMatrix(uint32_t base, uint32_t bytes) {
    std::vector<uint8_t> raw(bytes);
    if (!SafeReadGuest(base, bytes, raw.data())) return -1;

    auto at_float = [&raw](uint32_t off) {
        uint32_t bits = (uint32_t(raw[off]) << 24) | (uint32_t(raw[off + 1]) << 16) |
                        (uint32_t(raw[off + 2]) << 8) | uint32_t(raw[off + 3]);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    };

    for (uint32_t start = 0; start + 48 <= bytes; start += 4) {
        bool ok = true;
        for (int row = 0; row < 3 && ok; ++row) {
            float len2 = 0.0f;
            for (int c = 0; c < 3; ++c) {
                float f = at_float(start + uint32_t(row) * 16 + uint32_t(c) * 4);
                if (!std::isfinite(f)) { ok = false; break; }
                len2 += f * f;
            }
            if (ok && std::fabs(len2 - 1.0f) > 0.02f) ok = false;
        }
        if (ok) return int32_t(start);
    }
    return -1;
}

// Adds every plausible pointer found in `bytes` of `base` as its own region.
void ScanForChildren(uint32_t base, uint32_t bytes, uint32_t self_for_filter,
                     bool from_root, size_t cap) {
    std::vector<uint8_t> scan(bytes);
    if (!SafeReadGuest(base, bytes, scan.data())) return;

    for (uint32_t off = 0; off + 4 <= bytes; off += 4) {
        if (g_regions.size() >= cap) return;

        uint32_t v = (uint32_t(scan[off]) << 24) | (uint32_t(scan[off + 1]) << 16) |
                     (uint32_t(scan[off + 2]) << 8) | uint32_t(scan[off + 3]);
        if (!PlausibleGuestPtr(v, self_for_filter)) continue;

        bool dup = false;
        for (const auto& r : g_regions) {
            if (r.base == v) { dup = true; break; }
        }
        if (dup) continue;

        // Prove it reads before committing, so the snapshot itself never has
        // to cope with a dead region.
        uint8_t probe[4];
        if (!SafeReadGuest(v, 4, probe)) continue;

        g_regions.push_back({v, kChildFloats, from_root ? off : base, false});
    }
}

void BuildRegions(uint32_t sys) {
    g_regions.clear();
    g_regions.push_back({sys, kRootFloats, 0, true});

    ScanForChildren(sys, kRootScanBytes, sys, true, kMaxLevel1);

    // A second level. The camera the renderer reads was not among the first,
    // and one more hop is cheap next to another round trip.
    size_t first_level_end = g_regions.size();
    for (size_t i = 1; i < first_level_end && g_regions.size() < kMaxRegions; ++i) {
        ScanForChildren(g_regions[i].base, kChildScanBytes, sys, false, kMaxRegions);
    }
}

// The timed measurement. Arming it is the only thing that needs the overlay,
// and by the time either sample is taken the overlay is shut and the camera is
// back in the player's hands.
enum class ProbePhase { kIdle, kWaitingBaseline, kWaitingDiff };
ProbePhase g_phase = ProbePhase::kIdle;
std::chrono::steady_clock::time_point g_phase_deadline;
int g_countdown_logged = -1;

bool ReadRegions(std::vector<float>& out) {
    out.clear();
    for (const auto& r : g_regions) {
        size_t at = out.size();
        out.resize(at + r.count, 0.0f);
        std::vector<uint8_t> raw(size_t(r.count) * 4);
        if (!SafeReadGuest(r.base, r.count * 4, raw.data())) continue;
        for (uint32_t i = 0; i < r.count; ++i) {
            uint32_t bits = (uint32_t(raw[i * 4]) << 24) | (uint32_t(raw[i * 4 + 1]) << 16) |
                            (uint32_t(raw[i * 4 + 2]) << 8) | uint32_t(raw[i * 4 + 3]);
            std::memcpy(&out[at + i], &bits, sizeof(float));
        }
    }
    return !out.empty();
}

void TickCameraProbe() {
    if (g_phase == ProbePhase::kIdle) return;

    auto now = std::chrono::steady_clock::now();
    double left = std::chrono::duration<double>(g_phase_deadline - now).count();

    // A second-by-second countdown, because the whole point is that the player
    // cannot see a UI while this runs and has to be told by the log when to
    // start holding.
    int whole = int(left) + 1;
    if (whole != g_countdown_logged && left > 0.0) {
        g_countdown_logged = whole;
        if (g_phase == ProbePhase::kWaitingBaseline)
            MC_INFO("[cam-probe] baseline in {}...", whole);
        else
            MC_INFO("[cam-probe] hold the look -- {} to go...", whole);
    }
    if (left > 0.0) return;

    g_countdown_logged = -1;
    if (g_phase == ProbePhase::kWaitingBaseline) {
        CameraProbeMark();
        if (g_baseline.empty()) {
            g_phase = ProbePhase::kIdle;
            return;
        }
        g_phase = ProbePhase::kWaitingDiff;
        g_phase_deadline =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double>(
                          REXCVAR_GET(cam_probe_hold_delay)));
        MC_INFO("[cam-probe] BASELINE TAKEN -- hold the look now, for {:.0f}s",
                double(REXCVAR_GET(cam_probe_hold_delay)));
    } else {
        g_phase = ProbePhase::kIdle;
        CameraProbeDump();
    }
}

}  // namespace

void CameraProbeRun() {
    g_phase = ProbePhase::kWaitingBaseline;
    g_countdown_logged = -1;
    g_phase_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(REXCVAR_GET(cam_probe_arm_delay)));
    MC_INFO("[cam-probe] armed. Close the overlay and leave the camera alone; "
            "baseline in {:.0f}s, then hold the look for {:.0f}s.",
            double(REXCVAR_GET(cam_probe_arm_delay)),
            double(REXCVAR_GET(cam_probe_hold_delay)));
}

namespace {

// Reads the mouse and integrates it into the shared angles. Called from exactly
// one place, once per camera tick.
void UpdateLookAngles() {
    static std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
    static float idle = 0.0f;

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    if (!(dt > 0.0f) || dt > 0.5f) dt = 1.0f / 60.0f;

    float dx = 0.0f, dy = 0.0f;
    bool want = REXCVAR_GET(cam_freelook) &&
                !g_yield_pointer.load(std::memory_order_relaxed);
    bool active = MouseDelta(want, dx, dy);
    g_look_active.store(active, std::memory_order_relaxed);

    float rate = float(REXCVAR_GET(cam_freelook_return_rate));
    float ease = 1.0f - std::exp(-rate * dt);  // frame-rate independent

    if (!active) {
        // Whatever took the mouse away -- an overlay, lost focus, the feature
        // being switched off -- should not leave the view parked off to one
        // side. Ease it back rather than snapping, so it is the same motion
        // the player sees when they simply stop moving the mouse.
        g_yaw -= g_yaw * ease;
        g_pitch -= g_pitch * ease;
        idle = 0.0f;
        return;
    }

    float sens = float(REXCVAR_GET(cam_freelook_sens));
    if (REXCVAR_GET(cam_freelook_invert_x)) dx = -dx;
    if (REXCVAR_GET(cam_freelook_invert_y)) dy = -dy;

    // A pixel or two of pointer jitter is not the player looking around, and
    // treating it as such is what keeps the recentring from ever starting.
    bool moving = (std::fabs(dx) + std::fabs(dy)) > 1.5f;

    if (moving) {
        idle = 0.0f;
        // Displacement, not a rate: a mouse delta is already per-frame, so
        // scaling it by dt would make the same movement mean different amounts
        // at different frame rates.
        g_yaw += dx * sens;
        g_pitch += dy * sens;
    } else {
        idle += dt;
        if (idle >= float(REXCVAR_GET(cam_freelook_return_delay))) {
            g_yaw -= g_yaw * ease;
            g_pitch -= g_pitch * ease;
            // Below a twentieth of a degree there is nothing left to see, and
            // an exponential never quite arrives on its own.
            if (std::fabs(g_yaw) < 0.001f) g_yaw = 0.0f;
            if (std::fabs(g_pitch) < 0.001f) g_pitch = 0.0f;
        }
    }

    float yaw_limit = float(REXCVAR_GET(cam_freelook_yaw_limit));
    float pitch_limit = float(REXCVAR_GET(cam_freelook_pitch_limit));
    if (g_yaw > yaw_limit) g_yaw = yaw_limit;
    if (g_yaw < -yaw_limit) g_yaw = -yaw_limit;
    if (g_pitch > pitch_limit) g_pitch = pitch_limit;
    if (g_pitch < -pitch_limit) g_pitch = -pitch_limit;
}

// Resolves camsys -> the look object through the cvar-configured offset.
uint32_t LookObject(uint32_t sys) {
    int32_t off = REXCVAR_GET(cam_look_obj);
    if (off < 0) return 0;
    uint8_t raw[4];
    if (!SafeReadGuest(sys + uint32_t(off), 4, raw)) return 0;
    uint32_t v = (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) |
                 (uint32_t(raw[2]) << 8) | uint32_t(raw[3]);
    return PlausibleGuestPtr(v, sys) ? v : 0;
}

// ── The camera matrix ──────────────────────────────────────────────────
//
// Measured, not assumed. Writing an angle into the object before the update
// proved that the game recomputes it from its own input every frame -- the
// read-back came out 0.0000 no matter what went in -- so the look is applied
// to the finished matrix instead, at the end of the camera tick.
//
// The layout came out of the field diff: three unit-length rows and a world
// position, stride 16, the spare w lanes carrying scalars.
// A 4x4 of 16-byte rows: right, up, forward, position. Offsets from the matrix
// itself, not from any object -- the same sixteen floats turn up at +96 of the
// cine director's mirror and at +0 of the stack matrix the camera system
// actually publishes, and both need turning.
constexpr uint32_t kRowRight = 0;
constexpr uint32_t kRowUp = 16;
constexpr uint32_t kRowFwd = 32;
constexpr uint32_t kRowPos = 48;
constexpr uint32_t kRowWLane = 60;  // w of the position row: the orbit distance

// Where the mirror keeps its copy, for the phases that still write to it.
constexpr uint32_t kMirrorMatrix = 96;

// ── The camera state, one step upstream of everything above ─────────────
//
// sub_8231CE30 is the shared body of the camera-state update -- mcTrackCS and
// vehTrackCS both reach it from vtable slot 10 -- and it runs in this order:
//
//     sub_8231C4F8(cs)          builds the desired position and basis
//     sub_8231B368(cs)
//     sub_823228F0(cs)
//     if (cs+480) sub_8231B4E0(cs, old_matrix)
//     if (cs+464) sub_8231B740(cs, cs+64)     <- the collision
//
// sub_8231B740 is the camera collision: it sphere-tests the target with
// sub_82574CA0 and segment-tests target -> camera with sub_82574C38, both
// against the physics world at dword_828DA464, then rewrites the camera
// position as `target + dir * distance` with the distance shortened to the
// hit. The tune fields that steer it are named in the .rdata block next to the
// vtable: CollideType, CollideMinDist, CollideFudge, CollideHeight.
//
// So turning the camera HERE, before the collision runs, is the whole fix: the
// game's own collision then resolves the rotated position for free. Turning the
// finished matrix at sub_8220AA68 happens long after this and is why free look
// went through walls.
//
// Layout, all confirmed against sub_822AFA10 (which hands the camera system
// `cs+80` when cs+156 says the camera collided this frame, and `cs+16`
// otherwise) and against sub_8231B740's own reads:
constexpr uint32_t kCsMatrix = 16;        // right/up/fwd/pos at +16/+32/+48/+64
constexpr uint32_t kCsTarget = 320;       // what the camera looks at and orbits
constexpr uint32_t kCsCollideType = 464;  // 0 = collision off for this state
constexpr uint32_t kCsCollided = 156;     // set by the collision when it bit
constexpr uint32_t kCsOwner = 192;        // the thing being followed; null = idle
constexpr uint32_t kCsPitch = 772;        // pitch fed to sub_82227DE0 after the update
constexpr uint32_t kCsYaw = 776;          // yaw fed to sub_82204CB8 after the update

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 ReadVec3(uint32_t ea) {
    return {ReadGuestFloat(ea), ReadGuestFloat(ea + 4), ReadGuestFloat(ea + 8)};
}

void WriteVec3(uint32_t ea, const Vec3& v) {
    WriteGuestFloat(ea, v.x);
    WriteGuestFloat(ea + 4, v.y);
    WriteGuestFloat(ea + 8, v.z);
}

Vec3 RotateY(const Vec3& v, float c, float s) {
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

// Rotates `v` about `axis`, which is assumed already normalised. Used for the
// pitch, which turns about the camera's own right vector rather than a world
// axis -- doing it about world X instead would roll the view as soon as the
// car was not pointing down it.
Vec3 RotateAxis(const Vec3& v, const Vec3& axis, float c, float s) {
    float dot = axis.x * v.x + axis.y * v.y + axis.z * v.z;
    Vec3 cross{axis.y * v.z - axis.z * v.y, axis.z * v.x - axis.x * v.z,
               axis.x * v.y - axis.y * v.x};
    return {v.x * c + cross.x * s + axis.x * dot * (1.0f - c),
            v.y * c + cross.y * s + axis.y * dot * (1.0f - c),
            v.z * c + cross.z * s + axis.z * dot * (1.0f - c)};
}

// Turns a camera basis: yaw about world Y, pitch about the camera's own right
// vector. Pitching about world X instead would roll the view the moment the car
// stopped pointing down it.
//
// `clamp_elev` applies the absolute elevation limits. They exist to keep the
// camera POSITION out of the ground, so they only make sense where the position
// orbits; on a view bolted to the car they would do nothing but stop the driver
// looking at their own dashboard.
void RotateBasis(Vec3& right, Vec3& up, Vec3& fwd, float yaw, float pitch,
                 bool clamp_elev) {
    float cy = std::cos(yaw), sy = std::sin(yaw);
    right = RotateY(right, cy, sy);
    up = RotateY(up, cy, sy);
    fwd = RotateY(fwd, cy, sy);

    // Pitch, in absolute elevation rather than as a relative nudge.
    //
    // The mouse asks for `pitch` more tilt; what it gets is whatever keeps the
    // camera between the two elevation limits. Clamping the accumulator alone
    // could not do this -- the base matrix arrives with its own elevation and
    // it changes with speed and terrain, so the same relative tilt lands
    // somewhere different every frame, and some of those places are under the
    // ground.
    float e0 = std::asin(std::fmax(-1.0f, std::fmin(1.0f, fwd.y)));

    // Which way positive phi tilts depends on the handedness of the basis,
    // which is not worth deriving when it can be measured: rotate a copy, see
    // which way the elevation went, and use that sign from then on. Costs two
    // cross products and cannot be wrong.
    float probe_c = std::cos(0.01f), probe_s = std::sin(0.01f);
    Vec3 probe = RotateAxis(fwd, right, probe_c, probe_s);
    float sign = (std::asin(std::fmax(-1.0f, std::fmin(1.0f, probe.y))) >= e0)
                     ? 1.0f : -1.0f;

    float want = e0 + pitch;
    if (clamp_elev) {
        float lo = float(REXCVAR_GET(cam_freelook_min_elev));
        float hi = float(REXCVAR_GET(cam_freelook_max_elev));
        if (hi < lo) hi = lo;
        want = std::fmax(lo, std::fmin(hi, want));
    }

    float phi = sign * (want - e0);
    if (std::fabs(phi) > 1.0e-5f) {
        float cp = std::cos(phi), sp = std::sin(phi);
        up = RotateAxis(up, right, cp, sp);
        fwd = RotateAxis(fwd, right, cp, sp);
    }
}

// Returns the orbit distance it used, or 0 if it declined to touch anything.
float ApplyLookToMatrix(uint32_t mat, float yaw, float pitch,
                        bool w_lane_holds_distance, bool in_place) {
    Vec3 right = ReadVec3(mat + kRowRight);
    Vec3 up = ReadVec3(mat + kRowUp);
    Vec3 fwd = ReadVec3(mat + kRowFwd);
    Vec3 pos = ReadVec3(mat + kRowPos);

    // The field first, the cvar when it has nothing in it. It has nothing in it
    // most of the time: +156 is computed alongside the angle at +140 and both
    // sit at zero while the game's own look is idle, which is precisely when
    // this code is the one looking. An earlier version bailed out on that zero
    // and so never wrote anything at all, which looked exactly like the write
    // not working.
    auto usable = [](float v) {
        return std::isfinite(v) && std::fabs(v) >= 0.01f && std::fabs(v) <= 500.0f;
    };
    // In place, the position never moves, so there is no orbit and nothing to
    // find a radius for. Everything below that talks about `centre` is skipped.
    float d = 0.0f;
    if (!in_place) {
        d = w_lane_holds_distance ? ReadGuestFloat(mat + kRowWLane) : 0.0f;
        if (!usable(d)) {
            // The forward row points from the target back towards the camera
            // -- that is what made `pos + forward * (a negative d)` land on
            // the same world point across the sample -- so a fallback has to
            // carry the same sign as the field it stands in for.
            d = -std::fabs(float(REXCVAR_GET(cam_freelook_orbit)));
            if (!usable(d)) return 0.0f;
        }
    }

    Vec3 centre{pos.x + fwd.x * d, pos.y + fwd.y * d, pos.z + fwd.z * d};

    RotateBasis(right, up, fwd, yaw, pitch, !in_place);

    // pos - centre is exactly -forward * d, so the rotated position falls out
    // of the rotated forward. In place there is no centre and pos stands.
    if (!in_place)
        pos = {centre.x - fwd.x * d, centre.y - fwd.y * d, centre.z - fwd.z * d};

    WriteVec3(mat + kRowRight, right);
    WriteVec3(mat + kRowUp, up);
    WriteVec3(mat + kRowFwd, fwd);
    WriteVec3(mat + kRowPos, pos);
    return d;
}

// Turns the camera state itself, upstream of the game's camera collision.
//
// ── Why this does not touch the basis ───────────────────────────────────
//
// cs+16..+48 is NOT the camera's world basis. sub_8231CF48 finishes every
// update by running two more rotations over it, after sub_8231CE30 and after
// the collision:
//
//     lfs f1, 776(cs);  sub_82204CB8(cs+16, f1)   yaw
//     lfs f1, 772(cs);  sub_82227DE0(cs+16, f1)   pitch
//     if (cs+156) the same pair again on cs+80
//
// sub_82204CB8 builds its rotation with sub_82202E38, which is
//
//     row0 = ( sin(a), 0, -cos(a) )
//     row1 = (      0, 1,       0 )
//     row2 = ( cos(a), 0,  sin(a) )
//
// and applies it as `basis_row' = b.x*R0 + b.y*R1 + b.z*R2` -- a row times the
// matrix, so R turns each basis row about the WORLD Y. Matching those
// coefficients against an ordinary Y rotation by phi gives cos(phi) = sin(a)
// and sin(phi) = cos(a), so
//
//     phi = pi/2 - cs+776       and, by the same argument on sub_82227DE0,
//     psi = pi/2 - cs+772
//
// The stored angle carries a 90-degree offset and runs backwards. Two things
// follow. The forward row read out of cs+48 is about 90 degrees away from where
// the camera actually looks -- an earlier version of this dotted it against the
// direction to the target to decide whether the rig orbits, the dot came out
// near zero every time, the orbit was declined and the position never moved,
// which is exactly what "the collision switch turns free look off" looked like.
// And the game already owns two angle channels that do the orientation
// properly, so there is no reason to rotate a basis by hand at all.
//
// So: the position is orbited as world geometry (cs+64 about cs+320, no basis
// involved), and the orientation is handed to the game by adding to its own two
// angles. Neither channel accumulates -- sub_8231C4F8 rewrites both, and the
// position, before this hook runs.
bool ApplyLookToCameraState(uint32_t cs, float yaw, float pitch, float* out_d) {
    const uint32_t mat = cs + kCsMatrix;

    Vec3 pos = ReadVec3(mat + kRowPos);
    Vec3 centre = ReadVec3(cs + kCsTarget);
    if (!std::isfinite(pos.x) || !std::isfinite(centre.x)) return false;

    Vec3 rel{pos.x - centre.x, pos.y - centre.y, pos.z - centre.z};
    float dist = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    bool orbit = std::isfinite(dist) && dist > 0.05f && dist < 500.0f;

    // The pitch that survives the elevation clamp. The orientation has to be
    // given the same number the position was moved by, or the camera stops
    // pointing at the car the moment the clamp bites.
    float pitch_used = pitch;

    if (orbit) {
        // Yaw about world Y. Same convention as RotateY: phi with cos and sin
        // passed in.
        rel = RotateY(rel, std::cos(yaw), std::sin(yaw));

        // Pitch about the horizontal axis perpendicular to the orbit arm, which
        // is derived from the arm itself rather than read from a basis that is
        // in the wrong space.
        Vec3 axis{-rel.z, 0.0f, rel.x};
        float alen = std::sqrt(axis.x * axis.x + axis.z * axis.z);
        if (alen > 1.0e-4f) {
            axis = {axis.x / alen, 0.0f, axis.z / alen};

            // Elevation of the CAMERA above the target, which is what the
            // limits are actually about, and now measured in world space where
            // the number means something.
            float e0 = std::asin(std::fmax(-1.0f, std::fmin(1.0f, rel.y / dist)));
            float lo = float(REXCVAR_GET(cam_freelook_min_elev));
            float hi = float(REXCVAR_GET(cam_freelook_max_elev));
            if (hi < lo) hi = lo;
            float want = std::fmax(lo, std::fmin(hi, e0 + pitch));
            pitch_used = want - e0;

            if (std::fabs(pitch_used) > 1.0e-5f) {
                Vec3 probe = RotateAxis(rel, axis, std::cos(0.01f),
                                        std::sin(0.01f));
                // Which way positive turns about this axis raises the camera is
                // a handedness question; measure it rather than derive it.
                float sign = (probe.y >= rel.y) ? 1.0f : -1.0f;
                float t = sign * pitch_used;
                rel = RotateAxis(rel, axis, std::cos(t), std::sin(t));
            }
        }

        pos = {centre.x + rel.x, centre.y + rel.y, centre.z + rel.z};
        WriteVec3(mat + kRowPos, pos);
    }

    // The orientation, through the game's own channels. phi = pi/2 - angle, so
    // a turn of +phi means SUBTRACTING phi from the stored value.
    float sign = REXCVAR_GET(cam_freelook_cs_invert) ? -1.0f : 1.0f;
    float ya = ReadGuestFloat(cs + kCsYaw);
    float pa = ReadGuestFloat(cs + kCsPitch);
    if (std::isfinite(ya)) WriteGuestFloat(cs + kCsYaw, ya - sign * yaw);
    if (std::isfinite(pa)) WriteGuestFloat(cs + kCsPitch, pa - sign * pitch_used);

    if (out_d) *out_d = orbit ? dist : 0.0f;
    return true;
}

// ── Is the camera state the one the screen is drawn from? ───────────────
//
// Not assumed, measured. sub_822AFA10 hands the camera system
//
//     v1 = *(*(cam + 16) + 48);
//     return *(v1 + 156) ? v1 + 80 : v1 + 16;
//
// Two things in there are unproven. That `v1` is the same object this hook is
// handed in r31 -- it is reached by two indirections from somewhere else
// entirely -- and that the live matrix is the one at +16 rather than the
// collided copy at +80. Get either wrong and the rotation lands on an object
// nobody reads, which is what writing to the cine director's mirror looked
// like: the write succeeds and the screen never moves.
//
// Both fall out of one comparison. The camera state hook records where its two
// matrices put the camera; the publish hook, later in the same frame, holds the
// matrix the frame is actually rendered from. If the published position sits on
// top of one of the two, that one is live and this camera state is the right
// object. If it sits on neither, the object is wrong and no amount of tuning
// this end will ever show up.
struct CsSnapshot {
    uint32_t cs = 0;
    uint32_t first_cs = 0;
    uint32_t collide_type = 0;
    uint8_t collided = 0;
    Vec3 pos_uncollided;  // cs+64,  the matrix at cs+16
    Vec3 pos_collided;    // cs+128, the matrix at cs+80
    uint32_t hits = 0;      // camera states updated since the last publish
    uint32_t distinct = 0;  // how many of them were different objects
};
CsSnapshot g_cs_snap;

uint32_t ReadGuestU32(uint32_t ea) {
    uint8_t raw[4] = {0, 0, 0, 0};
    if (!SafeReadGuest(ea, 4, raw)) return 0;
    return (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) |
           (uint32_t(raw[2]) << 8) | uint32_t(raw[3]);
}

void RecordCsSnapshot(uint32_t cs) {
    if (g_cs_snap.hits == 0) {
        g_cs_snap.first_cs = cs;
        g_cs_snap.distinct = 1;
    } else if (cs != g_cs_snap.cs) {
        ++g_cs_snap.distinct;
    }
    ++g_cs_snap.hits;
    g_cs_snap.cs = cs;
    g_cs_snap.collide_type = ReadGuestU32(cs + kCsCollideType);
    g_cs_snap.collided = 0;
    SafeReadGuest(cs + kCsCollided, 1, &g_cs_snap.collided);
    g_cs_snap.pos_uncollided = ReadVec3(cs + kCsMatrix + kRowPos);
    g_cs_snap.pos_collided = ReadVec3(cs + 80 + kRowPos);
}

float Dist(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Called from the publish hook with the matrix the frame is rendered from.
void ReportCsMatch(uint32_t published_matrix) {
    static int throttle = 0;
    CsSnapshot snap = g_cs_snap;
    g_cs_snap.hits = 0;
    g_cs_snap.distinct = 0;

    if (--throttle > 0) return;
    throttle = 60;

    if (snap.hits == 0) {
        MC_INFO("[cam-look] match: no camera state was updated this frame -- "
                "sub_8231CE30 is not on the path of whatever is driving the "
                "view, so turning it there can never show up.");
        return;
    }

    Vec3 pub = ReadVec3(published_matrix + kRowPos);
    float d16 = Dist(pub, snap.pos_uncollided);
    float d80 = Dist(pub, snap.pos_collided);
    const char* live = "NEITHER -- wrong object";
    if (d16 <= 0.5f && d16 <= d80) live = "cs+16 (uncollided)";
    else if (d80 <= 0.5f) live = "cs+80 (collided)";

    MC_INFO("[cam-look] match: live={} cs=0x{:08X} states={} distinct={} "
            "CollideType={} collided={} published=({:.1f}, {:.1f}, {:.1f}) "
            "cs+64=({:.1f}, {:.1f}, {:.1f}) d={:.2f} "
            "cs+128=({:.1f}, {:.1f}, {:.1f}) d={:.2f}",
            live, snap.cs, snap.hits, snap.distinct, snap.collide_type,
            int(snap.collided), pub.x, pub.y, pub.z,
            snap.pos_uncollided.x, snap.pos_uncollided.y,
            snap.pos_uncollided.z, d16,
            snap.pos_collided.x, snap.pos_collided.y, snap.pos_collided.z, d80);
}

}  // namespace

void Hook_CameraSystem(PPCRegister& r20, PPCRegister& r24) {
    const uint32_t sys = r20.u32;
    if (!sys) return;

    uint32_t prev = g_cam_sys.exchange(sys, std::memory_order_relaxed);
    if (prev != sys)
        MC_INFO("[cam-probe] camera system is 0x{:08X}", sys);

    UpdateLookAngles();


    // r24 is the camera rig the system settled on this frame -- 1 is the
    // cockpit view and 2 a static one, per the note on Patch_DebugCam. If this
    // moves when the player looks around, the on-screen angles are whole rig
    // changes and no amount of angle tuning reaches them.
    const int32_t rig = r24.s32;
    if (g_cam_rig.exchange(rig, std::memory_order_relaxed) != rig)
        MC_INFO("[cam-probe] camera rig {} (r24 in sub_822C0320)", rig);
}

namespace {

// Shared by both hook sites; cam_look_phase picks which one is live.
// Is this rig one of the ones bolted to the car? Parsed rather than cached:
// the list is a handful of characters, this runs once a frame, and a cache
// would have to be invalidated when the cvar is edited live -- which is the
// whole point of it being hot-reloadable.
bool RigTurnsInPlace(int32_t rig) {
    // By value, not a view: the cvar accessor hands back a std::string and a
    // view of a temporary would dangle the moment this line ended.
    const std::string list = REXCVAR_GET(cam_freelook_inplace_rigs);
    size_t at = 0;
    while (at < list.size()) {
        while (at < list.size() && (list[at] == ' ' || list[at] == ',')) ++at;
        size_t start = at;
        bool neg = (at < list.size() && list[at] == '-');
        if (neg) ++at;
        int32_t value = 0;
        bool any = false;
        while (at < list.size() && list[at] >= '0' && list[at] <= '9') {
            value = value * 10 + (list[at] - '0');
            ++at;
            any = true;
        }
        if (!any) {
            if (at == start) ++at;  // a stray character; do not spin on it
            continue;
        }
        if ((neg ? -value : value) == rig) return true;
    }
    return false;
}

void ApplyFreeLook(uint32_t sys, int phase) {
    if (REXCVAR_GET(cam_look_phase) != phase) return;
    if (!g_look_active.load(std::memory_order_relaxed)) return;
    if (g_yaw == 0.0f && g_pitch == 0.0f) return;
    if (!sys) return;

    uint32_t obj = LookObject(sys);
    if (!obj) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true))
            MC_WARN("[cam-look] camsys+{} does not hold a usable pointer -- "
                    "check cam_look_obj against the region list at the top of "
                    "cam_fields.txt.", REXCVAR_GET(cam_look_obj));
        return;
    }

    float used = ApplyLookToMatrix(obj + kMirrorMatrix, g_yaw, g_pitch, true, false);

    if (REXCVAR_GET(cam_freelook_diag)) {
        static int throttle = 0;
        if (--throttle <= 0) {
            throttle = 60;
            Vec3 p = ReadVec3(obj + kMirrorMatrix + kRowPos);
            // `used` is what the orbit was actually done with, not what the
            // field holds -- the field reads zero most of the time and logging
            // it hid the fact that nothing was being written at all.
            MC_INFO("[cam-look] yaw={:.3f} pitch={:.3f} d={:.2f} (field {:.2f}) "
                    "pos=({:.1f}, {:.1f}, {:.1f}){}",
                    g_yaw, g_pitch, used,
                    ReadGuestFloat(obj + kMirrorMatrix + kRowWLane),
                    p.x, p.y, p.z,
                    used == 0.0f ? "  <-- DECLINED, nothing written" : "");
        }
    }
}

}  // namespace

// sub_8220AA68 @0x8220AA88 -- r30 is the camera matrix, one instruction after
// `mr r30, r4` and before a single byte of it has been read.
//
// This is the matrix, not a copy of one. Everything this session wrote to
// before now was the mirror the cine director keeps: sub_823CC2E8 turned out
// to be a plain setter --
//
//     obj+96..+156 = src[0..63];  *(float*)(obj+160) = fov;
//
// -- so camsys+0x35C is written FROM this matrix, which is why turning it
// changed nothing anyone could see.
//
// From here the game itself carries the rotation everywhere it needs to go:
// sub_8220AA68 saves this matrix to dword_8286D804+4208, applies whatever
// cutscene, spectator and shake blending is due, and publishes the result to
// dword_8286D804+4272, unk_8286D900 and flt_8286D8FC. Turning it at the door
// means all of that composes on top instead of being fought.
void Hook_CameraMatrixPublish(PPCRegister& r30) {
    // Said once, before any of the gates below, so "the hook never ran" and
    // "the hook ran and did nothing" can never again look the same from a log.
    // sub_8220AA68 is reached through two branches that can skip it, so
    // whether it runs at all in gameplay is a real question and not a
    // rhetorical one.
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true))
        MC_INFO("[cam-look] sub_8220AA68 hook is live, matrix at 0x{:08X}",
                r30.u32);

    // Before every gate below: this is the matrix the frame is rendered from,
    // and it is the only place that can say whether the camera state the other
    // hook turns is the object the screen is drawn from.
    if (REXCVAR_GET(cam_freelook_diag)) ReportCsMatch(r30.u32);

    int phase = REXCVAR_GET(cam_look_phase);
    // The camera-state hook turns the camera upstream of the game's collision.
    // On the frames it did that, turning the matrix again here would double the
    // angle, so this site stands down. It does not fire for every rig -- a
    // camera state of a class that does not route through sub_8231CE30 never
    // reaches it -- and on those frames this site still covers the view, as
    // collision-free as it always was.
    if (g_cs_applied.exchange(0, std::memory_order_relaxed) != 0) return;

    if (phase != 2) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true) && REXCVAR_GET(cam_freelook))
            MC_WARN("[cam-look] cam_look_phase is {}, so the look is being "
                    "applied to the cine director's MIRROR and cannot move the "
                    "screen. Set it to 2. The default is 2, but a value saved "
                    "in larecomp.toml from an earlier build overrides that.",
                    phase);
        return;
    }
    if (!g_look_active.load(std::memory_order_relaxed)) return;
    if (g_yaw == 0.0f && g_pitch == 0.0f) return;

    const uint32_t mat = r30.u32;
    if (!mat) return;

    // No w lane to read here: this is a bare 4x4, and the distance the mirror
    // keeps at +156 is something the camera system computes for its own use
    // and leaves at zero whenever its own look is idle -- which is exactly
    // when this is doing the looking.
    // Sampled before the rotation so the log can show that the write actually
    // changed something. A forward vector that comes out where it went in
    // means the maths is degenerate, which is a different problem from the
    // matrix being the wrong one.
    Vec3 before = ReadVec3(mat + kRowFwd);
    // A view mounted on the car turns the head; a chase view swings the rig.
    int32_t rig = g_cam_rig.load(std::memory_order_relaxed);
    bool in_place = RigTurnsInPlace(rig);

    float used = ApplyLookToMatrix(mat, g_yaw, g_pitch, false, in_place);

    if (REXCVAR_GET(cam_freelook_diag)) {
        static int throttle = 0;
        if (--throttle <= 0) {
            throttle = 60;
            Vec3 after = ReadVec3(mat + kRowFwd);
            Vec3 p = ReadVec3(mat + kRowPos);
            float moved = std::sqrt((after.x - before.x) * (after.x - before.x) +
                                    (after.y - before.y) * (after.y - before.y) +
                                    (after.z - before.z) * (after.z - before.z));
            MC_INFO("[cam-look] phase2 rig={} {} yaw={:.3f} d={:.2f} "
                    "fwd ({:.3f}, {:.3f}, {:.3f}) -> ({:.3f}, {:.3f}, {:.3f}) "
                    "moved={:.3f} pos=({:.1f}, {:.1f}, {:.1f}){}",
                    g_cam_rig.load(std::memory_order_relaxed),
                    in_place ? "in-place" : "orbit", g_yaw, used,
                    before.x, before.y, before.z, after.x, after.y, after.z,
                    moved, p.x, p.y, p.z,
                    (!in_place && used == 0.0f) ? "  <-- DECLINED" : "");
        }
    }
}

void Hook_CameraPrePublish(PPCRegister& r20) { ApplyFreeLook(r20.u32, 0); }
void Hook_CameraPostUpdate(PPCRegister& r20) { ApplyFreeLook(r20.u32, 1); }

// sub_8231CE30 @0x8231CEB0 -- r31 is the camera state, immediately after
// `bl sub_823228F0` and before the two passes that resolve the camera against
// the world:
//
//     0x8231CEB0  lwz  r7, 0x1E0(r31)   <- here
//                 if (cs+480) sub_8231B4E0(cs, old_matrix)
//                 if (cs+464) sub_8231B740(cs, cs+64)   the collision
//
// This is what makes free look stop going through walls. sub_8231B740 shortens
// the camera's distance from cs+320 until the segment to it is clear, so a
// rotation applied before it is collided by the game itself; a rotation applied
// at sub_8220AA68, where every earlier version of this lived, happens after the
// collision has already run and there is nothing left to clamp it.
void Hook_CameraStateUpdate(PPCRegister& r31) {
    const uint32_t cs = r31.u32;
    if (!cs) return;

    // Said once and before every gate below, so "the hook never ran" and "the
    // hook ran and declined" can never look the same from a log. The whole
    // 0x8231xxxx camera family was written off as dead in an earlier session on
    // the strength of two functions whose only xref turned out to be a .pdata
    // entry rather than a vtable slot, so this one gets to prove itself.
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) {
        uint8_t raw[4] = {0, 0, 0, 0};
        SafeReadGuest(cs + kCsCollideType, 4, raw);
        MC_INFO("[cam-look] camera-state hook is live, cs=0x{:08X} "
                "CollideType={}", cs,
                (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) |
                    (uint32_t(raw[2]) << 8) | uint32_t(raw[3]));
    }

    // Recorded before the switch below, so the comparison can be taken with
    // the collision OFF -- that is, with free look known to be working on the
    // published matrix. Measuring a feature while it is broken tells you much
    // less than measuring it while it works.
    const bool diag = REXCVAR_GET(cam_freelook_diag);
    if (diag) RecordCsSnapshot(cs);

    if (!REXCVAR_GET(cam_freelook_collide)) return;
    if (!g_look_active.load(std::memory_order_relaxed)) return;
    if (g_yaw == 0.0f && g_pitch == 0.0f) return;

    float d = 0.0f;
    if (!ApplyLookToCameraState(cs, g_yaw, g_pitch, &d)) return;

    // Tells the publish site to keep its hands off this frame.
    g_cs_applied.store(1, std::memory_order_relaxed);

    if (diag) {
        static int throttle = 0;
        if (--throttle <= 0) {
            throttle = 60;
            uint32_t collide_type = ReadGuestU32(cs + kCsCollideType);
            uint8_t hit = 0;
            SafeReadGuest(cs + kCsCollided, 1, &hit);
            Vec3 p = ReadVec3(cs + kCsMatrix + kRowPos);
            Vec3 c = ReadVec3(cs + kCsTarget);
            MC_INFO("[cam-look] collide cs=0x{:08X} {} yaw={:.3f} pitch={:.3f} "
                    "d={:.2f} CollideType={} collided={} "
                    "pos=({:.1f}, {:.1f}, {:.1f}) target=({:.1f}, {:.1f}, {:.1f}) "
                    "cs+776={:.3f} cs+772={:.3f}",
                    cs, d > 0.0f ? "orbit" : "in-place", g_yaw, g_pitch, d,
                    collide_type, int(hit), p.x, p.y, p.z, c.x, c.y, c.z,
                    ReadGuestFloat(cs + kCsYaw), ReadGuestFloat(cs + kCsPitch));
        }
    }
}

// ── The DEBUG CAMERA rows ──────────────────────────────────────────────
//
// The camera's pause-menu rows live here rather than in pause_menu.cpp, so the
// cvar, the code that reads it and the row that drives it are all one file
// apart. pause_menu.cpp appends whatever this returns to the end of its own
// DEBUG CAMERA tab.
//
// Only the rows worth a controller are here. cam_look_phase, the probe
// buttons, cam_freelook_tune and cam_freelook_inplace_rigs are diagnostics or
// dead experiments and stay console-only; a menu that lists everything is a
// menu nobody reads.
namespace {

// Mouse sensitivity, in radians per pixel. The default is 0.0035.
constexpr double kLookSensVals[] = {0.0015, 0.0025, 0.0035, 0.005, 0.007, 0.01};

// Seconds of a still mouse before the view eases back to centre. 0 means it
// starts returning the moment the mouse stops, which reads as the view
// fighting the player, so it renders as OFF rather than as a bare "0".
constexpr double kLookDelayVals[] = {0.0, 0.15, 0.35, 0.6, 1.0, 2.0};

// How fast it eases back once it starts, in units of 1/second.
constexpr double kLookRateVals[] = {1.0, 2.0, 4.0, 8.0, 16.0};

constexpr mcla_menu::ItemDef kCameraLookRows[] = {
    mcla_menu::Bool("PM_RxFreeLook",   "cam_freelook",              "FREE LOOK: "),
    mcla_menu::Dbl ("PM_RxFlSens",     "cam_freelook_sens",         "LOOK SENS: ",
                    kLookSensVals,
                    int(sizeof(kLookSensVals) / sizeof(kLookSensVals[0])), "%g"),
    mcla_menu::Bool("PM_RxFlInvX",     "cam_freelook_invert_x",     "INVERT LOOK X: "),
    mcla_menu::Bool("PM_RxFlInvY",     "cam_freelook_invert_y",     "INVERT LOOK Y: "),
    mcla_menu::Dbl ("PM_RxFlDelay",    "cam_freelook_return_delay", "RECENTER DELAY: ",
                    kLookDelayVals,
                    int(sizeof(kLookDelayVals) / sizeof(kLookDelayVals[0])),
                    "%gS", "", "OFF"),
    mcla_menu::Dbl ("PM_RxFlRate",     "cam_freelook_return_rate",  "RECENTER SPEED: ",
                    kLookRateVals,
                    int(sizeof(kLookRateVals) / sizeof(kLookRateVals[0])), "%gX"),
    // Turns the camera at the camera state instead of at the finished matrix,
    // so the game's own collision resolves it -- and the flip below covers the
    // one thing the maths could not settle. Both explained on the cvars.
    mcla_menu::Bool("PM_RxFlCollide",  "cam_freelook_collide",      "CAMERA COLLISION: "),
    mcla_menu::Bool("PM_RxFlCollFlip", "cam_freelook_cs_invert",    "COLLISION FLIP: "),
};

}  // namespace

const mcla_menu::ItemDef* CameraLookMenuItems(int& count) {
    count = int(sizeof(kCameraLookRows) / sizeof(kCameraLookRows[0]));
    return kCameraLookRows;
}

void CameraProbeMark() {
    uint32_t sys = g_cam_sys.load(std::memory_order_relaxed);
    if (!sys) {
        MC_WARN("[cam-probe] the camera system tick has not run. sub_822C0320 "
                "is what debug_cam works from, so if this never appears the "
                "hook is not being reached at all and something is wrong with "
                "the hook rather than with the guess.");
        return;
    }

    BuildRegions(sys);
    if (!ReadRegions(g_baseline)) {
        MC_WARN("[cam-probe] nothing readable at 0x{:08X}", sys);
        return;
    }
    g_probe_sys = sys;
    g_probe_rig = g_cam_rig.load(std::memory_order_relaxed);
    MC_INFO("[cam-probe] baseline: camsys=0x{:08X} rig={} regions={} floats={}",
            sys, g_probe_rig, g_regions.size(), g_baseline.size());
}

void CameraProbeDump() {
    if (g_baseline.empty() || g_regions.empty()) {
        MC_WARN("[cam-probe] no baseline -- use cam_probe_mark first.");
        return;
    }
    uint32_t sys = g_cam_sys.load(std::memory_order_relaxed);
    if (sys != g_probe_sys) {
        MC_WARN("[cam-probe] the camera system changed between mark and dump "
                "(0x{:08X} -> 0x{:08X}); the diff would be meaningless.",
                g_probe_sys, sys);
        return;
    }

    // Deliberately re-reads the SAME regions rather than rebuilding them: a
    // pointer that moved would shift every index and the diff would be noise.
    std::vector<float> now;
    if (!ReadRegions(now) || now.size() != g_baseline.size()) {
        MC_WARN("[cam-probe] the regions no longer read back the same shape.");
        return;
    }

    std::error_code ec;
    std::ofstream out(std::filesystem::current_path(ec) / "cam_fields.txt");
    if (!out) {
        MC_ERROR("[cam-probe] cannot open cam_fields.txt");
        return;
    }

    int32_t rig = g_cam_rig.load(std::memory_order_relaxed);
    out << "camera system 0x" << std::hex << sys << std::dec << "\n"
        << "rig (r24 in sub_822C0320): " << g_probe_rig << " -> " << rig
        << (g_probe_rig != rig
                ? "   *** CHANGED: looking around swaps the whole camera rig ***"
                : "   (unchanged)")
        << "\n\nregions:\n";
    for (const auto& r : g_regions) {
        out << "  0x" << std::hex << r.base << std::dec << "  " << r.count
            << " floats  ";
        if (r.is_root) out << "camera system itself";
        else if (r.from_offset < 0x10000u) out << "from camsys+" << r.from_offset;
        else out << "from 0x" << std::hex << r.from_offset << std::dec;
        // Three unit-length rows at +96 is what a camera basis looks like, and
        // it is the only way to pick the cameras out of a pile of pointers
        // without knowing a single one of their classes.
        int32_t mat = FindCameraMatrix(r.base, r.count * 4);
        if (mat >= 0) out << "   *** CAMERA MATRIX at +" << mat << " ***";
        out << "\n";
    }
    out << "\nregion          offset  before          after           delta\n";

    uint32_t changed = 0;
    size_t at = 0;
    for (const auto& r : g_regions) {
        for (uint32_t i = 0; i < r.count; ++i, ++at) {
            float a = g_baseline[at];
            float b = now[at];
            if (a == b) continue;
            float d = b - a;
            if (std::fabs(d) < 1.0e-5f) continue;
            if (!std::isfinite(a) || !std::isfinite(b)) continue;
            if (std::fabs(a) > 1.0e9f || std::fabs(b) > 1.0e9f) continue;

            out << "0x" << std::hex << r.base << std::dec << "\t+" << (i * 4)
                << "\t" << a << "\t" << b << "\t" << d;
            if (r.is_root) out << "\t(camera system)";
            else if (r.from_offset < 0x10000u)
                out << "\t(from camsys+" << r.from_offset << ")";
            else
                out << "\t(from 0x" << std::hex << r.from_offset << std::dec << ")";
            out << "\n";
            ++changed;
        }
    }

    out << "\n" << changed << " fields moved out of " << now.size() << ".\n";
    MC_INFO("[cam-probe] wrote cam_fields.txt: {} of {} floats moved, rig {} -> {}",
            changed, now.size(), g_probe_rig, rig);
}

#else  // REXGLUE_HAS_XEO3_TARGET

void CameraLookOnTuneField(const char*, uint32_t, uint32_t) {}
void TickCameraLook() {}
void Hook_CameraSystem(PPCRegister&, PPCRegister&) {}
void Hook_CameraPostUpdate(PPCRegister&) {}
void Hook_CameraPrePublish(PPCRegister&) {}
void Hook_CameraMatrixPublish(PPCRegister&) {}
void Hook_CameraStateUpdate(PPCRegister&) {}
void CameraProbeMark() {}
void CameraProbeDump() {}
void CameraProbeRun() {}

const mcla_menu::ItemDef* CameraLookMenuItems(int& count) {
    count = 0;
    return nullptr;
}

#endif  // REXGLUE_HAS_XEO3_TARGET
