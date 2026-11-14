// Check of the bounce light's cap, falloff and gain rules (ssgi_light.h) and of its strength levels (ssgi_level.h). A
// program of its own: it needs no renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ssgi_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ssgi_test [<src/gpu/hlsl dir>]
//
// With the hlsl folder as its argument it also checks that the shaders' constants are the header's (the shaders
// repeat them) and that ao.cpp's falloff ratio is the one the far ring's falloff assumes.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ssgi_level.h"
#include "ssgi_light.h"

using namespace mcla::gpu;
using namespace mcla::gpu::ssgi_light;

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
bool Near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

// A deterministic generator for the random sweeps.
struct Lcg {
  uint32_t state = 12345u;
  float Next() {  // 0 to 1
    state = state * 1664525u + 1013904223u;
    return float(state >> 8) / float(1u << 24);
  }
  float Range(float lo, float hi) { return lo + (hi - lo) * Next(); }
};

// ---------------------------------------------------------------------------
// SoftClip
// ---------------------------------------------------------------------------
void SoftClipChecks() {
  for (float limit : {0.5f, 1.0f, 2.6f, 8.0f}) {
    // linear up to the knee
    bool linear = true;
    for (int i = 0; i <= 50; ++i) {
      const float x = kKnee * limit * float(i) / 50.0f;
      linear = linear && SoftClip(x, limit) == x;
    }
    Check(linear, Num("SoftClip is the identity up to the knee (limit %.2f)", limit));
    // continuous with slope 1 at the knee
    const float knee = kKnee * limit;
    const float e = 1e-3f * limit;
    const float slope_above = (SoftClip(knee + e, limit) - SoftClip(knee, limit)) / e;
    Check(Near(slope_above, 1.0, 5e-3), Num("slope 1 at the knee, limit %.2f (got %.4f)", limit, slope_above));
    // never over the limit, never decreasing, close to it far out
    bool bounded = true;
    bool monotone = true;
    float last = 0.0f;
    for (int i = 0; i <= 4000; ++i) {
      const float x = limit * 0.01f * float(i);
      const float y = SoftClip(x, limit);
      bounded = bounded && y <= limit && y <= x + 1e-6f;
      monotone = monotone && y >= last - 1e-6f;
      last = y;
    }
    Check(bounded, Num("SoftClip is never over its limit nor over its input (limit %.2f)", limit));
    Check(monotone, Num("SoftClip never decreases (limit %.2f)", limit));
    Check(SoftClip(1e6f, limit) <= limit && SoftClip(1e6f, limit) > 0.999f * limit,
          Num("SoftClip(1e6) is the limit (limit %.2f)", limit));
    Check(Near(SoftClip(limit, limit), limit * (0.5 + 0.5 * (1.0 - std::exp(-1.0))), 1e-5),
          Num("SoftClip(limit) = 0.816 limit (limit %.2f)", limit));
  }
  Check(SoftClip(3.0f, 0.0f) == 0.0f && SoftClip(3.0f, -1.0f) == 0.0f, "a limit of 0 or less gives 0");
  Check(SoftClip(0.0f, 2.0f) == 0.0f, "SoftClip(0) = 0");
}

