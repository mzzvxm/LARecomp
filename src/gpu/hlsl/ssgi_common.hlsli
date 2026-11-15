// MCLA GPU renderer -- the bounce light's cap rules, shared by the colour chain (ao_color_ps.hlsl) and the apply
// (ao_apply_ps.hlsl, AO_GI). The same numbers and functions as ssgi_light.h, which ssgi_test.cpp checks (it also reads
// the constants below from this file and compares them). See ssgi_light.h for what each rule is for.

#ifndef MCLA_GPU_SSGI_COMMON_HLSLI
#define MCLA_GPU_SSGI_COMMON_HLSLI

// The least a radiance cap is, in the scene colour's units: a night's dim constants (ambient 0.05) must not cap a
// lamp's pool to nothing.
static const float kSsgiCapFloor = 0.5f;
// SoftClip is linear up to this share of its limit.
static const float kSsgiKnee = 0.5f;
// A colour starts to count as a source of light at kSsgiSourceMult times the frame's light and is entirely one at
// kSsgiSourceSpan times that (ao.cpp sends the start in luma; the span is here).
static const float kSsgiSourceSpan = 1.5f;
// The far ring (ao_main_ps.hlsl, AO_GI): never farther than this share of the image's height, and drawn where it
// reaches this much farther than the near samples and at least this many pixels farther.
static const float kSsgiFarMaxRadiusShare = 0.4f;
static const float kSsgiFarMinGrowth = 1.25f;
static const float kSsgiFarMinExtraPx = 2.0f;

// x up to kSsgiKnee * limit unchanged, then bending to `limit`: continuous, never over it, never decreasing. A limit
// of 0 or less gives 0.
float SsgiSoftClip(float x, float limit) {
  if (!(limit > 0.0f)) {
    return 0.0f;
  }
  const float knee = kSsgiKnee * limit;
  if (!(x > knee)) {
    return x;
  }
  const float span = limit - knee;
  return knee + span * (1.0f - exp(-(x - knee) / span));
}

// 0 a surface, 1 a source of light: a ramp of the colour's luma from `start` to kSsgiSourceSpan times it.
float SsgiSourceShare(float luma, float start) {
  return saturate((luma / max(start, 1e-3f) - 1.0f) / (kSsgiSourceSpan - 1.0f));
}

#endif  // MCLA_GPU_SSGI_COMMON_HLSLI
