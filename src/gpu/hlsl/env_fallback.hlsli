#ifndef MCLA_GPU_ENV_FALLBACK_HLSLI
#define MCLA_GPU_ENV_FALLBACK_HLSLI

// The existing four-level dual-paraboloid map. Twins in env_fallback_rules.h.
// CHECK: confirm the radius and sky fade on moving wet roads and sidewalks,
// and compare the rim blend and rough mips on glass with REF-7 captures.
static const float kEnvFallbackLastMip = 3.0f;
static const float kEnvFallbackProxyRadius = 40.0f;
static const float kEnvFallbackProxyFadeStart = 0.75f;
static const float kEnvFallbackSkyFadeEnd = 0.35f;

float EnvFallbackMip(float perceptualRoughness) {
  return saturate(perceptualRoughness) * kEnvFallbackLastMip;
}

float3 EnvFallbackDirection(float3 probeToReceiverWorld, float3 ray, bool groundReceiver) {
  if (!groundReceiver || ray.y >= kEnvFallbackSkyFadeEnd) {
    return ray;
  }
  const float3 q = probeToReceiverWorld / kEnvFallbackProxyRadius;
  const float d2 = dot(q, q);
  if (!(d2 < 1.0f)) {
    return ray;
  }
  const float b = dot(q, ray);
  const float exit = sqrt(b * b + (1.0f - d2)) - b;
  const float weight = (1.0f - smoothstep(kEnvFallbackProxyFadeStart * kEnvFallbackProxyFadeStart, 1.0f, d2)) *
                       (1.0f - smoothstep(0.0f, kEnvFallbackSkyFadeEnd, ray.y));
  return normalize(q * weight + ray * lerp(1.0f, exit, weight));
}

// Keep the trilinear footprint inside the valid disc at the coarser mip of
// the pair. Grow the margin continuously with the footprint as roughness
// changes. The two samples in this band meet at the same equatorial ray.
float EnvFallbackRimInset(float mip, float hemisphereSize) {
  return 4.0f * exp2(mip) / hemisphereSize;
}

float2 EnvFallbackUv(float3 r, bool left, float inset) {
  const float z = abs(r.z);
  float scale = 1.0f / (1.0f + z);
  const float rim = 1.0f - inset;
  if (1.0f - z > rim * rim * (1.0f + z)) {
    scale = rim / sqrt(1.0f - z * z);
  }
  // World orientation as in RtHitEnvUv: -Z is the left hemisphere, +Y is up.
  return float2(left ? 0.25f + 0.25f * r.x * scale : 0.75f - 0.25f * r.x * scale,
                0.5f - 0.5f * r.y * scale);
}

float EnvFallbackRightWeight(float z, float inset) {
  return smoothstep(-inset, inset, z);
}

#ifndef ENV_FALLBACK_MATH_ONLY
#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

bool EnvFallbackAvailable(uint mapWord) {
  return (mapWord >> 16u) != 0u;
}

float3 EnvFallbackSample(uint mapWord, float3 probeToReceiverWorld, float3 reflectedWorld,
                         float perceptualRoughness, bool groundReceiver) {
  if (!EnvFallbackAvailable(mapWord)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const uint slot = mapWord & 0xFFFFu;
  const float mip = EnvFallbackMip(perceptualRoughness);
  const float inset = EnvFallbackRimInset(mip, float(mapWord >> 16u));
  const float3 ray = EnvFallbackDirection(probeToReceiverWorld, reflectedWorld, groundReceiver);
  [branch]
  if (abs(ray.z) < inset) {
    const float3 left = g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], EnvFallbackUv(ray, true, inset), mip).rgb;
    const float3 right = g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], EnvFallbackUv(ray, false, inset), mip).rgb;
    return lerp(left, right, EnvFallbackRightWeight(ray.z, inset));
  }
  return g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], EnvFallbackUv(ray, ray.z < 0.0f, inset), mip).rgb;
}
#endif  // ENV_FALLBACK_MATH_ONLY

#endif  // MCLA_GPU_ENV_FALLBACK_HLSLI
