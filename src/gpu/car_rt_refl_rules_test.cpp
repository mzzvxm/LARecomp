// Check of the ray traced car self-reflection's rules (car_rt_refl_rules.h): the image's codes through a half float, the places a texel
// and a point of the view space have in it, the distance tolerances, the cone of rays (its pattern, its turn, its directions), the self-hit
// rule, the fallback colour of a hit the camera cannot see, the fade, the rays' sum, and the 3x3 B-spline read the paint variant does
// (with a model of a straight edge through both: the edge's ramp is wide and smooth), and the denoise's rules (the aux image's codes, the kernel,
// its edge stops, its strength by the cone's width, and the whole two-pass filter on a synthetic penumbra: how much of the grain it takes out and what
// it does to the edge). A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_refl_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_rt_refl_rules_test

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------------------------------------------------------
// the denoise: a synthetic image, and the passes as the compute shader makes them
// ---------------------------------------------------------------------------------------------------------------------

constexpr float kTanY = 0.5773503f;      // a 60 degree vertical field of view
constexpr float kImageHeight = 1080.0f;  // the history image of a 4K picture at half size
constexpr float kReach = 4.0f;           // the reflection ray's reach, metres
constexpr float kPanelDistance = 5.0f;   // the panel's view distance, metres: a texel is 5.3 mm there

struct Image {
  int w = 0;
  int h = 0;
  std::vector<float> alpha;
  std::vector<float> distance;
  std::vector<float> hit;
  std::vector<float> nv;
  std::vector<int> instance;
  std::vector<std::array<float, 3>> rgb;
  void Resize(int width, int height) {
    w = width;
    h = height;
    const size_t n = size_t(w) * size_t(h);
    alpha.assign(n, 0.0f);
    distance.assign(n, 0.0f);
    hit.assign(n, 0.0f);
    nv.assign(n, 0.0f);
    instance.assign(n, 0);
    rgb.assign(n, {0.0f, 0.0f, 0.0f});
  }
  size_t At(int x, int y) const { return size_t(y) * size_t(w) + size_t(x); }
};

// One pass over the image as hlsl/car_rt_refl_denoise_cs.hlsl makes it: a texel with data is filtered, the others are copied; the result goes through a half float.
Image DenoisePass(const Image& in, int pass, float tan_half) {
  const int step = rt::DenoiseStep(pass);
  Image out = in;
  rt::DenoiseSetup setup;
  setup.tan_y = kTanY;
  setup.image_height = kImageHeight;
  setup.tan_half = tan_half;
  setup.reach = kReach;
  setup.pass = pass;
  for (int y = 0; y < in.h; ++y) {
    for (int x = 0; x < in.w; ++x) {
      const size_t i = in.At(x, y);
      if (rt::DecodeAlpha(in.alpha[i]).kind == rt::Kind::kNoData || !(in.distance[i] > 0.0f)) {
        continue;
      }
      rt::DenoiseTap taps[9];
      for (int k = 0; k < 9; ++k) {
        const int tx = x + (k % 3 - 1) * step;
        const int ty = y + (k / 3 - 1) * step;
        if (tx < 0 || ty < 0 || tx >= in.w || ty >= in.h) {
          continue;
        }
        const size_t j = in.At(tx, ty);
        taps[k].inside = true;
        taps[k].alpha = in.alpha[j];
        for (int c = 0; c < 3; ++c) {
          taps[k].rgb[c] = in.rgb[j][c];
        }
        taps[k].history_distance = in.distance[j];
        taps[k].hit_code = in.hit[j];
        taps[k].instance = in.instance[j];
      }
      const rt::DenoiseOut o = rt::DenoiseTexel(taps, in.distance[i], in.nv[i], setup);
      out.alpha[i] = ThroughHalf(o.alpha);
      for (int c = 0; c < 3; ++c) {
        out.rgb[i][c] = ThroughHalf(o.rgb[c]);
      }
    }
  }
  return out;
}
Image Denoise(const Image& in, int passes, float tan_half) {
  Image image = in;
  for (int pass = 0; pass < passes; ++pass) {
    image = DenoisePass(image, pass, tan_half);
  }
  return image;
}

// The pass's own picture of a straight edge of a reflected part on a flat panel: texel x, row y, ray `ray` hits where x + disc.x radius < edge (the cone is
// 2 radius texels wide at the hit, `hit_distance` metres away), weight = hits / 4, the colour (0.5 0.3 0.2); the outer pair first and the inner two only if one of
// them hit (`early_out`: the pass's rule).
Image MakeEdgeScene(int w, int h, float edge, float radius, float hit_distance, uint32_t phase, bool early_out) {
  Image img;
  img.Resize(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = img.At(x, y);
      const float turns = rt::ConeRotation(uint32_t(x + 100), uint32_t(y), phase);
      int hits = 0;
      for (int slot = 0; slot < 4; ++slot) {
        if (early_out && slot == 2 && hits == 0) {
          break;
        }
        if (float(x) + rt::ConeDisc(rt::ConeOrder(slot), turns).x * radius < edge) {
          ++hits;
        }
      }
      img.distance[i] = kPanelDistance;
      img.nv[i] = 0.8f;
      img.instance[i] = 3;
      img.alpha[i] = ThroughHalf(rt::EncodeAlpha(hits > 0 ? rt::Kind::kHit : rt::Kind::kMiss, float(hits) / 4.0f));
      if (hits > 0) {
        img.rgb[i] = {ThroughHalf(0.5f), ThroughHalf(0.3f), ThroughHalf(0.2f)};  // the image holds half floats
        img.hit[i] = rt::EncodeHitDistance(hit_distance, kReach);
      }
    }
  }
  return img;
}

// The weight the cone's rays give on average at texel x (the expectation over the pattern's turn): ray j hits where radius_j cos(angle) < edge - x, angle uniform.
float IdealWeight(float x, float edge, float radius) {
  float sum = 0.0f;
  for (int ray = 0; ray < 4; ++ray) {
    const float r = rt::ConeRadius(ray) * radius;
    const float s = std::clamp((edge - x) / r, -1.0f, 1.0f);
    sum += 1.0f - std::acos(s) / 3.14159265358979f;
  }
  return sum / 4.0f;
}

// What the paint's 3x3 B-spline reads at the texel's centre (the weights of the quadratic B-spline at a distance of 0).
float PaintWeight(const Image& img, int x, int y) {
  float w[3];
  rt::BSplineWeights(0.0f, w);
  float sum = 0.0f;
  for (int k = 0; k < 9; ++k) {
    const int tx = std::clamp(x + k % 3 - 1, 0, img.w - 1);
    const int ty = std::clamp(y + k / 3 - 1, 0, img.h - 1);
    sum += w[k % 3] * w[k / 3] * rt::DecodeAlpha(img.alpha[img.At(tx, ty)]).weight;
  }
  return sum;
}

struct EdgeStats {
  double rms = 0.0;       // the paint's read against the ideal read, over the band (the noise and the blur's bias together)
  double grain = 0.0;     // the standard deviation of the paint's read along the edge (across rows at one place), root mean square over the band: what shows as grain
  double crossing = 0.0;  // where the rows' mean read crosses 0.5, texels
  double ramp = 0.0;      // the width of the rows' mean read between 0.9 and 0.1, texels
};
// Where a profile that falls from 1 to 0 crosses `level`, by linear interpolation (-1: it does not, from `from` on).
double CrossingOf(const std::vector<double>& profile, int from, double level) {
  for (int x = from; x + 1 < int(profile.size()); ++x) {
    if (profile[size_t(x)] >= level && profile[size_t(x) + 1] < level) {
      return double(x) + (profile[size_t(x)] - level) / (profile[size_t(x)] - profile[size_t(x) + 1]);
    }
  }
  return -1.0;
}
EdgeStats MeasureEdge(const Image& img, float edge, float radius) {
  float wx[3];
  rt::BSplineWeights(0.0f, wx);
  const int y0 = 6;
  const int y1 = img.h - 6;
  const int x0 = std::max(4, int(edge - radius) - 8);
  const int x1 = std::min(img.w - 4, int(edge + radius) + 8);
  std::vector<double> profile(size_t(img.w), 0.0);
  std::vector<double> ideal(size_t(img.w), 0.0);
  for (int x = 0; x < img.w; ++x) {
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) {
      sum += PaintWeight(img, x, y);
    }
    profile[size_t(x)] = sum / double(y1 - y0);
    double read = 0.0;
    for (int d = -1; d <= 1; ++d) {
      read += wx[d + 1] * IdealWeight(float(x + d), edge, radius);
    }
    ideal[size_t(x)] = read;
  }
  double square = 0.0;
  double grain_square = 0.0;
  int count = 0;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const double read = double(PaintWeight(img, x, y));
      const double e = read - ideal[size_t(x)];
      const double g = read - profile[size_t(x)];
      square += e * e;
      grain_square += g * g;
      ++count;
    }
  }
  EdgeStats stats;
  stats.rms = std::sqrt(square / double(std::max(count, 1)));
  stats.grain = std::sqrt(grain_square / double(std::max(count, 1)));
  stats.crossing = CrossingOf(profile, x0, 0.5);
  stats.ramp = CrossingOf(profile, x0, 0.1) - CrossingOf(profile, x0, 0.9);
  return stats;
}

}  // namespace