// ---------------------------------------------------------------------------
// the radiance cap and the source of light
// ---------------------------------------------------------------------------
void CapChecks() {
  // a day: ambient 0.3, sun 1.0 (T = 1.3), the default cap 2 -> 2.6
  const float day_cap = RadianceCap(2.0f, 0.3f, 1.0f);
  Check(Near(day_cap, 2.6, 1e-6), Num("the day's cap is 2 x (ambient + sun) = 2.6 (got %.3f)", day_cap));
  // a night: ambient 0.05, no sun: the floor
  Check(RadianceCap(2.0f, 0.05f, 0.0f) == kCapFloor, "a night's cap is the floor");
  Check(RadianceCap(4.0f, 0.05f, 0.0f) == kCapFloor, "a night's cap is the floor at the highest level too");

  // a white wall in full sun (albedo 1: colour = T = 1.3) keeps its radiance at every level's cap
  for (int level = 0; level < ssgi_level::kLevelCount; ++level) {
    const ssgi_level::Preset& p = ssgi_level::kPresets[level];
    const float cap = RadianceCap(p.cap, 0.3f, 1.0f);
    const Rgb white = {1.3f, 1.3f, 1.3f};
    const Rgb carried = CapRadiance(white, cap);
    Check(Luma(carried) > 0.78f * Luma(white),
          Num("a white wall in full sun carries >= 78%% of its radiance into the bounce (level %.0f: got %.3f)", level,
              Luma(carried) / Luma(white)));
    if (p.cap >= 2.0f) {
      Check(carried.r == white.r, Num("a surface under half the cap is not touched at all (level %.0f)", level));
    }
  }
  // a neon sign (luma 40) is cut to under the cap, its hue kept; a lamp's glow likewise
  const Rgb neon = {40.0f, 8.0f, 30.0f};
  const Rgb cut = CapRadiance(neon, day_cap);
  Check(Luma(cut) <= day_cap && Luma(cut) > 0.99f * day_cap, Num("neon is cut to the cap (luma %.3f of %.3f)", Luma(cut), day_cap));
  Check(Near(cut.r / cut.g, neon.r / neon.g, 1e-4) && Near(cut.b / cut.g, neon.b / neon.g, 1e-4), "the cut keeps the hue");
  Check(CapRadiance({0.0f, 0.0f, 0.0f}, day_cap).r == 0.0f, "a black sample stays black");

  // the source ramp: 0 under the start, 1 at 1.5 x it
  const float start = SourceStart(0.3f, 1.0f);
  Check(Near(start, 2.6, 1e-6), "a source starts at 2 x (ambient + sun)");
  Check(SourceShare(1.3f, start) == 0.0f && SourceShare(start, start) == 0.0f, "a surface is no source");
  Check(Near(SourceShare(1.25f * start, start), 0.5, 1e-5), "half a source at 1.25 x the start");
  Check(SourceShare(1.5f * start, start) == 1.0f && SourceShare(40.0f, start) == 1.0f, "a source from 1.5 x the start");
  bool mono = true;
  float last = -1.0f;
  for (int i = 0; i < 200; ++i) {
    const float s = SourceShare(0.05f * float(i), start);
    mono = mono && s >= last;
    last = s;
  }
  Check(mono, "the source ramp never decreases");
  Check(SourceStart(0.05f, 0.0f) == kCapFloor, "a night's source start is the floor");
}

