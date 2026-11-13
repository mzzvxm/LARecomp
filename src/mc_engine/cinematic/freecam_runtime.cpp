#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The free camera and the flight recorder in the game (trailer toolkit, work package M1d; feature-parity/CINEMATIC-DESIGN.md section 3.5).
// The flight, the pad and the file are in freecam.h; what is here is what touches the game. Controls: freecam.h (and documentation/DEBUG.md).
//
// THE CAMERA. Hook_MenuCameraFinal (menu_camera.cpp, 0x822C0868) is the one place every camera mode's result passes, one instruction
// before sub_822C0320 hands it to the renderer; camera_runtime.cpp (M1b) puts a scene's camera there and this file puts the flight's.
// The flight comes first: a pilot who armed it wants it even while a scene's own camera track runs. The slots (sub_822C0320's frame, r1):
// vertical field of view in degrees at +0x50, far +0x54, near +0x58, a cutscene's horizontal field of view +0x5C (-1.0 = none), rows right,
// up and BACK at +0x60, +0x70, +0x80 and the position at +0x90. The third row is the camera's BACK, minus the way it looks (the game's chase
// camera at the car's spawn reads (0.474, 0.067, -0.878) there while it looks along (-0.474, -0.067, 0.878) at the car, and the renderer's
// anchor has the same rows as its c12 to c14): the flight reads and writes the rows through cam::FromSlotRows / ToSlotRows (freecam.h's
// PoseFromSlotRows / SlotRowsOf), like camera_runtime.cpp, and the game's world is right-handed (the right is forward x up). At the hook the
// slots hold the game's own camera, which is where the flight starts (and where Back + X brings it home). E3 flew a scripted camera 600 m from the car and the streamer followed the camera by
// itself, so nothing here moves the car or locks the streaming.
//
// ONCE AN UPDATE. The hook may run more than once in an update (CHECK); the flight steps only when the game's frame counter has moved
// (clock + 0x2C), by the engine timer's frame delta BEFORE its time scale (timer + 0x58, what a scene's clock adds): a slow-motion scene
// does not slow the camera, and a recording's time is what a scene's camera track measures. A paused game keeps its own camera (the pause
// menu's view), a camera mode 6 is the front end's shot and is left alone, so the menus are always reachable.
//
// THE PAD. The camera takes the pad away from the game while it flies (FreecamObservePad, called when the XamInputGetState wrapper ends):
// the state the game would have seen goes to the flight and the game's copy is cleared but for Start. A hook that has not run for more than
// two game frames (a loading screen) stops counting as flying, so the pad is the game's again.
//
// THE CUTS. A frame whose camera is not the one before it for a reason of the toolkit's (the camera goes home, the game's camera comes back
// after a pause or when the camera is switched off, and the flight's comes back after a pause) is announced to the renderer with the camera
// of that frame (gpu/cut_request.h), as the scene camera does: the temporal upscalers' history starts again and the motion blur does not
// smear the frame.
//
// THE FILE. mcla_freecam_out names it (the next recording writes there, replacing what is there); empty: freecam\flight-<date>-<time>.cam.csv
// next to the game. The rows are written as the flight goes and flushed every 30 samples.
//
// Cvars (all debug-only and live):
//   mcla_freecam           arms the free camera. Off gives the game's own camera back (and ends a recording).
//   mcla_freecam_record    records the flight: on starts a file, off ends it. Back + A flips it. It only records while the camera flies.
//   mcla_freecam_out       the file of the next recording.
//   mcla_freecam_speed     metres a second at a full stick.   mcla_freecam_look     degrees a second at a full right stick.
//   mcla_freecam_inertia   seconds the movement takes to settle (0 = none).
//   mcla_freecam_log       log the pose every this many frames ([fly] lines); 0: only the events.
//
// Written without a build or a run; CHECK where nothing has shown it in a running game (listed in documentation/DEBUG.md).
//

#include "freecam_runtime.h"

#include <rex/cvar.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include "camera.h"
#include "freecam.h"
#include "larecomp_log.h"
#include "scene_runtime.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/cut_request.h"
#endif

