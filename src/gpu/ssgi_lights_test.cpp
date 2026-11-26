// Check of the bounce light's lights' share (ssgi_lights.h, GI-BOUNCE-SS, perf/GI-BOUNCE.md): the C++ twin of the colour chain's reprojection of the last frame's light (hlsl/ao_color_ps.hlsl LightsHistory) and of
// its energy rule (SsgiCarryLights), on synthetic scenes (a wall, the ground) the camera moves through; the rejection of a car, a disocclusion and what leaves the picture; the history's validity; the push block;
// and, given the src/gpu folder, the text of the shaders, ao.cpp, scene.cpp, the owner's files and the settings. A program of its own: no renderer, no ReXGlue, no GPU; exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssgi_lights_test        (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssgi_lights_test [<src/gpu dir>]
//
// perf/scripts/gi_bounce_ss_mutations.py changes one statement of the header or the shader at a time and checks that this test notices.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>

#include "ao_push.h"
#include "screen_extract.h"
#include "setting_source.h"
#include "ssgi_level.h"
#include "ssgi_light.h"
#include "ssgi_lights.h"

using namespace mcla::gpu;
namespace sl = mcla::gpu::ssgi_lights;
using ssgi_light::Rgb;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[240];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}
bool Near(float a, float b, float tolerance) {
  return std::fabs(a - b) <= tolerance;
}

// ---------------------------------------------------------------------------
// the scenes: images made analytically, the way the depth pass and the two captures would make them
// ---------------------------------------------------------------------------

constexpr int kW = 64;
constexpr int kH = 48;
const sl::Lens kLens{1.0f, 0.75f};
constexpr float kPre = 0.1f;  // the colour before the light pass: a dim wall

// A fronto-parallel wall at view distance D: every texel is at depth D. The lights lit a patch of it (texels [x0, x1] x [y0, y1]) by `lit` in each channel.
struct Wall {
  float depth = 10.0f;
  int x0 = 20, x1 = 30, y0 = 10, y1 = 20;
  float lit = 2.0f;
  bool vehicle_in_patch = false;   // the history's patch is a car
  float patch_depth = 0.0f;        // != 0: the patch's texels are at this depth (something nearer a frame ago)
  int hide_from_x = 1 << 20;       // the patch's texels from this x on are at hide_depth (a truck covered the patch's right part)
  float hide_depth = 5.0f;
  sl::HistoryTexel At(int x, int y) const {
    sl::HistoryTexel t;
    t.depth = depth;
    t.pre = {kPre, kPre, kPre};
    const bool in_patch = x >= x0 && x <= x1 && y >= y0 && y <= y1;
    const float l = in_patch ? lit : 0.0f;
    t.post = {kPre + l, kPre + l * 0.5f, kPre};
    if (in_patch) {
      t.vehicle = vehicle_in_patch;
      if (patch_depth != 0.0f) t.depth = patch_depth;
      if (x >= hide_from_x) t.depth = hide_depth;
    }
    return t;
  }
};

sl::Pixel WallPixel(const Wall& wall, int x, int y) {
  sl::Pixel p;
  p.x = x;
  p.y = y;
  p.depth = wall.depth;
  p.normal = {0.0f, 0.0f, -1.0f};  // facing the camera
  return p;
}

sl::Motion Translation(float tx, float ty, float tz) {
  sl::Motion m;
  m.rows[0][3] = tx;
  m.rows[1][3] = ty;
  m.rows[2][3] = tz;
  return m;
}

sl::Found Find(const Wall& wall, const sl::Pixel& p, const sl::Motion& motion) {
  return sl::HistoryLight(p, kW, kH, kLens, kLens, motion, [&](int x, int y) { return wall.At(x, y); });
}

float SmoothStepOf(float weight) {
  return sl::SmoothStep(sl::kConfidenceLow, sl::kConfidenceHigh, weight);
}