// ---------------------------------------------------------------------------
// the far ring
// ---------------------------------------------------------------------------
void FarRingChecks() {
  const float reach = 6.0f;
  Check(FarFalloff(0.0f, reach) == 1.0f, "falloff 1 at the pixel");
  Check(FarFalloff((1.0f - kFalloffRatio) * reach - 0.01f, reach) == 1.0f, "falloff 1 up to 38.5% of the reach");
  Check(FarFalloff(reach, reach) == 0.0f && FarFalloff(3.0f * reach, reach) == 0.0f, "falloff 0 at the reach and past it");
  Check(Near(FarFalloff(0.5f * reach, reach), 1.0 / kFalloffRatio - 0.5 / kFalloffRatio, 1e-5), "falloff at half the reach is 1/ratio - 0.5/ratio");
  bool mono = true;
  float last = 2.0f;
  for (int i = 0; i <= 200; ++i) {
    const float w = FarFalloff(0.05f * float(i), reach);
    mono = mono && w <= last && w >= 0.0f && w <= 1.0f;
    last = w;
  }
  Check(mono, "the far falloff never rises with distance and stays in 0 to 1");
  Check(FarFalloff(5.0f, 10.0f) > FarFalloff(5.0f, 6.0f), "a longer reach keeps a sample at the same distance more");

  // the ring's radius and when it exists (720p: 0.4 of 720 = 288 px)
  Check(FarRadiusPx(6.0f, 100.0f, 720.0f) == 288.0f, "the ring's radius is capped at 0.4 of the height");
  Check(Near(FarRadiusPx(6.0f, 20.0f, 720.0f), 120.0, 1e-4), "the ring's radius is the reach in pixels below the cap");
  Check(FarRingExists(80.0f, 120.0f, 2), "a ring that goes 50% farther exists");
  Check(!FarRingExists(216.0f, 270.0f, 2), "a ring that goes 25% farther than a capped near radius does not (270 < 216 x 1.25 + 2)");
  Check(FarRingExists(216.0f, 288.0f, 2), "the capped near radius at 720p still has a ring out to 288");
  Check(!FarRingExists(10.0f, 100.0f, 0), "no steps: no ring");
  Check(!FarRingExists(10.0f, 11.0f, 2), "a ring under 2 px wider than the near samples does not exist");

  // the samples: inside [near, far], increasing with the step, the first nearer than a uniform spacing
  for (uint32_t steps : {1u, 2u, 3u, 4u}) {
    bool inside = true;
    bool increasing = true;
    for (float noise : {0.0f, 0.3f, 0.99f}) {
      float prev = 0.0f;
      for (uint32_t s = 0; s < steps; ++s) {
        const float len = FarOffsetPx(50.0f, 200.0f, s, steps, noise);
        inside = inside && len >= 50.0f && len <= 200.0f;
        increasing = increasing && len >= prev;
        prev = len;
      }
    }
    Check(inside, Num("every ring sample is between the near end and the far end (steps %.0f)", double(steps)));
    Check(increasing, Num("ring samples go outward with the step (steps %.0f)", double(steps)));
  }
  Check(FarOffsetPx(50.0f, 200.0f, 0, 2, 0.5f) < 50.0f + 150.0f * 0.5f * 0.5f + 1e-3f,
        "the first of two ring samples is in the inner quarter of the ring (squared spacing)");
  Check(Near(FarOffsetPx(50.0f, 200.0f, 1, 2, 0.99999f), 200.0, 0.1), "the last sample reaches the far end");

  // chain levels
  Check(FarLevel(10.0f) == 1 && FarLevel(24.0f) == 1 && FarLevel(33.0f) == 2 && FarLevel(500.0f) == 2,
        "ring samples read level 1 (to 32 px) or 2 (from 32 px), never level 0");
  // steps by quality
  Check(FarSteps(true, -1, 6.0f) == kFarStepsHigh && FarSteps(false, -1, 6.0f) == kFarStepsLow, "ring steps: 2 at high, 1 at low");
  Check(FarSteps(true, -1, 0.0f) == 0 && FarSteps(true, 4, 0.0f) == 0, "no reach, no ring, whatever the steps");
  Check(FarSteps(true, 0, 6.0f) == 0 && FarSteps(false, 4, 6.0f) == 4 && FarSteps(true, 99, 6.0f) == kFarStepsMax,
        "forced ring steps: 0 is none, more is kept to what the push block holds");
  Check(kFarStepsMax == 7, "the push block holds 3 bits of steps");
}

// ---------------------------------------------------------------------------
// the push block's codes
// ---------------------------------------------------------------------------
void CodeChecks() {
  Check((kLimitMask & kCapMask) == 0, "the limit's bits and the cap's do not overlap");
  // the apply's flags: 0-7 view, 16 low, 17-19 denoise, 20-21 temporal, 22-24 apply terms
  const uint32_t others = 0xFFu | (1u << 16) | (7u << 17) | (3u << 20) | (7u << 22) | (1u << 25);
  Check(((kLimitMask | kCapMask) & others) == 0, "the limit and the cap use bits no other flag of the apply has");
  Check(kLimitShift == 8 && kCapShift == 26, "the shifts are the shaders' (8 and 26)");
  Check(Near(kLimitMax, 10.2, 1e-4) && Near(kCapMax, 15.75, 1e-6), "the codes hold 10.2 and 15.75");
  for (int level = 0; level < ssgi_level::kLevelCount; ++level) {
    const ssgi_level::Preset& p = ssgi_level::kPresets[level];
    Check(Near(LimitOfCode(LimitCode(p.limit)), p.limit, 0.5 * kLimitUnit + 1e-6),
          Num("the limit %.3f survives its code", p.limit));
    Check(Near(CapOfCode(CapCode(p.cap)), p.cap, 0.5 * kCapUnit + 1e-6), Num("the cap %.3f survives its code", p.cap));
    Check(CapOfCode(CapCode(p.cap)) == p.cap, Num("the cap %.3f is exact in units of 0.25", p.cap));
  }
  Check(((LimitCode(10.2f) << kLimitShift) & kLimitMask) == (255u << kLimitShift), "the largest limit code is 255");
  Check(((CapCode(15.75f) << kCapShift) & kCapMask) == (63u << kCapShift), "the largest cap code is 63 and fits the top bits");
  Check(CapCode(99.0f) == 63 && LimitCode(99.0f) == 255 && CapCode(-1.0f) == 0, "codes are clamped");
}

