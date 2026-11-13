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

  std::printf("ui_fade_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
