// Check of RT-HIT-SHADING part two's rules (car_rt_hit_rules.h): the level table by class and its overrides, the distance a reflection ray is cast and the weight of a hit by its own
// reach, the radius of the cars the scene holds against that reach, the clear coat's share by the paint's fresnel, the two lobes added (exact for the paint's blend), the aux image's
// word that keeps the denoise off the mirror, the push block's packed word; and that hlsl/car_rt_hit_shade.hlsli, hlsl/car_rt_refl_cs.hlsl, hlsl/car_rt_refl_opp.hlsli,
// hlsl/car_rt_refl_denoise_cs.hlsl, the host (car_rt_refl.cpp, car_rt_opp_refl.h, scene.cpp, volumetrics.h) and the settings hold the header's numbers.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_rt_hit_rules_test, then run src/gpu/larecomp_car_rt_hit_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and settings; by default the one this file is in)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "car_rt_hit_rules.h"
#include "car_rt_instances_rules.h"
#include "car_rt_material_rules.h"
#include "car_rt_refl_rules.h"

namespace hr = mcla::gpu::car_rt_hit;
namespace ci = mcla::gpu::car_rt_instances;
namespace crm = mcla::gpu::car_rt_material;
namespace rt = mcla::gpu::car_rt_refl;
using mcla::gpu::gpu_class::GpuClass;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The number after `name` in `text` ("static const float kX = 0.25;"), or NaN.
double Constant(const std::string& text, const std::string& name) {
  const size_t at = text.find(name + " = ");
  if (at == std::string::npos) {
    return std::nan("");
  }
  return std::strtod(text.c_str() + at + name.size() + 3, nullptr);
}

float Smoothstep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