// ---------------------------------------------------------------------------
// the apply: the gain limit and the ceiling
// ---------------------------------------------------------------------------
ApplyIn Shaded(const ssgi_level::Preset& p) {
  ApplyIn in;
  in.ambient = 0.3f;
  in.sun = 1.0f;
  in.sun_term = 0.0f;                   // in the shade: T = the ambient
  in.colour = {0.15f, 0.15f, 0.15f};    // albedo 0.5 x 0.3
  in.bounce_radiance = {0.20f, 0.20f, 0.20f};
  in.ao_part = 1.0f;
  in.strength = p.strength;
  in.limit = p.limit;
  in.cap = p.cap;
  return in;
}

void ApplyChecks() {
  // A shaded pixel beside a sunlit surface: the old rule gave it at most half of its own colour, the new one lets it gain
  {
    const ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kDefault));
    const Rgb fresh = BounceFactor(in);
    const Rgb old = OldBounceFactor(in, 0.5f);
    Check(old.r <= 0.5f + 1e-6f, "the old rule kept the factor to 0.5");
    Check(fresh.r > 1.5f * old.r, Num("a shaded pixel gains far more than under the old limit (new %.3f, old %.3f)", fresh.r, old.r));
    Check(fresh.r <= in.limit + 1e-5f, "... and never over the level's gain limit");
    Check(Near(fresh.r, 2.0 * 0.20 / 0.30, 1e-4), Num("below the knees the factor is strength x B / T = 1.333 (got %.4f)", fresh.r));
  }
  // every level, a shaded pixel: the gain grows with the level (the order of the row)
  {
    float last = -1.0f;
    bool grows = true;
    for (int level = 0; level < ssgi_level::kLevelCount; ++level) {
      const float f = BounceFactor(Shaded(ssgi_level::kPresets[level])).g;
      grows = grows && f > last;
      last = f;
    }
    Check(grows, "a shaded pixel's bounce grows with the level, LIGHT to STRONGEST");
  }
  // A source of light gets nothing; half a source half of it
  {
    ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kStrong));
    const float full = BounceFactor(in).g;
    in.source = 1.0f;
    Check(BounceFactor(in).g == 0.0f, "a pixel that is a source of light gets no bounce");
    in.source = 0.5f;
    Check(Near(BounceFactor(in).g, 0.5f * full, 0.2f * full), "half a source gets about half (below the knees it is exactly half)");
  }
  // The ceiling: with a huge incoming radiance and every level, no pixel ends up brighter than the cap's light, and a pixel
  // already above it gets nothing
  {
    Lcg rng;
    bool ok = true;
    bool limit_ok = true;
    bool nonneg = true;
    float worst = 0.0f;
    for (int i = 0; i < 40000; ++i) {
      const ssgi_level::Preset& p = ssgi_level::kPresets[i % ssgi_level::kLevelCount];
      ApplyIn in;
      in.ambient = rng.Range(0.02f, 0.8f);
      in.sun = rng.Next() < 0.2f ? 0.0f : rng.Range(0.1f, 2.0f);
      in.sun_term = in.sun * rng.Next() * (rng.Next() < 0.5f ? 0.0f : 1.0f);
      in.colour = {rng.Range(0.0f, 3.0f), rng.Range(0.0f, 3.0f), rng.Range(0.0f, 3.0f)};
      const float big = rng.Next() < 0.3f ? 60.0f : 3.0f;
      in.bounce_radiance = {rng.Range(0.0f, big), rng.Range(0.0f, big), rng.Range(0.0f, big)};
      in.source = rng.Next() < 0.3f ? rng.Next() : 0.0f;
      in.ao_part = rng.Range(0.3f, 1.0f);
      in.strength = p.strength * rng.Range(0.0f, 1.0f);
      in.fade = rng.Next();
      in.limit = p.limit;
      in.cap = p.cap;
      const Rgb b = BounceFactor(in);
      nonneg = nonneg && b.r >= 0.0f && b.g >= 0.0f && b.b >= 0.0f && std::isfinite(b.r + b.g + b.b);
      limit_ok = limit_ok && MaxChannel(b) <= in.limit + 1e-4f;
      const float own = Luma(in.colour) * in.ao_part;
      const float ceiling = RadianceCap(in.cap, in.ambient, in.sun);
      const float total = own + Luma({in.colour.r * b.r, in.colour.g * b.g, in.colour.b * b.b});
      const float allowed = std::max(own, ceiling);
      ok = ok && total <= allowed + 1e-3f * allowed + 1e-5f;
      worst = std::max(worst, total - allowed);
    }
    Check(nonneg, "the bounce factor is finite and not negative over 40000 random pixels");
    Check(limit_ok, "the factor is never over the level's gain limit (40000 random pixels)");
    Check(ok, Num("no pixel ends up brighter than the cap's light or than it was (40000 random pixels, worst excess %.6f)", worst));
  }
  // A pixel at the ceiling gets nothing
  {
    ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kDefault));
    const float ceiling = RadianceCap(in.cap, in.ambient, in.sun);
    in.colour = {ceiling, ceiling, ceiling};
    Check(BounceFactor(in).g == 0.0f, "a pixel already at the ceiling gets no bounce");
  }
  // NEON does not whiten a wall: a dark wall beside a huge emitter, the radiance capped as the colour chain caps it
  for (int level = 0; level < ssgi_level::kLevelCount; ++level) {
    const ssgi_level::Preset& p = ssgi_level::kPresets[level];
    ApplyIn in = Shaded(p);
    const float cap = RadianceCap(p.cap, in.ambient, in.sun);
    const Rgb neon = CapRadiance({60.0f, 5.0f, 40.0f}, cap);
    in.bounce_radiance = Scaled(neon, 0.5f);  // the neon fills half of the wall's hemisphere
    const Rgb b = BounceFactor(in);
    const float result = Luma({in.colour.r * (1.0f + b.r), in.colour.g * (1.0f + b.g), in.colour.b * (1.0f + b.b)});
    const float sunlit_white = in.ambient + in.sun;
    Check(result <= p.cap * sunlit_white + 1e-4f,
          Num("a dark wall beside neon ends under the cap's light (level %.0f: %.3f)", level, result));
    Check(result <= 0.75f * p.cap * sunlit_white + 1e-3f,
          Num("... and well under it, not white (level %.0f: %.3f of %.3f)", level, result, p.cap * sunlit_white));
  }
  // a night: a lamp's pool beside a dark wall: noticeably lit, never bright
  {
    ApplyIn in;
    in.ambient = 0.05f;
    in.sun = 0.0f;
    in.colour = {0.02f, 0.02f, 0.02f};
    in.bounce_radiance = {0.25f, 0.25f, 0.2f};  // the pool, capped at 0.5, over half the hemisphere
    const ssgi_level::Preset& p = ssgi_level::PresetOf(ssgi_level::Level::kDefault);
    in.strength = p.strength;
    in.limit = p.limit;
    in.cap = p.cap;
    const Rgb b = BounceFactor(in);
    const float lit = in.colour.g * (1.0f + b.g);
    Check(lit > 2.0f * in.colour.g, Num("a night wall beside a lamp's pool is lit (%.4f from 0.02)", lit));
    Check(lit < 0.35f, Num("... and stays dim, a lamp's light and not a white wall (%.4f)", lit));
  }
  // monotone in the incoming radiance
  {
    ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kStronger));
    float last = -1.0f;
    bool grows = true;
    for (int i = 0; i <= 100; ++i) {
      const float b = 0.01f * float(i);
      in.bounce_radiance = {b, b, b};
      const Rgb out = BounceFactor(in);
      const float added = in.colour.g * out.g;
      grows = grows && added >= last - 1e-6f;
      last = added;
    }
    Check(grows, "the light added never falls as the incoming radiance rises");
  }
  // the hue of the bounce is kept by the soft limits
  {
    ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kStrongest));
    in.bounce_radiance = {2.0f, 0.5f, 0.1f};  // a red wall
    const Rgb b = BounceFactor(in);
    Check(Near(b.g / b.r, 0.25, 1e-4) && Near(b.b / b.r, 0.05, 1e-4), "the limits scale the factor's channels together: the hue stays");
  }
  // flat ground (no blocked light) gets nothing
  {
    ApplyIn in = Shaded(ssgi_level::PresetOf(ssgi_level::Level::kStrongest));
    in.bounce_radiance = {0.0f, 0.0f, 0.0f};
    const Rgb b = BounceFactor(in);
    Check(b.r == 0.0f && b.g == 0.0f && b.b == 0.0f, "no blocked light, no bounce");
  }
}

