// A check of the HUD fade's pure half (ui_fade.h) that needs no renderer and no game.
//   cmake --build <dir> --target larecomp_ui_fade_test, then run src/gpu/larecomp_ui_fade_test (exit 0 = passed).
// Or by hand: clang++ -std=c++20 -I src/gpu src/gpu/ui_fade_test.cpp -o ui_fade_test

#include <cmath>
#include <cstdio>
#include <limits>

#include "ui_fade.h"

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED line %d: %s\n", line, what);
  }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

bool Near(float a, float b, float eps = 1e-6f) {
  return std::fabs(a - b) <= eps;
}

// A frame-by-frame model of Renderer::ApplyUiFade and of the consumers (the compose, the frame
// generations): the cvar, the time a caller left (SetUiAlphaFade), the layers' state, and the alpha
// the picture shows for a frame that is, or is not, a pause view. The renderer's own code around the
// follower is the layer request and the logging only; this repeats its decisions in the same order.
struct Sim {
  mcla::gpu::UiFadeFollower follower;
  double cvar = 1.0;          // mcla_cin_hud_alpha
  float left_seconds = 0.0f;  // g_next_fade_seconds
  bool armed = false;         // ArmUiLayer
  bool layers_on = false;     // Renderer::ui_fade_layers_
  float shown = 1.0f;         // FrameLayers::ui_alpha(): what the last boundary gave the layers
  double now = 100.0;
  int rest_frames = 0;        // boundaries that returned at once
  int boundaries = 0;

  // hud.cpp SetHudAlpha: nothing moves when the value is the cvar's; the time goes before the cvar.
  void Set(double alpha, float seconds = 0.0f) {
    const double target = mcla::gpu::ClampUiAlpha(alpha);
    if (cvar == target) {
      return;
    }
    left_seconds = seconds > 0.0f ? seconds : 0.0f;
    cvar = target;
  }

  // One guest frame: the frame boundary, then what the picture shows of this frame's UI.
  float Frame(bool pause_view) {
    ++boundaries;
    const float asked = mcla::gpu::ClampUiAlpha(cvar);
    if (follower.AtRest(asked, armed, layers_on)) {
      ++rest_frames;
    } else {
      const mcla::gpu::UiFadeFollower::Step step =
          follower.Update(asked, armed, now, [this] {
            const float s = left_seconds;
            left_seconds = 0.0f;
            return s;
          });
      layers_on = step.want_layers;
      shown = layers_on ? step.alpha : 1.0f;
    }
    now += 1.0 / 60.0;
    return mcla::gpu::ShownUiAlpha(pause_view, shown);
  }
  // n frames of one kind: true when every one of them showed the UI at `expect`.
  bool Run(int n, bool pause_view, float expect, float eps = 1e-6f) {
    bool all = true;
    for (int i = 0; i < n; ++i) {
      all = Near(Frame(pause_view), expect, eps) && all;
    }
    return all;
  }
};

}  // namespace

