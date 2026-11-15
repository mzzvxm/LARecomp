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

// The alpha a consumer shows a frame's UI layer at (FrameLayers::UiAlphaFor): the fade's alpha, except
// that a pause view's frame -- the pause menu, the DEBUG menu, the race pause, the full map, the
// player's own menus and every piece of their UI -- shows it whole. Nothing about the fade changes
// while such a frame is shown: the alpha it follows is held, and the first frame of play after it is
// shown at the fade's alpha again (a value set during the pause included).
inline float ShownUiAlpha(bool pause_view, float fade_alpha) {
  return pause_view ? 1.0f : fade_alpha;
}

// The renderer's follow of the cvar at a frame boundary (Renderer::ApplyUiFade), as pure code: the
// cvar's value last seen, the move in progress, and what the renderer must do about it -- whether the
// layers are wanted and the alpha the layers' consumers get. It knows nothing about pauses on
// purpose: the frame boundary comes the same in play, in a pause view and while the game loads, a
// value written at any time is followed at the next boundary, and the pause is a property of the frame
// the layers captured (ShownUiAlpha), not of this state.
class UiFadeFollower {
 public:
  struct Step {
    bool new_value = false;    // the cvar differs from the value seen last: a move to it began
    float from = 1.0f;         // (new_value) where that move started from
    float seconds = 0.0f;      // (new_value) the time the caller left for it (0: at once)
    float alpha = 1.0f;        // the alpha now
    bool want_layers = false;  // the layers are wanted: below 1 now or heading below 1, or armed
  };

  // At rest at 1: nothing is asked for, the layers are not on and no value is pending, so the
  // renderer returns after a cvar read and a compare (`asked` is the clamped cvar).
  bool AtRest(float asked, bool armed, bool layers_on) const {
    return !layers_on && !armed && asked == 1.0f && seen_ == 1.0f;
  }

  // Follows `asked` at the time `now`. `take_seconds` is called (once) only when the value is new,
  // and returns the time the caller left with it (SetUiAlphaFade); a value that has not changed
  // leaves what a caller left for the next change where it is.
  template <class TakeSeconds>
  Step Update(float asked, bool armed, double now, TakeSeconds&& take_seconds) {
    Step step;
    if (asked != seen_) {
      seen_ = asked;
      step.new_value = true;
      step.from = fade_.Value(now);
      step.seconds = take_seconds();
      fade_.Go(asked, step.seconds, now);
    }
    step.alpha = fade_.Value(now);
    step.want_layers = armed || step.alpha < 1.0f || fade_.Target() < 1.0f;
    return step;
  }

  // The cvar's value last seen, and where the move is heading.
  float seen() const { return seen_; }
  float target() const { return fade_.Target(); }

 private:
  UiFade fade_;
  float seen_ = 1.0f;
};

}  // namespace mcla::gpu