// ---------------------------------------------------------------------------
// the levels
// ---------------------------------------------------------------------------
void LevelChecks() {
  using namespace ssgi_level;
  Check(kLevelCount == 5, "five levels");
  const char* const words[] = {"light", "default", "strong", "stronger", "strongest"};
  for (int i = 0; i < kLevelCount; ++i) {
    Level level = Level::kDefault;
    Check(Parse(words[i], &level) && int(level) == i && Name(level) == words[i], Num("level %.0f parses to its word and back", i));
    bool known = false;
    std::string upper = words[i];
    for (char& ch : upper) {
      ch = char(ch - 'a' + 'A');
    }
    Check(Resolve(upper, &known) == Level(i) && known, Num("level %.0f in capitals is the same level", i));
  }
  bool known = true;
  Check(Resolve("", &known) == Level::kDefault && !known, "an empty text is default, not known");
  Check(Resolve("max", &known) == Level::kDefault && !known, "a word of another setting is default");
  Check(Resolve("stronge", &known) == Level::kDefault && !known, "a cut word (a 7 character text cache) is default: the levels do not use one");
  Check(Resolve("auto", &known) == Level::kDefault, "auto is default");
  Check(kDefaultLevel == Level::kDefault, "the setting's default is `default`");
  Check(Name(Level::kStrongest).size() == 9, "STRONGEST is 9 characters (more than a LiveText's 7: the level is read through its own atomic)");

  // the presets grow in every number, are within the ranges, and DEFAULT is as documented
  for (int i = 1; i < kLevelCount; ++i) {
    const Preset& a = kPresets[i - 1];
    const Preset& b = kPresets[i];
    Check(b.strength > a.strength && b.limit > a.limit && b.reach > a.reach && b.cap > a.cap,
          Num("level %.0f is stronger than the one before in every number", i));
  }
  for (const Preset& p : kPresets) {
    Check(p.strength > 0.0f && p.strength <= kStrengthMax, Num("strength %.2f in range", p.strength));
    Check(p.limit >= kLimitMin && p.limit <= ssgi_level::kLimitMax, Num("limit %.2f in range", p.limit));
    Check(p.reach > 0.0f && p.reach <= kReachMax, Num("reach %.2f in range", p.reach));
    Check(p.cap >= kCapMin && p.cap <= ssgi_level::kCapMax, Num("cap %.2f in range", p.cap));
    Check(p.limit > 0.5f, Num("every limit is above the old 0.5 (%.2f)", p.limit));
    Check(p.reach > 3.0f, Num("every reach is past the occlusion's default 3 m (%.2f)", p.reach));
  }
  const Preset& d = PresetOf(Level::kDefault);
  Check(d.strength == 2.0f && d.limit == 4.5f && d.reach == 9.0f && d.cap == 2.0f, "DEFAULT is 2, 4.5, 9 m, 2");
  // the owner's most with the old cvars (clamp 4, radius x3 of a 3 m occlusion radius = 9 m, strength 2) looked "still subtle": DEFAULT is at least that
  Check(d.strength >= 2.0f && d.limit >= 4.0f && d.reach >= 9.0f, "DEFAULT is at or over the most the old cvars gave (strength 2, limit 4, reach 9 m)");
  Check(PresetOf(Level::kStrong).strength > d.strength * 1.25f && PresetOf(Level::kStrongest).strength >= 3.0f * d.strength, "STRONG to STRONGEST go clearly beyond DEFAULT");
  Check(PresetOf(Level::kStrongest).cap <= 4.0f, "the cap (the whitening guard) stays modest even at STRONGEST");
  Check(Near(ssgi_level::kLimitMax, ssgi_light::kLimitMax, 1e-4), "the level range's limit maximum is the push block's");
  Check(Near(ssgi_level::kCapMax, ssgi_light::kCapMax, 1e-6), "the level range's cap maximum is the push block's");

  // the overrides
  Overrides none;
  for (int i = 0; i < kLevelCount; ++i) {
    const Effective e = Choose(Level(i), none);
    const Preset& p = kPresets[i];
    Check(e.strength == p.strength && e.limit == p.limit && e.reach == p.reach && e.cap == p.cap && e.far_steps == -1 && !e.overridden,
          Num("level %.0f with no override is its preset", i));
  }
  {
    Overrides ov;
    ov.strength = 0.0;
    Effective e = Choose(Level::kDefault, ov);
    Check(e.strength == 0.0f && e.overridden && e.limit == 4.5f, "a strength override of 0 is 0 (not 'not set') and changes only that number");
    ov = Overrides{};
    ov.reach = 0.0;
    e = Choose(Level::kStrong, ov);
    Check(e.reach == 0.0f && e.strength == 3.0f && e.overridden, "a reach override of 0 turns the ring off");
    ov = Overrides{};
    ov.strength = 50.0;
    ov.limit = 0.0;
    ov.cap = 0.0;
    ov.reach = 99.0;
    e = Choose(Level::kLight, ov);
    Check(e.strength == kStrengthMax && e.limit == kLimitMin && e.cap == kCapMin && e.reach == kReachMax, "overrides are kept in their ranges");
    ov = Overrides{};
    ov.far_steps = 4;
    e = Choose(Level::kLight, ov);
    Check(e.far_steps == 4 && e.overridden, "the ring's steps can be forced");
    ov = Overrides{};
    ov.strength = -1.0;
    ov.limit = -0.5;
    e = Choose(Level::kStrongest, ov);
    Check(!e.overridden && e.strength == 7.0f, "a negative override is not set");
  }
  // the row's width: "BOUNCE LIGHT STRENGTH: " + the longest word, under the page's 41
  size_t longest = 0;
  for (int i = 0; i < kLevelCount; ++i) {
    longest = std::max(longest, Name(Level(i)).size());
  }
  Check(std::string("BOUNCE LIGHT STRENGTH: ").size() + longest <= 41, "the row fits the page's 41 characters");
}