void CheckWall() {
  const Wall wall;
  // A still camera: the texel finds exactly its own place, all of the surface (weight 1), the whole light there (its blue channel got none).
  const sl::Found still = Find(wall, WallPixel(wall, 25, 15), sl::Motion{});
  Check(!still.skipped && Near(still.weight, 1.0f, 1e-4f) && Near(still.confidence, 1.0f, 1e-4f), "a still camera: the texel finds its own place whole (weight " + Num("%.4f", still.weight) + ")");
  Check(Near(still.delta.r, 2.0f, 1e-4f) && Near(still.delta.g, 1.0f, 1e-4f) && Near(still.delta.b, 0.0f, 1e-6f), "... and takes the difference of the two captures: (2, 1, 0), not the dim wall's own colour");
  const sl::Found dark = Find(wall, WallPixel(wall, 40, 30), sl::Motion{});
  Check(dark.delta.r == 0.0f && dark.delta.g == 0.0f && dark.delta.b == 0.0f && dark.weight > 0.99f, "a place the lights did not reach: found whole, no light (a day without lamps in view is today's chain)");

  // The camera moved right by tx: the same wall point is tx further right in the last frame's view space; the shift is 0.5 * w * tx / (D * tan_x) pixels. tx = 0.625 is exactly two texels.
  const float tx = 2.0f * wall.depth * kLens.tan_x / (0.5f * float(kW));
  const sl::Motion right = Translation(tx, 0.0f, 0.0f);
  const sl::Found a = Find(wall, WallPixel(wall, wall.x0 - 2, 15), right);  // finds texel x0 of the history
  const sl::Found b = Find(wall, WallPixel(wall, wall.x0 - 3, 15), right);  // finds texel x0 - 1: outside the patch
  const sl::Found c = Find(wall, WallPixel(wall, wall.x1 - 2, 15), right);  // finds texel x1
  const sl::Found d = Find(wall, WallPixel(wall, wall.x1 - 1, 15), right);  // finds texel x1 + 1: outside
  Check(Near(a.delta.r, 2.0f, 1e-3f) && Near(c.delta.r, 2.0f, 1e-3f), "a camera that moved by two texels: the light is found where the texel was (both edges of the patch)");
  Check(b.delta.r == 0.0f && d.delta.r == 0.0f, "... and one texel beyond the patch it is not");
  // A half texel: the bilinear mean of the two neighbours.
  const sl::Motion half = Translation(tx * 0.25f, 0.0f, 0.0f);  // half a texel
  const sl::Found edge = Find(wall, WallPixel(wall, wall.x0 - 1, 15), half);  // between history texels x0 - 1 (dark) and x0 (lit)
  Check(Near(edge.delta.r, 1.0f, 1e-3f), "half a texel off a patch's edge: the mean of the two texels (1 of 2), not a step");

  // What leaves the picture (the last frame's place is outside): nothing, not a clamped edge.
  const sl::Motion far = Translation(30.0f, 0.0f, 0.0f);
  const sl::Found out = Find(wall, WallPixel(wall, 25, 15), far);
  Check(out.delta.r == 0.0f && out.weight == 0.0f && out.confidence == 0.0f, "a place that was outside the last frame's picture takes no light");
  // Behind the camera in the last frame: no place.
  const sl::Found behind = Find(wall, WallPixel(wall, 25, 15), Translation(0.0f, 0.0f, -20.0f));
  Check(behind.skipped && behind.delta.r == 0.0f, "a point that was behind the camera a frame ago has no place in the history");

  // The lens of the last frame is the one its texels are placed with (a pause view's zoom, a field of view that changed): with a wider last frame (tan 1.1) the pixel 2 of the picture's left edge is where the last frame's
  // texel 4.67 was, and a patch of texels 4 to 6 is lit.
  Wall edge_wall;
  edge_wall.x0 = 4;
  edge_wall.x1 = 6;
  const sl::Lens wider{1.1f, 0.75f};
  const sl::Found zoomed = sl::HistoryLight(WallPixel(edge_wall, 2, 15), kW, kH, kLens, wider, sl::Motion{}, [&](int x, int y) { return edge_wall.At(x, y); });
  Check(Near(zoomed.delta.r, 2.0f, 1e-2f) && zoomed.confidence > 0.99f, "a last frame with another lens: the texel is found where THAT lens put it (4.67 of the last frame, not 2)");

  // Half of the four texels are covered by something nearer (a truck): the other half counts by its weight, and the confidence fades the light with it (smoothstep of 0.5, not the whole of the half texel's light).
  Wall covered;
  covered.hide_from_x = 25;
  const sl::Found partial = sl::HistoryLight(WallPixel(covered, 24, 15), kW, kH, kLens, kLens, Translation(0.5f * covered.depth * kLens.tan_x / (0.5f * float(kW)), 0.0f, 0.0f),
                                             [&](int x, int y) { return covered.At(x, y); });  // half a texel to the right: texels 24 (kept) and 25 (covered)
  Check(Near(partial.weight, 0.5f, 1e-3f) && Near(partial.confidence, SmoothStepOf(0.5f), 1e-3f) && partial.delta.r > 1.0f && partial.delta.r < 1.5f,
        "half covered: the weight is a half and the light is faded by the confidence (" + Num("%.3f", partial.delta.r) + " of 2), not taken whole");
}

void CheckRejection() {
  // A car in the last frame on the patch: rejected, whatever the light on it (the car's own neon, a frame behind the road it left).
  Wall car;
  car.vehicle_in_patch = true;
  const sl::Found on_car = Find(car, WallPixel(car, 25, 15), sl::Motion{});
  Check(on_car.delta.r == 0.0f && on_car.weight == 0.0f && on_car.confidence == 0.0f, "a history texel that was a vehicle gives no light (the road a car left is not lit by its neon)");
  // This frame's texel is a car: skipped altogether.
  const Wall wall;
  sl::Pixel p = WallPixel(wall, 25, 15);
  p.vehicle = true;
  const sl::Found me = Find(wall, p, sl::Motion{});
  Check(me.skipped && me.delta.r == 0.0f, "a texel of a vehicle takes no light (its own neon and headlights lit its own body: not a radiance that bounces onto the road)");
  // The sky.
  p = WallPixel(wall, 25, 15);
  p.depth = sl::kSky;
  Check(Find(wall, p, sl::Motion{}).skipped, "the sky takes none");
  // A disocclusion: the last frame had something nearer there (a truck at half the distance): the plane and depth tests refuse it.
  Wall truck;
  truck.patch_depth = 5.0f;
  const sl::Found hidden = Find(truck, WallPixel(truck, 25, 15), sl::Motion{});
  Check(hidden.delta.r == 0.0f && hidden.weight < 0.01f, "a surface that was another one a frame ago (a disocclusion) takes none of that one's light");
  // Within the depth tolerance but off the plane: the plane test alone refuses a texel whose position is far from the tangent plane: a wall that was 0.5 m nearer (5%).
  Wall step;
  step.patch_depth = 9.5f;
  const sl::Found near_step = Find(step, WallPixel(step, 25, 15), sl::Motion{});
  Check(near_step.weight < 0.05f, "5 percent nearer is still another surface: the plane test (sigma 0.14 m at 10 m) refuses it (weight " + Num("%.4f", near_step.weight) + ")");
  // A small error of the depth (1 percent: the jitter of an upscaler, the lens' rounding) is the same surface.
  Wall jitter;
  jitter.patch_depth = 10.05f;
  Check(Find(jitter, WallPixel(jitter, 25, 15), sl::Motion{}).confidence > 0.9f, "half a percent of depth error is the same surface");
  // Half of the four texels are a car (an edge of the car): the other half still counts, by its weight, and the confidence fades the light.
  Wall edge;
  edge.vehicle_in_patch = true;
  edge.x0 = 25;  // texels 25.. are the car; the pixel at 24.5 reads texels 24 and 25
  const sl::Found partial = sl::HistoryLight(WallPixel(edge, 24, 15), kW, kH, kLens, kLens, Translation(0.5f * 10.0f * 1.0f / (0.5f * 64.0f) * 1.0f, 0.0f, 0.0f), [&](int x, int y) { return edge.At(x, y); });
  Check(partial.confidence <= 1.0f && partial.delta.r == 0.0f, "an edge of a car: the car's texels carry none of the light (the half that is not a car has none here)");
  // Not a number in a capture: no light, not a NaN.
  Check(sl::HeldDelta(std::nanf(""), 0.1f) == 0.0f && sl::HeldDelta(0.1f, std::nanf("")) == 0.0f && sl::HeldDelta(0.1f, 0.2f) == 0.0f && sl::HeldDelta(0.5f, 0.2f) > 0.29f, "a difference is never negative and a NaN is none");
}