int main(int argc, char** argv) {
  std::string src = argc > 1 ? argv[1] : std::string();
  if (src.empty()) {
    std::string self = __FILE__;
    for (char& c : self) {
      c = c == '\\' ? '/' : c;
    }
    const size_t cut = self.rfind("/gpu/");
    src = cut != std::string::npos ? self.substr(0, cut) : ".";
  }

  // ---- the levels ---------------------------------------------------------------------------------------------------------------------
  {
    const hr::Level high = hr::LevelFor(GpuClass::kHigh);
    const hr::Level mid = hr::LevelFor(GpuClass::kMid);
    const hr::Level low = hr::LevelFor(GpuClass::kLow);
    Check(Near(high.others_reach, 18.0) && high.coat > 0.0f && high.cone_rays == 4 && high.cascade_shadow && high.sun_ray, "the High class: the reach 18 m, the clear coat on, the whole cone, both kinds of shadow");
    Check(mid == high && low == high, "reach, cone and coat are identical across classes");
  }

  // ---- the distance cast, and the weight of a hit by the reach of its part ---------------------------------------------------------------
  {
    Check(Near(hr::CastReach(rt::kPartBody | rt::kPartGlass | rt::kPartTyres, 4.0f, 18.0f), 4.0), "parts of the player's own car alone: the ray is cast the player's reach");
    Check(Near(hr::CastReach(rt::kDefaultReflectionParts, 4.0f, 18.0f), 18.0), "parts that include another car: the ray is cast the longer reach");
    Check(Near(hr::CastReach(rt::kPartOthers, 6.0f, 3.0f), 6.0), "an others' reach shorter than the player's: the longer is cast");
    Check(rt::kDefaultReflectionParts == (rt::kPartBody | rt::kPartGlass | rt::kPartTyres | rt::kPartOthers), "the default parts hold the other cars");

    // the player's own parts keep their old fade exactly: 1 up to 0.6 of 4 m, a smoothstep to 0 at 4 m
    bool same = true;
    for (int i = 0; i <= 80; ++i) {
      const float d = 0.05f * float(i);
      same = same && Near(hr::HitFade(d, 4.0f, rt::kFadeFrom), 1.0 - Smoothstep(rt::kFadeFrom * 4.0f, 4.0f, d), 1e-5);
    }
    Check(same, "the weight of a hit on the player's own parts is what it was (smoothstep from 0.6 of 4 m)");
    Check(Near(hr::HitFade(0.0f, 18.0f, 0.75f), 1.0) && Near(hr::HitFade(13.5f, 18.0f, 0.75f), 1.0) && Near(hr::HitFade(18.0f, 18.0f, 0.75f), 0.0) && Near(hr::HitFade(25.0f, 18.0f, 0.75f), 0.0),
          "a hit on another car weighs 1 to 75% of its reach and nothing at it");
    float last = 2.0f;
    bool monotonic = true;
    for (int i = 0; i <= 200; ++i) {
      const float f = hr::HitFade(0.1f * float(i), 18.0f, 0.75f);
      monotonic = monotonic && f <= last + 1e-6f && f >= 0.0f && f <= 1.0f;
      last = f;
    }
    Check(monotonic, "the weight never rises with the distance and stays in 0 .. 1");
    Check(hr::HitFade(5.0f, 0.0f, 0.75f) == 0.0f && hr::HitFade(5.0f, -1.0f, 0.75f) == 0.0f, "no reach, no weight");
    Check(hr::HitFade(17.0f, 18.0f, 0.75f) > 0.0f && hr::HitFade(17.0f, 18.0f, 0.75f) < 0.2f, "a car 17 m away still shows, faintly");
    // the fade is where the player's reflection of a car at the class's distances reads: a car 3 m away is whole, one 12 m away is most of it
    Check(Near(hr::HitFade(3.0f, 18.0f, 0.75f), 1.0) && hr::HitFade(12.0f, 18.0f, 0.75f) > 0.99f && hr::HitFade(15.0f, 18.0f, 0.75f) > 0.5f, "cars at 3, 12 and 15 m weigh in full, in full and by more than half");
  }

  // ---- the clear coat's share ------------------------------------------------------------------------------------------------------------
  {
    Check(hr::CoatShare(0.5f, 4.0f, 0.0f) == 0.0f && hr::CoatShare(0.5f, 4.0f, -1.0f) == 0.0f, "no gain, no coat");
    Check(Near(hr::CoatShare(1.0f, 4.0f, 1.0f), hr::kCoatNormal), "facing the camera the coat's share is the normal one");
    Check(Near(hr::CoatShare(0.0f, 4.0f, 1.0f), hr::kCoatGrazing), "at a grazing view it is the grazing one");
    float last = 0.0f;
    bool rising = true, bounded = true;
    for (int i = 100; i >= 0; --i) {
      const float nv = float(i) / 100.0f;
      const float share = hr::CoatShare(nv, 5.0f, 1.0f);
      rising = rising && share >= last - 1e-6f;
      bounded = bounded && share >= hr::kCoatNormal - 1e-5f && share <= hr::kCoatGrazing + 1e-5f;
      last = share;
    }
    Check(rising && bounded, "the share rises from the normal to the grazing one as the view turns away, by the fresnel's curve, and stays between them");
    Check(hr::CoatShare(0.5f, 8.0f, 1.0f) < hr::CoatShare(0.5f, 2.0f, 1.0f), "a paint with a steeper fresnel (a larger exponent) has less coat at the same angle");
    Check(Near(hr::CoatShare(0.3f, 0.0f, 1.0f), hr::CoatShare(0.3f, hr::kCoatMinPower, 1.0f)), "an exponent under the floor is the floor");
    Check(hr::CoatShare(0.0f, 4.0f, 4.0f) <= hr::kCoatMax + 1e-6f && hr::CoatShare(1.0f, 4.0f, 100.0f) <= hr::kCoatMax + 1e-6f, "no gain takes it over the most");
    Check(Near(hr::CoatShare(1.0f, 4.0f, 0.5f), 0.5 * hr::kCoatNormal), "the gain scales the share");
    Check(hr::kCoatMax < 1.0f && hr::kCoatNormal > 0.0f && hr::kCoatNormal < hr::kCoatGrazing && hr::kCoatGrazing <= hr::kCoatMax, "the numbers order: normal < grazing <= max < 1");
  }

  // ---- the two lobes added ----------------------------------------------------------------------------------------------------------------
  {
    std::mt19937 rng(20261120);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f), colour(0.0f, 6.0f);
    bool exact = true, bounded = true, share0 = true, share1 = true, sharp_ok = true;
    for (int trial = 0; trial < 5000; ++trial) {
      hr::Lobe cone, coat;
      cone.weight = u01(rng) < 0.2f ? 0.0f : u01(rng);
      coat.weight = u01(rng) < 0.2f ? 0.0f : u01(rng);
      cone.car_weight = cone.weight * u01(rng);
      coat.car_weight = coat.weight * u01(rng);
      for (int c = 0; c < 3; ++c) {
        cone.colour[c] = colour(rng);
        coat.colour[c] = colour(rng);
      }
      const float share = u01(rng) * hr::kCoatMax;
      float sharp = -1.0f;
      const hr::Lobe out = hr::MixCoat(cone, coat, share, &sharp);
      // the paint's blend: lerp(environment, colour, weight) = environment + (1 - a) w_c (c_c - environment) + a w_s (c_s - environment)
      for (int k = 0; k < 4; ++k) {
        const float env[3] = {colour(rng), colour(rng), colour(rng)};
        for (int c = 0; c < 3; ++c) {
          const float mixed = env[c] + out.weight * (out.colour[c] - env[c]);
          const float two = env[c] + (1.0f - share) * cone.weight * (cone.colour[c] - env[c]) + share * coat.weight * (coat.colour[c] - env[c]);
          exact = exact && Near(mixed, two, 2e-4);
        }
      }
      bounded = bounded && out.weight >= 0.0f && out.weight <= 1.0f + 1e-6f && out.car_weight <= out.weight + 1e-6f;
      sharp_ok = sharp_ok && sharp >= 0.0f && sharp <= 1.0f && (out.weight > 0.0f ? Near(sharp, share * coat.weight / out.weight, 1e-4) : sharp == 0.0f);
      float s0;
      const hr::Lobe zero = hr::MixCoat(cone, coat, 0.0f, &s0);
      share0 = share0 && Near(zero.weight, cone.weight, 1e-6) && s0 == 0.0f;
      for (int c = 0; c < 3 && cone.weight > 0.0f; ++c) {
        share0 = share0 && Near(zero.colour[c], cone.colour[c], 1e-4);
      }
      float s1;
      const hr::Lobe one = hr::MixCoat(cone, coat, 1.0f, &s1);
      share1 = share1 && Near(one.weight, coat.weight, 1e-6);
      for (int c = 0; c < 3 && coat.weight > 0.0f; ++c) {
        share1 = share1 && Near(one.colour[c], coat.colour[c], 1e-4);
      }
    }
    Check(exact, "the two lobes added are exact for the paint's blend over its environment: environment + (1 - a) w_c (c_c - environment) + a w_s (c_s - environment)");
    Check(bounded, "the weight stays in 0 .. 1 and the part of it on another car is no more than the weight");
    Check(sharp_ok, "the mirror's part of the weight is the share times its weight over the whole, in 0 .. 1");
    Check(share0, "with a share of 0 the texel is the cone's, as the pass always made it (the weight and the colour)");
    Check(share1, "with a share of 1 it is the coat's");

    // a worked case: a car edge. The cone sees the car by half (its rays split), the mirror misses: the texel is the car by (1 - a) half
    hr::Lobe cone, coat;
    cone.weight = 0.5f;
    cone.colour[0] = cone.colour[1] = cone.colour[2] = 2.0f;
    coat.weight = 0.0f;
    float sharp;
    hr::Lobe out = hr::MixCoat(cone, coat, 0.6f, &sharp);
    Check(Near(out.weight, 0.2) && Near(out.colour[0], 2.0) && sharp == 0.0f, "a cone that half sees a car and a mirror that sees none: the car shows by (1 - 0.6) x 0.5, its colour unchanged");
    cone.weight = 0.0f;
    coat.weight = 1.0f;
    coat.colour[0] = coat.colour[1] = coat.colour[2] = 3.0f;
    out = hr::MixCoat(cone, coat, 0.6f, &sharp);
    Check(Near(out.weight, 0.6) && Near(out.colour[1], 3.0) && Near(sharp, 1.0), "a mirror that sees a car where the cone sees none: the car shows by the share, all of it the mirror's");
    out = hr::MixCoat(hr::Lobe{}, hr::Lobe{}, 0.5f, &sharp);
    Check(out.weight == 0.0f && sharp == 0.0f && out.colour[0] == 0.0f, "nothing in either lobe: nothing");
    out = hr::MixCoat(cone, coat, 2.0f, nullptr);
    Check(Near(out.weight, 1.0), "a share over 1 is clamped (and no `sharp` is fine)");
  }

  // ---- the aux image's word and the denoise's scale ---------------------------------------------------------------------------------------
  {
    Check(Near(hr::EncodeAuxSharp(0.0f), 1.0) && Near(hr::EncodeAuxSharp(1.0f), 1.0 - hr::kAuxSharpScale), "no coat is the aux's old 1, all coat 1 - 0.49");
    bool ok = true, data = true, round = true;
    for (int i = 0; i <= 100; ++i) {
      const float s = float(i) / 100.0f;
      const float w = hr::EncodeAuxSharp(s);
      data = data && w > 0.5f && w <= 1.0f;
      round = round && Near(hr::DecodeAuxSharp(w), s, 1e-4);
      ok = ok && Near(hr::DenoiseScale(w), 1.0 - s, 1e-4);
    }
    Check(data, "the word stays over 0.5: the denoise's `has aux` test (w > 0.5) still holds");
    Check(round, "the share decodes from the word");
    Check(ok, "the denoise's strength is multiplied by what the mirror's share leaves");
    Check(Near(hr::DenoiseScale(1.0f), 1.0) && Near(hr::DenoiseScale(0.0f), 0.0) && Near(hr::DecodeAuxSharp(0.0f), 1.0), "w 1 is the whole strength; a word outside the range is clamped");
    Check(hr::EncodeAuxSharp(5.0f) == hr::EncodeAuxSharp(1.0f) && hr::EncodeAuxSharp(-5.0f) == 1.0f, "a share outside 0 .. 1 is clamped");
    // a texel that is all mirror is not touched by the denoise: lerp(own, blurred, strength * scale) = own
    const float own = 0.8f, blurred = 0.3f, strength = 1.0f;
    Check(Near(own + (blurred - own) * strength * hr::DenoiseScale(hr::EncodeAuxSharp(1.0f)), own), "a texel that is all mirror comes out of the denoise as it went in");
    Check(Near(own + (blurred - own) * strength * hr::DenoiseScale(hr::EncodeAuxSharp(0.0f)), blurred), "a texel with no mirror is denoised in full");
  }

  // ---- RT-HIT-SHADING-4: the mirror decides for another car, and the clear coat's layer ------------------------------------------------------------------
  {
    // the mirror's share
    Check(hr::kOthersSharpShare == 0.9f && hr::kOthersSharpShare <= hr::kCoatMax, "a texel whose mirror ray hit another car is at least 0.9 the mirror's (the coat's own cap is 0.9: the cone stays a little of the texel)");
    bool share_ok = true;
    for (float nv : {0.05f, 0.2f, 0.3f, 0.42f, 0.6f, 0.9f, 1.0f}) {
      const float share = hr::CoatShare(nv, 5.0f, 1.0f);
      share_ok = share_ok && hr::MirrorShare(share, false) == share && hr::MirrorShare(share, true) >= hr::kOthersSharpShare && hr::MirrorShare(share, true) >= share;
    }
    Check(share_ok, "where the mirror hit another car the share is at least 0.9 and never less than the class's; where it did not, the class's");
    Check(hr::MirrorShare(0.0f, false) == 0.0f && hr::MirrorShare(0.0f, true) == hr::kOthersSharpShare, "with no coat share (a level with no mirror ray) nothing changes when there was no mirror hit");

    // which cone hits count
    Check(hr::ConeHitCounts(false, true, false) && hr::ConeHitCounts(false, true, true) && hr::ConeHitCounts(false, false, false) && hr::ConeHitCounts(false, false, true),
          "a cone ray's hit on the player's own part always counts");
    Check(!hr::ConeHitCounts(true, true, false) && hr::ConeHitCounts(true, true, true), "a cone ray's hit on another car counts only where the mirror ray hit one too");
    Check(hr::ConeHitCounts(true, false, false), "a level with no mirror ray (the Mid class) keeps the cone's hits on another car");

    // the layer's fresnel: Schlick with F0 0.04, power 5
    Check(Near(hr::CoatLayerFresnel(1.0f), 0.04) && Near(hr::CoatLayerFresnel(0.5f), 0.04 + 0.96 / 32.0, 1e-6) && Near(hr::CoatLayerFresnel(0.3f), 0.04 + 0.96 * std::pow(0.7, 5.0), 1e-6) &&
              Near(hr::CoatLayerFresnel(0.2f), 0.04 + 0.96 * std::pow(0.8, 5.0), 1e-6) && Near(hr::CoatLayerFresnel(0.0f), hr::kCoatLayerMax),
          "the layer's fresnel is Schlick's: 0.04 facing the camera, 0.07 at N.V 0.5, 0.20 at 0.3, 0.35 at 0.2, capped at 0.6");
    bool monotone = true;
    for (float nv = 0.01f; nv < 1.0f; nv += 0.01f) {
      monotone = monotone && hr::CoatLayerFresnel(nv) >= hr::CoatLayerFresnel(nv + 0.01f) - 1e-7f && hr::CoatLayerFresnel(nv) <= hr::kCoatLayerMax && hr::CoatLayerFresnel(nv) >= hr::kCoatLayerF0;
    }
    Check(monotone, "the fresnel falls as the paint faces the camera and stays between 0.04 and the cap");
    Check(Near(hr::CoatLayerAmount(0.3f, 1.0f, 1.0f), hr::CoatLayerFresnel(0.3f)) && hr::CoatLayerAmount(0.3f, 0.0f, 1.0f) == 0.0f && hr::CoatLayerAmount(0.3f, 1.0f, 0.0f) == 0.0f &&
              Near(hr::CoatLayerAmount(0.3f, 0.5f, 0.5f), 0.25 * hr::CoatLayerFresnel(0.3f)) && Near(hr::CoatLayerAmount(0.3f, 7.0f, 3.0f), hr::CoatLayerFresnel(0.3f)),
          "the layer's amount is the fresnel by the weight and by the share of the hit that is on another car (each clamped to 0..1): none where there is no hit or the hit is the player's own");
    Check(Near(hr::CoatLayerMix(0.2f, 0.8f, 0.0f), 0.2) && Near(hr::CoatLayerMix(0.2f, 0.8f, 1.0f), 0.8) && Near(hr::CoatLayerMix(0.2f, 0.8f, 0.25), 0.35, 1e-6),
          "the layer is a lerp: the paint's colour at amount 0, the car's at 1");

    // WHAT THE FIX BUYS, in the numbers of the rthit2 door (day, camera b): the paint's own reflectance for the environment there is 0.0146 (the door's luminance over the buildings it
    // reflects), and the layer's fresnel at the door's N.V (0.2 to 0.42) is 8 to 24 times that: the neighbour's flank (a quarter of its sunlit fender's radiance) lands over the
    // buildings' 0.0049, not under it.
    {
      const double paint_reflectance = 0.0146;   // rthit2: 0.0024 of the buildings' 0.165
      const double flank_over_fender = 0.25;     // the lower flank 0.062 / the fender 0.256 (red channel), in the shade of the player's car
      const double fender = 0.19;                // the sunlit fender's luminance
      const double buildings_in_door = 0.0049;   // OFF, mean luminance of the pixels the neighbour changes
      const double before = flank_over_fender * fender * paint_reflectance + 0.0;  // through the paint's own path
      double layer_low = 1e9, layer_high = 0.0;
      for (double nv : {0.2, 0.3, 0.42}) {
        const double amount = hr::CoatLayerFresnel(float(nv));
        layer_low = std::min(layer_low, amount);
        layer_high = std::max(layer_high, amount);
      }
      Check(layer_low / paint_reflectance > 5.0 && layer_high / paint_reflectance < 30.0, "the layer's fresnel at the door's N.V is 5 to 30 times the paint's own reflectance (about 8 to 24)");
      const double after = before + flank_over_fender * fender * layer_low;
      Check(before < 0.5 * buildings_in_door && after > buildings_in_door, "the neighbour's shaded flank in the door: under half the buildings' luminance through the paint's path alone, over it with the layer at the door's weakest view");
    }

    // THE EDGE: a straight edge of a reflected car at an angular distance `e` from the mirror direction. A texel's response is the cone's share by (1 - share) and the mirror's by share; the cone's four
    // rays are turned by every texel's own angle (averaged here over 400 turns). Before: the response of the door's share (0.52 to 0.58) goes from 5 % to 95 % over about 3.2 degrees, and
    // reaches 7 % half a cone outside the edge. After: a step (the mirror's), and nothing outside it. At 57 to 101 px a degree (the showcase cameras, 4K) 3.2 degrees are 180 to 320 px.
    {
      const double tan_half = std::tan(2.5 * 0.017453292519943295);
      const int n_turns = 400;
      auto cone_coverage = [&](double e) {
        int hit = 0;
        for (int t = 0; t < n_turns; ++t) {
          for (int j = 0; j < 4; ++j) {
            hit += tan_half * rt::ConeDisc(j, float(t) / float(n_turns)).x < e ? 1 : 0;
          }
        }
        return double(hit) / double(n_turns * 4);
      };
      const int steps = 600;
      std::vector<double> e(steps + 1), before(steps + 1), after(steps + 1);
      const double share = hr::CoatShare(0.3f, 5.0f, 1.0f);  // the door's
      Check(share > 0.5 && share < 0.6, "the class's coat share at the door's N.V of 0.3 is 0.5 to 0.6");
      for (int i = 0; i <= steps; ++i) {
        e[i] = (-3.0 + 6.0 * double(i) / double(steps)) * tan_half;
        const double mirror_hit = e[i] > 0.0 ? 1.0 : 0.0;
        const double cone = cone_coverage(e[i]);
        before[i] = (1.0 - share) * cone + share * mirror_hit;
        // after: the cone's hits on the car count only where the mirror hit it; the mirror's share is at least 0.9 there
        const double a = hr::MirrorShare(float(share), mirror_hit > 0.0);
        after[i] = mirror_hit > 0.0 ? (1.0 - a) * cone + a * 1.0 : 0.0;
      }
      auto angle = [](double tangent) { return std::atan(tangent) / 0.017453292519943295; };
      auto width = [&](const std::vector<double>& response, double lo, double hi) {
        int i_lo = 0, i_hi = 0;
        while (i_lo < steps && !(response[i_lo] > lo)) ++i_lo;
        while (i_hi < steps && !(response[i_hi] >= hi)) ++i_hi;
        return angle(e[i_hi]) - angle(e[i_lo]);
      };
      const double w_before = width(before, 0.05, 0.95);
      const double w_after = width(after, 0.05, 0.95);
      Check(w_before > 3.0 && w_before < 3.6, "before: the door's reflected edge goes from 5 % to 95 % over 3 to 3.6 degrees (" + std::to_string(w_before) + ")");
      Check(w_after < 0.05, "after: a step (" + std::to_string(w_after) + " degrees at the model's 0.025 degree grid)");
      int outside = 0;
      for (int i = 0; i <= steps; ++i) {
        outside += (e[i] < 0.0 && before[i] > 0.05) ? 1 : 0;
      }
      const double haze_degrees = double(outside) * (6.0 * 2.5 / double(steps));  // the grid's step is 0.025 degrees
      Check(outside > 0 && haze_degrees > 0.5, "before: the car's colour reaches past the edge into the environment's reflection");
      bool clean = true;
      for (int i = 0; i <= steps; ++i) {
        clean = clean && (e[i] > 0.0 || after[i] == 0.0);
      }
      Check(clean, "after: no weight at all outside the mirror's own edge");
    }
  }

  // ---- the push block's packed word -----------------------------------------------------------------------------------------------------
  {
    bool ok = true;
    for (uint32_t phase = 0; phase < 1024; ++phase) {
      for (uint32_t rays = 1; rays <= 4; ++rays) {
        const uint32_t w = hr::PackPhaseRays(phase, rays);
        ok = ok && hr::PhaseOf(w) == phase && hr::RaysOf(w) == rays && ((w >> 18) & 1023u) == phase && (w & 0x3FFFFu) == (rays << 8);
      }
    }
    Check(ok, "the phase (0 .. 1023, NOISE-FOLLOW) is in bits 18 to 27 and the cone's rays are in bits 8 to 10: no other bit is set, so the stats flag (16) and the blue flag (17) stay free");
    Check(hr::RaysOf(5u) == 4 && hr::RaysOf(0u) == 4, "a word with no rays (a host that does not say) is the whole cone");
    Check(hr::PackPhaseRays(1025, 4) == hr::PackPhaseRays(1, 4) && hr::PackPhaseRays(9, 4) != hr::PackPhaseRays(1, 4) && hr::RaysOf(hr::PackPhaseRays(0, 9)) == 4 && hr::RaysOf(hr::PackPhaseRays(0, 0)) == 1,
          "the phase is taken mod 1024 (it was mod 8: a period of 8 frames) and the rays clamped to 1 .. 4");
  }

  // ---- RT-HIT-SHADING-3: the census, the exposure and the sky fill ---------------------------------------------------------------------------
  {
    namespace ce = hr::census;
    // the fixed-point log of a luminance
    bool round = true;
    for (float lum : {0.02f, 0.05f, 0.25f, 1.0f, 3.0f, 16.0f, 200.0f}) {
      const float back = ce::FromFixedLog2(float(ce::FixedLog2(lum)));
      round = round && std::fabs(std::log2(back) - std::log2(lum)) <= 1.0f / ce::kLogScale + 1e-4f;
    }
    Check(round, "a luminance comes back from its fixed-point log within a sixty-fourth of a stop");
    Check(ce::FixedLog2(0.0f) == ce::FixedLog2(ce::kLogFloor) && ce::FixedLog2(1e-9f) == ce::FixedLog2(ce::kLogFloor) && ce::FixedLog2(-3.0f) == ce::FixedLog2(ce::kLogFloor),
          "no, zero or negative luminance is the floor's");
    Check(ce::FixedLog2(1e12f) == ce::kLogMax && ce::FixedLog2(1.0f) == uint32_t(ce::kLogBias * ce::kLogScale), "a huge luminance is clamped; 1 is the bias");
    Check(uint64_t(ce::kLogMax) * 2000000ull < (1ull << 32), "two million samples at the largest value still sum under 2^32 (the shader words are 32 bits)");
    Check(ce::kWords * 4 == ce::kBytes && ce::kCoat < ce::kWords && ce::kAllCalModel < ce::kCoat && ce::kByClass + ce::kClassCount == ce::kLogLum,
          "the counters layout: 32 words, the class words are eight before the log sum, the coat's is the last used");

    // the exposure: samples to stops, the gain's move, convergence on a model that is two stops dark
    float stops = 0.0f;
    Check(!ce::CalibrationStops(ce::kCalMinSamples - 1, 1000, 100, &stops) && ce::CalibrationStops(ce::kCalMinSamples, 1000, 100, &stops), "fewer than kCalMinSamples samples say nothing");
    const uint64_t n = 500;
    Check(ce::CalibrationStops(n, n * 3000, n * (3000 - 2 * 64), &stops) && Near(stops, 2.0, 1e-4), "a history two stops brighter than the model reads as +2 stops");
    Check(ce::CalibrationStops(n, n * 3000, n * (3000 + 64), &stops) && Near(stops, -1.0, 1e-4), "a history a stop darker reads as -1");
    Check(ce::CalibrationStops(n, 0, n * 2047, &stops) && Near(stops, ce::kCalMinStops, 1e-5) && ce::CalibrationStops(n, n * 2047, 0, &stops) && Near(stops, ce::kCalMaxStops, 1e-5),
          "a measurement is clamped to the gain's range");
    float gain_stops = 0.0f;
    const float truth = 2.0f;  // the model is two stops too dark against the game's own picture
    for (int window = 0; window < 12; ++window) {
      const float residual = truth - gain_stops;  // the pass shades with the gain in force: what it finds is what is left
      gain_stops = ce::NextStops(gain_stops, residual);
    }
    Check(std::fabs(gain_stops - truth) < 0.01f, "the gain settles on the model's error in a dozen windows (each moves half the way)");
    Check(Near(ce::NextStops(0.0f, 100.0f), ce::kCalMaxStops) && Near(ce::NextStops(0.0f, -100.0f), ce::kCalMinStops), "one wild window cannot take the gain out of its range");
    Check(Near(ce::GainOf(0.0f), 1.0) && Near(ce::GainOf(3.0f), 8.0) && Near(ce::GainOf(-3.0f), 0.125) && Near(ce::GainOf(9.0f), 8.0), "the gain is a power of two within a factor of 8");
    {
      float g = 0.0f;
      for (int window = 0; window < 10; ++window) g = ce::NextStops(g, 0.0f - g);  // the day's model fits
      const float day = g;
      for (int window = 0; window < 10; ++window) g = ce::NextStops(g, -2.5f - g);  // at night the picture is 2.5 stops under the model
      Check(std::fabs(day) < 0.01f && std::fabs(g + 2.5f) < 0.01f, "the gain follows the hour (the model's light does not)");
    }

    // the stats frame: when, and the push word
    Check(!ce::StatsDue(ce::kStatsFirst - 1, 0, false) && ce::StatsDue(ce::kStatsFirst, 0, false), "the first stats frame is at kStatsFirst");
    Check(!ce::StatsDue(100 + ce::kStatsEvery - 1, 100, true) && ce::StatsDue(100 + ce::kStatsEvery, 100, true), "then every kStatsEvery frames");
    bool word = true;
    for (uint32_t phase = 0; phase < 1024; ++phase) {
      for (uint32_t rays = 1; rays <= 4; ++rays) {
        const uint32_t w = hr::PackPhaseRays(phase, rays);
        const uint32_t on = ce::PackWithStats(w, true);
        word = word && ce::PackWithStats(w, false) == w && hr::PhaseOf(on) == phase && hr::RaysOf(on) == rays && (on & ce::kStatsFlag) != 0 && (w & ce::kStatsFlag) == 0;
      }
    }
    Check(word && ce::kStatsFlag == (1u << 16), "the stats flag is bit 16: the phase (bits 18 to 27) and the cone's rays (8 to 10) are clear of it");

    // the log line
    {
      ce::Totals t;
      t.w[ce::kTexels] = 1200;
      t.w[ce::kRays] = 900;
      t.w[ce::kOwn] = 100;
      t.w[ce::kSeen] = 300;
      t.w[ce::kBlend] = 50;
      t.w[ce::kShaded] = 400;
      t.w[ce::kStandIn] = 30;
      t.w[ce::kDropped] = 20;
      t.w[ce::kByClass + 1] = 300;
      t.w[ce::kByClass + 2] = 100;
      t.w[ce::kByClass + 5] = 50;
      t.w[ce::kLogLum] = 450ull * uint64_t(ce::FixedLog2(0.5f));
      t.w[ce::kLit] = 100;
      t.w[ce::kShadow] = 250;
      t.w[ce::kAway] = 100;
      t.w[ce::kGlow] = 7;
      t.w[ce::kCalCount] = 200;
      t.w[ce::kCalSeen] = 200ull * 3000;
      t.w[ce::kCalModel] = 200ull * (3000 - 64);
      t.w[ce::kAllCalCount] = 260;
      const std::string text = ce::Text(t, 15, 0.5f, true);
      Check(Has(text, "15 stats frames") && Has(text, "1200 texels") && Has(text, "300 took the history's colour") && Has(text, "50 shaded and blended") && Has(text, "400 shaded") &&
                Has(text, "30 the stand-in colour") && Has(text, "20 dropped") && Has(text, "paint 300 glass 100 light 50") && !Has(text, "tyre") && Has(text, "100 lit, 250 in shadow, 100 facing away") &&
                Has(text, "7 lamps glowing") && Has(text, "luminance 0.5") && Has(text, "+1.00 stops from it") && Has(text, "the gain now +0.50 stops (x1.41)") && !Has(text, "not applied"),
            "the census line says what the hits did, by class and by the sun, and the exposure against the game's picture: " + text);
      Check(Has(ce::Text(t, 15, 0.5f, false), "not applied"), "with the calibration off the line says the gain is not applied");
      t.w[ce::kCalCount] = 3;
      Check(Has(ce::Text(t, 1, 0.0f, true), "too few"), "a window with too few samples says so and keeps the gain");
    }

    // the sky fill: a flank the sun does not reach is lit by a fifth of the sun's colour, in its hue, where the cars' c26 has none (a daylight run's was 0, 0.004, 0.004)
    {
      crm::ShadeInput in;
      in.cls = crm::Class::kPaint;
      in.paint = true;
      in.tint_a[0] = 0.64f; in.tint_a[1] = 0.19f; in.tint_a[2] = 0.03f;
      in.tint_b[0] = 0.64f; in.tint_b[1] = 0.19f; in.tint_b[2] = 0.03f;
      in.flop = 0.0f;
      in.light.valid = true;
      in.light.sun[0] = 0.060f; in.light.sun[1] = 0.044f; in.light.sun[2] = 0.024f;
      in.light.ambient[0] = 0.0f; in.light.ambient[1] = 0.004f; in.light.ambient[2] = 0.004f;
      in.fresnel[0] = 0.0f; in.fresnel[1] = 0.0f;  // no environment: the diffuse alone
      in.env_known = false;
      in.normal[0] = 0.0f; in.normal[1] = 0.0f; in.normal[2] = -1.0f;
      in.ray[0] = 0.0f; in.ray[1] = 0.0f; in.ray[2] = 1.0f;
      in.to_sun[0] = 0.0f; in.to_sun[1] = 1.0f; in.to_sun[2] = 0.0f;  // the sun is in the surface's plane: N.L 0
      in.up[0] = 0.0f; in.up[1] = 1.0f; in.up[2] = 0.0f;
      float shade[3];
      crm::ShadeReference(in, shade);
      const double hemisphere = 0.6;  // the normal is horizontal
      Check(Near(shade[0], 0.64 * 0.060 * double(crm::kFillShare) * hemisphere, 1e-4) && shade[0] > shade[1] && shade[1] > shade[2],
            "a shaded orange flank is orange: the fill is the sun's colour times kFillShare, not c26's (which has no red)");
      in.light.ambient[0] = 0.05f;  // an ambient over the fill is used as it was
      crm::ShadeReference(in, shade);
      Check(Near(shade[0], 0.64 * 0.05 * hemisphere, 1e-4), "an ambient brighter than the fill is the ambient");
      in.light.ambient[0] = 0.0f;
      crm::ShadeReference(in, shade);
      in.to_sun[1] = 0.0f; in.to_sun[2] = -1.0f;  // facing the sun
      float lit[3];
      crm::ShadeReference(in, lit);
      Check(lit[0] > shade[0] * 3.0f, "the sun-lit flank is several times the shaded one");
      in.tuning.gain = ce::GainOf(1.0f);
      float doubled[3];
      crm::ShadeReference(in, doubled);
      Check(Near(doubled[0], lit[0] * 2.0, 1e-4), "the exposure is the gain the reference multiplies by (a stop is twice)");
    }
  }

  // ---- the shaders, the host and the settings hold the numbers ------------------------------------------------------------------------------
  {
    // the car's file and the two halves of what every hit shading shares (hlsl/rt_hit_frame.hlsli, rt_hit_sample.hlsli)
    const std::string hlsli = ReadFile(src + "/gpu/hlsl/car_rt_hit_shade.hlsli") + ReadFile(src + "/gpu/hlsl/rt_hit_frame.hlsli") + ReadFile(src + "/gpu/hlsl/rt_hit_sample.hlsli");
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string opp = ReadFile(src + "/gpu/hlsl/car_rt_refl_opp.hlsli");
    const std::string denoise = ReadFile(src + "/gpu/hlsl/car_rt_refl_denoise_cs.hlsl");
    const std::string host = ReadFile(src + "/gpu/car_rt_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/car_rt_refl.h");
    const std::string opp_h = ReadFile(src + "/gpu/car_rt_opp_refl.h");
    const std::string opp_cpp = ReadFile(src + "/gpu/car_rt_opp_refl.cpp");
    const std::string scene = ReadFile(src + "/gpu/scene.cpp");
    const std::string vol_h = ReadFile(src + "/gpu/volumetrics.h");
    const std::string vol_cpp = ReadFile(src + "/gpu/volumetrics.cpp");
    const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
    const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
    const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    const std::string scene_cpp = ReadFile(src + "/gpu/car_rt_scene.cpp");
    const std::string scene_h = ReadFile(src + "/gpu/car_rt_scene.h");
    if (hlsli.empty() || refl.empty() || host.empty() || settings.empty()) {
      std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      Check(Near(Constant(hlsli, "static const float kCoatNormal"), hr::kCoatNormal) && Near(Constant(hlsli, "static const float kCoatGrazing"), hr::kCoatGrazing) &&
                Near(Constant(hlsli, "static const float kCoatMax"), hr::kCoatMax) && Near(Constant(hlsli, "static const float kCoatMinPower"), hr::kCoatMinPower) &&
                Near(Constant(hlsli, "static const float kAuxSharpScale"), hr::kAuxSharpScale) && Near(Constant(denoise, "static const float kAuxSharpScale"), hr::kAuxSharpScale),
            "the shaders' clear-coat numbers and the aux's scale are the header's");
      Check(Has(hlsli, "RtHitSunVisible(hit + normal * (0.004 + 0.002 * abs(hit.z)), toSun, f.shadowReach, CAR_RT_MASK_ALL)") && !Has(hlsli, "0.002 * hit.z"),
            "the sun ray's lift follows the magnitude of the view z (a hit behind the camera has a negative z: a negative lift puts the origin under the surface and the hit in its own shadow)");
      Check(Has(hlsli, "const float curve = pow(saturate(1.0 - nv), max(power, kCoatMinPower));") && Has(hlsli, "return clamp(gain * (kCoatNormal + (kCoatGrazing - kCoatNormal) * curve), 0.0, kCoatMax);") &&
                Has(hlsli, "return float((gCarRtMaterials[kRtHitFrameRows + instance * kMatRows].x >> 24) & 0xFFu) / 16.0;"),
            "the shader's coat share is CoatShare's, read from the paint's fresnel exponent in the instance's row (bits 24..31, x 16)");
      Check(Has(hlsli, "const float weight = (1.0 - a) * coneWeight + a * coatWeight;") && Has(hlsli, "carWeight = (1.0 - a) * min(coneCarSum / rays, 1.0) + a * coatCarWeight;") &&
                Has(hlsli, "colour = ((1.0 - a) * coneColourSum / rays + a * coatWeight * coatColour) / weight;") && Has(hlsli, "sharp = saturate(a * coatWeight / weight);") &&
                Has(hlsli, "const float coneWeight = min(coneWeightSum / rays, 1.0);"),
            "the shader's mix of the two lobes is MixCoat's");
      Check(Has(refl, "float4 coat;") && Has(refl, "uint LevelRays(uint word)") && Has(refl, "const uint rays = (word >> 8) & 7u;") && Has(refl, "return rays == 0u ? 4u : min(rays, 4u);") &&
                Has(refl, "const bool farHit = hitsOther || kShadePlayerHits;") && Has(refl, "const float hitReach = farHit ? g_push.coat.y : g_push.coat.x;") && Has(refl, "const float hitFadeFrom = farHit ? g_push.coat.z : g_push.tune.z;") &&
                Has(refl, "const float faded = 1.0 - smoothstep(hitFadeFrom * hitReach, hitReach, distance);"),
            "the trace's reach is the push's: the player's own parts' coat.x from tune.z of it, another car's coat.y from coat.z of it (HitFade), the cone's rays from slots.w bit 8 (RaysOf)");
      Check(Has(refl, "ConeRotation(texel, ConePhase(g_push.slots.w), (g_push.slots.w & kConeBlueFlag) != 0u)") && Has(opp, "ConeRotation(texel, ConePhase(g_push.slots.w), (g_push.slots.w & kConeBlueFlag) != 0u)") &&
                Has(refl, "uint ConePhase(uint word)") && Has(refl, "return (word >> 18u) & 1023u;") && hr::kPhaseShift == 18 && hr::kPhaseMask == 1023u,
            "the pattern's phase is slots.w bits 18 to 27 in both variants (ConePhase, PhaseOf)");
      Check(Has(refl, "const bool haveMirror = g_push.coat.w > 0.0 && tanHalf > 0.0 && levelRays >= 4u;") && Has(refl, "if (haveMirror)") && Has(refl, "coatShare = CarCoatShare(nv, CarMatFresnelPower(primaryInstance), g_push.coat.w);") &&
                Has(refl, "ConeDirection(mirror, normal, 0.0, 0, 0.0), reach, 0.0, primaryInstance") && Has(refl, "bool found = coatHit;") && Has(refl, "if (slot >= int(levelRays))") &&
                Has(refl, "aux.w = 1.0 - kAuxSharpScale * sharp;") && Has(refl, "const float weight = CarCoatMix(weightSum, colourSum, carSum, rays, MirrorShare(coatShare, mirrorOnCar), coatWeight, coatColour, coatCar ? coatWeight : 0.0, mixed, mixedCar, sharp);"),
            "the trace casts the clear coat's ray (the exact mirror direction, cone tangent 0) at the whole cone only, mixes it by the share and says the mirror's part in the aux's w");
      Check(Has(opp, "const bool haveMirror = g_push.coat.w > 0.0 && tanHalf > 0.0 && maxRays >= 4u;") && Has(opp, "if (haveMirror)") && Has(opp, "coatShare = CarCoatShare(nv, CarMatFresnelPower(primaryInstance), g_push.coat.w);") &&
                Has(opp, "bool found = coatHit;") && Has(opp, "aux.w = 1.0 - kAuxSharpScale * sharp;") &&
                Has(opp, "const float weight = CarCoatMix(weightSum, colourSum, carSum, rays, MirrorShare(coatShare, mirrorOnCar), coatWeight, coatColour, coatCar ? coatWeight : 0.0, mixed, mixedCar, sharp);") &&
                Has(opp, "TraceRay(history, origin, direction2, reach, tanHalf, primaryInstance"),
            "the opponents' variant does the same for the opponents' paint (at the budget's full cone)");
      Check(Has(denoise, "const float sharp = saturate((1.0 - ownAux.w) / kAuxSharpScale);") && Has(denoise, "* (1.0 - sharp);") && Has(denoise, "!(ownAux.w > 0.5)"),
            "the denoise scales its strength by what the mirror's share leaves, and the aux's 'has data' test is still w > 0.5");

      Check(Has(host_h, "float coat[4];") && Has(host_h, "static_assert(sizeof(Push) == 96);") && Has(opp_h, "float coat[4];") && Has(opp_h, "static_assert(sizeof(Push) == 112);") && Has(opp_h, "kBasePushBytes = 96;"),
            "the host's push blocks are the shader's: 96 bytes with coat, 112 with the opponents' opp");
      Check(Has(host, "car_rt_hit::LevelWith(base_level, double(larecomp::settings::mcla_gpu_car_rt_hit_reach.Get()),") &&
                Has(host, "double(larecomp::settings::mcla_gpu_car_rt_clearcoat.Get()));") && Has(host, "car_rt_hit::CastReach(parts, self_reach, others_reach)") &&
                Has(host, "car_rt_hit::PackPhaseRays(pattern_phase, cone_rays)") && Has(host, "const uint32_t cone_rays = city.on ? std::min(level.cone_rays, city.cell.cone_rays) : level.cone_rays;") &&
                Has(host, "push.coat[0] = self_reach;") && Has(host, "push.coat[1] = others_reach;") &&
                Has(host, "push.coat[2] = level.others_fade_from;") && Has(host, "push.coat[3] = level.coat;") && Has(host, "float(info.tan_y), length, tan_half, trace_regions);"),
            "the host's push is the level's: the reaches, the fade, the clear coat's gain, the cone's rays; the denoise decodes distances by the reach that was cast");
      Check(Has(opp_cpp, "std::memcpy(&push, f.base_push, kBasePushBytes);"), "the opponents' push is the player's with opp");
      Check(Has(scene, "vol_->CurrentSun(&sun, &fade)") && Has(scene, "car_rt_->SetSunCascades(cascades);") && Has(vol_h, "bool CurrentSun(volumetrics::VolSun* sun, float* fade) const;") &&
                Has(vol_cpp, "bool Volumetrics::CurrentSun(vol::VolSun* sun, float* fade) const {") && Has(vol_cpp, "bool Volumetrics::FillSun(vol::VolSun* out) const {"),
            "Scene hands the volumetric light's collector sun to the car scene before the trace's top level is built");
      for (const char* name : {"mcla_gpu_car_rt_hit_reach", "mcla_gpu_car_rt_clearcoat", "mcla_gpu_car_rt_hit_cascade", "mcla_gpu_car_rt_coat_layer"}) {
        Check(Has(settings, std::string(", ") + name + ", ") && Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";") && Has(manifest, std::string("\"") + name + "\": {") &&
                  Has(meta, std::string("{\"") + name + "\", \"MCLA/GPU\"}"),
              std::string("the setting ") + name + " is defined, aliased, in the manifest and in the meta list");
      }

      // RT-HIT-SHADING-3: the census, the exposure and the fill, in the shaders, the host and the settings
      {
        namespace ce = hr::census;
        const std::pair<const char*, uint32_t> words[] = {{"kStatTexels", ce::kTexels}, {"kStatRays", ce::kRays}, {"kStatOwn", ce::kOwn}, {"kStatSeen", ce::kSeen}, {"kStatBlend", ce::kBlend},
                                                          {"kStatShaded", ce::kShaded}, {"kStatStandIn", ce::kStandIn}, {"kStatDropped", ce::kDropped}, {"kStatByClass", ce::kByClass},
                                                          {"kStatLogLum", ce::kLogLum}, {"kStatLit", ce::kLit}, {"kStatShadow", ce::kShadow}, {"kStatAway", ce::kAway}, {"kStatGlow", ce::kGlow},
                                                          {"kStatCalCount", ce::kCalCount}, {"kStatCalSeen", ce::kCalSeen}, {"kStatCalModel", ce::kCalModel},
                                                          {"kStatAllCalCount", ce::kAllCalCount}, {"kStatAllCalSeen", ce::kAllCalSeen}, {"kStatAllCalModel", ce::kAllCalModel}, {"kStatCoat", ce::kCoat}};
        bool same = true;
        for (const auto& w : words) {
          same = same && Near(Constant(refl, std::string("static const uint ") + w.first), double(w.second), 0.0);
        }
        Check(same, "the shader's counter words are the header's");
        Check(Near(Constant(refl, "static const uint kStatsFlag"), double(ce::kStatsFlag), 0.0) && Near(Constant(refl, "static const float kCalFloor"), ce::kCalFloor) &&
                  Near(Constant(refl, "static const float kLogFloor"), ce::kLogFloor) && Near(Constant(refl, "static const float kLogBias"), ce::kLogBias) &&
                  Near(Constant(refl, "static const float kLogScale"), ce::kLogScale) && Near(Constant(refl, "static const float kLogMax"), double(ce::kLogMax)),
              "the shader's stats flag, the calibration's floor and the fixed-point log are the header's");
        Check(Has(refl, "RWByteAddressBuffer g_stats : register(u2, space1);") && Has(refl, "return (g_push.slots.w & kStatsFlag) != 0u;") && Has(refl, "g_stats.InterlockedAdd(word * 4u, value, original);") &&
                  Has(refl, "return uint(clamp((log2(max(luminance, kLogFloor)) + kLogBias) * kLogScale, 0.0, kLogMax));") && Has(refl, "return dot(colour, float3(0.2126, 0.7152, 0.0722));"),
              "the shader counts in a stats frame only, into the buffer at u2, with the header's luminance and fixed-point log");
        Check(Has(refl, "if (hitsOther && usable)") && Has(refl, "CensusCalibrate(instance, primitive, barycentrics, hitNormal, direction, hit, footprint, coneTan, CarHitHeight(hitInstance, hitsOther, hit), seen);") &&
                  Has(refl, "if (gCarHitClass == CAR_MAT_CLASS_PAINT)") && Has(refl, "CensusShaded(seenShare, shaded);") && Has(refl, "StatAdd(kStatDropped, 1u);") && Has(refl, "StatAdd(kStatStandIn, 1u);"),
              "the exposure's samples are hits on another car's parts the history shows in full (the player's own car in the opponents' pass is no sample), the paint's drive the gain; every outcome is counted");
        Check(Near(Constant(hlsli, "static const float kRtHitFillShare"), crm::kFillShare) && Has(hlsli, "const float3 fill = max(f.ambient, f.sun * lerp(kRtHitFillShare, kNightFillShare, night));") &&
                  Has(hlsli, "const float3 light = fill * (f.ambientGain * hemisphere * classAmbient * ground) + f.sun * (ndl * visible * daySun + lampLight);") && Has(hlsli, "gCarHitGlow = (m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u ? m.emissive * glowNight : 0.0;"),
              "the shader's sky fill is ShadeReference's");
        Check(Has(host, "output_builder_.addReadWriteByteAddressBuffer(2);") && Has(host, "bool CarRtRefl::MakeStatsBuffers(Device& device) {") &&
                  Has(host, "car_rt_hit::census::PackWithStats(car_rt_hit::PackPhaseRays(pattern_phase, cone_rays), stats_frame) |") &&
                  Has(host, "out.set->setBuffer(2, stats, car_rt_hit::census::kBytes);") && Has(host, "stats ? 3u : (second ? 2u : 1u)") &&
                  Has(host, "EnsureSet(trace_set_, output_builder_, image_, &aux_, \"car rt refl output set\", stats_.get())") &&
                  Has(host, "frames_->RetireAction([this, frame = info.frame, generation = stats_generation_] { ProcessStats(frame, generation); });") &&
                  Has(host, "hit_stops_ = census::NextStops(hit_stops_, residual);") && Has(host, "scene_->SetHitStops(HitStops());") &&
                  Has(host, "census::Text(window_, window_frames_, hit_stops_, calibrating)") && Has(host_h, "float HitStops() const;"),
              "the host makes the counters' buffers and binds them at u2, zeroes them before a stats frame, reads them back, moves the gain and logs the line");
        Check(Has(scene_cpp, "float(larecomp::settings::mcla_gpu_car_rt_hit_gain.Get()) * car_rt_hit::census::GainOf(hit_stops_)") && Has(scene_h, "void SetHitStops(float stops) { hit_stops_ = stops; }"),
              "the scene's frame gain is the setting's times the measured exposure");
        const std::string name = "mcla_gpu_car_rt_hit_calibrate";
        Check(Has(settings, std::string(", ") + name + ", ") && Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";") && Has(manifest, std::string("\"") + name + "\": {") &&
                  Has(meta, std::string("{\"") + name + "\", \"MCLA/GPU\"}"),
              "the setting mcla_gpu_car_rt_hit_calibrate is defined, aliased, in the manifest and in the meta list");
      }
      // RT-HIT-SHADING-4: the shaders', the variant's and the host's copies
      {
        const std::string variant = ReadFile(src + "/shaderpack/variants/car_selfrefl.cpp");
        const std::string selfrefl_cpp = ReadFile(src + "/gpu/car_selfrefl.cpp");
        Check(Near(Constant(hlsli, "static const float kOthersSharpShare"), hr::kOthersSharpShare) && Has(hlsli, "return mirrorOnCar ? max(coatShare, kOthersSharpShare) : coatShare;") &&
                  Has(hlsli, "return !coneOnCar || !haveMirror || mirrorOnCar;"),
              "the shaders' MirrorShare and ConeHitCounts are the header's");
        Check(Has(refl, "const bool mirrorOnCar = coatHit && coatCar;") && Has(refl, "if (fade[q] > 0.0 && !ConeHitCounts(car[q], haveMirror, mirrorOnCar))") &&
                  Has(refl, "carSum += car[j] ? w : 0.0;") && Has(refl, "return float4(colour, mixedCar >= 0.5 * weight ? -(1.0 + min(weight, 1.0)) : 1.0 + min(weight, 1.0));"),
              "the player's trace drops a cone ray's hit on another car where the mirror did not hit one, sums the weight on cars and signs the code by its majority");
        Check(Has(opp, "const bool mirrorOnCar = coatHit && coatCar;") && Has(opp, "if (fade[q] > 0.0 && !ConeHitCounts(car[q], haveMirror, mirrorOnCar))") &&
                  Has(opp, "carSum += car[j] ? w : 0.0;") && Has(opp, "return float4(colour, mixedCar >= 0.5 * weight ? -(1.0 + min(weight, 1.0)) : 1.0 + min(weight, 1.0));"),
              "the opponents' trace does the same");
        Check(Has(refl, "out bool other, out float3 hidden, out bool car)") && Has(refl, "car = hitsCar;") && Has(refl, "bool hitsCar = hitsOther;") &&
                  Has(refl, "hitsCar = !hitsOther || hitInstance.draw != CarRtLoadInstance(originInstance).draw;"),
              "TraceRay's `car`: another car in the player's pass; in the opponents' variant the player's car and the other opponents, not the opponent's own other parts");
        Check(Near(Constant(variant, "static const float kCarReflCoatF0"), hr::kCoatLayerF0) && Near(Constant(variant, "static const float kCarReflCoatPower"), hr::kCoatLayerPower) &&
                  Near(Constant(variant, "static const float kCarReflCoatMax"), hr::kCoatLayerMax),
              "the paint variant's layer numbers are the header's");
        Check(Has(variant, "(g_CarReflC.w & 2u) == 0u") && Has(selfrefl_cpp, "larecomp::settings::mcla_gpu_car_rt_coat_layer.Get()") && Has(selfrefl_cpp, "car_selfrefl::ExpBiasGain(exp_bias), rt_slot, coat_layer, parked);"),
              "the paint's layer is switched by the constants' bit 1, which the host sets from mcla_gpu_car_rt_coat_layer");
      }
      Check(!cmake.empty() && Has(cmake, "larecomp_car_rt_hit_rules_test"), "the CMake file builds this test");
    }
  }

  {
    const auto paint = hr::PaintLevel();
    Check(paint.cone_rays == 1u && paint.others_reach == 18.0f && paint.coat == 1.0f,
          "EV-9 uses one sample for the cone and coat at a fixed reach");
    const float lo[3] = {-1.0f, -1.0f, -1.0f};
    const float hi[3] = {1.0f, 1.0f, 1.0f};
    const float near_lo[3] = {19.0f, -1.0f, -1.0f};
    const float far_lo[3] = {19.0f, 2.0f, -1.0f};
    const float parked_hi[3] = {23.0f, 4.0f, 1.0f};
    Check(hr::PaintBoxInReach(lo, hi, near_lo, parked_hi), "a parked car on the reach boundary is retained");
    Check(!hr::PaintBoxInReach(lo, hi, far_lo, parked_hi), "diagonal gaps count toward the parked car reach");
    Check(hr::PaintBoxInReach(nullptr, hi, far_lo, parked_hi), "unknown bounds keep a parked car");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
