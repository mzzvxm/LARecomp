// The game's own UI on and off for a trailer: the cvar, and what a scene calls. See hud.h.

#include "hud.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(LARECOMP_HAS_GPU)
#include "gpu/gpu.h"
#endif

REXCVAR_DEFINE_DOUBLE(mcla_cin_hud_alpha, 1.0, "MCLA/Cinematic",
    "TRAILER TOOLKIT: how much of the game's own UI (HUD, map, phone, prompts, gamertags, world icons) the picture shows, "
    "from 0 to 1. 1 is the game's frame and costs nothing; 0 is the 3D picture with no UI; in between is a fade. The pause "
    "menu, the DEBUG menu, the race pause, the map and the F3 card are never faded. Frames whose UI the renderer cannot "
    "keep apart from the picture (loading, movies) show it whole. Plume renderer only. Live; a scene's hud keys write it.")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload)
    .debug_only();

namespace mc::cinematic {

namespace {

// What the alpha was before the first SetHudAlpha of a scene, for ReleaseHud.
std::mutex g_mutex;
bool g_held = false;
double g_before = 1.0;

double Clamp(float alpha) {
    return std::isfinite(alpha) ? std::clamp(double(alpha), 0.0, 1.0) : 1.0;
}

}  // namespace

void SetHudAlpha(float alpha, float fade_seconds) {
    const double target = Clamp(alpha);
    std::lock_guard lock(g_mutex);
    if (!g_held) {
        g_held = true;
        g_before = REXCVAR_GET(mcla_cin_hud_alpha);
        REXLOG_INFO("[hud] a scene takes the UI alpha (it was {:.2f}; it goes back when the scene ends)", g_before);
    }
    if (REXCVAR_GET(mcla_cin_hud_alpha) == target) {
        return;  // nothing moves
    }
#if defined(LARECOMP_HAS_GPU)
    // Before the cvar: the renderer takes the time with the first change it sees.
    mcla::gpu::SetUiAlphaFade(fade_seconds);
#else
    (void)fade_seconds;
#endif
    REXCVAR_SET(mcla_cin_hud_alpha, target);
}

float HudAlpha() {
    return float(std::clamp(REXCVAR_GET(mcla_cin_hud_alpha), 0.0, 1.0));
}

void ArmHud(bool armed) {
#if defined(LARECOMP_HAS_GPU)
    mcla::gpu::ArmUiLayer(armed);
#else
    (void)armed;
#endif
}

void ReleaseHud(float fade_seconds) {
    double back = 1.0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_held) {
            return;
        }
        g_held = false;
        back = g_before;
    }
    ArmHud(false);
    if (REXCVAR_GET(mcla_cin_hud_alpha) != back) {
#if defined(LARECOMP_HAS_GPU)
        mcla::gpu::SetUiAlphaFade(fade_seconds);
#else
        (void)fade_seconds;
#endif
        REXCVAR_SET(mcla_cin_hud_alpha, back);
    }
    REXLOG_INFO("[hud] the scene is over: the UI alpha is back at {:.2f}", back);
}

}  // namespace mc::cinematic
