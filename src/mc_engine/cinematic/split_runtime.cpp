// The scene's `split` keys on the renderer's before/after split (trailer toolkit, work package M5b on the scene runner of M1a): the
// TrackApplier that asks the scene's split track (scene.h EvaluateSplit) for the position at the scene's time on every update and puts
// it in the debug-only cvar mcla_gpu_split (src/gpu/split.h), which the renderer reads when it records the occlusion's apply and the
// present's dither.
//
//   begin   (the scene starts or is loaded again) the scene's split keys are counted; with none nothing else happens. With keys the
//           value of mcla_gpu_split is kept for the end.
//   apply   (every update while the scene runs) mcla_gpu_split = the track's position, or -1 (off) before the first key and after
//           `split off`. A value that has not changed is not written. The scene's time is stepped by exactly one frame in a
//           capture (M2a), so the split's position is a function of the frame, not of the wall clock.
//   end     (the scene finishes, is replaced or turned off) mcla_gpu_split goes back to what it was before the scene.
//
// Which effects the split cuts is the cvar mcla_gpu_split_effects (ao, dither or all); a scene sets it with an ordinary
// `set mcla_gpu_split_effects ao` line, which the runner puts back at the end like any setting.
//
// A scene with no `split` key costs one empty loop at begin and a flag test an update. The renderer is the Plume one (the cvar belongs to it);
// without it the keys are read and the log says nothing can show them. No game memory is read here.

#include <rex/cvar.h>

#include <cmath>
#include <string>

#include "larecomp_log.h"
#include "scene_runtime.h"

#if defined(LARECOMP_HAS_GPU)
REXCVAR_DECLARE(double, mcla_gpu_split);  // gpu/split.cpp
#endif

namespace mc::cinematic {

namespace {

bool g_active = false;
double g_before = -1.0;

double CvarValue() {
#if defined(LARECOMP_HAS_GPU)
    return REXCVAR_GET(mcla_gpu_split);
#else
    return -1.0;
#endif
}

void SetCvar(double value) {
#if defined(LARECOMP_HAS_GPU)
    REXCVAR_SET(mcla_gpu_split, value);
#else
    (void)value;
#endif
}

void ApplierBegin(const Scene& scene) {
    g_active = false;
    int keys = 0;
    for (const Event& e : scene.events) keys += e.track == Track::kSplit ? 1 : 0;
    if (keys == 0) return;
#if defined(LARECOMP_HAS_GPU)
    g_active = true;
    g_before = CvarValue();
    LARECOMP_APP_INFO("[split] {} key(s); mcla_gpu_split was {:.3f} and goes back to it when the scene ends", keys, g_before);
#else
    LARECOMP_APP_WARN("[split] {} key(s), but this build has no Plume renderer: nothing shows the split", keys);
#endif
}

void ApplierApply(const Scene& scene, double t) {
    if (!g_active) return;
    const SplitState s = EvaluateSplit(scene, t);
    const double want = s.set ? s.position : -1.0;
    if (CvarValue() != want) SetCvar(want);
}

void ApplierEnd() {
    if (!g_active) return;
    g_active = false;
    SetCvar(g_before);
    LARECOMP_APP_INFO("[split] the scene is over: mcla_gpu_split is back at {:.3f}", g_before);
}

const TrackApplier kSplitApplier = {
    "split",
    TrackBit(Track::kSplit),
    &ApplierBegin,
    &ApplierApply,
    &ApplierEnd,
};

// Registered when the program starts, like the other packages' appliers: the runner's own tables are constant-initialised, so the
// order of the translation units does not matter.
struct Registrar {
    Registrar() {
        if (!RegisterTrackApplier(&kSplitApplier)) {
            // no way to log yet: the runner would report the split keys as "keys nobody applies"
        }
    }
} g_registrar;

}  // namespace

}  // namespace mc::cinematic