// ---------------------------------------------------------------------------
// the shaders' constants
// ---------------------------------------------------------------------------
bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

// The value of `static const float <name> = <number>f;` in `text`; NaN when it is not there.
double ConstOf(const std::string& text, const std::string& name) {
  const std::string key = name + " = ";
  const size_t at = text.find(key);
  if (at == std::string::npos) {
    return std::nan("");
  }
  return std::strtod(text.c_str() + at + key.size(), nullptr);
}

void ShaderConstantsMatch(const std::string& dir) {
  std::string common, color, main_pass, apply, ao_cpp;
  if (!ReadFile(dir + "/ssgi_common.hlsli", &common) || !ReadFile(dir + "/ao_color_ps.hlsl", &color) ||
      !ReadFile(dir + "/ao_main_ps.hlsl", &main_pass) || !ReadFile(dir + "/ao_apply_ps.hlsl", &apply)) {
    Check(false, "the shader files could not be read from " + dir);
    return;
  }
  Check(Near(ConstOf(common, "kSsgiCapFloor"), kCapFloor, 1e-9), "ssgi_common.hlsli kSsgiCapFloor is the header's kCapFloor");
  Check(Near(ConstOf(common, "kSsgiKnee"), kKnee, 1e-9), "ssgi_common.hlsli kSsgiKnee is kKnee");
  Check(Near(ConstOf(common, "kSsgiSourceSpan"), kSourceSpan, 1e-9), "ssgi_common.hlsli kSsgiSourceSpan is kSourceSpan");
  Check(Near(ConstOf(common, "kSsgiFarMaxRadiusShare"), kFarMaxRadiusShare, 1e-6), "kSsgiFarMaxRadiusShare is the header's");
  Check(Near(ConstOf(common, "kSsgiFarMinGrowth"), kFarMinGrowth, 1e-9), "kSsgiFarMinGrowth is the header's");
  Check(Near(ConstOf(common, "kSsgiFarMinExtraPx"), kFarMinExtraPx, 1e-9), "kSsgiFarMinExtraPx is the header's");
  Check(common.find("exp(-(x - knee) / span)") != std::string::npos && common.find("knee + span * (1.0f - exp(") != std::string::npos,
        "the shader's soft clip is the header's formula");
  Check(common.find("saturate((luma / max(start, 1e-3f) - 1.0f) / (kSsgiSourceSpan - 1.0f))") != std::string::npos,
        "the shader's source ramp is the header's formula");
  // the colour pass uses both; the main pass reads its steps from bits 26-28 and the reach from extra.w
  Check(color.find("SsgiSoftClip(luma, cap)") != std::string::npos && color.find("SsgiSourceShare(luma, g_push.shape.y)") != std::string::npos,
        "the colour pass takes the soft cap from shape.x and the source start from shape.y");
  Check(main_pass.find(">> 26) & 0x7u") != std::string::npos && main_pass.find("g_push.extra.w") != std::string::npos,
        "the main pass reads the ring's steps from flags bits 26-28 and its reach from extra.w");
  Check(main_pass.find("kSsgiFarMaxRadiusShare") != std::string::npos && main_pass.find("kSsgiFarMinGrowth") != std::string::npos,
        "the main pass uses the ring's constants");
  Check(main_pass.find("countbits(occupied_near)") != std::string::npos,
        "the visibility is counted from the near samples' mask");
  // the apply: the limit's unit and bits, the cap's unit and bits
  const std::string unit_limit = Num("(g_push.slots2.z >> %.0f) & 0xFFu) * %.2ff", double(kLimitShift), double(kLimitUnit));
  const std::string unit_cap = Num("(g_push.slots2.z >> %.0f) & 0x3Fu) * %.2ff", double(kCapShift), double(kCapUnit));
  Check(apply.find(unit_limit) != std::string::npos, "the apply reads the limit from bits 8-15 in units of 0.04");
  Check(apply.find(unit_cap) != std::string::npos, "the apply reads the cap from bits 26-31 in units of 0.25");
  Check(apply.find("SsgiSoftClip(bounce_peak, GiLimit())") != std::string::npos && apply.find("SsgiSoftClip(added, room)") != std::string::npos,
        "the apply clips the gain and then the light added to the room under the ceiling");
  Check(apply.find("max(GiCap() * (ambient + g_push.extra.x), kSsgiCapFloor)") != std::string::npos,
        "the apply's ceiling is the header's RadianceCap");
  Check(apply.find("dot(gi_colour.rgb * bounce, kLuma)") != std::string::npos, "the apply's added light is the colour times the bounce, in luma");
  // ao.cpp
  if (ReadFile(dir + "/../ao.cpp", &ao_cpp)) {
    Check(Near(ConstOf(ao_cpp, "kFalloffRatio"), kFalloffRatio, 1e-6), "ao.cpp's kFalloffRatio is the far falloff's");
    Check(ao_cpp.find("ssgi_light::RadianceCap(plan.gi.cap") != std::string::npos && ao_cpp.find("ssgi_light::SourceStart(") != std::string::npos,
          "ao.cpp sends the colour pass the cap and the source start from the header");
    Check(ao_cpp.find("kGiFarStepsShift = 26") != std::string::npos, "ao.cpp puts the ring's steps in bits 26-28 of the main pass's flags");
  } else {
    Check(false, "ao.cpp could not be read next to the hlsl folder");
  }
}

}  // namespace

int main(int argc, char** argv) {
  SoftClipChecks();
  CapChecks();
  FarRingChecks();
  CodeChecks();
  ApplyChecks();
  LevelChecks();
  if (argc > 1) {
    ShaderConstantsMatch(argv[1]);
  } else {
    std::printf("(no hlsl folder given: the shaders' constants were not compared)\n");
  }
  std::printf("ssgi_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