// The ground the camera drives over: a plane 1.5 m under the camera, the camera moving forward by f. Per pixel the linear depth is analytic; the lights lit a Gaussian pool at world (0, 12) of the last frame.
struct Ground {
  float h = 1.5f;
  float pool_z = 12.0f;
  float sigma = 2.0f;
  float amp = 3.0f;
  float DepthAt(int y) const {
    const float ndc = 1.0f - 2.0f * (float(y) + 0.5f) / float(kH);
    return ndc < -1e-3f ? -h / (ndc * kLens.tan_y) : sl::kSky;
  }
  float Light(float x, float z) const { return amp * std::exp(-0.5f * (x * x + (z - pool_z) * (z - pool_z)) / (sigma * sigma)); }
  sl::HistoryTexel At(int x, int y) const {
    sl::HistoryTexel t;
    t.depth = DepthAt(y);
    t.pre = {kPre, kPre, kPre};
    t.post = t.pre;
    if (t.depth < sl::kSky * 0.5f) {
      const sl::Vec3 v = sl::ViewPos(float(x) + 0.5f, float(y) + 0.5f, float(kW), float(kH), t.depth, kLens);
      const float l = Light(v.x, v.z);
      t.post = {kPre + l, kPre + l, kPre + l};
    }
    return t;
  }
};

void CheckGround() {
  const Ground g;
  const float f = 0.6f;  // the camera moved forward by 0.6 m: a point's distance in the last frame was z + f
  const sl::Motion forward = Translation(0.0f, 0.0f, f);
  int tested = 0, close = 0, confident = 0;
  float worst = 0.0f;
  for (int y = 36; y < kH - 1; ++y) {  // the near ground: neighbours' depths differ by under 5 percent
    for (int x = 8; x < kW - 8; ++x) {
      sl::Pixel p;
      p.x = x;
      p.y = y;
      p.depth = g.DepthAt(y);
      p.normal = {0.0f, 1.0f, 0.0f};
      const sl::Found found = sl::HistoryLight(p, kW, kH, kLens, kLens, forward, [&](int tx, int ty) { return g.At(tx, ty); });
      const sl::Vec3 pc = sl::ViewPos(float(x) + 0.5f, float(y) + 0.5f, float(kW), float(kH), p.depth, kLens);
      const float expect = g.Light(pc.x, pc.z + f);
      ++tested;
      if (found.confidence > 0.9f) ++confident;
      const float error = std::fabs(found.delta.r - expect);
      worst = std::max(worst, error);
      if (error < 0.2f) ++close;
    }
  }
  Check(confident == tested, "the near ground the camera drives over keeps its history: the plane test does not refuse a slanted surface (" + std::to_string(confident) + " of " + std::to_string(tested) + ")");
  Check(close == tested && worst < 0.2f, "the pool of light is found where the ground point was, to a few percent of its amplitude (worst " + Num("%.3f", worst) + " of 3)");
  // The pool is in front of the camera and moves down the picture as it comes nearer: a pixel at the pool's old distance sees it, one a metre short of it does not.
  sl::Pixel p;
  p.x = kW / 2;
  p.normal = {0.0f, 1.0f, 0.0f};
  float best = 0.0f;
  int best_y = 0;
  for (int y = 24; y < kH; ++y) {
    p.y = y;
    p.depth = g.DepthAt(y);
    const float l = sl::HistoryLight(p, kW, kH, kLens, kLens, forward, [&](int tx, int ty) { return g.At(tx, ty); }).delta.r;
    if (l > best) {
      best = l;
      best_y = y;
    }
  }
  // The far ground: the rows are 3 m apart at 11 m, all on one plane (the plane test accepts every texel) but 28 percent apart in depth: only the texel within 8 percent of the point counts (the same depth test as the
  // occlusion's history), so a pixel there gets the weight of one row, not both.
  sl::Pixel far;
  far.x = kW / 2;
  far.y = 28;
  far.depth = g.DepthAt(28);
  far.normal = {0.0f, 1.0f, 0.0f};
  const sl::Found far_found = sl::HistoryLight(far, kW, kH, kLens, kLens, forward, [&](int tx, int ty) { return g.At(tx, ty); });
  Check(far_found.weight > 0.5f && far_found.weight < 0.9f, "the far ground, where the texels' depths differ by over 8 percent, keeps only the near one (weight " + Num("%.3f", far_found.weight) + ")");
  Check(best > 2.0f && g.DepthAt(best_y) > g.pool_z - f - 1.5f && g.DepthAt(best_y) < g.pool_z - f + 1.5f, "the brightest texel of the column is where the pool now is (" + Num("%.1f", double(g.DepthAt(best_y))) + " m, the pool is at " + Num("%.1f", double(g.pool_z - f)) + ")");
}

// The camera turned by 20 degrees about the vertical axis since the last frame (this frame's view space is the last frame's rotated): a wall that is a plane z = D in the last frame's view space. The normal of each
// texel is rotated by the same 3x4 as its position, so the texel lies exactly on its tangent plane in the last frame: weight 1 (no tap off the plane).
void CheckRotation() {
  const float yaw = 20.0f * 3.14159265f / 180.0f;
  const float c = std::cos(yaw), s = std::sin(yaw);
  sl::Motion turn;  // p_last = R p_now
  turn.rows[0][0] = c;
  turn.rows[0][2] = s;
  turn.rows[2][0] = -s;
  turn.rows[2][2] = c;
  const Wall wall;
  int tested = 0, found = 0, whole = 0;
  for (int x = 10; x < kW - 10; ++x) {
    const float ndc = (float(x) + 0.5f) / float(kW) * 2.0f - 1.0f;
    sl::Pixel p;
    p.x = x;
    p.y = 15;
    p.depth = wall.depth / (c - s * ndc * kLens.tan_x);   // z_last = -s x_now + c z_now = D
    p.normal = {s, 0.0f, -c};                              // R^T (0, 0, -1)
    const sl::Vec3 now = sl::ViewPos(float(x) + 0.5f, 15.5f, float(kW), float(kH), p.depth, kLens);
    const float x_last = c * now.x + s * now.z;
    const float hx = (0.5f + 0.5f * x_last / (wall.depth * kLens.tan_x)) * float(kW) - 0.5f;  // continuous texel coordinate in the last frame
    const sl::Found f = sl::HistoryLight(p, kW, kH, kLens, kLens, turn, [&](int tx, int ty) { return wall.At(tx, ty); });
    if (hx > float(wall.x0) + 1.0f && hx < float(wall.x1) - 1.0f) {
      ++tested;
      found += Near(f.delta.r, 2.0f, 1e-2f) ? 1 : 0;
      whole += f.weight > 0.98f ? 1 : 0;
    }
  }
  Check(tested > 5 && found == tested, "a camera that turned 20 degrees: the light of the patch is found where the texels were (" + std::to_string(found) + " of " + std::to_string(tested) + ")");
  Check(whole == tested, "... and every texel lies on its rotated tangent plane (the normal is moved with the position)");
}

