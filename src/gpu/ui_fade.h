#pragma once
// ===========================================================================
// MCLA GPU renderer -- the HUD fade: how much of the game's UI the picture shows
// ===========================================================================
// Trailer toolkit M1c (feature-parity/CINEMATIC-PLAN.md). The frame layers (frame_layers.h) keep the
// game's UI as a layer of its own, so the present can show the 3D picture plus the UI times an alpha:
// 1 is the game's frame, 0 the picture with no HUD, no phone, no map icons, no prompts and no world
// markers the UI phase draws. The alpha is the debug-only cvar mcla_cin_hud_alpha (0..1, live; a
// scene's HUD track and the debug menu's row both set it, mc_engine/cinematic/hud.h); the renderer
// follows it at each frame boundary (Renderer::ApplyUiFade) and moves to a new value over the time a
// caller asked for with SetUiAlphaFade (gpu.h), or at once.
//
// What it never touches: the pause menu (and the DEBUG menu, which is a page of it), the race pause
// and the full map -- a pause view's frame shows its UI at 1 whatever the alpha is
// (FrameLayers::UiAlphaFor) -- and the SDK's overlays (the F3 card, the F4 panel, toasts), which are
// drawn on the back buffer after the present pass.
//
// Cost: at alpha 1 with nothing armed nobody asks for the layers: no draw changes, nothing is
// allocated or copied, and a frame boundary costs one cvar read and a compare. Below 1 the layers
// run as they do for HDR output (about 0.4 ms at 3x on a desktop GPU: two copies of the UI target's
// region, a clear and a put-back) and the present composes them.
//
// This file is the pure half: the clamp and the ramp, with no renderer in it (ui_fade_test.cpp).
// ===========================================================================

#include <algorithm>

namespace mcla::gpu {

// The alpha a cvar value stands for: 0..1, and 1 for anything that is not a number.
inline float ClampUiAlpha(double alpha) {
  if (!(alpha == alpha)) {
    return 1.0f;
  }
  return float(std::clamp(alpha, 0.0, 1.0));
}

// A linear move from one alpha to another over some seconds, on whatever steady clock the caller
// reads (seconds as a double). Linear in the alpha: a caller that wants an ease sets it in steps.
class UiFade {
 public:
  // The alpha at `now`.
  float Value(double now) const {
    if (now >= end_ || !(end_ > start_)) {
      return to_;
    }
    if (now <= start_) {
      return from_;
    }
    const double t = (now - start_) / (end_ - start_);
    return float(double(from_) + (double(to_) - double(from_)) * t);
  }
  // Where it is heading.
  float Target() const { return to_; }
  // Has arrived (or was never moving).
  bool Settled(double now) const { return now >= end_ || !(end_ > start_); }
  // Heads for `target` from the value it has at `now`, over `seconds` (0 or less: at once; NaN
  // counts as 0).
  void Go(float target, double seconds, double now) {
    from_ = Value(now);
    to_ = ClampUiAlpha(double(target));
    start_ = now;
    end_ = seconds > 0.0 ? now + seconds : now;
  }

 private:
  float from_ = 1.0f;
  float to_ = 1.0f;
  double start_ = 0.0;
  double end_ = 0.0;
};

}  // namespace mcla::gpu
