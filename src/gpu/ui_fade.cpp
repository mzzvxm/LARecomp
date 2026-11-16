// MCLA GPU renderer -- the HUD fade: the renderer follows mcla_cin_hud_alpha. See ui_fade.h.

#include "ui_fade.h"

#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu.h"
#include "renderer.h"

// Defined with the scene's HUD track (mc_engine/cinematic/hud.cpp), in the same executable.
REXCVAR_DECLARE(double, mcla_cin_hud_alpha);

namespace mcla::gpu {

namespace {

// What other threads hand to the render thread (gpu.h): the time the next change takes, the layer
// armed at alpha 1, and the alpha the renderer last applied.
std::atomic<float> g_next_fade_seconds{0.0f};
std::atomic<bool> g_armed{false};
std::atomic<float> g_shown{1.0f};

double SecondsNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void SetUiAlphaFade(float seconds) {
  // Sequentially consistent: this comes before the cvar write that the renderer reads first.
  g_next_fade_seconds.store(seconds > 0.0f ? seconds : 0.0f);
}

void ArmUiLayer(bool armed) {
  g_armed.store(armed, std::memory_order_relaxed);
}

float UiAlpha() {
  return g_shown.load(std::memory_order_relaxed);
}

bool Renderer::ApplyUiFade() {
  const float asked = ClampUiAlpha(REXCVAR_GET(mcla_cin_hud_alpha));
  const bool armed = g_armed.load(std::memory_order_relaxed);
  if (ui_fade_.AtRest(asked, armed, ui_fade_layers_)) {
    return false;  // at rest at 1: the game's frame, nothing asked for
  }
  // Follows the value at every frame boundary, whatever the game is showing (play, a pause view,
  // loading): a new value takes the time a caller left with it (once), else it is at once. A pause
  // view's frame is shown whole by what the layers captured (ShownUiAlpha), never by holding this.
  const UiFadeFollower::Step step = ui_fade_.Update(
      asked, armed, SecondsNow(), [] { return g_next_fade_seconds.exchange(0.0f); });
  if (step.new_value && step.seconds > 0.0f) {
    REXLOG_INFO("[gpu] hud fade: alpha {:.2f} -> {:.2f} over {:.2f} s", step.from, asked,
                step.seconds);
  }
  const float alpha = step.alpha;
  // The layers are wanted while the UI is below 1 (now or where it is heading) and while armed.
  const bool want = step.want_layers;
  bool changed = false;
  if (want != ui_fade_layers_) {
    ui_fade_layers_ = want;
    changed = true;
    if (want) {
      layers_.Request(FrameLayers::kRequesterUiFade, true);
      bool ok = layers_.active();
      if (ok && !hdr_scene_.ready()) {
        ok = hdr_scene_.Initialize(passes_, layers_, pass_timing_);
      }
      if (ok) {
        // In an SDR output nothing else composes the layers; with HDR output this is a no-op.
        present_.SetHdrScene(&hdr_scene_);
      } else {
        REXLOG_ERROR("[gpu] hud fade: the frame layers or the compose pass could not be made; the "
                     "picture stays as the game draws it");
      }
      ui_fade_composing_ = ok;
      REXLOG_INFO("[gpu] hud fade: on, alpha {:.2f}{}, {} output, the layers {}", alpha,
                  armed ? " (armed)" : "", swap_chain_.hdr() ? "HDR10" : "SDR",
                  ok ? "compose at the present" : "failed");
    } else {
      layers_.Request(FrameLayers::kRequesterUiFade, false);
      ui_fade_composing_ = false;
      if (!swap_chain_.hdr()) {
        // Nothing else composes the layers in an SDR output (frame generation reads them itself).
        present_.SetHdrScene(nullptr);
        hdr_scene_.Release();
      }
      REXLOG_INFO("[gpu] hud fade: off, the game's frame again");
    }
  }
  const float shown = ui_fade_composing_ ? alpha : 1.0f;
  if (shown != layers_.ui_alpha()) {
    layers_.SetUiAlpha(shown);
    changed = true;
  }
  g_shown.store(shown, std::memory_order_relaxed);
  return changed;
}

}  // namespace mcla::gpu