// ---------------------------------------------------------------------------
// the energy rule
// ---------------------------------------------------------------------------

void CheckCarry() {
  const Rgb dim{0.05f, 0.04f, 0.03f};
  // No light, no gain, a source: the texel as it was, to the bit.
  Check(sl::CarryLights(dim, {0, 0, 0}, 1.0f, 0.5f, 0.0f).r == dim.r && sl::CarryLights(dim, {0, 0, 0}, 1.0f, 0.5f, 0.0f).b == dim.b, "no light: the chain's colour is untouched to the bit (the day is today's picture)");
  const Rgb lit{2.0f, 1.0f, 0.0f};
  const Rgb none = sl::CarryLights(dim, lit, 0.0f, 0.5f, 0.0f);
  Check(none.r == dim.r && none.g == dim.g && none.b == dim.b, "a gain of 0 is the chain as it was");
  const Rgb source = sl::CarryLights(dim, lit, 1.0f, 0.5f, 1.0f);
  Check(source.r == dim.r && source.g == dim.g && source.b == dim.b, "a texel that is a source of light (the lamp itself) takes none of the lights' share");
  const Rgb half_source = sl::CarryLights(dim, {0.1f, 0.1f, 0.1f}, 1.0f, 5.0f, 0.5f);
  Check(Near(half_source.r, dim.r + 0.05f, 1e-5f), "half a source takes half");
  // Hue and the cap: the carried colour's luma never passes the cap, the added light has the lights' hue.
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  bool under_cap = true, never_darker = true, hue = true, monotone = true, linear_small = true;
  for (int i = 0; i < 20000; ++i) {
    const float cap = 0.5f + 8.0f * u(rng);
    const float scale = cap / (1.0f + 2.0f * u(rng));
    const Rgb pre = ssgi_light::Scaled({u(rng), u(rng), u(rng)}, scale / 3.0f);  // luma < cap
    const Rgb d = {6.0f * u(rng), 6.0f * u(rng), 6.0f * u(rng)};
    const float gain = 4.0f * u(rng);
    const float share = u(rng) < 0.3f ? u(rng) : 0.0f;
    const Rgb out = sl::CarryLights(pre, d, gain, cap, share);
    under_cap = under_cap && ssgi_light::Luma(out) <= std::max(cap, ssgi_light::Luma(pre)) + 1e-4f;
    never_darker = never_darker && out.r >= pre.r - 1e-6f && out.g >= pre.g - 1e-6f && out.b >= pre.b - 1e-6f;
    const Rgb added = {out.r - pre.r, out.g - pre.g, out.b - pre.b};
    // the added light is a multiple of the lights': the cross product of the two is 0
    const float cx = added.g * d.b - added.b * d.g, cy = added.b * d.r - added.r * d.b, cz = added.r * d.g - added.g * d.r;
    hue = hue && std::sqrt(cx * cx + cy * cy + cz * cz) < 1e-3f * (1.0f + std::fabs(added.r) + std::fabs(added.g) + std::fabs(added.b));
    // more light, never less (the same texel, the light scaled up)
    const Rgb more = sl::CarryLights(pre, ssgi_light::Scaled(d, 1.5f), gain, cap, share);
    monotone = monotone && ssgi_light::Luma(more) >= ssgi_light::Luma(out) - 1e-5f;
  }
  Check(under_cap, "the carried colour's luma is never over the cap (20000 random texels): the apply's ceiling and gain limit see what they have always seen");
  Check(never_darker, "the lights only add");
  Check(hue, "the added light keeps the lights' hue (the colour is scaled, never clipped per channel)");
  Check(monotone, "more light is never less radiance");
  // Linear below half the room: a small light is added as it is (the soft clip's knee), so a dim lamp is not dimmed.
  const Rgb pre{0.1f, 0.1f, 0.1f};
  const float cap = 2.0f;
  const float room = cap - ssgi_light::Luma(pre);
  const float luma = 0.4f * room;
  const Rgb out = sl::CarryLights(pre, {luma, luma, luma}, 1.0f, cap, 0.0f);
  linear_small = Near(out.r, pre.r + luma, 1e-5f);
  Check(linear_small, "a light of 40 percent of the room is added as it is");
  const Rgb big = sl::CarryLights(pre, {100.0f, 100.0f, 100.0f}, 1.0f, cap, 0.0f);
  Check(ssgi_light::Luma(big) < cap && ssgi_light::Luma(big) > 0.95f * cap, "a lamp far over the room fills the texel to just under the cap, no more");
  // No room (a texel already at the cap): nothing added.
  const Rgb full = sl::CarryLights({1.0f, 1.0f, 1.0f}, {5.0f, 5.0f, 5.0f}, 1.0f, 1.0f, 0.0f);
  Check(full.r == 1.0f, "a texel with no room under the cap takes nothing");
  Check(sl::GainOf(std::nan("")) == sl::kGainDefault && sl::GainOf(-3.0) == 0.0f && sl::GainOf(99.0) == sl::kGainMax && sl::GainOf(2.0) == 2.0f, "the gain setting is held to 0..8 and a NaN is the default");
}

// ---------------------------------------------------------------------------
// the lights' own cap floor (GI-BOUNCE-SS-2): the night's room for the lights, the day to the bit
// ---------------------------------------------------------------------------

