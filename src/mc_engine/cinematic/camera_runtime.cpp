#ifndef REXGLUE_HAS_XEO3_TARGET
//
// The scene camera in the game (trailer toolkit, work package M1b; feature-parity/CINEMATIC-DESIGN.md section 3). The keys, the
// rig and the maths are in camera.h; what is here is the part that touches the game.
//
// WHERE. Hook_MenuCameraFinal (menu_camera.cpp, 0x822C0868) is the one place every camera mode's result passes, one instruction
// before sub_822C0320 copies it to the renderer: the gameplay chase and free-fly cameras, the menu, the cutscene and the replay
// all end there. E5 put a scripted camera there in every mode and the renderer followed it (the temporal system's camera, the
// shadows and LOD at the frustum edge, field of view 30 to 110 degrees and roll to 30 degrees); E3 flew it 600 m from the car and
// the streamer followed the camera by itself (the sector under the camera loaded, City_DoneStreaming true), so nothing here moves
// the car or locks the streaming. The slots (sub_822C0320's frame, r1): vertical field of view in degrees at +0x50, far +0x54,
// near +0x58, a cutscene's horizontal field of view +0x5C (-1.0 = none), rows right, up, forward at +0x60, +0x70, +0x80 and the
// position at +0x90.
//
// WHEN. The runner calls `apply` once an update (Patch_DeltaTimePre, before the game's update) with the scene time; that only stores
// the time. The rig is evaluated in the hook, so that a car the camera is attached to has had this update's physics: a camera that
// reads its car before the physics lags it by a frame, which at 80 m/s is more than a metre of jitter.
//
// CUTS. The frame a shot starts on (a `cut` key, the take-over, a restart) is announced to the renderer with the camera the hook
// just wrote (gpu/cut_request.h): the first frame whose motion blur draw has that camera starts the temporal upscalers' history and
// the occlusion's history again, and has its blur constants made neutral. The hand-back (`cam <T> off`, the scene's end) is a cut
// as well: it is announced with the game's own camera as it comes out of the camera code.
//
// STALE. The hook only overrides while the runner is ticking (the scene time was stored in the last two game frames): in a menu, the
// front end or a loading screen the runner does not tick, and the game's camera is left alone; when it ticks again the rig starts
// over (a take-over, a cut). A paused game keeps its own camera too, so the pause menu's view works.
//
// Cvar (debug only, live):
//   mcla_cam_log    log the scene's camera every this many frames ([cam] lines); 0: only the cuts.
//
// Written without a build or a run; CHECK where nothing has shown it in a running game (listed in documentation/DEBUG.md).
//

#include "camera_runtime.h"

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "camera.h"
#include "larecomp_log.h"
#include "scene_runtime.h"

#if defined(LARECOMP_HAS_GPU)
#include "gpu/cut_request.h"
#endif

