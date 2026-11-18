// Check of the ray traced car self-reflection's rules (car_rt_refl_rules.h): the image's codes through a half float, the places a texel
// and a point of the view space have in it, the distance tolerances, the fallback colour of a hit the camera cannot see, the fade and
// the edge-aware read the paint variant does. A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0
// when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_refl_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_refl_rules_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "car_rt_refl_rules.h"

namespace rt = mcla::gpu::car_rt_refl;
namespace cr = mcla::gpu::car_selfrefl;

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

bool Near(double a, double b, double tolerance = 1e-6) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

// A half float round trip (round to nearest even), what the image's alpha goes through on the GPU.
float ThroughHalf(float value) {
  if (value == 0.0f) {
    return 0.0f;
  }
  const bool negative = value < 0.0f;
  float a = std::fabs(value);
  int exponent = 0;
  const float mantissa = std::frexp(a, &exponent);  // a = mantissa 2^exponent, mantissa in [0.5, 1)
  // a half float has 11 significant bits (10 stored) for normal numbers
  const float scaled = std::ldexp(mantissa, 11);
  const float rounded = std::nearbyint(scaled);
  a = std::ldexp(rounded, exponent - 11);
  return negative ? -a : a;
}

}  // namespace

int main() {
  // the setting
  Check(rt::SettingOf("auto") == rt::Setting::kAuto && rt::SettingOf("off") == rt::Setting::kOff && rt::SettingOf("garbage") == rt::Setting::kAuto &&
            rt::SettingOf("") == rt::Setting::kAuto,
        "off is off, auto and a word that is no value are auto");
  Check(rt::Wanted(rt::Setting::kAuto, true, true) && !rt::Wanted(rt::Setting::kOff, true, true) && !rt::Wanted(rt::Setting::kAuto, false, true) &&
            !rt::Wanted(rt::Setting::kAuto, true, false),
        "wanted: auto, on a device that can, with the self-reflection on");

  // the codes, through a half float
  Check(rt::DecodeAlpha(0.0f).kind == rt::Kind::kNoData && rt::DecodeAlpha(rt::EncodeAlpha(rt::Kind::kNoData, 1.0f)).kind == rt::Kind::kNoData,
        "alpha 0 is no data, whatever weight was asked");
  Check(rt::DecodeAlpha(rt::EncodeAlpha(rt::Kind::kMiss, 0.7f)).kind == rt::Kind::kMiss && rt::EncodeAlpha(rt::Kind::kMiss, 0.7f) == 1.0f,
        "a miss is alpha 1 and carries no weight");
  for (float w : {0.004f, 0.1f, 0.25f, 0.5f, 0.73f, 1.0f}) {
    const rt::Texel hit = rt::DecodeAlpha(ThroughHalf(rt::EncodeAlpha(rt::Kind::kHit, w)));
    const rt::Texel hidden = rt::DecodeAlpha(ThroughHalf(rt::EncodeAlpha(rt::Kind::kHidden, w)));
    Check(hit.kind == rt::Kind::kHit && Near(hit.weight, w, 1.0 / 1024.0), "a visible hit's weight " + std::to_string(w) + " survives a half float");
    Check(hidden.kind == rt::Kind::kHidden && Near(hidden.weight, w, 1.0 / 1024.0), "a hidden hit's weight " + std::to_string(w) + " survives it too, by its sign");
  }
  Check(rt::DecodeAlpha(ThroughHalf(rt::EncodeAlpha(rt::Kind::kHit, 1.0f))).weight == 1.0f && rt::EncodeAlpha(rt::Kind::kHit, 5.0f) == 2.0f &&
            rt::EncodeAlpha(rt::Kind::kHidden, -1.0f) == -1.0f,
        "weights are clamped to 0..1");
  Check(rt::DecodeAlpha(0.4f).kind == rt::Kind::kNoData && rt::DecodeAlpha(-0.4f).kind == rt::Kind::kNoData,
        "a value below one half is no data (a stray number is not a trace)");

  // the numbers: a half float's step, the slope, the tolerances
  Check(rt::HalfStep(1.0f) == std::ldexp(1.0f, -10) && rt::HalfStep(7.0f) == std::ldexp(1.0f, -8) && rt::HalfStep(1.9999f) == std::ldexp(1.0f, -10) &&
            rt::HalfStep(2.0f) == std::ldexp(1.0f, -9),
        "the half float's step: 2^(floor(log2 t) - 10)");
  Check(Near(rt::SurfaceSlope(1.0f), 0.0) && rt::SurfaceSlope(0.5f) > 1.0f && rt::SurfaceSlope(0.0f) == 8.0f && rt::SurfaceSlope(0.1f) <= 8.0f,
        "the slope of a surface: none face on, capped at 8");
  {
    const float tan_y = std::tan(0.5f * 0.7f);  // a 40 degree lens
    const float pixel = rt::PixelMeters(7.0f, tan_y, 1080.0f);
    Check(Near(pixel, 7.0 * 2.0 * tan_y / 1080.0), "a pixel's side in metres at 7 m on 1080 lines");
    const float face_on = rt::Tolerance(7.0f, pixel, 1.0f, 1.0f);
    const float slanted = rt::Tolerance(7.0f, pixel, 1.0f, 0.25f);
    const float grazing = rt::Tolerance(7.0f, pixel, 1.0f, 0.0f);
    Check(face_on < slanted && slanted < grazing && grazing - face_on <= rt::kMaxSlack + 1e-6f,
          "the tolerance grows with the slope and the slope's share is capped");
    Check(face_on > rt::HalfStep(7.0f) + 0.01f && face_on < 0.06f, "face on at 7 m the tolerance is a few centimetres (half a percent and a centimetre)");
    Check(rt::PrimaryWindow(7.0f, pixel) >= rt::PrimaryTolerance(7.0f, pixel, 0.3f) && rt::PrimaryWindow(7.0f, pixel) >= rt::PrimaryTolerance(7.0f, pixel, 1.0f),
          "the primary ray's window holds the match for every slope");
    Check(rt::Tolerance(2.0f, pixel, 1.0f, 1.0f) < rt::Tolerance(20.0f, pixel, 1.0f, 1.0f), "farther, more tolerance");
    // the car dump measured the rebuilt triangles within 0.33% of the depth at the 95th percentile: that match passes face on at any distance
    for (float t : {2.0f, 5.0f, 12.0f, 30.0f}) {
      Check(rt::Tolerance(t, rt::PixelMeters(t, tan_y, 1080.0f), 0.75f, 1.0f) > 0.0033f * t, "a 0.33% depth error matches at " + std::to_string(t) + " m");
    }
    Check(rt::OriginOffset(7.0f) > rt::OriginOffset(0.0f) && rt::OriginOffset(0.0f) > 0.0f && rt::OriginOffset(7.0f) < 0.05f,
          "the ray's origin is off the surface by a few millimetres");
  }

  // the places: the texel to its render pixel and back
  Check(rt::TexelCentre(1.0f) == 0.5f && rt::TexelCentre(2.0f) == 1.5f, "texel centres: a pixel's own at divisor 1, the lower right of the block at 2");
  Check(rt::PixelOfTexel(0, 1.0f, 100.0f) == 0.0f && rt::PixelOfTexel(7, 1.0f, 100.0f) == 7.0f && rt::PixelOfTexel(0, 2.0f, 100.0f) == 1.0f &&
            rt::PixelOfTexel(3, 2.0f, 100.0f) == 7.0f && rt::PixelOfTexel(49, 2.0f, 99.0f) == 98.0f && rt::PixelOfTexel(49, 2.0f, 98.0f) == 97.0f,
        "the render pixel of a texel: floor((i + 0.5) divisor), kept inside the picture");
  {
    // a texel's ray, taken to a point at the history's distance, comes back to the same texel: the pass and the paint's reprojection agree
    const float render_w = 2560.0f;
    const float render_h = 1440.0f;
    const float tan_x = 0.4f * 16.0f / 9.0f;
    const float tan_y = 0.4f;
    bool all = true;
    float worst = 0.0f;
    for (float divisor : {1.0f, 2.0f}) {
      const uint32_t width = uint32_t(std::ceil(render_w / divisor));
      const uint32_t height = uint32_t(std::ceil(render_h / divisor));
      for (uint32_t ty : {0u, 1u, 37u, 359u, height - 1}) {
        for (uint32_t tx : {0u, 1u, 100u, 640u, width - 1}) {
          const float px = rt::PixelOfTexel(tx, divisor, render_w);
          const float py = rt::PixelOfTexel(ty, divisor, render_h);
          const rt::Direction d = rt::PixelDirection(px, py, render_w, render_h, tan_x, tan_y);
          const float t = 6.5f;
          const float q[3] = {d.x * t, d.y * t, d.z * t};
          const rt::ImagePoint at = rt::ViewToImage(q, tan_x, tan_y, render_w, render_h, divisor);
          worst = std::max(worst, std::max(std::fabs(at.x - float(tx)), std::fabs(at.y - float(ty))));
          all = all && at.inside;
        }
      }
    }
    Check(all && worst < 2e-3f, "a texel's own ray comes back to the texel's coordinates (worst " + std::to_string(worst) + " texels)");
  }
  {
    // AoViewPos (hlsl/ao_common.hlsli) of the pixel's centre at distance t: (uv.x 2 - 1) lens.x t, (1 - uv.y 2) lens.y t, t
    const float render_w = 1920.0f, render_h = 1080.0f, tan_x = 0.7f, tan_y = 0.39375f;
    const rt::Direction d = rt::PixelDirection(300.0f, 700.0f, render_w, render_h, tan_x, tan_y);
    const float t = 4.0f;
    const float u = (300.0f + 0.5f) / render_w, v = (700.0f + 0.5f) / render_h;
    Check(Near(d.x * t, (u * 2.0f - 1.0f) * tan_x * t) && Near(d.y * t, (1.0f - v * 2.0f) * tan_y * t) && d.z * t == t,
          "the pixel's direction times t is AoViewPos's position: z is the distance the history holds");
    const float behind[3] = {0.0f, 0.0f, -1.0f};
    const float at_camera[3] = {0.0f, 0.0f, 0.01f};
    const float off_left[3] = {-5.0f, 0.0f, 3.0f};
    const float middle[3] = {0.0f, 0.0f, 3.0f};
    Check(!rt::ViewToImage(behind, tan_x, tan_y, render_w, render_h, 1.0f).inside && !rt::ViewToImage(at_camera, tan_x, tan_y, render_w, render_h, 1.0f).inside &&
              !rt::ViewToImage(off_left, tan_x, tan_y, render_w, render_h, 1.0f).inside && rt::ViewToImage(middle, tan_x, tan_y, render_w, render_h, 1.0f).inside,
          "a point behind or at the camera, or off the picture, has no place in the image");
    const rt::ImagePoint m1 = rt::ViewToImage(middle, tan_x, tan_y, render_w, render_h, 1.0f);
    const rt::ImagePoint m2 = rt::ViewToImage(middle, tan_x, tan_y, render_w, render_h, 2.0f);
    Check(Near(m1.x, 959.5) && Near(m1.y, 539.5) && Near(m2.x, 479.25, 1e-5) && Near(m2.y, 269.25, 1e-5),
          "the middle of the picture: between the two middle texels at divisor 1, a quarter of a texel off the middle at 2 (the block's lower right pixel)");
  }

  // the taps
  {
    const rt::Taps taps = rt::BilinearTaps(10.25f, 20.75f, 100, 100);
    float sum = 0.0f;
    for (int k = 0; k < 4; ++k) {
      sum += taps.weight[k];
    }
    Check(Near(sum, 1.0) && taps.x[0] == 10 && taps.x[1] == 11 && taps.y[0] == 20 && taps.y[2] == 21 && Near(taps.weight[0], 0.75 * 0.25) &&
              Near(taps.weight[3], 0.25 * 0.75),
          "the bilinear taps: tap k is (k & 1, k >> 1), the weights sum to 1");
    const rt::Taps edge = rt::BilinearTaps(-0.25f, 99.5f, 100, 100);
    Check(edge.x[0] == 0 && edge.x[1] == 0 && edge.y[2] == 99 && edge.y[3] == 99, "the taps are clamped to the image");
  }

  // a reflection ray's hit: what it shows
  const float own[3] = {0.5f, 0.25f, 0.125f};
  const float colour[4][3] = {{0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}};
  {
    const rt::Taps taps = rt::BilinearTaps(50.5f, 50.5f, 100, 100);
    const float all_there[4] = {3.0f, 3.0f, 3.0f, 3.0f};
    const float tolerance = 0.05f;
    const rt::VisibleColour seen = rt::FindVisible(taps, all_there, colour, 3.0f, tolerance);
    Check(Near(seen.coverage, 1.0) && Near(seen.rgb[0], 0.9) && Near(seen.rgb[1], 0.1), "a hit on a surface the four texels show is fully visible, in the surface's colour");
    // the hit at 3 m, the texels show a nearer surface (another part hides it): hidden
    const float hides[4] = {2.5f, 2.5f, 2.5f, 2.5f};
    const rt::VisibleColour hidden = rt::FindVisible(taps, hides, colour, 3.0f, tolerance);
    Check(hidden.coverage == 0.0f && hidden.rgb[0] == 0.0f, "a hit the texels show a nearer surface for is not visible");
    // two of four texels on the outline
    const float edge[4] = {3.0f, 0.0f, 3.0f, 0.0f};
    const rt::VisibleColour half = rt::FindVisible(taps, edge, colour, 3.0f, tolerance);
    Check(Near(half.coverage, 0.5) && Near(half.rgb[0], 0.9), "on the outline half of the footprint matches: coverage a half, the colour of the matching texels only");
    // within the tolerance, with the ramp: a half of it is whole, the full tolerance is none
    Check(rt::MatchWeight(3.0f + 0.5f * tolerance, 3.0f, tolerance) == 1.0f && rt::MatchWeight(3.0f + tolerance, 3.0f, tolerance) == 0.0f &&
              rt::MatchWeight(3.0f + 0.75f * tolerance, 3.0f, tolerance) > 0.0f && rt::MatchWeight(0.0f, 3.0f, tolerance) == 0.0f,
          "the match ramps from half the tolerance to the tolerance and a texel with no vehicle never matches");

    // the fallback rule: a visible hit keeps the colour and the fade's weight; a hidden one has the pixel's own colour in shade at
    // half the weight; a partly visible one is between
    const float length = 4.0f;
    const rt::Reflection full = rt::Combine(1.0f, length, seen, own);
    Check(full.kind == rt::Kind::kHit && Near(full.weight, 1.0) && Near(full.rgb[0], 0.9), "a visible hit near: the whole weight, the hit's own colour");
    const rt::Reflection back = rt::Combine(1.0f, length, hidden, own);
    Check(back.kind == rt::Kind::kHidden && Near(back.weight, rt::kHiddenWeight) && Near(back.weight, cr::kBackFloor) && Near(back.rgb[0], own[0] * cr::kShade) &&
              Near(back.rgb[1], own[1] * 0.4) && Near(back.rgb[2], own[2] * 0.4),
          "a hidden hit: the shaded pixel's own colour times 0.4 at half the weight (version 4's hidden-face rule)");
    const rt::Reflection partly = rt::Combine(1.0f, length, half, own);  // coverage 0.5 = the full share: whole
    Check(partly.kind == rt::Kind::kHit && Near(partly.weight, 1.0), "half the footprint matching is enough to be visible (kVisibleFullShare)");
    rt::VisibleColour quarter = half;
    quarter.coverage = 0.2f;  // under the half that is kVisibleFullShare: visible by 0.4
    const rt::Reflection between = rt::Combine(1.0f, length, quarter, own);
    Check(between.kind == rt::Kind::kHidden && Near(between.weight, 0.7) && between.rgb[0] > own[0] * 0.4f && between.rgb[0] < 0.9f,
          "a fifth of the footprint: between the two in weight and colour, still counted as hidden for the debug view");
    quarter.coverage = 0.25f;
    Check(rt::Combine(1.0f, length, quarter, own).kind == rt::Kind::kHit, "at half the visible share the hit counts as seen");
    // the fade with the distance along the ray
    Check(rt::Combine(0.6f * length, length, seen, own).weight > 0.999f && rt::Combine(0.6f * length + 1e-3f, length, seen, own).weight <= 1.0f,
          "no fade up to 0.6 of the ray's length");
    const float w1 = rt::Combine(0.7f * length, length, seen, own).weight;
    const float w2 = rt::Combine(0.85f * length, length, seen, own).weight;
    Check(w1 < 1.0f && w2 < w1 && w2 > 0.0f, "the weight falls along the ray");
    const rt::Reflection gone = rt::Combine(length, length, seen, own);
    Check(gone.kind == rt::Kind::kMiss && gone.weight == 0.0f && gone.rgb[0] == 0.0f, "at the reach the hit is gone, and gives the miss's code");
  }

  // what the paint reads
  {
    const rt::Taps taps = rt::BilinearTaps(30.5f, 30.5f, 100, 100);  // every weight a quarter
    const float depth = 5.0f;
    const float tolerance = 0.06f;
    auto tap = [](float history, rt::Kind kind, float w, float r) {
      rt::PaintTap t;
      t.history_distance = history;
      t.alpha = rt::EncodeAlpha(kind, w);
      t.rgb[0] = t.rgb[1] = t.rgb[2] = r;
      return t;
    };
    // all four are the player's car at the paint's distance and hit with weight 0.8, colour 0.4: the answer
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kHit, 0.8f, 0.4f), tap(5.0f, rt::Kind::kHit, 0.8f, 0.4f), tap(5.0f, rt::Kind::kHit, 0.8f, 0.4f),
                                    tap(5.0f, rt::Kind::kHit, 0.8f, 0.4f)};
      const rt::PaintRead read = rt::ReadForPaint(taps, four, depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kHit && Near(read.weight, 0.8, 1e-5) && Near(read.rgb[0], 0.4, 1e-5), "four hits: their weight and colour");
    }
    // two hits (weight 1, colour 0.6) and two misses: the weight is the share of the footprint that hits, the colour the hits' own
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f), tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f),
                                    tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f)};
      const rt::PaintRead read = rt::ReadForPaint(taps, four, depth, tolerance);
      Check(read.use && Near(read.weight, 0.5, 1e-5) && Near(read.rgb[0], 0.6, 1e-5),
            "two hits and two misses: half the weight, and the miss's black does not darken the hit's colour");
    }
    // all misses: used (the environment stays), no weight
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f), tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f), tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f),
                                    tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f)};
      const rt::PaintRead read = rt::ReadForPaint(taps, four, depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kMiss && read.weight == 0.0f, "four misses: the image answers with no weight, so the march is not run");
    }
    // texels on another surface (a part in front, a neighbouring panel): not counted
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(3.0f, rt::Kind::kHit, 1.0f, 0.1f), tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f),
                                    tap(0.0f, rt::Kind::kNoData, 0.0f, 0.0f)};
      const rt::PaintRead read = rt::ReadForPaint(taps, four, depth, tolerance);
      Check(read.use && Near(read.weight, 1.0, 1e-5) && Near(read.rgb[0], 0.6, 1e-5),
            "texels on another surface, or on none, are left out of the mean (the part in front's colour does not leak)");
    }
    // the footprint's data is under half of the matching texels: the march
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f), tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f),
                                    tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f)};
      Check(!rt::ReadForPaint(taps, four, depth, tolerance).use, "only a quarter of the matching texels have data: the march answers (no seam of half-lit pixels)");
      const rt::PaintTap three[4] = {tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f),
                                     tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f)};
      Check(rt::ReadForPaint(taps, three, depth, tolerance).use, "three of four have data: the image answers");
    }
    // no texel is the paint's surface: the march (the paint's own agreement test has failed as well)
    {
      const rt::PaintTap four[4] = {tap(9.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(9.0f, rt::Kind::kHit, 1.0f, 0.6f), tap(9.0f, rt::Kind::kHit, 1.0f, 0.6f),
                                    tap(9.0f, rt::Kind::kHit, 1.0f, 0.6f)};
      Check(!rt::ReadForPaint(taps, four, depth, tolerance).use, "no texel is the surface the paint is on: no answer");
    }
    // hidden hits: the debug kind
    {
      const rt::PaintTap four[4] = {tap(5.0f, rt::Kind::kHidden, 0.5f, 0.2f), tap(5.0f, rt::Kind::kHidden, 0.5f, 0.2f), tap(5.0f, rt::Kind::kHidden, 0.5f, 0.2f),
                                    tap(5.0f, rt::Kind::kHit, 0.5f, 0.9f)};
      const rt::PaintRead read = rt::ReadForPaint(taps, four, depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kHidden && Near(read.weight, 0.5, 1e-5), "mostly hidden hits are a hidden hit, at their weight");
    }
  }

  // the constants of the paint variant: the image's slot and the flag are in the reserved words of the 112 bytes
  {
    cr::Tuning tuning;
    const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const cr::CarSelfReflConstants off = cr::MakeConstants(cr::Mode::kReflection, tuning, matrix, 77, 3, 1.0f);
    Check(off.c[2] == 0 && off.c[3] == 0, "no image for the frame: the reserved words stay 0 and the paint is version 4's");
    const cr::CarSelfReflConstants on = cr::MakeConstants(cr::Mode::kReflection, tuning, matrix, 77, 3, 1.0f, 1234);
    Check(on.c[0] == 77 && on.c[1] == 3 && on.c[2] == 1234 && (on.c[3] & cr::kRtImageFlag) != 0 && sizeof(on) == 112,
          "the image's bindless slot in c[2] and the flag in c[3]; the block is still 112 bytes");
    Check(cr::kMaxView == 5, "the debug view has a fifth value, the image");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