void CheckFloor() {
  // The setting: held to 0..4, a NaN is the default; the default is a white wall in full sun (ambient 0.3 + sun 1.0 of ssgi_test.cpp's day) and is over the night's cap floor.
  Check(sl::FloorOf(std::nan("")) == sl::kFloorDefault && sl::FloorOf(-1.0) == 0.0f && sl::FloorOf(99.0) == sl::kFloorMax && sl::FloorOf(2.0) == 2.0f, "the floor setting is held to 0..4 and a NaN is the default");
  Check(Near(sl::kFloorDefault, 0.3f + 1.0f, 1e-6f) && sl::kFloorDefault > ssgi_light::kCapFloor, "the default floor is one white wall in full sun (ambient 0.3 + sun 1.0) and sits over the night cap's floor");
  // THE DAY IS TODAY'S TO THE BIT: wherever the frame's cap is at or over the floor, the lights' cap is the cap itself (every strength level, a sunny and an overcast noon).
  bool day_same = true;
  for (const ssgi_level::Preset& preset : ssgi_level::kPresets) {
    for (const float sun : {1.0f, 0.6f}) {
      const float cap = ssgi_light::RadianceCap(preset.cap, 0.3f, sun);
      day_same = day_same && cap >= sl::kFloorDefault && sl::LightsCap(cap, sl::kFloorDefault) == cap;
      // ... and so the whole carry is the old one's: the same bits for the same texel
      const Rgb pre{0.2f, 0.15f, 0.1f};
      const Rgb d{0.8f, 0.5f, 0.1f};
      const Rgb old = sl::CarryLights(pre, d, 1.0f, cap, 0.0f);
      const Rgb now = sl::CarryLights(pre, d, 1.0f, sl::LightsCap(cap, sl::kFloorDefault), 0.0f);
      day_same = day_same && old.r == now.r && old.g == now.g && old.b == now.b;
    }
  }
  Check(day_same, "by day (every strength level, sunny and overcast noon) the lights' cap is the chain's cap and the carried colour is the old one's, bit for bit");
  // A floor of 0 or any under the cap is the old rule at every frame (the A/B), at night too.
  bool zero_same = true;
  std::mt19937 rng(11);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  for (int i = 0; i < 5000; ++i) {
    const float cap = ssgi_light::RadianceCap(0.25f + 3.25f * u(rng), 0.02f + 0.8f * u(rng), 1.2f * u(rng));
    zero_same = zero_same && sl::LightsCap(cap, 0.0f) == cap && sl::LightsCap(cap, ssgi_light::kCapFloor) == cap;
  }
  Check(zero_same, "a floor of 0, or the cap's own floor, is the rule as it was in every frame (the A/B arm)");
  // THE NIGHT: the cap is its floor 0.5; the lights' room is the lights' floor. A neon pool (luma 2) on a dim wall (0.05): before, cut under 0.5; now carried up to just under 1.3, hue kept.
  const float night_cap = ssgi_light::RadianceCap(2.0f, 0.05f, 0.0f);
  Check(night_cap == ssgi_light::kCapFloor && sl::LightsCap(night_cap, sl::kFloorDefault) == sl::kFloorDefault, "at night the cap is 0.5 and the lights' cap is the floor");
  const Rgb dim{0.05f, 0.05f, 0.05f};
  const Rgb pool{0.5f, 3.0f, 3.5f};  // a cyan pool, luma about 2.3
  const Rgb crushed = sl::CarryLights(dim, pool, 1.0f, night_cap, 0.0f);
  const Rgb carried = sl::CarryLights(dim, pool, 1.0f, sl::LightsCap(night_cap, sl::kFloorDefault), 0.0f);
  const float l_crushed = ssgi_light::Luma(crushed), l_carried = ssgi_light::Luma(carried);
  Check(l_crushed < ssgi_light::kCapFloor && l_carried > 2.0f * l_crushed && l_carried < sl::kFloorDefault, "at night the same pool carries more than twice the radiance (" + Num("%.3f", l_crushed) + " -> " + Num("%.3f", l_carried) + ") and stays under the floor");
  const float tint_old = (crushed.g - dim.g) / std::max(crushed.b - dim.b, 1e-6f), tint_new = (carried.g - dim.g) / std::max(carried.b - dim.b, 1e-6f);
  Check(Near(tint_old, tint_new, 1e-4f) && Near(tint_new, 3.0f / 3.5f, 1e-4f), "... with the pool's hue, not a whiter one");
  // A dim lamp (luma 0.1) is added as it is whichever floor (the soft clip's knee): the floor changes the room of a big light, not a small one's.
  const Rgb small = sl::CarryLights(dim, {0.1f, 0.1f, 0.1f}, 1.0f, sl::LightsCap(night_cap, sl::kFloorDefault), 0.0f);
  const Rgb small_old = sl::CarryLights(dim, {0.1f, 0.1f, 0.1f}, 1.0f, night_cap, 0.0f);
  Check(Near(small.r, small_old.r, 1e-6f) && Near(small.r, 0.15f, 1e-5f), "a dim lamp is added as it is, floor or not");
  // The floor only widens the room: a larger floor never carries less, and never more than the floor itself holds.
  bool monotone = true, under = true;
  for (int i = 0; i < 5000; ++i) {
    const float cap = ssgi_light::kCapFloor + 2.0f * u(rng);
    const float lo = 3.0f * u(rng), hi = lo + 2.0f * u(rng);
    const Rgb pre = ssgi_light::Scaled({u(rng), u(rng), u(rng)}, 0.15f);
    const Rgb d = {6.0f * u(rng), 6.0f * u(rng), 6.0f * u(rng)};
    const Rgb a = sl::CarryLights(pre, d, 1.0f, sl::LightsCap(cap, lo), 0.0f);
    const Rgb b = sl::CarryLights(pre, d, 1.0f, sl::LightsCap(cap, hi), 0.0f);
    monotone = monotone && ssgi_light::Luma(b) >= ssgi_light::Luma(a) - 1e-5f;
    under = under && ssgi_light::Luma(b) <= std::max(sl::LightsCap(cap, hi), ssgi_light::Luma(pre)) + 1e-4f;
  }
  Check(monotone && under, "a larger floor never carries less light, and nothing is carried over max(cap, floor) (5000 random texels)");
}

// ---------------------------------------------------------------------------
// the history's validity and the decisions
// ---------------------------------------------------------------------------

