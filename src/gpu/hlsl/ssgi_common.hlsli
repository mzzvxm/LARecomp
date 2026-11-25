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

// A car's own pixels take less of the bounce (ssgi_light.h VehicleStrength has the reasons): its strength is this share of
// the level's, never over kSsgiVehicleStrengthMax.
static const float kSsgiVehicleShare = 0.35f;
static const float kSsgiVehicleStrengthMax = 0.8f;

// The strength a pixel gets: the level's on the world, the car's on a vehicle (`vehicle` 0 to 1: the share of the
// pixel's depth neighbourhood that is a vehicle, so a silhouette fades from one to the other).
float SsgiStrengthFor(float strength, float vehicle) {
  const float on_car = min(strength * kSsgiVehicleShare, kSsgiVehicleStrengthMax);
  return lerp(strength, on_car, saturate(vehicle));
}

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

// THE LIGHTS' SHARE of the radiance (GI-BOUNCE-SS, ssgi_lights.h; ao_color_ps.hlsl LightsHistory / SsgiCarryLights): what the deferred light pass (neon, headlights, street lamps) added to the colour in the last frame,
// found at the place a texel was, taken into the chain under the same cap. The tests that find it are the occlusion's accumulation pass's (ao_temporal_ps.hlsl), the same numbers.
static const float kSsgiLightsPlaneShare = 0.012f;
static const float kSsgiLightsPlaneFloor = 0.02f;
static const float kSsgiLightsDepthShare = 0.08f;
static const float kSsgiLightsDepthFloor = 0.05f;
static const float kSsgiLightsConfidenceLow = 0.15f;
static const float kSsgiLightsConfidenceHigh = 0.75f;
static const float kSsgiLightsNearest = 0.05f;
static const float3 kSsgiLuma = float3(0.2126f, 0.7152f, 0.0722f);

// The chain's colour for a texel whose cleaned and capped colour is `pre` (luma under `cap`) and whose last frame's light is `delta`: the part of the lights' luma (times `gain`, times what is not a source of light)
// that fits under the cap, soft-clipped to the room the capped colour leaves, hue kept. No light or a gain of 0 is `pre` to the bit.
float3 SsgiCarryLights(float3 pre, float3 delta, float gain, float cap, float source_share) {
  const float keep = gain * (1.0f - saturate(source_share));
  const float3 d = delta * keep;
  const float luma_d = dot(d, kSsgiLuma);
  if (!(luma_d > 1e-6f)) {
    return pre;
  }
  const float room = max(cap - dot(pre, kSsgiLuma), 0.0f);
  const float scale = SsgiSoftClip(luma_d, room) / luma_d;
  return pre + d * scale;
}

#endif  // MCLA_GPU_SSGI_COMMON_HLSLI