int main() {
  using mcla::gpu::ClampUiAlpha;
  using mcla::gpu::UiFade;

  // The clamp: 0..1, and 1 for what is not a number.
  CHECK(ClampUiAlpha(0.5) == 0.5f);
  CHECK(ClampUiAlpha(0.0) == 0.0f);
  CHECK(ClampUiAlpha(1.0) == 1.0f);
  CHECK(ClampUiAlpha(-3.0) == 0.0f);
  CHECK(ClampUiAlpha(7.0) == 1.0f);
  CHECK(ClampUiAlpha(std::numeric_limits<double>::quiet_NaN()) == 1.0f);
  CHECK(ClampUiAlpha(std::numeric_limits<double>::infinity()) == 1.0f);
  CHECK(ClampUiAlpha(-std::numeric_limits<double>::infinity()) == 0.0f);

  // A new fade rests at 1 and is settled.
  {
    UiFade f;
    CHECK(f.Value(0.0) == 1.0f);
    CHECK(f.Value(1e9) == 1.0f);
    CHECK(f.Target() == 1.0f);
    CHECK(f.Settled(0.0));
  }

  // An instant change (0 seconds, a negative time and NaN all mean at once).
  {
    UiFade f;
    f.Go(0.25f, 0.0, 10.0);
    CHECK(f.Value(10.0) == 0.25f);
    CHECK(f.Value(9.0) == 0.25f);  // a clock that runs back does not matter: nothing was moving
    CHECK(f.Settled(10.0));
    f.Go(0.75f, -1.0, 11.0);
    CHECK(f.Value(11.0) == 0.75f);
    f.Go(0.5f, std::numeric_limits<double>::quiet_NaN(), 12.0);
    CHECK(f.Value(12.0) == 0.5f);
    CHECK(f.Target() == 0.5f);
  }

  // A straight line over some seconds, from where it was.
  {
    UiFade f;
    f.Go(0.0f, 2.0, 100.0);  // 1 -> 0 over 2 s
    CHECK(Near(f.Value(100.0), 1.0f));
    CHECK(Near(f.Value(100.5), 0.75f));
    CHECK(Near(f.Value(101.0), 0.5f));
    CHECK(Near(f.Value(101.5), 0.25f));
    CHECK(Near(f.Value(102.0), 0.0f));
    CHECK(Near(f.Value(500.0), 0.0f));
    CHECK(!f.Settled(101.0));
    CHECK(f.Settled(102.0));
    CHECK(f.Target() == 0.0f);
    CHECK(Near(f.Value(99.0), 1.0f));  // before it began
  }

  // A change in the middle of a fade starts from where the fade has got to.
  {
    UiFade f;
    f.Go(0.0f, 2.0, 0.0);
    f.Go(1.0f, 1.0, 1.0);  // at 1 s it is at 0.5; back to 1 over 1 s
    CHECK(Near(f.Value(1.0), 0.5f));
    CHECK(Near(f.Value(1.5), 0.75f));
    CHECK(Near(f.Value(2.0), 1.0f));
    CHECK(Near(f.Value(3.0), 1.0f));
  }

  // A target outside 0..1 is clamped.
  {
    UiFade f;
    f.Go(2.0f, 0.0, 0.0);
    CHECK(f.Value(0.0) == 1.0f);
    f.Go(-1.0f, 0.0, 0.0);
    CHECK(f.Value(0.0) == 0.0f);
    f.Go(std::numeric_limits<float>::quiet_NaN(), 0.0, 0.0);
    CHECK(f.Value(0.0) == 1.0f);
  }

  // Many small steps (a runner that calls it every frame with the value it evaluated) never leave 0..1.
  {
    UiFade f;
    float last = 1.0f;
    bool inside = true;
    for (int i = 0; i <= 600; ++i) {
      const double t = i / 60.0;
      const float target = float(i) / 600.0f;
      f.Go(1.0f - target, 0.0, t);
      const float v = f.Value(t);
      inside = inside && v >= 0.0f && v <= 1.0f;
      last = v;
    }
    CHECK(inside);
    CHECK(Near(last, 0.0f));
  }

  // ---- what a frame shows of its UI: a pause view whole at every alpha, any other at the fade's
  for (const float a : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    CHECK(mcla::gpu::ShownUiAlpha(true, a) == 1.0f);
    CHECK(mcla::gpu::ShownUiAlpha(false, a) == a);
  }

  // ---- the follower alone
  {
    using mcla::gpu::UiFadeFollower;
    UiFadeFollower f;
    int taken = 0;
    const auto take = [&taken](float s) {
      return [&taken, s] {
        ++taken;
        return s;
      };
    };
    CHECK(f.AtRest(1.0f, false, false));
    CHECK(!f.AtRest(1.0f, true, false));  // armed: the layers are wanted at 1
    CHECK(!f.AtRest(1.0f, false, true));  // layers on: they are released first
    CHECK(!f.AtRest(0.5f, false, false));
    UiFadeFollower::Step s = f.Update(1.0f, false, 10.0, take(9.0f));
    CHECK(!s.new_value && taken == 0 && s.alpha == 1.0f && !s.want_layers);
    s = f.Update(0.0f, false, 10.0, take(2.0f));  // a move to 0 over 2 s
    CHECK(s.new_value && taken == 1 && s.from == 1.0f && s.seconds == 2.0f);
    CHECK(Near(s.alpha, 1.0f) && s.want_layers);  // not below 1 yet, but heading there
    CHECK(f.seen() == 0.0f && f.target() == 0.0f);
    s = f.Update(0.0f, false, 11.0, take(5.0f));  // the same value: the time is not asked for
    CHECK(!s.new_value && taken == 1 && Near(s.alpha, 0.5f) && s.want_layers);
    s = f.Update(1.0f, false, 11.0, take(0.0f));  // back to 1 from half way, at once
    CHECK(s.new_value && taken == 2 && Near(s.from, 0.5f) && s.alpha == 1.0f && !s.want_layers);
    CHECK(f.AtRest(1.0f, false, false));
  }

  // ---- frame by frame: the free-roam tour of the M1c check (tours/cin-hud.tour) with its pause menu
  // and map. The alpha is followed through every kind of frame; a pause view is shown whole and the
  // fade is back on the first frame of play after it, whatever was set in between.
  {
    Sim s;
    CHECK(s.Run(180, false, 1.0f));                // alpha 1: nothing asked for
    CHECK(!s.layers_on && s.rest_frames == 180);
    s.Set(0.75);
    CHECK(s.Run(60, false, 0.75f) && s.layers_on);
    s.Set(0.5);
    CHECK(s.Run(60, false, 0.5f));
    s.Set(0.25);
    CHECK(s.Run(60, false, 0.25f));
    s.Set(0.0);
    CHECK(s.Run(60, false, 0.0f));
    CHECK(s.Run(480, false, 0.0f));                // the phone's time: still hidden
    CHECK(s.Run(90, true, 1.0f) && s.layers_on);   // the pause menu at 0 is shown whole; the alpha is held
    s.Set(1.0);                                    // 1 inside the pause: the layers go, the menu is the same
    CHECK(s.Run(60, true, 1.0f) && !s.layers_on);
    s.Set(0.0);                                    // 0 again inside it: the layers come back
    CHECK(s.Run(60, true, 1.0f) && s.layers_on);
    CHECK(s.Run(120, false, 0.0f));                // play resumes: hidden from the first frame
    CHECK(s.Run(240, true, 1.0f));                 // the full map (a pause view): whole
    s.Set(0.5);                                    // set inside the map
    CHECK(s.Run(60, true, 1.0f));                  // still whole there
    CHECK(s.Run(120, false, 0.5f));                // and at half the moment play is back
    s.Set(0.0);
    CHECK(s.Run(60, false, 0.0f));
    s.Set(1.0);
    CHECK(s.Run(60, false, 1.0f) && !s.layers_on); // the game's frame again, the layers released
    const int rest = s.rest_frames;
    CHECK(s.Run(60, false, 1.0f) && s.rest_frames == rest + 60);  // and nothing runs at rest again
  }

  // A value set while paused, from rest at 1: it takes effect when play resumes.
  {
    Sim s;
    CHECK(s.Run(60, false, 1.0f));
    CHECK(s.Run(30, true, 1.0f));
    s.Set(0.0);
    CHECK(s.Run(30, true, 1.0f) && s.layers_on);
    CHECK(s.Run(60, false, 0.0f));
  }

  // A timed fade written during a pause runs on the wall clock behind the menu: half done after a
  // second of a two second fade, and finished by the end of the third second.
  {
    Sim s;
    CHECK(s.Run(10, false, 1.0f));
    s.Set(0.0, 2.0f);
    CHECK(s.Run(60, true, 1.0f));
    float last = s.Frame(false);
    CHECK(Near(last, 0.5f, 0.01f));
    bool falling = true;
    for (int i = 0; i < 119; ++i) {
      const float v = s.Frame(false);
      falling = falling && v <= last + 1e-6f;
      last = v;
    }
    CHECK(falling && Near(last, 0.0f));
  }

  // Two writes between two frame boundaries: the last one is followed. A value set and put back
  // between two boundaries is never seen, and costs nothing.
  {
    Sim s;
    CHECK(s.Run(5, false, 1.0f));
    s.Set(0.3);
    s.Set(0.9);
    CHECK(Near(s.Frame(false), 0.9f) && s.layers_on);
    Sim t;
    CHECK(t.Run(5, false, 1.0f));
    t.Set(0.2);
    t.Set(1.0);
    CHECK(t.Run(30, false, 1.0f) && !t.layers_on && t.rest_frames == 35);
  }

  // A time left for a value that was already there is not kept for the next change.
  {
    Sim s;
    s.Set(0.5);
    CHECK(s.Run(3, false, 0.5f));
    s.Set(0.5, 4.0f);  // nothing moves: no time is left behind
    s.Set(0.0);
    CHECK(Near(s.Frame(false), 0.0f));
  }

  // Armed (a scene with HUD keys, before its first key): the layers are ready at alpha 1 and are
  // kept until the scene lets go.
  {
    Sim s;
    s.armed = true;
    CHECK(s.Run(10, false, 1.0f) && s.layers_on && s.rest_frames == 0);
    s.Set(0.0);
    CHECK(s.Run(10, false, 0.0f));
    s.Set(1.0);
    CHECK(s.Run(10, false, 1.0f) && s.layers_on);
    s.armed = false;
    CHECK(s.Run(1, false, 1.0f) && !s.layers_on);
    CHECK(s.Run(10, false, 1.0f) && s.rest_frames == 10);
  }

  std::printf("ui_fade_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