void CheckValidity() {
  sl::Key key;
  key.valid = true;
  key.frame = 41;
  key.pause = false;
  key.size[0] = 960;
  key.size[1] = 540;
  Check(sl::Usable(key, 42, false, 960, 540, true), "the frame after the one that made the pair, same view and size, with the AO's history: usable");
  Check(!sl::Usable(key, 42, false, 960, 540, false), "the AO's own history does not hold (the first frame, a camera cut): not usable (the motion is the AO's)");
  Check(!sl::Usable(key, 43, false, 960, 540, true) && !sl::Usable(key, 41, false, 960, 540, true), "a gap of a frame, or the same frame again: not usable");
  Check(!sl::Usable(key, 42, true, 960, 540, true), "the pause menu's world after the play view: not usable");
  Check(!sl::Usable(key, 42, false, 961, 540, true) && !sl::Usable(key, 42, false, 960, 541, true), "another size: not usable");
  sl::Key none;
  Check(!sl::Usable(none, 1, false, 960, 540, true), "no pair yet: not usable");
  Check(!sl::Usable(key, 42, false, 0, 0, true), "no size: not usable");
  // The captures: both sides, only in a frame that ran the chain and the light pass.
  Check(sl::CaptureBefore(true, true, true) && !sl::CaptureBefore(false, true, true) && !sl::CaptureBefore(true, false, true) && !sl::CaptureBefore(true, true, false), "the capture before the light pass: wanted, the chain made, the pass asked");
  Check(sl::CaptureAfter(true, true, true, true) && !sl::CaptureAfter(true, true, false, true) && !sl::CaptureAfter(true, true, true, false) && !sl::CaptureAfter(false, true, true, true),
        "the capture after: only with the one before and a light pass that recorded (else nothing was added)");
  Check(sl::Wanted(true, true, true) && !sl::Wanted(false, true, true) && !sl::Wanted(true, false, true) && !sl::Wanted(true, true, false), "wanted where the bounce light runs, the switch is on and the owner extracts");
  Check((sl::kFlag & 0xFFFFu) == 0u && sl::kFlag == (1u << 23) && sl::kNormalShift == 16 && sl::kHostSamplesMask == 0xFFu, "the flag is bit 23, the normal's slot rides in the high half of slots.y");
  // GI-BOUNCE-JOIN: the pair is of a partition of the lights; a chain uses a pair of its own kind only.
}

void CheckPush() {
  AoPush base{};
  std::mt19937 rng(3);
  uint32_t* words = reinterpret_cast<uint32_t*>(&base);
  for (size_t i = 0; i < sizeof(base) / 4; ++i) words[i] = rng() & 0xFFFFu;
  base.slots2[2] &= ~(sl::kFlag);  // FillPush sets nothing at bit 23
  screen::extract::Frame frame;
  frame.traced[0] = 960;
  frame.traced[1] = 540;
  frame.render[0] = 1920;
  frame.render[1] = 1080;
  screen::extract::ColourRecipe recipe;
  recipe.colour_slot = 77;
  recipe.host_samples = 4;
  recipe.cap = 1.5f;
  recipe.source_start = 3.0f;
  const AoPush off = screen::extract::ColourLevel0Push(base, frame, recipe);
  Check(off.slots[1] == 4u && (off.slots2[2] & sl::kFlag) == 0u && off.slots[2] == base.slots[2] && off.slots[3] == base.slots[3] && off.slots2[1] == base.slots2[1] && off.slots2[3] == base.slots2[3] &&
            off.lens[2] == base.lens[2] && off.lin[0] == base.lin[0] && off.light[1] == base.light[1] && off.extra[2] == base.extra[2] && off.shape[2] == base.shape[2] && off.shape[3] == base.shape[3],
        "a recipe with no lights builds the block as it always was: not a field of it changes");
  recipe.lights.on = true;
  recipe.lights.post_slot = 301;
  recipe.lights.pre_slot = 302;
  recipe.lights.depth_slot = 303;
  recipe.lights.depth_prev_slot = 304;
  recipe.lights.normal_slot = 305;
  for (int i = 0; i < 12; ++i) recipe.lights.motion[i] = float(i) + 0.5f;
  recipe.lights.tan_prev[0] = 1.25f;
  recipe.lights.tan_prev[1] = 0.7f;
  recipe.lights.gain = 2.5f;
  recipe.lights.cap_floor = 1.3f;
  const AoPush on = screen::extract::ColourLevel0Push(base, frame, recipe);
  Check((on.slots2[2] & sl::kFlag) != 0u && (on.slots[1] & sl::kHostSamplesMask) == 4u && (on.slots[1] >> sl::kNormalShift) == 305u, "with lights: the flag, the host samples in the low bits and the normal's slot in the high half of slots.y");
  Check(on.slots[0] == 77u && on.slots[2] == 301u && on.slots[3] == 302u && on.slots2[1] == 303u && on.slots2[3] == 304u, "... the colour plane, the two captures and the two depths in their slots");
  Check(on.lens[2] == 1.25f && on.lens[3] == 0.7f && on.shape[2] == 2.5f && on.shape[0] == 1.5f && on.shape[1] == 3.0f, "... the last frame's lens in lens.zw, the gain in shape.z, the cap and the source start where they were");
  Check(on.shape[3] == 1.3f, "... and the lights' own cap floor in shape.w");
  bool rows = true;
  for (int k = 0; k < 4; ++k) {
    rows = rows && on.lin[k] == float(k) + 0.5f && on.light[k] == float(4 + k) + 0.5f && on.extra[k] == float(8 + k) + 0.5f;
  }
  Check(rows, "... the 3x4 in lin, light and extra, row by row");
  recipe.one_sample = true;
  recipe.sample = 2;
  const AoPush one = screen::extract::ColourLevel0Push(base, frame, recipe);
  Check((one.slots2[2] & screen::extract::kColourOneSampleFlag) != 0u && one.slots2[0] == 2u && (one.slots2[2] & sl::kFlag) != 0u, "the one-sample flag and the lights' flag go together");
}

// ---------------------------------------------------------------------------
// the text: the shaders, ao.cpp, scene.cpp, the owner, frame_order.h, the settings
// ---------------------------------------------------------------------------

std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& what) {
  return text.find(what) != std::string::npos;
}
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

