// Pure SSR sampling and history rules. ssr_test.cpp compiles this as C++ too.
#ifndef MCLA_GPU_SSR_HISTORY_HLSLI
#define MCLA_GPU_SSR_HISTORY_HLSLI

#include "ssr_common.hlsli"

static const uint kSsrWorldHit = 0u;
static const uint kSsrUnknownVehicle = 24u;
static const uint kSsrDistantHit = 25u;
static const uint kSsrMaxMisses = 3u;
static const uint kSsrMaxHistorySamples = 15u;

float SsrSmooth01(float ratio) {
  const float t = saturate(ratio);
  return t * t * (3.0f - 2.0f * t);
}

// At grazing angles, a small animated wet-road normal perturbation moves a
// reflection many pixels. Reduce that perturbation before sampling the lobe.
float SsrWetNormalWeight(float wetness, float geometric_facing) {
  return 0.65f * saturate(wetness) * (1.0f - SsrSmooth01(geometric_facing / 0.45f));
}

float SsrScreenHitConfidence(float edge, float facing, float hit_facing, float depth_error_fraction) {
  const float front_face = SsrSmooth01((0.15f - hit_facing) / 0.30f);
  const float thickness = 1.0f - SsrSmooth01((depth_error_fraction - 0.5f) * 2.0f);
  return edge * facing * front_face * thickness;
}

// The list still ends at the confidence cutoff. Fade the RT contribution
// to zero before that boundary so a change of list membership is continuous.
float SsrRtHistoryEdge(float confidence, float threshold) {
  return SsrSmooth01((threshold - confidence) / 0.20f);
}

uint SsrHistorySource(float hit) {
  if (SsrHitDistance(hit) >= kSsrMissLength - 1.0f) {
    return kSsrDistantHit;
  }
  if (!SsrHitIsVehicle(hit)) {
    return kSsrWorldHit;
  }
  const uint id = SsrHitVehicleId(hit);
  return id > 0u && id < kSsrUnknownVehicle ? id : kSsrUnknownVehicle;
}

bool SsrPreferHistoryHit(bool centre_hit, float candidate, float weight, float best_weight) {
  return !centre_hit && SsrHistorySource(candidate) != kSsrDistantHit && weight > best_weight;
}

// These integers fit exactly in a half, including at the maximum sample count.
uint SsrGuideState(uint source, uint misses, uint samples) {
  return source | (misses << 5u) | (samples << 7u);
}
uint SsrGuideSource(uint state) { return state & 31u; }
uint SsrGuideMisses(uint state) { return (state >> 5u) & 3u; }
uint SsrGuideSamples(uint state) { return state >> 7u; }

float2 SsrEncodePlaneNormal(float3 n) {
  const float inv_l1 = 1.0f / (abs(n.x) + abs(n.y) + abs(n.z));
  const float x = n.x * inv_l1;
  const float y = n.y * inv_l1;
  return n.z >= 0.0f ? float2(x, y) :
      float2((1.0f - abs(y)) * (x < 0.0f ? -1.0f : 1.0f),
             (1.0f - abs(x)) * (y < 0.0f ? -1.0f : 1.0f));
}

float3 SsrDecodePlaneNormal(float2 p) {
  const float z = 1.0f - abs(p.x) - abs(p.y);
  const float t = max(0.0f, -z);
  return normalize(float3(p.x + (p.x < 0.0f ? t : -t),
                          p.y + (p.y < 0.0f ? t : -t), z));
}

// A plane test keeps a road coherent at a grazing angle, where two pixels
// on it can be metres apart in Z. Its tolerance grows with the pixel footprint.
float SsrPlaneWeight(float3 sample_position, float3 centre, float3 normal, float footprint) {
  const float tolerance = 0.025f + 1.5f * footprint;
  return 1.0f - SsrSmooth01(abs(dot(sample_position - centre, normal)) / tolerance);
}

float SsrImageDepthWeight(float stored, float expected, uint source) {
  if (source == kSsrDistantHit) {
    return 1.0f;
  }
  // Rounding the packed car distance and changing the sampled point on a
  // rough lobe both perturb the virtual depth slightly.
  return 1.0f - SsrSmooth01((abs(stored - expected) / max(expected, 0.1f) - 0.04f) / 0.12f);
}

bool SsrCanCarryHit(uint state) {
  const uint source = SsrGuideSource(state);
  const uint misses = SsrGuideMisses(state);
  return SsrGuideSamples(state) > 0u && source < kSsrUnknownVehicle &&
      misses < (source == kSsrWorldHit ? kSsrMaxMisses : 1u);
}

float SsrCarryDistance(float virtual_z, float receiver_z, float receiver_length) {
  return max(virtual_z - receiver_z, 0.0f) * (receiver_length / max(receiver_z, 0.01f));
}

float SsrHistoryMix(float maximum, float coverage, uint samples) {
  const float n = float(samples);
  return min(maximum, n / (n + 1.0f)) * saturate(coverage);
}

// Preserve some signed reflection correction when the current sparse rays
// provide weak evidence. This applies only after the plane, image depth and
// source tests. Repeated misses still decay and expire.
float SsrValidatedClipKeep(float support, float coverage, bool upscaler, bool sparkle) {
  const float floor = upscaler && !sparkle ? 0.75f : 0.85f;
  return floor * saturate(coverage) * (1.0f - SsrSmooth01(support));
}

float SsrKeepSigned(float previous, float clipped, float keep) {
  if (keep <= 0.0f || previous == 0.0f) {
    return clipped;
  }
  return previous > 0.0f ? max(clipped, keep * previous) : min(clipped, keep * previous);
}

#endif  // MCLA_GPU_SSR_HISTORY_HLSLI