REXCVAR_DEFINE_BOOL(mcla_freecam, false, "MCLA/Cinematic",
    "TRAILER TOOLKIT: the free camera. The pad flies the camera (left stick moves, right stick looks, triggers up and down, LB and RB tilt, "
    "D-pad zooms and sets the speed, a stick click is fast or slow) and the game gets no pad input but Start, so the car does not drive. "
    "Back + A records the flight, Back + X is home. Off: the game's own camera.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_BOOL(mcla_freecam_record, false, "MCLA/Cinematic",
    "TRAILER TOOLKIT: record the free camera's flight into a raw track file (one sample a game update; ports\\scripts\\scene_cam.py turns it "
    "into a scene's camera track). On starts a file, off ends it. Back + A on the pad flips it. Only records while the free camera flies.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_STRING(mcla_freecam_out, "", "MCLA/Cinematic",
    "TRAILER TOOLKIT: the file the next free camera recording is written to (replaced when it exists). Empty: freecam\\flight-<date>-<time>.cam.csv "
    "next to the game.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(mcla_freecam_speed, 12.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: how fast the free camera flies at a full stick, in metres a second (L3 four times as fast, R3 a quarter, D-pad left and right "
    "step it).")
    .range(0.5, 500.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(mcla_freecam_look, 70.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: how fast the free camera turns at a full right stick, in degrees a second.")
    .range(5.0, 360.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(mcla_freecam_inertia, 0.25, "MCLA/Cinematic",
    "TRAILER TOOLKIT: how long the free camera takes to settle after the sticks move, in seconds (the turn and the tilt take half). 0: none.")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

REXCVAR_DEFINE_UINT32(mcla_freecam_log, 0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: while the free camera flies, log its pose every this many frames ([fly] lines). 0: only the events.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

namespace mc::cinematic {

namespace {

namespace cam = mc::cinematic::camera;
namespace fc = mc::cinematic::freecam;

// sub_822C0320's stack slots at loc_822C0868 (camera_runtime.cpp and menu_camera.cpp have the same list).
constexpr uint32_t kSlotFov = 0x50;
constexpr uint32_t kSlotHFov = 0x5C;
constexpr uint32_t kSlotRow0 = 0x60;
constexpr uint32_t kSlotRow1 = 0x70;
constexpr uint32_t kSlotRow2 = 0x80;
constexpr uint32_t kSlotPos = 0x90;

constexpr uint32_t kClock = 0x827D7500;           // the engine timer; +0x2C the frame counter
constexpr uint32_t kUnscaledDelta = 0x827D7558;   // timer + 0x58: the frame's delta before the time scale (scene_runtime.cpp)
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;   // +4 paused, +10 run-one-frame (map_warp.cpp, cutscene_gallery.cpp)
constexpr uint32_t kModeMenu = 6;                 // menu_camera.cpp: the front end's shots
constexpr uint32_t kStaleFrames = 2;
constexpr uint32_t kPadStaleFrames = 4;  // a pad nobody has polled for this long reads as at rest
constexpr uint32_t kFlushEvery = 30;

uint32_t R32(const uint8_t* base, uint32_t ea) {
    return (uint32_t(base[ea + 0]) << 24) | (uint32_t(base[ea + 1]) << 16) | (uint32_t(base[ea + 2]) << 8) | uint32_t(base[ea + 3]);
}

float RF(const uint8_t* base, uint32_t ea) {
    const uint32_t v = R32(base, ea);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void WF(uint8_t* base, uint32_t ea, float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof(v));
    base[ea + 0] = static_cast<uint8_t>(v >> 24);
    base[ea + 1] = static_cast<uint8_t>(v >> 16);
    base[ea + 2] = static_cast<uint8_t>(v >> 8);
    base[ea + 3] = static_cast<uint8_t>(v);
}

bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;  // the test map_warp.cpp uses
}

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// What the pad flying the camera and the XamInputGetState wrapper share.
std::atomic<bool> g_flying{false};
std::atomic<uint32_t> g_hook_frame{0};  // the game frame of the last hook run that flew
std::atomic<uint32_t> g_pad_frame{0};   // the game frame the shared pad was taken in
std::mutex g_pad_mutex;
fc::Pad g_pad;

struct State {
    bool armed = false;      // the cvar was on at the last hook
    bool seeded = false;     // the flight has a pose of its own (a pause does not lose it; switching the camera off does)
    bool flying = false;     // the camera was the flight's at the last hook
    fc::Flyer flyer;
    bool have_frame = false;
    uint32_t last_frame = 0;
    bool cut_flag = false;   // the next sample starts a new shot
    uint32_t frames = 0;

    // the recording
    bool recording = false;
    std::ofstream file;
    std::string path;
    std::string pending;
    uint32_t pending_rows = 0;
    double t = 0.0;
    fc::RawTrack record;
};
State g;

// A vector of the slots' frame (a row or the position).
cam::Vec3 SlotVec(const uint8_t* base, uint32_t sp, uint32_t off) {
    return {RF(base, sp + off + 0), RF(base, sp + off + 4), RF(base, sp + off + 8)};
}

// The camera rows of sub_822C0320's frame as they are now: right, up and BACK (not forward: see the top of this file).
cam::SlotRows ReadRows(const uint8_t* base, uint32_t sp) {
    return {SlotVec(base, sp, kSlotRow0), SlotVec(base, sp, kSlotRow1), SlotVec(base, sp, kSlotRow2)};
}

void AnnounceCut(const uint8_t* base, uint32_t sp, const char* why) {
    const uint32_t frame = R32(base, kClock + 0x2C);
    const cam::Vec3 at = SlotVec(base, sp, kSlotPos);
    const cam::Basis shot = cam::FromSlotRows(ReadRows(base, sp));  // forward is minus the third row: where the camera looks
    LARECOMP_APP_INFO("[fly] cut ({}) at game frame {}: the camera is at {:.2f} {:.2f} {:.2f}, looking along {:.3f} {:.3f} {:.3f}", why, frame, at.x, at.y, at.z,
                      shot.forward.x, shot.forward.y, shot.forward.z);
#if defined(LARECOMP_HAS_GPU)
    // The pose is what the renderer's anchor will read: its forward is minus the third row (the camera's back), see cut_request.h.
    mcla::gpu::cut::CameraPose pose;
    pose.position[0] = at.x;
    pose.position[1] = at.y;
    pose.position[2] = at.z;
    pose.up[0] = shot.up.x;
    pose.up[1] = shot.up.y;
    pose.up[2] = shot.up.z;
    pose.forward[0] = shot.forward.x;
    pose.forward[1] = shot.forward.y;
    pose.forward[2] = shot.forward.z;
    mcla::gpu::cut::Submit(frame, pose);
#else
    (void)frame;
#endif
}

// The game's own camera as it stands in the slots.
fc::Pose GameCamera(const uint8_t* base, uint32_t sp, double keep_yaw) {
    const cam::SlotRows rows = ReadRows(base, sp);
    const cam::Vec3 pos = SlotVec(base, sp, kSlotPos);
    double fov = RF(base, sp + kSlotFov);
    const bool sane = std::isfinite(fov) && fov > 5.0 && fov < 150.0 && std::isfinite(pos.x + pos.y + pos.z) && cam::Length(rows.back) > 0.5 &&
                      cam::Length(rows.right) > 0.5 && cam::Length(rows.up) > 0.5;
    if (!sane) {
        fc::Pose p;
        p.yaw = keep_yaw;
        return p;
    }
    return fc::PoseFromSlotRows(pos, rows, fov, keep_yaw);
}

void WriteCamera(uint8_t* base, uint32_t sp, const fc::Pose& pose) {
    const cam::SlotRows rows = fc::SlotRowsOf(pose);  // right, up and BACK (minus forward)
    const auto put = [&](uint32_t off, cam::Vec3 a, float w) {
        WF(base, sp + off + 0, static_cast<float>(a.x));
        WF(base, sp + off + 4, static_cast<float>(a.y));
        WF(base, sp + off + 8, static_cast<float>(a.z));
        WF(base, sp + off + 12, w);
    };
    put(kSlotRow0, rows.right, 0.0f);
    put(kSlotRow1, rows.up, 0.0f);
    put(kSlotRow2, rows.back, 0.0f);
    put(kSlotPos, pose.position, 1.0f);
    WF(base, sp + kSlotFov, static_cast<float>(pose.fov));
    WF(base, sp + kSlotHFov, -1.0f);  // not a cutscene's horizontal lens: the vertical one of +0x50 (E5)
}

fc::Params ParamsFromCvars() {
    fc::Params p;
    p.speed = REXCVAR_GET(mcla_freecam_speed);
    p.look = REXCVAR_GET(mcla_freecam_look);
    p.inertia = REXCVAR_GET(mcla_freecam_inertia);
    return p;
}

// ---------------------------------------------------------------- the recording

std::string TimeStamp(const char* format) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    return std::strftime(buf, sizeof(buf), format, &tm) ? buf : "";
}

void FlushPending() {
    if (!g.file.is_open() || g.pending.empty()) return;
    g.file.write(g.pending.data(), static_cast<std::streamsize>(g.pending.size()));
    g.file.flush();
    g.pending.clear();
    g.pending_rows = 0;
}

void StopRecording(const char* why) {
    if (!g.recording) return;
    FlushPending();
    g.file.close();
    g.recording = false;
    const fc::Stats st = fc::Summarize(g.record);
    LARECOMP_APP_INFO("[fly] recording ended ({}): {} samples over {:.2f} s, {:.1f} m flown, fastest {:.1f} m/s, {} cut mark(s): {}", why, st.samples, st.duration,
                      st.distance, st.max_speed, st.cuts, g.path);
    LARECOMP_APP_INFO("[fly] to make a scene's camera track of it: python -I ports\\scripts\\scene_cam.py convert {} -o flight.scene", g.path);
    g.record.samples.clear();
    g.pending.clear();
    if (REXCVAR_GET(mcla_freecam_record)) rex::cvar::SetFlagByName("mcla_freecam_record", "false");
}

void StartRecording(const uint8_t* base) {
    std::string path = REXCVAR_GET(mcla_freecam_out);
    if (path.empty()) path = "freecam/flight-" + TimeStamp("%Y%m%d-%H%M%S") + ".cam.csv";
    std::error_code ec;
    const std::filesystem::path fs_path = PathFromUtf8(path);
    if (fs_path.has_parent_path()) std::filesystem::create_directories(fs_path.parent_path(), ec);
    g.file.open(fs_path, std::ios::binary | std::ios::trunc);
    if (!g.file) {
        LARECOMP_APP_ERROR("[fly] cannot write {}: no recording", path);
        rex::cvar::SetFlagByName("mcla_freecam_record", "false");
        return;
    }
    g.path = path;
    g.recording = true;
    g.t = 0.0;
    g.pending.clear();
    g.pending_rows = 0;
    g.record = fc::RawTrack{};
    const fc::Pose& p = g.flyer.pose();
    char seed[200];
    std::snprintf(seed, sizeof(seed), "seed %.3f %.3f %.3f yaw %.3f pitch %.3f roll %.3f fov %.3f", p.position.x, p.position.y, p.position.z, p.yaw, p.pitch, p.roll,
                  p.fov);
    const std::vector<std::string> notes = {
        "started " + TimeStamp("%Y-%m-%d %H:%M:%S"),
        "game frame " + std::to_string(R32(base, kClock + 0x2C)),
        std::string("scene ") + SceneStateName(CurrentSceneState()),
        seed,
    };
    g.pending = fc::RawHeader(notes);
    FlushPending();
    LARECOMP_APP_INFO("[fly] recording to {}", path);
}

void AddSample(const uint8_t* base, double dt) {
    if (!g.record.samples.empty()) g.t += dt;
    fc::Sample s;
    s.t = g.t;
    s.scene_t = CurrentSceneState() == SceneState::kRunning ? SceneTime() : -1.0;
    s.frame = R32(base, kClock + 0x2C);
    s.pose = g.flyer.pose();
    s.flags = g.cut_flag ? fc::kFlagCut : 0u;
    g.cut_flag = false;
    g.record.samples.push_back(s);
    g.pending += fc::RawRow(s);
    if (++g.pending_rows >= kFlushEvery) FlushPending();
}

// Ends everything the flight had going (the camera switched off, or the game gone away from a place where it can fly).
void Disengage(uint8_t* base, uint32_t sp, const char* why, bool forget_pose) {
    if (g.flying) {
        g.flying = false;
        g_flying.store(false, std::memory_order_relaxed);
        AnnounceCut(base, sp, why);  // the slots still hold the game's own camera: it comes back this frame
    }
    if (forget_pose) {
        g.seeded = false;
        g.have_frame = false;
    }
}

}  // namespace

bool FreecamApply(uint8_t* base, uint32_t sp, uint32_t mode) {
    const bool armed = REXCVAR_GET(mcla_freecam);
    if (!armed && !g.armed && !g.flying) return false;  // the one flag test
    if (!base) return false;

    if (!armed) {
        if (g.armed) LARECOMP_APP_INFO("[fly] the free camera is off: the game's camera is back");
        g.armed = false;
        StopRecording("the camera is switched off");
        Disengage(base, sp, "the game's camera is back", true);
        return false;
    }
    if (!g.armed) {
        g.armed = true;
        LARECOMP_APP_INFO("[fly] the free camera is on: left stick moves, right stick looks, triggers up and down, LB and RB tilt, D-pad zooms and sets the speed, "
                          "L3 fast, R3 slow, Y levels, Back + A records, Back + X goes home; the game gets no pad input but Start (pause)");
    }
    if (mode == kModeMenu) {  // the front end's shots: the menus must stay usable
        Disengage(base, sp, "the front end's camera", false);
        return false;
    }
    if (GamePaused(base)) {
        Disengage(base, sp, "paused: the game's own view", false);
        return false;
    }

    const uint32_t frame = R32(base, kClock + 0x2C);
    const fc::Params params = ParamsFromCvars();
    bool announce_back = false;
    if (!g.seeded) {
        g.flyer.Seed(GameCamera(base, sp, 0.0));
        g.seeded = true;
        const fc::Pose& p = g.flyer.pose();
        LARECOMP_APP_INFO("[fly] seeded from the game's camera at {:.2f} {:.2f} {:.2f}, yaw {:.1f} pitch {:.1f} roll {:.1f} fov {:.1f}", p.position.x, p.position.y,
                          p.position.z, p.yaw, p.pitch, p.roll, p.fov);
    } else if (!g.flying) {
        announce_back = true;  // out of a pause or the front end: the flight's camera comes back
    }
    g.flying = true;
    g_hook_frame.store(frame, std::memory_order_relaxed);
    g_flying.store(true, std::memory_order_relaxed);

    if (!g.have_frame || frame != g.last_frame) {  // one step an update
        float dtf = RF(base, kUnscaledDelta);
        if (!std::isfinite(dtf) || dtf <= 0.0f || dtf > 0.25f) dtf = 1.0f / 60.0f;
        const double dt = dtf;
        fc::Pad pad;
        {
            std::lock_guard<std::mutex> lock(g_pad_mutex);
            if (frame - g_pad_frame.load(std::memory_order_relaxed) <= kPadStaleFrames) pad = g_pad;  // else at rest: no stick stuck on a stale read
        }
        const fc::Events ev = g.flyer.Step(pad, dt, params);
        g.have_frame = true;
        g.last_frame = frame;
        ++g.frames;
        if (ev.toggle_record) rex::cvar::SetFlagByName("mcla_freecam_record", REXCVAR_GET(mcla_freecam_record) ? "false" : "true");
        if (ev.home) {
            g.flyer.Seed(GameCamera(base, sp, g.flyer.pose().yaw));
            g.cut_flag = true;
            announce_back = true;
            LARECOMP_APP_INFO("[fly] home: the flight starts again from the game's camera");
        }
        if (ev.mark_cut) {
            g.cut_flag = true;
            LARECOMP_APP_INFO("[fly] a cut is marked here");
        }
        if (ev.reset_view) LARECOMP_APP_INFO("[fly] the horizon is level and the field of view is back to {:.1f}", g.flyer.pose().fov);
        const bool want = REXCVAR_GET(mcla_freecam_record);
        if (want && !g.recording) StartRecording(base);
        else if (!want && g.recording) StopRecording("stopped");
        if (g.recording) AddSample(base, dt);
        const uint32_t every = REXCVAR_GET(mcla_freecam_log);
        if (every != 0 && g.frames % every == 0) {
            const fc::Pose& p = g.flyer.pose();
            LARECOMP_APP_INFO("[fly] frame {}: at {:.2f} {:.2f} {:.2f}, yaw {:.2f} pitch {:.2f} roll {:.2f} fov {:.2f}, speed x{:.2f}{}", frame, p.position.x,
                              p.position.y, p.position.z, p.yaw, p.pitch, p.roll, p.fov, g.flyer.speed_multiplier(), g.recording ? ", recording" : "");
        }
    }

    WriteCamera(base, sp, g.flyer.pose());
    if (announce_back) AnnounceCut(base, sp, "the flight's camera");
    return true;
}

void FreecamObservePad(uint8_t* base, uint32_t user, uint32_t state) {
    if (!g_flying.load(std::memory_order_relaxed)) return;
    if (!base || state == 0) return;
    const uint32_t u = user & 0xFF;
    if (u != 0 && u != 0xFF) return;
    // the camera's hook has not run for a while (a loading screen): the pad is the game's
    if (R32(base, kClock + 0x2C) - g_hook_frame.load(std::memory_order_relaxed) > kStaleFrames) {
        g_flying.store(false, std::memory_order_relaxed);
        return;
    }
    const fc::Pad pad = fc::PadFromGuest(base + state);
    {
        std::lock_guard<std::mutex> lock(g_pad_mutex);
        g_pad = pad;
        g_pad_frame.store(R32(base, kClock + 0x2C), std::memory_order_relaxed);
    }
    fc::ClearGuestPad(base + state);
}

bool FreecamActive() {
    return g_flying.load(std::memory_order_relaxed);
}

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "freecam_runtime.h"

namespace mc::cinematic {
bool FreecamApply(uint8_t*, uint32_t, uint32_t) { return false; }
void FreecamObservePad(uint8_t*, uint32_t, uint32_t) {}
bool FreecamActive() { return false; }
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
