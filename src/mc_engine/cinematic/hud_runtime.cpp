// The scene's `hud` keys on the game's own UI (trailer toolkit, work package M1c on the scene runner of M1a): the TrackApplier
// that reads the keys of a scene with HudKey's parser (hud_track.h), asks the track for the alpha at the scene's time on every
// update and puts it on the screen with SetHudAlpha (hud.h). Written for the integration of the trailer toolkit
// (feature-parity/CIN-INTEGRATION.md): M1c made the pure track and the renderer's fade on a branch off main, which has no scene
// runner, so this glue is the one piece neither branch could hold.
//
// What it does, and when:
//   begin   (the scene starts or is loaded again) every `hud` key of the scene is parsed; a key that is not one is an error with its
//           line number and the whole HUD track is left out ("the scene's HUD is not applied"); with keys: ArmHud(true), so that the
//           renderer has made its pipeline and images before the first key.
//   apply   (every update while the scene runs) SetHudAlpha(track.AlphaAt(scene time), 0). Zero fade seconds: the track is a function
//           of scene time, which a capture steps by exactly one frame, and the renderer's own wall-clock ramp would run apart from
//           it. The first call takes the alpha the game had (hud.h keeps it for the end).
//   end     (the scene finishes, is replaced or turned off) ReleaseHud(0): the alpha goes back to what it was before the scene.
//
// A scene with no `hud` key costs one empty loop at begin and a flag test an update. Plume renderer only (the rest is a cvar the
// other renderers ignore); no game memory is read here.
//
// CHECK: written without a build or a run (clang -fsyntax-only clean with the build tree's flags). Not seen in a game: that the cvar
// written from the update thread is the alpha of the frame after it (the renderer reads it at its frame boundary), that inside a take
// the fade is one step a frame (perf\scripts\chain-cinint.ps1's hud-full / hud-half / hud-off pictures), and that a reload of the
// scene (end, then begin) leaves the alpha back at what it was and arms the layer again.

#include <string>

#include "hud.h"
#include "settings/immortal.h"
#include "hud_track.h"
#include "larecomp_log.h"
#include "scene_runtime.h"

namespace mc::cinematic {

namespace {

HudTrack& g_track = larecomp::Leak<HudTrack>();
bool g_active = false;

void ApplierBegin(const Scene& scene) {
    g_track.Clear();
    g_active = false;
    int errors = 0;
    for (const Event& e : scene.events) {
        if (e.track != Track::kHud) continue;
        HudKey key;
        std::string why;
        if (!ParseHudKey(e.words, e.t, &key, &why)) {
            LARECOMP_APP_ERROR("[hud] line {}: {}", e.line, why);
            ++errors;
            continue;
        }
        g_track.Add(key);
    }
    if (errors > 0) {
        LARECOMP_APP_ERROR("[hud] {} error(s) in the hud keys: the scene's HUD is not applied (the game's own UI stays)", errors);
        g_track.Clear();
        return;
    }
    if (g_track.empty()) return;  // no hud keys: nothing to do and nothing to say
    g_active = true;
    ArmHud(true);
    LARECOMP_APP_INFO("[hud] {} key(s), the last one arrives at {:.2f} s; alpha {:.2f} before the first", g_track.keys().size(), g_track.End(),
                      g_track.AlphaAt(-1.0));
}

void ApplierApply(const Scene&, double t) {
    if (!g_active) return;
    SetHudAlpha(g_track.AlphaAt(t), 0.0f);
}

void ApplierEnd() {
    if (!g_active) return;
    g_active = false;
    ReleaseHud(0.0f);
}

const TrackApplier kHudApplier = {
    "hud",
    TrackBit(Track::kHud),
    &ApplierBegin,
    &ApplierApply,
    &ApplierEnd,
};

// Registered when the program starts, like the camera's, the capture's and the cars' appliers: the runner's own tables are
// constant-initialised, so the order of the translation units does not matter.
struct Registrar {
    Registrar() {
        if (!RegisterTrackApplier(&kHudApplier)) {
            // no way to log yet: the runner would report the hud keys as "keys nobody applies"
        }
    }
} g_registrar;

}  // namespace

}  // namespace mc::cinematic