REXCVAR_DEFINE_UINT32(mcla_cam_log, 0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: while a scene's camera is running, log it every this many frames: scene time, position, direction, field of "
    "view. 0: only the cuts.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

namespace mc::cinematic {

namespace {

namespace cam = mc::cinematic::camera;

// sub_822C0320's stack slots at loc_822C0868 (menu_camera.cpp has the same list).
constexpr uint32_t kSlotFov = 0x50;
constexpr uint32_t kSlotFar = 0x54;
constexpr uint32_t kSlotNear = 0x58;
constexpr uint32_t kSlotHFov = 0x5C;  // a cutscene's horizontal field of view; -1.0 = none
constexpr uint32_t kSlotRow0 = 0x60;
constexpr uint32_t kSlotRow1 = 0x70;
constexpr uint32_t kSlotRow2 = 0x80;
constexpr uint32_t kSlotPos = 0x90;

constexpr uint32_t kClock = 0x827D7500;        // the engine timer; +0x2C the frame counter (cin_camtest.cpp of test/cin-e5)
constexpr uint32_t kPauseStatePtr = 0x828CCEA0;  // +4 paused, +10 run-one-frame (map_warp.cpp, cutscene_gallery.cpp)
constexpr uint32_t kPlayerMgrPtr = 0x82874374;
constexpr uint32_t kPlayerValidFn = 0x821E80F8;  // (playerMgr, index) -> bool
constexpr uint32_t kPlayerRacerFn = 0x822A3998;  // (playerMgr, index) -> racer
constexpr uint32_t kPlayerSweep = 4;
constexpr uint32_t kRacerEntityAt = 48;   // racer -> entity
constexpr uint32_t kEntityMatrixAt = 28;  // entity -> matrix object; its rows start at +16
constexpr uint32_t kStaleFrames = 2;      // the runner has not ticked for more than this many game frames: it is not running

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

uint8_t* Membase() {
    auto* rt = rex::Runtime::instance();
    return rt ? rt->virtual_membase() : nullptr;
}

PPCFunc* GuestFn(uint32_t addr) {
    auto* rt = rex::Runtime::instance();
    if (!rt || !rt->function_dispatcher()) return nullptr;
    return rt->function_dispatcher()->GetFunction(addr);
}

bool GamePaused(const uint8_t* base) {
    const uint32_t p = R32(base, kPauseStatePtr);
    return p != 0 && base[p + 4] != 0 && base[p + 10] == 0;  // the test map_warp.cpp uses
}

// The player's racer (0 in a menu, the garage, a loading screen).
uint32_t PlayerRacer(const uint8_t* base) {
    const uint32_t mgr = R32(base, kPlayerMgrPtr);
    PPCFunc* valid_fn = GuestFn(kPlayerValidFn);
    PPCFunc* racer_fn = GuestFn(kPlayerRacerFn);
    if (!mgr || !valid_fn || !racer_fn) return 0;
    for (uint32_t i = 0; i < kPlayerSweep; ++i) {
        if (!(rex::ppc::GuestToHostFunction<uint32_t>(valid_fn, mgr, i) & 0xFF)) continue;
        if (const uint32_t r = rex::ppc::GuestToHostFunction<uint32_t>(racer_fn, mgr, i)) return r;
    }
    return 0;
}

// The car's live matrix (rows right, up, back, position), or false. CHECK: the rows are the entity's world matrix as the draw reads
// it (E5 read the position and the facing of the player's car this way and the log agreed with the state log's car pose).
bool CarMatrix(const uint8_t* base, uint32_t racer, float out[16]) {
    const uint32_t ent = R32(base, racer + kRacerEntityAt);
    const uint32_t mtx = ent ? R32(base, ent + kEntityMatrixAt) : 0;
    if (!mtx) return false;
    for (int i = 0; i < 16; ++i) out[i] = RF(base, mtx + 16 + 4 * static_cast<uint32_t>(i));
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

CarResolver g_resolver = nullptr;

// The cars a scene's `car:<id>` names: the player's own, and what the car package resolves.
class GuestCars final : public cam::CarSource {
  public:
    explicit GuestCars(const uint8_t* base) : base_(base) {}
    bool Get(const std::string& id, cam::CarPose* out) override {
        float m[16];
        if (id == "player") {
            const uint32_t racer = PlayerRacer(base_);
            if (!racer || !CarMatrix(base_, racer, m)) return false;
        } else if (g_resolver) {
            if (!g_resolver(id.c_str(), m)) return false;
        } else {
            return false;
        }
        out->position = {m[12], m[13], m[14]};
        out->frame.right = {m[0], m[1], m[2]};
        out->frame.up = {m[4], m[5], m[6]};
        out->frame.forward = {-m[8], -m[9], -m[10]};  // the third row is the car's BACK
        return true;
    }

  private:
    const uint8_t* base_;
};

struct State {
    bool running = false;      // a scene with a camera track is running (begin .. end)
    bool track_ok = false;
    cam::CameraTrack track;
    cam::Rig rig;
    std::string scene_name;

    double t = 0.0;            // the scene time of the last apply
    uint64_t t_serial = 0;     // applies so far
    uint32_t apply_frame = 0;  // the game's frame counter at the last apply
    uint64_t eval_serial = 0;  // the apply the cached view is of
    cam::View view;
    bool cut_pending = false;  // the cached view is a cut that has not been announced yet
    bool stale = true;         // the runner is not ticking (true until the first apply: the rig starts over when it does)
    bool was_applied = false;  // the camera was written in the last hook run
    bool handback_pending = false;  // the scene ended while the camera was ours: announce the game's camera as a cut
    uint32_t frames = 0;
    uint32_t cuts = 0;
    uint32_t skipped_pause = 0;
    std::string missing_logged;
};
State g;

void AnnounceCut(const uint8_t* base, uint32_t sp, const char* why, double t) {
    const uint32_t frame = R32(base, kClock + 0x2C);
    ++g.cuts;
    const float px = RF(base, sp + kSlotPos + 0), py = RF(base, sp + kSlotPos + 4), pz = RF(base, sp + kSlotPos + 8);
    LARECOMP_APP_INFO("[cam] cut #{} ({}) at game frame {}, scene t={:.2f}: the camera is at {:.2f} {:.2f} {:.2f}, looking along {:.3f} {:.3f} {:.3f}",
                      g.cuts, why, frame, t, px, py, pz, RF(base, sp + kSlotRow2 + 0), RF(base, sp + kSlotRow2 + 4),
                      RF(base, sp + kSlotRow2 + 8));
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::cut::CameraPose pose;
    pose.position[0] = px;
    pose.position[1] = py;
    pose.position[2] = pz;
    for (int c = 0; c < 3; ++c) {
        pose.up[c] = RF(base, sp + kSlotRow1 + 4 * static_cast<uint32_t>(c));
        pose.forward[c] = RF(base, sp + kSlotRow2 + 4 * static_cast<uint32_t>(c));
    }
    mcla::gpu::cut::Submit(frame, pose);
#else
    (void)frame;
#endif
}

void WriteCamera(uint8_t* base, uint32_t sp, const cam::View& v) {
    const auto put = [&](uint32_t off, cam::Vec3 a, float w) {
        WF(base, sp + off + 0, static_cast<float>(a.x));
        WF(base, sp + off + 4, static_cast<float>(a.y));
        WF(base, sp + off + 8, static_cast<float>(a.z));
        WF(base, sp + off + 12, w);
    };
    put(kSlotRow0, v.basis.right, 0.0f);
    put(kSlotRow1, v.basis.up, 0.0f);
    put(kSlotRow2, v.basis.forward, 0.0f);
    put(kSlotPos, v.position, 1.0f);
    if (v.fov > 0.0) {
        WF(base, sp + kSlotFov, static_cast<float>(v.fov));
        WF(base, sp + kSlotHFov, -1.0f);  // not a cutscene's horizontal lens: the vertical one of +0x50 (E5)
    }
    if (v.near_plane > 0.0) WF(base, sp + kSlotNear, static_cast<float>(v.near_plane));
    if (v.far_plane > 0.0) WF(base, sp + kSlotFar, static_cast<float>(v.far_plane));
}

// ---------------------------------------------------------------- the applier

void ApplierBegin(const Scene& scene) {
    g = State{};
    cam::BuildResult r = cam::BuildTrack(scene);
    for (const Diagnostic& d : r.diagnostics) {
        const std::string line = FormatDiagnostic(d, "camera");
        if (d.severity == Severity::kError) LARECOMP_APP_ERROR("[cam] {}", line);
        else LARECOMP_APP_WARN("[cam] {}", line);
    }
    if (r.track.Empty() && r.Ok()) return;  // no camera keys: nothing to do and nothing to say
    if (!r.Ok()) {
        LARECOMP_APP_ERROR("[cam] {} error(s) in the camera keys: the scene's camera is not applied (the game's own stays)", r.ErrorCount());
        return;
    }
    g.track = std::move(r.track);
    g.track_ok = true;
    g.running = true;
    g.scene_name = scene.name;
    LARECOMP_APP_INFO("[cam] {}", cam::Summarize(g.track));
}

void ApplierApply(const Scene&, double t) {
    if (!g.running) return;
    uint8_t* base = Membase();
    g.t = t;
    ++g.t_serial;
    g.apply_frame = base ? R32(base, kClock + 0x2C) : 0;
}

void ApplierEnd() {
    if (g.was_applied) {
        g.handback_pending = true;  // the next hook run announces the game's own camera as a cut
        LARECOMP_APP_INFO("[cam] the scene's camera is handed back ({} frames, {} cuts, {} frames of pause left to the game's own camera)", g.frames,
                          g.cuts, g.skipped_pause);
    }
    g.running = false;
    g.track_ok = false;
    g.was_applied = false;
}

const TrackApplier kCameraApplier = {
    "camera",
    TrackBit(Track::kCamera) | TrackBit(Track::kCut),
    &ApplierBegin,
    &ApplierApply,
    &ApplierEnd,
};

// The applier is registered when the program starts; the runner's own tables are constant-initialised, so the order of the
// translation units does not matter.
struct Registrar {
    Registrar() {
        if (!RegisterTrackApplier(&kCameraApplier)) {
            // no way to log yet: the camera track would be reported as "keys nobody applies" by the runner
        }
    }
} g_registrar;

}  // namespace

// ---------------------------------------------------------------- the hook

bool CinematicCameraApply(uint8_t* base, uint32_t sp, uint32_t mode) {
    if (!g.running && !g.handback_pending) return false;  // the one flag test
    if (!base) return false;

    if (g.handback_pending) {
        g.handback_pending = false;
        // The camera the game made, as it comes out of its own code: the first frame it is back is a cut.
        AnnounceCut(base, sp, "the scene's camera is handed back", g.t);
        return false;
    }
    if (!g.track_ok) return false;

    // The runner has to be ticking: not in a menu, the front end or a loading screen.
    const uint32_t frame = R32(base, kClock + 0x2C);
    if (frame - g.apply_frame > kStaleFrames) {
        if (!g.stale) {
            g.stale = true;
            g.rig.Reset();  // the rig starts over when the runner ticks again: a take-over, and a cut
            g.eval_serial = g.t_serial;
            g.view = cam::View{};
            g.cut_pending = false;
            if (g.was_applied) {
                g.was_applied = false;
                LARECOMP_APP_INFO("[cam] the scene is not ticking (a menu or a loading screen): the game's camera is back");
            }
        }
        return false;
    }
    if (g.stale) {
        g.stale = false;
        if (g.frames > 0) LARECOMP_APP_INFO("[cam] the scene ticks again");
    }
    if (GamePaused(base)) {
        ++g.skipped_pause;
        return false;  // the pause menu's own view
    }

    if (g.eval_serial != g.t_serial) {
        GuestCars cars(base);
        g.view = g.rig.Evaluate(g.track, g.t, cars);
        g.eval_serial = g.t_serial;
        if (g.view.cut) g.cut_pending = true;
        if (g.view.missing_cars > 0) {
            for (const std::string& id : g.rig.MissingCars()) {
                if (g.missing_logged.find("|" + id + "|") == std::string::npos) {
                    g.missing_logged += "|" + id + "|";
                    LARECOMP_APP_WARN("[cam] t={:.2f}: car '{}' does not exist (yet): the camera holds that car's last position, or the origin", g.t,
                                      id);
                }
            }
        }
    }
    const cam::View& v = g.view;
    if (!v.active) {
        if (g.was_applied) {
            g.was_applied = false;
            // `cam <T> off`: the game's camera is back from this frame.
            AnnounceCut(base, sp, "cam off: the game's camera is back", g.t);
            g.cut_pending = false;
        }
        return false;
    }

    WriteCamera(base, sp, v);
    g.was_applied = true;
    ++g.frames;
    if (g.cut_pending) {
        g.cut_pending = false;
        AnnounceCut(base, sp, v.took_over ? "the scene's camera takes over" : "a cut of the scene", g.t);
    }
    const uint32_t every = REXCVAR_GET(mcla_cam_log);
    if (every != 0 && g.frames % every == 0) {
        LARECOMP_APP_INFO("[cam] t={:.3f} mode {} shot {}: at {:.3f} {:.3f} {:.3f}, looking along {:.4f} {:.4f} {:.4f}, up {:.4f} {:.4f} {:.4f}, fov {:.3f}", g.t,
                          mode, v.shot, v.position.x, v.position.y, v.position.z, v.basis.forward.x, v.basis.forward.y, v.basis.forward.z,
                          v.basis.up.x, v.basis.up.y, v.basis.up.z, v.fov);
    }
    return true;
}

bool CinematicCameraActive() {
    return g.running && g.was_applied;
}

void SetCarResolver(CarResolver resolver) {
    g_resolver = resolver;
}

}  // namespace mc::cinematic

#else  // REXGLUE_HAS_XEO3_TARGET

#include "camera_runtime.h"

namespace mc::cinematic {
bool CinematicCameraApply(uint8_t*, uint32_t, uint32_t) { return false; }
bool CinematicCameraActive() { return false; }
void SetCarResolver(CarResolver) {}
}  // namespace mc::cinematic

#endif  // REXGLUE_HAS_XEO3_TARGET