int main() {
  // the setting
  Check(rt::SettingOf("auto") == rt::Setting::kAuto && rt::SettingOf("off") == rt::Setting::kOff && rt::SettingOf("garbage") == rt::Setting::kAuto &&
            rt::SettingOf("") == rt::Setting::kAuto,
        "off is off, auto and a word that is no value are auto");
  // The ray tracing tier's `auto` table (rt_tier.h kRtEffects) hands this setting "on" (a strong or a weak card) or "off" (no ray tracing): on is the default
  Check(rt::SettingOf("on") == rt::Setting::kAuto && rt::SettingOf("off") == rt::Setting::kOff, "the tier's cells: on is the default, off is off");
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
    const rt::Texel car = rt::DecodeAlpha(ThroughHalf(rt::EncodeAlpha(rt::Kind::kOtherCar, w)));
    Check(hit.kind == rt::Kind::kHit && Near(hit.weight, w, 1.0 / 1024.0), "a hit's weight " + std::to_string(w) + " survives a half float");
    Check(car.kind == rt::Kind::kOtherCar && Near(car.weight, w, 1.0 / 1024.0), "a hit on another car's weight " + std::to_string(w) + " survives it too, by its sign");
  }
  Check(rt::DecodeAlpha(ThroughHalf(rt::EncodeAlpha(rt::Kind::kHit, 1.0f))).weight == 1.0f && rt::EncodeAlpha(rt::Kind::kHit, 5.0f) == 2.0f &&
            rt::EncodeAlpha(rt::Kind::kOtherCar, -1.0f) == -1.0f,
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

  // the cone: the pattern, its turn, its directions
  {
    // the rings: a quarter of the disc's area each, so the mean squared radius is the disc's one half; the pairs are opposite, so the offsets add up to nothing
    float r2 = 0.0f;
    for (int ray = 0; ray < rt::kConeRays; ++ray) {
      r2 += rt::ConeRadius(ray) * rt::ConeRadius(ray);
    }
    Check(Near(r2 / rt::kConeRays, 0.5, 1e-6) && rt::kConeRays == 4, "the four rays' squared radii average the disc's one half (two rings of equal area)");
    bool balanced = true;
    bool opposite = true;
    for (float turns : {0.0f, 0.1f, 0.37f, 0.5f, 0.93f}) {
      float sx = 0.0f, sy = 0.0f;
      for (int ray = 0; ray < rt::kConeRays; ++ray) {
        const rt::Disc d = rt::ConeDisc(ray, turns);
        sx += d.x;
        sy += d.y;
        const rt::Disc other = rt::ConeDisc(ray ^ 2, turns);
        opposite = opposite && std::fabs(d.x + other.x) < 1e-5f && std::fabs(d.y + other.y) < 1e-5f;
        balanced = balanced && std::fabs(std::hypot(d.x, d.y) - rt::ConeRadius(ray)) < 1e-5f;
      }
      balanced = balanced && std::fabs(sx) < 1e-5f && std::fabs(sy) < 1e-5f;
    }
    Check(balanced && opposite, "the four offsets add up to nothing for every turn: rays j and j + 2 are opposite, at their own radius");
    Check(rt::ConeOrder(0) == 1 && rt::ConeOrder(1) == 3 && rt::ConeOrder(2) == 0 && rt::ConeOrder(3) == 2 && rt::kFirstRays == 2 &&
              rt::ConeRadius(rt::ConeOrder(0)) > rt::ConeRadius(rt::ConeOrder(2)) && rt::ConeRadius(rt::ConeOrder(1)) > rt::ConeRadius(rt::ConeOrder(3)),
          "the outer pair is cast first, the inner pair after");
    // the turn: in [0, 1), different from texel to texel, uniform over a block, a step per phase
    {
      bool inside = true;
      double mean = 0.0;
      int distinct = 0;
      float previous = -1.0f;
      for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
          const float turn = rt::ConeRotation(x, y, 0);
          inside = inside && turn >= 0.0f && turn < 1.0f;
          mean += turn;
          distinct += turn != previous ? 1 : 0;
          previous = turn;
        }
      }
      mean /= 64.0 * 64.0;
      Check(inside && std::fabs(mean - 0.5) < 0.03 && distinct > 3900, "the turn of the pattern is in [0, 1), 0.5 on average over a block, a new value at almost every texel");
      // the same texel by phase: eight different turns, spread (the golden step), and phase 0 is the still pattern
      float phases[8];
      for (uint32_t k = 0; k < 8; ++k) {
        phases[k] = rt::ConeRotation(31, 17, k);
      }
      float least = 1.0f;
      for (int i = 0; i < 8; ++i) {
        for (int j = i + 1; j < 8; ++j) {
          const float gap = std::fabs(phases[i] - phases[j]);
          least = std::min(least, std::min(gap, 1.0f - gap));
        }
      }
      Check(least > 0.05f && phases[0] == rt::ConeRotation(31, 17, 0), "eight frames' turns of one texel are eight well spread angles");
    }
    // the directions
    const float mirror[3] = {0.0f, 0.6f, 0.8f};
    const float normal_up[3] = {0.0f, 1.0f, 0.0f};
    bool unit = true;
    bool same = true;
    bool angle = true;
    for (int ray = 0; ray < rt::kConeRays; ++ray) {
      float d[3];
      rt::ConeDirection(mirror, normal_up, 0.0f, ray, 0.3f, d);
      same = same && std::fabs(d[0] - mirror[0]) < 1e-5f && std::fabs(d[1] - mirror[1]) < 1e-5f && std::fabs(d[2] - mirror[2]) < 1e-5f;
      const float tan_half = rt::ConeTan(3.0f);
      rt::ConeDirection(mirror, normal_up, tan_half, ray, 0.3f, d);
      unit = unit && Near(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0, 1e-5);
      const float dot = d[0] * mirror[0] + d[1] * mirror[1] + d[2] * mirror[2];
      const float want = 1.0f / std::sqrt(1.0f + tan_half * rt::ConeRadius(ray) * tan_half * rt::ConeRadius(ray));
      angle = angle && std::fabs(dot - want) < 1e-5f;
    }
    Check(same && unit && angle, "a cone of tangent 0 is the mirror; the directions are unit vectors at the angle atan(tan x radius) from the mirror");
    // the mean of the four directions is the mirror's to second order, for every turn
    {
      float worst = 0.0f;
      for (float turns : {0.0f, 0.2f, 0.45f, 0.8f}) {
        float sum[3] = {};
        for (int ray = 0; ray < rt::kConeRays; ++ray) {
          float d[3];
          rt::ConeDirection(mirror, normal_up, rt::ConeTan(3.0f), ray, turns, d);
          for (int c = 0; c < 3; ++c) {
            sum[c] += d[c] * 0.25f;
          }
        }
        const float along = sum[0] * mirror[0] + sum[1] * mirror[1] + sum[2] * mirror[2];
        const float across = std::sqrt(std::max(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] - along * along, 0.0f));
        worst = std::max(worst, across);
      }
      Check(worst < 1e-3f, "the mean of the cone's directions is the mirror's to second order (an unbiased glossy reflection)");
    }
    // above the surface: a grazing mirror ray has the cone's lower rays turned up, all stay at least kMinAbove over the tangent plane
    {
      const float graze[3] = {0.0f, 0.04f, 0.99919f};
      bool above = true;
      bool moved = false;
      for (float turns : {0.0f, 0.13f, 0.5f, 0.77f}) {
        for (int ray = 0; ray < rt::kConeRays; ++ray) {
          float d[3];
          rt::ConeDirection(graze, normal_up, rt::ConeTan(10.0f), ray, turns, d);
          above = above && d[1] >= rt::kMinAbove - 1e-4f && Near(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0, 1e-5);
          float plain[3];
          rt::ConeDirection(graze, normal_up, rt::ConeTan(10.0f), ray, turns, plain);
          moved = moved || d[1] > 0.04f + 1e-3f;
        }
      }
      Check(above && moved, "at a grazing mirror direction the cone's rays under the tangent plane are lifted to kMinAbove, the others kept");
    }
    Check(rt::ConeTan(0.0f) == 0.0f && Near(rt::ConeTan(3.0f), std::tan(3.0 * 3.14159265358979 / 180.0), 1e-6) && Near(rt::ConeTan(100.0f), rt::ConeTan(rt::kConeMaxDegrees)) &&
              rt::ConeTan(-1.0f) == 0.0f && rt::kConeDefaultDegrees == 2.5f,
          "the gloss: 0 is a mirror, 2.5 degrees by default, clamped to 12");
  }

  // the self-hit rule
  Check(!rt::IsSelfHit(false, true, 1.0f) && !rt::IsSelfHit(false, false, 1.0f) && rt::IsSelfHit(true, true, -1.0f) && rt::IsSelfHit(true, false, 0.9f) &&
            !rt::IsSelfHit(true, false, 0.5f) && !rt::IsSelfHit(true, false, -0.9f) && !rt::IsSelfHit(true, false, rt::kSelfCos),
        "a hit is the panel itself on the same instance on its own triangle, or facing the point's own way; another instance (the wing on the lid) never; "
        "the same instance facing across (a crease, a scoop's far wall) is a real hit");

  // the fallback colour: the mean of the pixel's own surface around it
  {
    rt::SurfaceTap taps[9];
    for (int k = 0; k < 9; ++k) {
      taps[k].distance = 3.0f;
      taps[k].rgb[0] = 0.5f;
      taps[k].rgb[1] = 0.25f;
      taps[k].rgb[2] = 0.125f;
    }
    float mean[3];
    rt::SurfaceMean(taps, 3.0f, 0.05f, mean);
    Check(Near(mean[0], 0.5) && Near(mean[1], 0.25) && Near(mean[2], 0.125), "one surface, one colour: that colour");
    // a bright glint at the pixel itself (the sky in the paint) is averaged with its neighbours, and a texel on a part in front is left out
    taps[4].rgb[0] = 2.0f;
    taps[0].distance = 2.0f;
    taps[0].rgb[0] = 9.0f;
    rt::SurfaceMean(taps, 3.0f, 0.05f, mean);
    const float tents = 1.0f + 4.0f * 0.5f + 3.0f * 0.25f;  // the middle, four edges, three corners (one left out)
    const float expected = (2.0f * 1.0f + 4.0f * 0.5f * 0.5f + 3.0f * 0.25f * 0.5f) / tents;
    Check(Near(mean[0], expected, 1e-5) && mean[0] < 0.9f * 2.0f && mean[0] > 0.5f,
          "a glint on the pixel is averaged with the surface around it, and a texel on a part in front does not count");
    // no neighbour on the surface: the pixel's own colour
    for (int k = 0; k < 9; ++k) {
      if (k != 4) {
        taps[k].distance = 0.0f;
      }
    }
    rt::SurfaceMean(taps, 3.0f, 0.05f, mean);
    Check(Near(mean[0], 2.0) && Near(mean[1], 0.25), "with no neighbour on the surface the mean is the pixel's own colour");
  }

  // a reflection ray's hit: what it shows
  const float surface[3] = {0.5f, 0.25f, 0.125f};
  const float colour[4][3] = {{0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}, {0.9f, 0.1f, 0.1f}};
  rt::VisibleColour seen;
  rt::VisibleColour hidden;
  rt::VisibleColour half;
  {
    const rt::Taps taps = rt::BilinearTaps(50.5f, 50.5f, 100, 100);
    const float all_there[4] = {3.0f, 3.0f, 3.0f, 3.0f};
    const float tolerance = 0.05f;
    seen = rt::FindVisible(taps, all_there, colour, 3.0f, tolerance);
    Check(Near(seen.coverage, 1.0) && Near(seen.rgb[0], 0.9) && Near(seen.rgb[1], 0.1), "a hit on a surface the four texels show is fully visible, in the surface's colour");
    // the hit at 3 m, the texels show a nearer surface (another part hides it): hidden
    const float hides[4] = {2.5f, 2.5f, 2.5f, 2.5f};
    hidden = rt::FindVisible(taps, hides, colour, 3.0f, tolerance);
    Check(hidden.coverage == 0.0f && hidden.rgb[0] == 0.0f, "a hit the texels show a nearer surface for is not visible");
    // two of four texels on the outline
    const float edge[4] = {3.0f, 0.0f, 3.0f, 0.0f};
    half = rt::FindVisible(taps, edge, colour, 3.0f, tolerance);
    Check(Near(half.coverage, 0.5) && Near(half.rgb[0], 0.9), "on the outline half of the footprint matches: coverage a half, the colour of the matching texels only");
    // within the tolerance, with the ramp: a half of it is whole, the full tolerance is none
    Check(rt::MatchWeight(3.0f + 0.5f * tolerance, 3.0f, tolerance) == 1.0f && rt::MatchWeight(3.0f + tolerance, 3.0f, tolerance) == 0.0f &&
              rt::MatchWeight(3.0f + 0.75f * tolerance, 3.0f, tolerance) > 0.0f && rt::MatchWeight(0.0f, 3.0f, tolerance) == 0.0f,
          "the match ramps from half the tolerance to the tolerance and a texel with no vehicle never matches");
  }
  {
    // the fallback rule: a visible hit keeps the colour and the fade's weight; a hidden one has the surface's mean colour in shade at nearly the whole
    // weight; a partly visible one is between
    const float length = 4.0f;
    const rt::RayShare full = rt::MakeShare(1.0f, length, seen, surface);
    Check(Near(full.weight, 1.0) && Near(full.visible, 1.0) && Near(full.rgb[0], 0.9), "a visible hit near: the whole weight, the hit's own colour");
    const rt::RayShare back = rt::MakeShare(1.0f, length, hidden, surface);
    Check(rt::kHiddenWeight == 0.85f && Near(back.weight, 0.85) && back.visible == 0.0f && Near(back.rgb[0], surface[0] * cr::kShade) &&
              Near(back.rgb[1], surface[1] * 0.4) && Near(back.rgb[2], surface[2] * 0.4),
          "a hidden hit: the surface's mean colour times 0.4 at 0.85 of the weight (the shaded underside is there, and dark; the colour is a guess)");
    Check(rt::kHiddenWeight > 0.5f * (1.0f + cr::kBackFloor) && rt::kHiddenWeight < 1.0f,
          "the hidden weight is nearer the visible one than version 1's half (the switch of a ray from visible to hidden no longer halves a texel)");
    const rt::RayShare partly = rt::MakeShare(1.0f, length, half, surface);  // coverage 0.5 = the full share: whole
    Check(Near(partly.weight, 1.0) && Near(partly.visible, 1.0), "half the footprint matching is enough to be visible (kVisibleFullShare)");
    rt::VisibleColour quarter = half;
    quarter.coverage = 0.2f;  // under the half that is kVisibleFullShare: visible by 0.4
    const rt::RayShare between = rt::MakeShare(1.0f, length, quarter, surface);
    Check(Near(between.weight, 0.85 + 0.15 * 0.4) && Near(between.visible, 0.4) && between.rgb[0] > surface[0] * 0.4f && between.rgb[0] < 0.9f,
          "a fifth of the footprint: between the two in weight and colour");
    // the fade with the distance along the ray
    Check(rt::MakeShare(0.6f * length, length, seen, surface).weight > 0.999f && rt::MakeShare(0.6f * length + 1e-3f, length, seen, surface).weight <= 1.0f,
          "no fade up to 0.6 of the ray's length");
    const float w1 = rt::MakeShare(0.7f * length, length, seen, surface).weight;
    const float w2 = rt::MakeShare(0.85f * length, length, seen, surface).weight;
    Check(w1 < 1.0f && w2 < w1 && w2 > 0.0f, "the weight falls along the ray");
    const rt::RayShare gone = rt::MakeShare(length, length, seen, surface);
    Check(gone.weight == 0.0f && gone.rgb[0] == 0.0f, "at the reach the hit is gone");

    // the rays' sum
    rt::RayShare shares[4];
    for (auto& share : shares) {
      share = full;
    }
    rt::Reflection all = rt::CombineRays(shares, 4, 4.0f);
    Check(all.kind == rt::Kind::kHit && Near(all.weight, 1.0) && Near(all.rgb[0], 0.9), "four visible hits: the whole weight, the hit's colour");
    all = rt::CombineRays(shares, 2, 4.0f);
    Check(all.kind == rt::Kind::kHit && Near(all.weight, 0.5) && Near(all.rgb[0], 0.9),
          "two hits of four rays (the others missed or were not cast): half the weight, and the misses do not darken the hits' colour");
    all = rt::CombineRays(shares, 1, 1.0f);
    Check(Near(all.weight, 1.0), "a gloss of 0: one ray counts for the whole");
    all = rt::CombineRays(shares, 0, 4.0f);
    Check(all.kind == rt::Kind::kMiss && all.weight == 0.0f && all.rgb[0] == 0.0f, "no hit: the miss's code");
    shares[0] = back;
    shares[1] = back;
    shares[2] = back;
    all = rt::CombineRays(shares, 4, 4.0f);
    Check(all.kind == rt::Kind::kHit && Near(all.weight, (3.0 * 0.85 + 1.0) / 4.0) && all.rgb[0] > surface[0] * 0.4f && all.rgb[0] < 0.9f,
          "three hidden hits and a visible one, none of them on another car: still a plain hit (the sign says another car, not what the camera sees), the colour between the shade and the hit's");
    // RT-HIT-SHADING-4: the code's sign is the weight's majority ON ANOTHER CAR, whatever the camera sees
    for (auto& share : shares) {
      share = rt::MakeShare(1.0f, length, hidden, surface, true);
    }
    shares[3] = rt::MakeShare(1.0f, length, seen, surface, false);
    all = rt::CombineRays(shares, 4, 4.0f);
    Check(all.kind == rt::Kind::kOtherCar && Near(all.weight, (3.0 * 0.85 + 1.0) / 4.0), "three hits on another car and one on the player's own part: another car, by the majority of the weight");
    shares[0] = rt::MakeShare(1.0f, length, seen, surface, false);
    shares[1] = rt::MakeShare(1.0f, length, seen, surface, false);
    shares[2] = rt::MakeShare(1.0f, length, seen, surface, true);
    shares[3] = rt::MakeShare(1.0f, length, seen, surface, true);
    all = rt::CombineRays(shares, 4, 4.0f);
    Check(all.kind == rt::Kind::kOtherCar, "half the weight on another car is another car (the sign's rule is >= a half)");
    shares[1] = rt::MakeShare(1.0f, length, seen, surface, true);
    shares[2] = rt::MakeShare(1.0f, length, seen, surface, false);
    shares[3] = rt::MakeShare(1.0f, length, seen, surface, false);
    shares[0] = rt::MakeShare(1.0f, length, seen, surface, false);
    all = rt::CombineRays(shares, 4, 4.0f);
    Check(all.kind == rt::Kind::kHit, "a quarter of the weight on another car: the player's own");
    rt::RayShare faint;
    faint.weight = 0.001f;
    faint.rgb[0] = 1.0f;
    Check(rt::CombineRays(&faint, 1, 4.0f).kind == rt::Kind::kMiss, "a faint weight (under 1/512) is a miss");
  }

  // the paint's weights: the quadratic B-spline
  {
    bool sums = true;
    bool mirrored = true;
    bool continuous = true;
    for (float d = -0.5f; d <= 0.5001f; d += 0.0625f) {
      float w[3];
      float m[3];
      rt::BSplineWeights(d, w);
      rt::BSplineWeights(-d, m);
      sums = sums && Near(w[0] + w[1] + w[2], 1.0, 1e-6) && w[0] >= 0.0f && w[1] >= 0.0f && w[2] >= 0.0f;
      mirrored = mirrored && Near(w[0], m[2], 1e-6) && Near(w[1], m[1], 1e-6);
    }
    // moving the place across the half way between two texels changes which texel is the nearest, and nothing about the weights of the texels
    float at_edge[3];
    float next_edge[3];
    rt::BSplineWeights(0.5f, at_edge);   // texel n nearest, the place is half way to n + 1
    rt::BSplineWeights(-0.5f, next_edge);  // texel n + 1 nearest, the same place
    continuous = Near(at_edge[1], next_edge[0]) && Near(at_edge[2], next_edge[1]) && Near(at_edge[0], 0.0) && Near(next_edge[2], 0.0);
    // and the slope too (a bilinear read's weights have a corner there)
    const float step = 1e-3f;
    float lo[3], hi[3], lo2[3], hi2[3];
    rt::BSplineWeights(0.5f - step, lo);
    rt::BSplineWeights(0.5f, hi);
    rt::BSplineWeights(-0.5f, lo2);
    rt::BSplineWeights(-0.5f + step, hi2);
    const float slope_before = (hi[2] - lo[2]) / step;  // the texel n + 1's weight, texel n nearest
    const float slope_after = (hi2[1] - lo2[1]) / step;  // the same texel, now the nearest
    Check(sums && mirrored && continuous, "the B-spline's weights: they add up to 1, mirror, and carry over from one nearest texel to the next");
    Check(std::fabs(slope_before - slope_after) < 0.01f, "the B-spline's weights have no corner at the change of the nearest texel (their slope is continuous)");
    float d = 0.0f;
    Check(rt::NearestTexel(3.4f, &d) == 3 && Near(d, 0.4, 1e-5) && rt::NearestTexel(3.5f, &d) == 4 && Near(d, -0.5, 1e-5) && rt::NearestTexel(-0.2f, &d) == 0 && Near(d, -0.2, 1e-5) &&
              rt::NearestTexel(7.0f, &d) == 7 && d == 0.0f,
          "the nearest texel of a place and the offset from its centre, texel i centred at i");
  }

  // what the paint reads
  {
    float wx[3], wy[3];
    rt::BSplineWeights(0.0f, wx);
    rt::BSplineWeights(0.0f, wy);
    const float depth = 5.0f;
    const float tolerance = 0.06f;
    auto tap = [](float history, rt::Kind kind, float w, float r) {
      rt::PaintTap t;
      t.history_distance = history;
      t.alpha = rt::EncodeAlpha(kind, w);
      t.rgb[0] = t.rgb[1] = t.rgb[2] = r;
      return t;
    };
    auto nine = [](const rt::PaintTap& each) {
      std::array<rt::PaintTap, 9> out;
      out.fill(each);
      return out;
    };
    // all nine are the player's car at the paint's distance and hit with weight 0.8, colour 0.4: the answer
    {
      auto all = nine(tap(5.0f, rt::Kind::kHit, 0.8f, 0.4f));
      const rt::PaintRead read = rt::ReadForPaint(wx, wy, all.data(), depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kHit && Near(read.weight, 0.8, 1e-5) && Near(read.rgb[0], 0.4, 1e-5), "nine hits: their weight and colour");
    }
    // the middle column hits (weight 1, colour 0.6), the sides miss: the weight is the B-spline's share of the middle column (0.75 at the middle), the colour the hits' own
    {
      auto grid = nine(tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f));
      for (int k : {1, 4, 7}) {
        grid[k] = tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f);
      }
      const rt::PaintRead read = rt::ReadForPaint(wx, wy, grid.data(), depth, tolerance);
      Check(read.use && Near(read.weight, 0.75, 1e-5) && Near(read.rgb[0], 0.6, 1e-5),
            "a column of hits and two of misses: the weight is the middle column's share of the footprint, and the miss's black does not darken the hit's colour");
    }
    // all misses: used (the environment stays), no weight
    {
      auto all = nine(tap(5.0f, rt::Kind::kMiss, 0.0f, 0.0f));
      const rt::PaintRead read = rt::ReadForPaint(wx, wy, all.data(), depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kMiss && read.weight == 0.0f, "nine misses: the image answers with no weight, so the march is not run");
    }
    // texels on another surface (a part in front, a neighbouring panel): not counted
    {
      auto grid = nine(tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f));
      grid[1] = tap(3.0f, rt::Kind::kHit, 1.0f, 0.1f);
      grid[5] = tap(3.0f, rt::Kind::kHit, 1.0f, 0.1f);
      grid[8] = tap(0.0f, rt::Kind::kNoData, 0.0f, 0.0f);
      const rt::PaintRead read = rt::ReadForPaint(wx, wy, grid.data(), depth, tolerance);
      Check(read.use && Near(read.weight, 1.0, 1e-5) && Near(read.rgb[0], 0.6, 1e-5),
            "texels on another surface, or on none, are left out of the mean (the part in front's colour does not leak)");
    }
    // the footprint's data is under half of the matching texels: the march
    {
      auto grid = nine(tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f));
      grid[0] = tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f);
      Check(!rt::ReadForPaint(wx, wy, grid.data(), depth, tolerance).use, "only a corner texel (a sixty-fourth of the footprint) has data: the march answers (no seam of half-lit pixels)");
      grid[4] = tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f);
      Check(rt::ReadForPaint(wx, wy, grid.data(), depth, tolerance).use, "the middle texel (more than half of the footprint) has data: the image answers");
      auto most = nine(tap(5.0f, rt::Kind::kHit, 1.0f, 0.6f));
      most[0] = most[2] = most[6] = tap(5.0f, rt::Kind::kNoData, 0.0f, 0.0f);
      Check(rt::ReadForPaint(wx, wy, most.data(), depth, tolerance).use, "six of nine have data: the image answers");
    }
    // no texel is the paint's surface: the march (the paint's own agreement test has failed as well)
    {
      auto all = nine(tap(9.0f, rt::Kind::kHit, 1.0f, 0.6f));
      Check(!rt::ReadForPaint(wx, wy, all.data(), depth, tolerance).use, "no texel is the surface the paint is on: no answer");
    }
    // hits on another car: the sign (the clear coat's layer follows the share of the weight that is on a car)
    {
      auto grid = nine(tap(5.0f, rt::Kind::kOtherCar, 0.5f, 0.2f));
      grid[8] = tap(5.0f, rt::Kind::kHit, 0.5f, 0.9f);
      const rt::PaintRead read = rt::ReadForPaint(wx, wy, grid.data(), depth, tolerance);
      Check(read.use && read.kind == rt::Kind::kOtherCar && Near(read.weight, 0.5, 1e-5), "mostly hits on another car are a hit on another car, at their weight");
      Check(read.car > 0.9f && read.car < 1.0f, "and the share of the weight that is on a car is what the layer takes (0.9 to 1 with one ninth of the weight on the player's own part)");
      const auto none = nine(tap(5.0f, rt::Kind::kHit, 0.5f, 0.2f));
      const rt::PaintRead own = rt::ReadForPaint(wx, wy, none.data(), depth, tolerance);
      Check(own.use && own.kind == rt::Kind::kHit && own.car == 0.0f, "hits on the player's own parts have no car share: no layer");
      const auto all_car = nine(tap(5.0f, rt::Kind::kOtherCar, 0.5f, 0.2f));
      Check(Near(rt::ReadForPaint(wx, wy, all_car.data(), depth, tolerance).car, 1.0, 1e-5), "all of it on a car: a share of 1");
    }
  }

  // THE EDGE: a straight edge of a reflected part crossing the image, through the pass's cone (the texels' own turns of the pattern) and the paint's B-spline.
  // The model is flat: texel i, row j, ray `ray` hits where the position i + disc.x x width < `edge` (the cone is `width` texels wide at the hit's distance),
  // weight = hits / 4, colour 1; the paint reads at places along the row. What the first version's edge was (one ray, a gloss of 0) is the comparison.
  {
    auto weight_at = [](float place, float edge, float width, int row_phase) {
      float wx[3], wy[3];
      float d = 0.0f;
      const int n = rt::NearestTexel(place, &d);
      rt::BSplineWeights(d, wx);
      rt::BSplineWeights(0.0f, wy);
      rt::PaintTap taps[9];
      for (int k = 0; k < 9; ++k) {
        const int tx = n + (k % 3) - 1;
        const int ty = 20 + (k / 3) - 1;
        int hits = 0;
        int cast = 4;
        if (width <= 0.0f) {
          hits = float(tx) < edge ? 1 : 0;
          cast = 1;
        } else {
          const float turns = rt::ConeRotation(uint32_t(tx + 100), uint32_t(ty + row_phase), 0);
          for (int ray = 0; ray < 4; ++ray) {
            hits += float(tx) + rt::ConeDisc(ray, turns).x * width < edge ? 1 : 0;
          }
        }
        taps[k].history_distance = 5.0f;
        taps[k].alpha = rt::EncodeAlpha(hits > 0 ? rt::Kind::kHit : rt::Kind::kMiss, float(hits) / float(cast));
        taps[k].rgb[0] = taps[k].rgb[1] = taps[k].rgb[2] = 1.0f;
      }
      return rt::ReadForPaint(wx, wy, taps, 5.0f, 0.06f).weight;
    };
    for (float width : {0.0f, 3.0f, 6.0f}) {
      float widest = 0.0f;      // the largest change of the weight between two reads a quarter texel apart
      float run_start = -1.0f;  // the places where the weight is between 0.1 and 0.9: the edge's ramp
      float run_end = -1.0f;
      float backwards = 0.0f;   // the worst the weight goes back up as the place moves away from the part
      float previous = weight_at(10.0f, 40.0f, width, 0);
      for (float place = 10.0f; place < 70.0f; place += 0.25f) {
        const float w = weight_at(place, 40.0f, width, 0);
        widest = std::max(widest, std::fabs(w - previous));
        if (w < previous) {
          // the part is to the left of the edge: weight 1 left, 0 right: it should only fall as the place moves right
        } else {
          backwards = std::max(backwards, w - previous);
        }
        if (w > 0.1f && w < 0.9f) {
          if (run_start < 0.0f) {
            run_start = place;
          }
          run_end = place;
        }
        previous = w;
      }
      const float ramp = run_end >= 0.0f ? run_end - run_start + 0.25f : 0.0f;
      if (width == 0.0f) {
        Check(ramp < 1.6f && widest > 0.18f, "one ray per texel (a gloss of 0): a ramp of a texel and a quarter (2.5 pixels at 4K) and a step of nearly a fifth of the "
                                              "weight between reads a quarter texel apart (measured " + std::to_string(widest) + ", ramp " + std::to_string(ramp) + ")");
      } else {
        Check(ramp > 0.9f * width && ramp < 2.2f * width && widest < 0.13f && backwards < 0.12f,
              "the cone's edge (width " + std::to_string(width) + " texels): a ramp as wide as the cone (" + std::to_string(ramp) + " texels), no step above 0.13 between "
              "reads a quarter texel apart (" + std::to_string(widest) + "), and no ripple back (" + std::to_string(backwards) + ")");
      }
    }
  }

  // THE DENOISE: the aux image's codes, the pass count, the strength by the cone's width
  {
    Check(rt::EncodeHitDistance(0.0f, 4.0f) == 0.0f && rt::EncodeHitDistance(-1.0f, 4.0f) == 0.0f, "no hit: the aux's distance is 0");
    Check(rt::EncodeHitDistance(1e-6f, 4.0f) >= 1.0f / 255.0f, "a hit never rounds to 'no hit'");
    Check(rt::EncodeHitDistance(40.0f, 4.0f) == 1.0f, "a distance past the reach saturates");
    for (float d : {0.02f, 0.1f, 0.5f, 1.0f, 2.5f, 4.0f}) {
      const float back = rt::DecodeHitDistance(rt::EncodeHitDistance(d, 4.0f), 4.0f);
      Check(std::fabs(back - d) <= 0.03f * d + 0.001f, "the hit distance " + std::to_string(d) + " m survives 8 bits within 3% (came back " + std::to_string(back) + ")");
    }
    Check(rt::EncodeInstance(0) == 0.0f && rt::InstanceOfCode(rt::EncodeInstance(25)) == 25 && rt::InstanceOfCode(rt::EncodeInstance(300)) == 255,
          "an instance number is exact up to 255 in 8 bits, and clamps");
    Check(rt::DenoisePassCount(2, 0.0437f) == 2 && rt::DenoisePassCount(1, 0.0437f) == 1 && rt::DenoisePassCount(0, 0.0437f) == 0 &&
              rt::DenoisePassCount(9, 0.0437f) == 2 && rt::DenoisePassCount(-3, 0.0437f) == 0 && rt::DenoisePassCount(2, 0.0f) == 0,
          "the passes: the setting clamped to 0 .. 2, none for a mirror (one ray, no noise)");
    Check(rt::DenoiseStep(0) == 1 && rt::DenoiseStep(1) == 2, "the a-trous steps are 1 and 2 texels");
    // a 2.5 degree cone 0.49 m from a panel 5 m away (a texel is 5.3 mm there) is 8 texels wide
    const float texel_m = rt::PixelMeters(5.0f, kTanY, kImageHeight);
    const float footprint = rt::ConeFootprint(0.49f, rt::ConeTan(2.5f), texel_m);
    Check(footprint > 7.5f && footprint < 8.5f, "the cone's width at a hit half a metre away is about 8 texels at 5 m in a 1080 row image (" + std::to_string(footprint) + ")");
    Check(rt::DenoiseStrength(0.0f, 0) == 0.0f && rt::DenoiseStrength(rt::kDenoiseFootprintFrom, 0) == 0.0f && rt::DenoiseStrength(rt::kDenoiseFootprintTo, 0) == 1.0f &&
              rt::DenoiseStrength(50.0f, 0) == 1.0f && rt::DenoiseStrength(1.2f, 0) > 0.1f && rt::DenoiseStrength(1.2f, 0) < 0.9f,
          "the first pass's strength: 0 under the narrow cone's width, 1 from the wide one's, between them smooth");
    Check(rt::DenoiseStrength(3.0f, 1) == 0.0f && rt::DenoiseStrength(rt::kDenoiseSecondFrom, 1) == 0.0f && rt::DenoiseStrength(rt::kDenoiseSecondTo, 1) == 1.0f &&
              rt::DenoiseStrength(6.0f, 1) > 0.1f && rt::DenoiseStrength(6.0f, 1) < 0.9f,
          "the second pass (the wider kernel) starts later: nothing under 4 texels of cone, full from 10");
    float sum = 0.0f;
    for (int k = 0; k < 9; ++k) {
      sum += rt::DenoiseKernel(k % 3 - 1, k / 3 - 1);
    }
    Check(sum == 16.0f && rt::DenoiseKernel(0, 0) == 4.0f && rt::DenoiseKernel(1, 0) == 2.0f && rt::DenoiseKernel(-1, 1) == 1.0f, "the kernel is 1 2 1 / 2 4 2 / 1 2 1");
  }

  // THE DENOISE, ONE TEXEL: what its edge stops do
  {
    rt::DenoiseSetup setup;
    setup.tan_y = kTanY;
    setup.image_height = kImageHeight;
    setup.tan_half = rt::ConeTan(2.5f);
    setup.reach = kReach;
    setup.pass = 0;
    const float wide_hit = rt::EncodeHitDistance(0.5f, kReach);  // a cone 8 texels wide at the hit: the filter is applied in full
    auto make = [&](float alpha_other, float rgb_other, float distance_other, int instance_other, float hit_other, bool inside_other) {
      std::array<rt::DenoiseTap, 9> taps;
      for (int k = 0; k < 9; ++k) {
        taps[size_t(k)].inside = true;
        taps[size_t(k)].alpha = k == 4 ? rt::EncodeAlpha(rt::Kind::kHit, 1.0f) : alpha_other;
        taps[size_t(k)].rgb[0] = taps[size_t(k)].rgb[1] = taps[size_t(k)].rgb[2] = k == 4 ? 1.0f : rgb_other;
        taps[size_t(k)].history_distance = k == 4 ? kPanelDistance : distance_other;
        taps[size_t(k)].hit_code = k == 4 ? wide_hit : hit_other;
        taps[size_t(k)].instance = k == 4 ? 3 : instance_other;
        taps[size_t(k)].inside = k == 4 || inside_other;
      }
      return taps;
    };
    const float miss = rt::EncodeAlpha(rt::Kind::kMiss, 0.0f);
    // eight misses around a full hit: a miss counts as weight 0, and the colour is not darkened by it
    {
      const auto taps = make(miss, 0.0f, kPanelDistance, 3, 0.0f, true);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      const rt::Texel t = rt::DecodeAlpha(o.alpha);
      Check(t.kind == rt::Kind::kHit && Near(t.weight, 4.0 / 16.0, 0.01), "a lone hit among misses of the same surface: its weight is the kernel's share, 4/16 (" + std::to_string(t.weight) + ")");
      Check(Near(o.rgb[0], 1.0, 0.005) && Near(o.rgb[1], 1.0, 0.005), "and its colour is the hit's own, not darkened by the misses' black");
    }
    // another instance: the same distance does not make it the same panel
    {
      const auto taps = make(miss, 0.0f, kPanelDistance, 4, 0.0f, true);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(rt::DecodeAlpha(o.alpha).weight == 1.0f && o.rgb[0] == 1.0f, "the taps of another instance are left out: the texel keeps weight 1");
    }
    // another surface: 0.4 m further (a part behind)
    {
      const auto taps = make(miss, 0.0f, kPanelDistance + 0.4f, 3, 0.0f, true);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(rt::DecodeAlpha(o.alpha).weight == 1.0f, "the taps at another distance are left out");
    }
    // no data around the texel: nothing to average with
    {
      const auto taps = make(0.0f, 0.0f, 0.0f, 0, 0.0f, true);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(rt::DecodeAlpha(o.alpha).weight == 1.0f && o.rgb[0] == 1.0f, "no data around: the texel is as it was, and no data is not spread");
    }
    // outside the image
    {
      const auto taps = make(miss, 0.0f, kPanelDistance, 3, 0.0f, false);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(rt::DecodeAlpha(o.alpha).weight == 1.0f, "taps outside the image are left out");
    }
    // a narrow cone (a part 2 cm away: under half a texel wide): the texel is not touched, though every tap would be taken
    {
      auto taps = make(miss, 0.0f, kPanelDistance, 3, 0.0f, true);
      taps[4].hit_code = rt::EncodeHitDistance(0.02f, kReach);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(o.alpha == rt::EncodeAlpha(rt::Kind::kHit, 1.0f) && o.rgb[0] == 1.0f, "a cone narrower than a texel's half at its hit leaves the texel alone");
    }
    // the neighbour's hit gives a miss texel the strength: a texel whose rays all missed beside a wide penumbra takes its share of it
    {
      auto taps = make(rt::EncodeAlpha(rt::Kind::kHit, 1.0f), 1.0f, kPanelDistance, 3, wide_hit, true);
      taps[4].alpha = miss;
      taps[4].rgb[0] = taps[4].rgb[1] = taps[4].rgb[2] = 0.0f;
      taps[4].hit_code = 0.0f;
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(rt::DecodeAlpha(o.alpha).weight > 0.5f, "a texel whose rays all missed, among texels that hit with a wide cone, takes their share of the weight (the penumbra's tail)");
    }
    // another car against the player's own: the majority of the weight decides the sign, and the colour is the weighted mean
    {
      auto taps = make(rt::EncodeAlpha(rt::Kind::kOtherCar, 1.0f), 0.2f, kPanelDistance, 3, wide_hit, true);
      const rt::DenoiseOut o = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.8f, setup);
      Check(o.alpha < -1.0f && Near(o.rgb[0], (4.0 * 1.0 + 12.0 * 0.2) / 16.0, 0.01), "a hit on the player's own part among hits on another car (12/16 of the weight): another car, and the colours are mixed by weight");
    }
    // a slope: the depth changes 17 mm a texel at N.V 0.3, and the taps of step 2 are still the same surface
    {
      auto taps = make(miss, 0.0f, kPanelDistance, 3, 0.0f, true);
      taps[4].hit_code = rt::EncodeHitDistance(2.0f, kReach);  // a cone 33 texels wide: both passes in full
      for (int k = 0; k < 9; ++k) {
        taps[size_t(k)].history_distance = kPanelDistance + 0.017f * float(k % 3 - 1) * 2.0f;  // the depth falls along x, 17 mm a texel, taps 2 texels apart
      }
      setup.pass = 1;
      const rt::DenoiseOut steep = rt::DenoiseTexel(taps.data(), kPanelDistance, 0.3f, setup);
      const rt::DenoiseOut head_on = rt::DenoiseTexel(taps.data(), kPanelDistance, 1.0f, setup);
      setup.pass = 0;
      Check(Near(rt::DecodeAlpha(steep.alpha).weight, 4.0 / 16.0, 0.01), "a steep panel's taps (34 mm apart in depth at step 2, N.V 0.3) are the same surface and all count (4/16, " +
                                                                              std::to_string(rt::DecodeAlpha(steep.alpha).weight) + ")");
      Check(rt::DecodeAlpha(head_on.alpha).weight > rt::DecodeAlpha(steep.alpha).weight + 0.1f,
            "the same depths on a panel seen head-on (N.V 1: no slope's slack) are mostly another surface and left out");
    }
  }

  // THE DENOISE ON A PENUMBRA: the whole filter over an image of a straight edge, as the paint reads it
  {
    const float tan_half = rt::ConeTan(2.5f);
    // the cone's half width in texels -> the hit distance (metres) that gives it at the panel's distance: width = 2 d tan / texel
    const float texel_m = rt::PixelMeters(kPanelDistance, kTanY, kImageHeight);
    const float edge = 48.37f;  // not on a texel's boundary (a boundary makes every texel's pattern a half-half split: no grain at all)
    for (float radius : {0.15f, 1.0f, 2.0f, 4.0f, 6.0f, 9.6f}) {
      const float width = 2.0f * radius;
      const float hit_distance = width * texel_m / (2.0f * tan_half);
      const Image raw = MakeEdgeScene(96, 64, edge, radius, hit_distance, 0, false);
      const Image both = Denoise(raw, 2, tan_half);
      const Image first = Denoise(raw, 1, tan_half);
      const EdgeStats s0 = MeasureEdge(raw, edge, radius);
      const EdgeStats s1 = MeasureEdge(first, edge, radius);
      const EdgeStats s2 = MeasureEdge(both, edge, radius);
      std::printf("  cone %5.1f texels wide (strength %.2f, %.2f): grain %.4f -> %.4f (one pass %.4f), ramp %.2f -> %.2f, 50%% at %.2f (edge %.2f)\n", width,
                  rt::DenoiseStrength(width, 0), rt::DenoiseStrength(width, 1), s0.grain, s2.grain, s1.grain, s0.ramp, s2.ramp, s2.crossing, edge);
      if (width < rt::kDenoiseFootprintFrom) {
        Check(both.alpha == raw.alpha && both.rgb == raw.rgb,
              "a cone narrower than " + std::to_string(rt::kDenoiseFootprintFrom) + " texels (a part a few cm off the panel): the image is left exactly as the trace made it");
      } else if (width >= 4.0f) {
        // The turn is the blue noise tile's (NOISE-AUDIT), no longer interleaved gradient noise: the gradient noise is even over any 3 x 3 block, which the first pass of the filter (all a 4 texel cone gets)
        // takes out well, but a lattice that the filter leaves as thin diagonal lines (car_refl_noise_test.cpp: 9% of the error in one frequency, a direction 4 times the mean, against 0.6% and 2).
        // The blue tile leaves a little more grain at the narrowest cone (0.015 against 0.010) and the same, or less, from 8 texels (0.010 against 0.0085).
        Check(s2.grain <= 0.55 * s0.grain && s2.grain < 0.017,
              "cone " + std::to_string(width) + " texels wide: the grain along the edge falls to under 0.55 of the raw's and under 0.017 (" + std::to_string(s0.grain) + " -> " + std::to_string(s2.grain) + ")");
        Check(s2.ramp <= 1.3 * s0.ramp + 0.2,
              "cone " + std::to_string(width) + " texels wide: the edge's ramp (0.9 to 0.1) grows by at most 30% (" + std::to_string(s0.ramp) + " -> " + std::to_string(s2.ramp) + ")");
        Check(std::fabs(s2.crossing - double(edge)) < 0.12, "cone " + std::to_string(width) + " texels wide: the edge stays where it was (50% at " + std::to_string(s2.crossing) + ")");
      }
      if (width >= 2.0f) {
        Check(s1.grain < 0.6 * s0.grain + 0.003, "one pass alone takes a good part of the grain out too (" + std::to_string(s0.grain) + " -> " + std::to_string(s1.grain) + ")");
      }
    }
    // the weight is conserved by the filter (a straight edge across a flat panel): the sums over the image agree within 1.5%; a colour is not darkened
    {
      const Image raw = MakeEdgeScene(96, 64, edge, 6.0f, 0.74f, 0, true);
      const Image both = Denoise(raw, 2, tan_half);
      double before = 0.0;
      double after = 0.0;
      double worst_hue = 0.0;
      for (size_t i = 0; i < raw.alpha.size(); ++i) {
        before += rt::DecodeAlpha(raw.alpha[i]).weight;
        const rt::Texel t = rt::DecodeAlpha(both.alpha[i]);
        after += t.weight;
        if (t.weight > 0.02f) {
          worst_hue = std::max({worst_hue, std::fabs(double(both.rgb[i][0]) - 0.5) / 0.5, std::fabs(double(both.rgb[i][1]) - 0.3) / 0.3, std::fabs(double(both.rgb[i][2]) - 0.2) / 0.2});
        }
      }
      Check(std::fabs(after - before) < 0.015 * before, "the weight is conserved by the two passes (" + std::to_string(before) + " -> " + std::to_string(after) + ")");
      Check(worst_hue < 0.01, "a single colour stays that colour wherever the weight is more than 0.02 (premultiplied sums: the misses' black does not darken it); worst " + std::to_string(worst_hue));
    }
    // the stops, over a whole image: a hit panel next to another instance, another distance, or no data: not one texel of it is touched by the other side
    for (int other = 0; other < 3; ++other) {
      Image img;
      img.Resize(64, 32);
      for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
          const size_t i = img.At(x, y);
          img.nv[i] = 0.8f;
          if (x < 32) {
            img.alpha[i] = rt::EncodeAlpha(rt::Kind::kHit, 1.0f);
            img.rgb[i] = {1.0f, 0.0f, 0.0f};
            img.distance[i] = kPanelDistance;
            img.instance[i] = 3;
            img.hit[i] = rt::EncodeHitDistance(1.0f, kReach);
          } else if (other < 2) {
            img.alpha[i] = rt::EncodeAlpha(rt::Kind::kMiss, 0.0f);
            img.distance[i] = other == 0 ? kPanelDistance : kPanelDistance + 0.5f;
            img.instance[i] = other == 0 ? 4 : 3;
          }  // other == 2: no data at all (alpha 0, no vehicle)
        }
      }
      const Image out = Denoise(img, 2, rt::ConeTan(2.5f));
      bool left_whole = true;
      bool right_whole = true;
      for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
          const size_t i = img.At(x, y);
          if (x < 32) {
            left_whole = left_whole && out.alpha[i] == rt::EncodeAlpha(rt::Kind::kHit, 1.0f) && out.rgb[i][0] == 1.0f && out.rgb[i][1] == 0.0f;
          } else {
            right_whole = right_whole && out.alpha[i] == img.alpha[i] && out.rgb[i] == img.rgb[i];
          }
        }
      }
      const char* what[] = {"another instance at the same distance", "the same instance at another distance (0.5 m)", "no data"};
      Check(left_whole && right_whole, std::string("a hit panel beside ") + what[other] + ": the panel is untouched (weight 1, its colour), and the other side is exactly as it was");
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
    Check(cr::kMaxView == 7, "the debug view has a fifth value, the image, and (version 8) a sixth and seventh, the clear coat's layer");
    const cr::CarSelfReflConstants coat = cr::MakeConstants(cr::Mode::kReflection, tuning, matrix, 77, 3, 1.0f, 1234, true);
    Check((coat.c[3] & cr::kRtImageFlag) != 0 && (coat.c[3] & cr::kCoatLayerFlag) != 0 && cr::kCoatLayerFlag == 2u && (on.c[3] & cr::kCoatLayerFlag) == 0 &&
              (cr::MakeConstants(cr::Mode::kReflection, tuning, matrix, 77, 3, 1.0f).c[3] & cr::kCoatLayerFlag) == 0,
          "the coat layer's flag is bit 1 of c[3], set only with the image and when asked");
  }

  // the other cars (hlsl/car_rt_refl_cs.hlsl OtherCarColour; car_rt_instances_rules.h)
  {
    Check(rt::kPartOthers == 0x10 && rt::kPartAll == 0x1F, "the other cars' part is 16 and the parts' mask has five bits");
    Check((rt::kPrimaryParts & rt::kPartOthers) == 0, "the primary ray never finds another car");
    Check((rt::kDefaultReflectionParts & rt::kPartOthers) != 0 && rt::kDefaultReflectionParts == 0x17, "the reflection ray hits them by default (23)");
    Check((rt::kDefaultReflectionParts & ~rt::kPartDriver & rt::kPartAll) == rt::kDefaultReflectionParts, "and not the driver");

    // The taps: tap k at the offset (k % 3 - 1, k / 3 - 1); every texel at the centre's distance 10 holds the colour (0.5, 0.25, 0.125).
    rt::SurfaceTap taps[9];
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 10.0f;
      tap.rgb[0] = 0.5f;
      tap.rgb[1] = 0.25f;
      tap.rgb[2] = 0.125f;
    }
    rt::OtherCarColour all = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(Near(all.share, 1.0, 1e-5) && all.known() && Near(all.rgb[0], 0.5, 1e-5) && Near(all.rgb[1], 0.25, 1e-5) && Near(all.rgb[2], 0.125, 1e-5),
          "every tap at the centre's distance: the whole share, the colour");
    // The depth window: kOtherCarDepth radii (1.92 m at a radius of 1.6): full weight to half of it, nothing at it.
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 10.0f + 0.9f;
    }
    Check(Near(rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f).share, 1.0, 1e-5), "within half the window every tap counts in full");
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 10.0f + 1.92f;
    }
    const rt::OtherCarColour edge = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(edge.share < 1e-3f && !edge.known() && edge.rgb[0] == 0.0f, "at the window's edge the taps are no longer the car's: no colour");
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 14.0f;  // the road or a car behind it: another surface
    }
    Check(!rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f).known(), "a surface far behind the car is not the car");
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 0.0f;  // no vehicle here (the history's alpha 0)
    }
    const rt::OtherCarColour none = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(none.share == 0.0f && !none.known() && none.rgb[0] == 0.0f && none.rgb[1] == 0.0f, "no vehicle texel: share 0, black");
    // The left column only (the taps 0, 3, 6 hold the car): the tent weights 1/2 x (1/2 + 1 + 1/2) over the sum 4.
    for (int k = 0; k < 9; ++k) {
      taps[k].distance = (k % 3) == 0 ? 10.0f : 0.0f;
      taps[k].rgb[0] = (k % 3) == 0 ? 0.8f : 0.0f;
    }
    const rt::OtherCarColour column = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(Near(column.share, 0.25, 1e-5) && column.known() && Near(column.rgb[0], 0.8, 1e-5), "a column of taps: a quarter of the share, the colour of those");
    // Only the middle tap (the centre's own place): 1/4 of the sum.
    for (int k = 0; k < 9; ++k) {
      taps[k].distance = k == 4 ? 10.0f : 0.0f;
      taps[k].rgb[0] = k == 4 ? 0.3f : 0.0f;
    }
    const rt::OtherCarColour middle = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(Near(middle.share, 0.25, 1e-5) && Near(middle.rgb[0], 0.3, 1e-5), "the middle tap alone: a quarter of the share");
    // A corner alone is 1/16 of the sum: under kOtherCarMinShare, no colour.
    for (int k = 0; k < 9; ++k) {
      taps[k].distance = k == 0 ? 10.0f : 0.0f;
      taps[k].rgb[0] = k == 0 ? 0.3f : 0.0f;
    }
    const rt::OtherCarColour corner = rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f);
    Check(Near(corner.share, 0.0625, 1e-5) && !corner.known(), "a corner alone is too little to be the car's colour");
    // A car placed from memory (a negative radius in its record): the history holds nothing of it, whatever the taps hold; it is known in full, a fixed grey.
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 0.0f;
      tap.rgb[0] = tap.rgb[1] = tap.rgb[2] = 0.9f;
    }
    const rt::OtherCarColour remembered = rt::FindOtherCarColour(taps, 10.0f, -1.6f, nullptr, 1.0f);
    Check(remembered.known() && Near(remembered.share, 1.0, 1e-6) && Near(remembered.rgb[0], rt::kRememberedCarColour, 1e-6) &&
              Near(remembered.rgb[1], rt::kRememberedCarColour, 1e-6) && Near(remembered.rgb[2], rt::kRememberedCarColour, 1e-6),
          "a car placed from memory (negative radius): the fixed grey, known in full, the taps not looked at");
    Check(rt::kRememberedCarColour > 0.0f && rt::kRememberedCarColour < 1.0f, "the grey is a colour");
    // the same car with its paint known (the record's second row): its hidden hits take the paint, the grey is the fallback
    const float paint[3] = {0.063f, 0.349f, 0.071f};
    const rt::OtherCarColour painted = rt::FindOtherCarColour(taps, 10.0f, -1.6f, paint, 1.0f);
    Check(painted.known() && Near(painted.share, 1.0, 1e-6) && Near(painted.rgb[0], 0.063, 1e-6) && Near(painted.rgb[1], 0.349, 1e-6) && Near(painted.rgb[2], 0.071, 1e-6),
          "a car the camera does not draw, with its paint known: the paint, known in full");
    // A car the camera draws (a positive radius) whose taps hold nothing (the history has no vehicle there): its paint stands in, in full (RT-MODEL-CARS-3: the neighbour
    // the game draws beside the player while the camera looks at the player's door); with no paint, no colour, as before.
    const rt::OtherCarColour drawn_unseen = rt::FindOtherCarColour(taps, 10.0f, 1.6f, paint, 1.0f);
    Check(drawn_unseen.known() && Near(drawn_unseen.share, 1.0, 1e-6) && Near(drawn_unseen.rgb[0], 0.063, 1e-6) && Near(drawn_unseen.rgb[2], 0.071, 1e-6),
          "a car the camera draws but the history shows nothing of, with its paint known: the paint, known in full");
    Check(!rt::FindOtherCarColour(taps, 10.0f, 1.6f, nullptr, 1.0f).known(), "... and with no paint known: no colour, as before");
    // its centre behind the camera, or no centre in its record (a radius of 0): no taps are looked at, the paint or nothing
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = -3.0f;
      tap.rgb[0] = tap.rgb[1] = tap.rgb[2] = 0.9f;
    }
    Check(rt::FindOtherCarColour(taps, -3.0f, 1.6f, paint, 1.0f).known() && Near(rt::FindOtherCarColour(taps, -3.0f, 1.6f, paint, 1.0f).rgb[0], 0.063, 1e-6),
          "a centre behind the camera: the paint");
    Check(!rt::FindOtherCarColour(taps, -3.0f, 1.6f, nullptr, 1.0f).known() && rt::FindOtherCarColour(taps, -3.0f, 1.6f, nullptr, 1.0f).rgb[0] == 0.0f, "a centre behind the camera, no paint: no colour");
    Check(Near(rt::FindOtherCarColour(taps, 10.0f, 0.0f, paint, 1.0f).rgb[1], 0.349, 1e-6) && !rt::FindOtherCarColour(taps, 10.0f, 0.0f, nullptr, 1.0f).known(), "a record with no centre (radius 0): the paint or nothing");
    // where the history shows the car, its colour stays, whatever the paint row says
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 10.0f;
      tap.rgb[0] = 0.5f;
      tap.rgb[1] = 0.25f;
      tap.rgb[2] = 0.125f;
    }
    const rt::OtherCarColour seen = rt::FindOtherCarColour(taps, 10.0f, 1.6f, paint, 1.0f);
    Check(Near(seen.share, 1.0, 1e-5) && Near(seen.rgb[0], 0.5, 1e-5) && Near(seen.rgb[1], 0.25, 1e-5), "a car the camera draws and the history shows keeps the colour of the history, whatever its paint row says");

    // UNITS (RT-MODEL-CARS-4). The image is in the history's units (the target's: the shader's output times 2^bias), the paint's constants and the grey are in the paint's (the
    // shader's output): at the game's bias of 4 they are multiplied by 16; what the history shows is the history's already and is not multiplied.
    const float units = 16.0f;
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 0.0f;
      tap.rgb[0] = tap.rgb[1] = tap.rgb[2] = 0.9f;
    }
    const rt::OtherCarColour grey16 = rt::FindOtherCarColour(taps, 10.0f, -1.6f, nullptr, units);
    Check(Near(grey16.rgb[0], rt::kRememberedCarColour * 16.0, 1e-5) && Near(grey16.rgb[2], rt::kRememberedCarColour * 16.0, 1e-5),
          "the grey of a car placed from memory is multiplied by the units");
    const rt::OtherCarColour paint16 = rt::FindOtherCarColour(taps, 10.0f, -1.6f, paint, units);
    Check(Near(paint16.rgb[0], 0.063 * 16.0, 1e-5) && Near(paint16.rgb[1], 0.349 * 16.0, 1e-5) && Near(paint16.rgb[2], 0.071 * 16.0, 1e-5) && paint16.known(),
          "the paint of a car placed from memory is multiplied by the units");
    const rt::OtherCarColour drawn16 = rt::FindOtherCarColour(taps, 10.0f, 1.6f, paint, units);
    Check(drawn16.known() && Near(drawn16.rgb[1], 0.349 * 16.0, 1e-5), "the paint that stands in for a drawn car the history shows nothing of is multiplied by the units");
    for (rt::SurfaceTap& tap : taps) {
      tap.distance = 10.0f;
      tap.rgb[0] = 0.5f;
      tap.rgb[1] = 0.25f;
      tap.rgb[2] = 0.125f;
    }
    const rt::OtherCarColour seen16 = rt::FindOtherCarColour(taps, 10.0f, 1.6f, paint, units);
    Check(Near(seen16.rgb[0], 0.5, 1e-5) && Near(seen16.rgb[1], 0.25, 1e-5), "what the history shows keeps its own units (not multiplied)");
    // What the paint then makes of it (the reflection ray's hidden hit: kShade of the colour; the paint reads the image times 2^-bias): a neighbour of paint radiance p shows
    // p x kShade in the paint's units, not p x kShade / 16 (the dark silhouette of the first model-data check).
    {
      const float white[3] = {0.35f, 0.35f, 0.35f};  // a white car's row (car_model_rt_rules.h PaintRadiance)
      const rt::OtherCarColour hit = rt::FindOtherCarColour(taps, -3.0f, 1.6f, white, units);
      const double image = double(hit.rgb[0]) * rt::kShade;                  // what the pass writes for the hit (hidden: the colour times the shade)
      const double in_paint_units = image * cr::ExpBiasGain(4);    // what the paint's debug view 2 and its blend read
      Check(Near(in_paint_units, 0.35 * rt::kShade, 1e-5), "a hidden hit on a car of paint radiance p reaches the paint as p x kShade (the units cancel)");
      const double before = 0.35 * rt::kShade * cr::ExpBiasGain(4);  // the first model-data check: no units, so 1/16 of that
      Check(Near(before * 16.0, in_paint_units, 1e-6) && before < 0.01, "... where it reached it as 1/16 of that before: the dark silhouette");
    }
    // The step: kOtherCarSpread radii in texels, from one to the most.
    Check(Near(rt::OtherCarStep(1.6f, 0.01f), rt::kOtherCarMaxStep) && Near(rt::OtherCarStep(1.6f, 2.0f), 1.0) && Near(rt::OtherCarStep(1.6f, 0.2f), 4.0, 1e-5) &&
              Near(rt::OtherCarStep(1.6f, 0.0f), 1.0),
          "the taps' spacing follows the car's size in texels, from one texel to the most");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