void CheckSources(const std::string& gpu) {
  const std::string common = Slurp(gpu + "/hlsl/ssgi_common.hlsli");
  const std::string color = Slurp(gpu + "/hlsl/ao_color_ps.hlsl");
  const std::string temporal = Slurp(gpu + "/hlsl/ao_temporal_ps.hlsl");
  const std::string ao = Slurp(gpu + "/ao.cpp");
  const std::string scene = Slurp(gpu + "/scene.cpp");
  const std::string lights_cpp = Slurp(gpu + "/screen_lights.cpp");
  const std::string shared_cpp = Slurp(gpu + "/screen_shared.cpp");
  const std::string order = Slurp(gpu + "/frame_order.h");
  const std::string cmake = Slurp(gpu + "/CMakeLists.txt");
  Check(!common.empty() && !color.empty() && !temporal.empty() && !ao.empty() && !scene.empty() && !lights_cpp.empty() && !shared_cpp.empty() && !order.empty() && !cmake.empty(), "the sources are readable from " + gpu);
  if (common.empty() || color.empty() || temporal.empty() || ao.empty() || scene.empty() || lights_cpp.empty() || shared_cpp.empty() || order.empty() || cmake.empty()) return;

  // the shader's constants are the header's (and the occlusion's accumulation pass's: the same tests)
  Check(Has(common, Num("kSsgiLightsPlaneShare = %.3ff;", double(sl::kPlaneShare))) && Has(common, Num("kSsgiLightsPlaneFloor = %.2ff;", double(sl::kPlaneFloor))) &&
            Has(common, Num("kSsgiLightsDepthShare = %.2ff;", double(sl::kDepthShare))) && Has(common, Num("kSsgiLightsDepthFloor = %.2ff;", double(sl::kDepthFloor))) &&
            Has(common, Num("kSsgiLightsConfidenceLow = %.2ff;", double(sl::kConfidenceLow))) && Has(common, Num("kSsgiLightsConfidenceHigh = %.2ff;", double(sl::kConfidenceHigh))) &&
            Has(common, Num("kSsgiLightsNearest = %.2ff;", double(sl::kNearest))),
        "ssgi_common.hlsli: the lights' constants are the header's");
  Check(Has(temporal, Num("kPlaneShare = %.3ff;", double(sl::kPlaneShare))) && Has(temporal, Num("kPlaneFloor = %.2ff;", double(sl::kPlaneFloor))) && Has(temporal, Num("kDepthShare = %.2ff;", double(sl::kDepthShare))) &&
            Has(temporal, Num("kConfidenceLow = %.2ff;", double(sl::kConfidenceLow))) && Has(temporal, Num("kConfidenceHigh = %.2ff;", double(sl::kConfidenceHigh))),
        "ao_temporal_ps.hlsl: the occlusion's accumulation pass has the tests the lights follow (the same numbers)");
  Check(Has(common, "const float keep = gain * (1.0f - saturate(source_share));") && Has(common, "const float room = max(cap - dot(pre, kSsgiLuma), 0.0f);") &&
            Has(common, "const float scale = SsgiSoftClip(luma_d, room) / luma_d;") && Has(common, "return pre + d * scale;") && Has(common, "if (!(luma_d > 1e-6f)) {\n    return pre;"),
        "ssgi_common.hlsli: SsgiCarryLights is the header's CarryLights (the room under the cap, a soft clip of the lights' luma, hue kept)");
  // the colour pass: the flag, the reprojection's statements, the rejections, the carry
  Check(Has(color, "bool AoLights() { return (g_push.slots2.z & (1u << 23)) != 0u; }"), "ao_color_ps.hlsl: the flag is bit 23");
  Check(Has(color, "if (tc >= kAoSky * 0.5f || AoIsVehicle(tc)) {\n    return float3(0.0f, 0.0f, 0.0f);"), "ao_color_ps.hlsl: the sky and a vehicle's own texel take no light");
  Check(Has(color, "if (texel_depth >= kAoSky * 0.5f || AoIsVehicle(texel_depth)) {\n        continue;"), "ao_color_ps.hlsl: a history texel that was the sky or a vehicle counts for nothing");
  Check(Has(color, "const float3 pe = float3(dot(g_push.lin, q), dot(g_push.light, q), dot(g_push.extra, q));") &&
            Has(color, "const float3 ne = float3(dot(g_push.lin.xyz, normal), dot(g_push.light.xyz, normal), dot(g_push.extra.xyz, normal));"),
        "ao_color_ps.hlsl: the position and the normal are moved by the 3x4 in lin, light and extra");
  Check(Has(color, "const float off_plane = dot(ne, ph - pe) / sigma;") && Has(color, "w *= abs(texel_depth - pe.z) < kSsgiLightsDepthShare * pe.z + kSsgiLightsDepthFloor ? 1.0f : 0.0f;") &&
            Has(color, "w *= (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);") && Has(color, "const float sigma = kSsgiLightsPlaneShare * pe.z + kSsgiLightsPlaneFloor;"),
        "ao_color_ps.hlsl: the plane test, the depth test and the bilinear weight are the twin's");
  Check(Has(color, "sum += max(post - pre, 0.0f) * w;") && Has(color, "const float confidence = smoothstep(kSsgiLightsConfidenceLow, kSsgiLightsConfidenceHigh, weight);") &&
            Has(color, "return sum * (confidence / max(weight, 1e-4f));"),
        "ao_color_ps.hlsl: the light is the difference of the two captures (never negative), by the confidence of the weights");
  Check(Has(color, "result.rgb = SsgiCarryLights(result.rgb, LightsHistory(int2(position.xy)), g_push.shape.z, SsgiLightsCap(max(g_push.shape.x, 1e-3f), g_push.shape.w), result.a);") && Count(color, "SsgiCarryLights(") == 1,
        "ao_color_ps.hlsl: level 0 carries the light under the cap it was held to (or the lights' own floor where that is higher), by the texel's source share, with the gain in shape.z and the floor in shape.w");
  Check(!Has(color, "AoLightsOnly"), "the world-light chain always includes the base radiance");
  Check(Has(common, "float SsgiLightsCap(float cap, float floor_luma) {\n  return max(cap, floor_luma);\n}") && Count(common, "SsgiLightsCap(") == 1,
        "ssgi_common.hlsli: SsgiLightsCap is the header's LightsCap (the larger of the cap and the floor: the cap itself, to the bit, wherever it is the larger)");
  Check(Has(color, "max(g_push.slots.y & 0xFFu, 1u)"), "ao_color_ps.hlsl: the host samples are the low byte of slots.y (the high half is the normal's slot)");
  Check(Has(color, "AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, isize, tc, pc)") && Has(color, "const float2 tan_half = g_push.lens.zw;"), "ao_color_ps.hlsl: the normal image's slot and the last frame's lens are where the host puts them");
  // ao.cpp: the plan, the want, the recipe from the history the accumulation uses
  Check(Has(ao, "g.lights = ssgi_lights::Wanted(true, larecomp::settings::mcla_gpu_ssgi_lights.Get(), Extracting());") && Has(ao, "g.lights_gain = ssgi_lights::GainOf(larecomp::settings::mcla_gpu_ssgi_lights_gain.Get());"),
        "ao.cpp: the plan reads the settings through the headers; all four group switches off is no lights' share");
  Check(Has(ao, "g.lights_floor = ssgi_lights::FloorOf(larecomp::settings::mcla_gpu_ssgi_lights_floor.Get());") && Has(ao, "lights.cap_floor = plan.gi.lights_floor;"),
        "ao.cpp: the lights' own floor is read through the header and goes into the recipe");
  Check(Has(ao, "screen_->WantLights(plan.gi.on && plan.gi.lights);"), "ao.cpp: the owner is told once a frame whether the lights are wanted, and of which partition");
  Check(Has(ao, "const HistoryUse use = recipe.quarter ? ChooseQuarterHistory(plan) : ChooseHistory(plan);") &&
        Has(ao, "trigger_pause_, gi_frame.traced[0], gi_frame.traced[1], use.on && previous.texture);"),
        "ao.cpp: the light pair uses the selected trace's size and history");
  Check(Has(ao, "lights.motion[j * 4 + k] = float(sign * use.motion.m[k][j]);") && Has(ao, "const double sign = ((k == 2) != (j == 2)) ? -1.0 : 1.0;"), "ao.cpp: the 3x4 is the accumulation pass's (S G S, row by row)");
  Check(Has(ao, "lights.depth_slot = recipe.quarter ? recipe.matched_depth_slot : Depth(0).slot;") &&
        Has(ao, "lights.depth_prev_slot = previous.slot;") && Has(ao, "lights.tan_prev[0] = tan_half[0];"),
        "ao.cpp: the selected trace's current depth, previous depth and previous lens");
  Check(Has(temporal, "push.lin[k] = motion(k, 0);") || Has(ao, "push.lin[k] = motion(k, 0);"), "(the accumulation pass is given lin = motion(k, 0), shape = motion(k, 1), light = motion(k, 2))");
  Check(Has(ao, "push.shape[k] = motion(k, 1);") && Has(ao, "push.light[k] = motion(k, 2);"), "ao.cpp: the accumulation pass's rows are the ones the lights' 3x4 repeats");
  // the owner
  Check(Has(lights_cpp, "ssgi_lights::Usable(lights_key_, frame_count_, pause, width, height, ao_history)") && Has(lights_cpp, "lights_key_.frame = frame_count_;"), "screen_lights.cpp: the history is usable by the header's rule and the pair is stamped with the frame");
  Check(Has(lights_cpp, "if (!CaptureLightsInto(lights_post_, \"ssgi lights after\", record, summary, list, &pause) || pause != lights_pause_) {\n    return false;"), "screen_lights.cpp: the second capture makes the pair valid only when it was recorded and of the same view");
  Check(Has(lights_cpp, "ColourPipeline(ms ? kColourMs : kColourPlain)") && Has(lights_cpp, "push.slots[2] = (frame_.flags & screen::kFrameHalfTraced) != 0 ? 2u : 1u;"), "screen_lights.cpp: the colour_post capture's pipelines, at the traced size (half where the frame is half traced)");
  Check(Has(shared_cpp, "LightsBeginFrame();"), "screen_shared.cpp: the frame's start clears the frame's captures and counts the idle frames");
  // scene.cpp: the captures at the two sides of the light pass
  const size_t before_at = scene.find("screen_->CaptureLightsBefore(record, summary, list)");
  const size_t lights_at = scene.find("local_lights_->OnResolve(record, summary, list, gbuffer_published, slot_,");
  const size_t after_at = scene.find("screen_->CaptureLightsAfter(record, summary, list)");
  Check(before_at != std::string::npos && lights_at != std::string::npos && after_at != std::string::npos && before_at < lights_at && lights_at < after_at, "scene.cpp: the capture before stands ahead of the light pass and the one after right behind it");
  Check(Has(scene, "if (lights_before && local_lights_ran) {") && Has(scene, "lights_pair.capture_before = [&]() {") &&
        Has(scene, "local_lights_->SetLightsPair(want_pair ? &lights_pair : nullptr);"), "scene.cpp: the second capture only when the first was made and the light pass recorded");
  // the table
  // (kAll is frame_order_test.cpp's pin: it follows every branch that adds a data bit)
  Check(Has(order, "kLightHistory = 1u << 29") && Has(order, "kLightsBefore = 1u << 28"), "frame_order.h: the two data bits");
  Check(Has(order, ".reads_previous = data::kLightHistory, .makes = data::kColourChain"), "frame_order.h: the colour chain reads the last frame's lights history");
  // CMake
  Check(Has(cmake, "screen_lights.cpp") && Has(cmake, "ssgi_lights_test.cpp"), "CMakeLists.txt: the owner's file and this test are listed");
  // the settings: debug only, live, and the defaults the header says
  const setting_source::Info lights = setting_source::Find(gpu, ao, "mcla_gpu_ssgi_lights");
  Check(lights.found && lights.debug_only && lights.live && setting_source::DefaultIsTrue(lights), "mcla_gpu_ssgi_lights: a live debug setting, on by default");
  const setting_source::Info gain = setting_source::Find(gpu, ao, "mcla_gpu_ssgi_lights_gain");
  Check(gain.found && gain.debug_only && gain.live && setting_source::DefaultIs(gain, double(sl::kGainDefault)) && gain.has_range && gain.range_lo == double(sl::kGainMin) && gain.range_hi == double(sl::kGainMax),
        "mcla_gpu_ssgi_lights_gain: a live debug setting, default 1, range 0 to 8 (the header's)");
  const setting_source::Info floor_setting = setting_source::Find(gpu, ao, "mcla_gpu_ssgi_lights_floor");
  Check(floor_setting.found && floor_setting.debug_only && floor_setting.live && setting_source::DefaultIs(floor_setting, double(sl::kFloorDefault)) && floor_setting.has_range && floor_setting.range_lo == double(sl::kFloorMin) &&
            floor_setting.range_hi == double(sl::kFloorMax),
        "mcla_gpu_ssgi_lights_floor: a live debug setting, default 1.3, range 0 to 4 (the header's)");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) gpu_dir = argv[i];
  CheckWall();
  CheckRejection();
  CheckGround();
  CheckRotation();
  CheckCarry();
  CheckFloor();
  CheckValidity();
  CheckPush();
  if (!gpu_dir.empty()) {
    CheckSources(gpu_dir);
  } else {
    std::printf("(no src/gpu folder given: the shader, ao.cpp, scene.cpp and settings text checks were skipped)\n");
  }
  std::printf("ssgi_lights_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
