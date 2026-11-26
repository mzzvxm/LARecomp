// MCLA GPU renderer -- volumetric light, pass 3: the composite (volumetrics.h).
//
// Drawn into the main view's colour target at the game's end-of-scene copy (after the fog quad, before the copy, the bloom, the temporal
// upscaler and the tone map see the colour), a full-screen triangle on the host pass layout (host_passes.h), in a pipeline whose blend is
//   dst = src.rgb + dst.rgb * src.a        (ONE, SRC_ALPHA; the alpha is not written)
// on every sample of a multisampled target. The pixel is placed in the volume by its view depth (from the main depth plane, linearised with
// the lens as the occlusion does: t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored; the sky, stored 0, takes the far plane) and
// reads the accumulated volume at that depth: two bilinear fetches of the 2D atlas (the slices around it), (S.rgb, T).
// The output is (S, lerp(1, T, extinction_scale)): the added medium dims the picture only by the share the setting asks for (the game's own
// fog attenuates already). The light S is soft limited first (SoftLimit, the push's cap): no number of lights and no thickness of air may add more
// than the cap to a pixel (VOL-LOOK-TUNE: the first look's white ball around the oncoming headlights in rain).
//
// The depth is the guest's sample 0 of the (possibly multisampled) depth plane: APPLY_MS 1 reads a Texture2DMS in space 1.
// VOL-STABLE: with the push's all-samples flag a multisampled pixel reads EVERY sample's depth and looks the volume up at each (one lookup at their mean where they
// agree within half a slice), and its light is their mean: at a depth edge (a lamp post against the street behind it) the light is the near and the far one by the
// coverage, an anti-aliased edge, where sample 0 alone gave one of the two by a hard flip that crawled with the motion (volumetrics_stable.h DepthsAgree,
// AverageOverSamples are the twins). The debug view (the push's slots.w, bits 9 and 10) shows the volume's light alone: the picture is replaced by it (the alpha 0
// of the ONE, SRC_ALPHA blend), x1, x8 or x32, to measure how steady the volume is without the scene under it.

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"

#ifndef APPLY_MS
#define APPLY_MS 0
#endif

#ifndef ATM_SHAFTS
#define ATM_SHAFTS 0
#endif
#if ATM_SHAFTS
#include "shafts_apply.hlsli"
#endif

struct VolApplyPush {
  // x the main depth plane's bindless slot, y the accumulated volume's slot, z the grid packed as nx | ny << 8 | nz << 16 | tiles_x << 24,
  // w bits 0 to 7 the depth's guest sample (the host sample that is the guest's sample 0), bit 8 every sample's depth is used, bits 9 and 10 the debug view
  // (0 the picture, 1 the volume's light alone, 2 the same times 8, 3 times 32): volumetrics_stable.h ApplyWord
  uint4 slots;
  // xy the picture's size in host pixels, z the extinction scale (0: purely additive), w the cap of the light the pixel may get (scene units, 0: none)
  float4 sizes;
  // x near, y far (metres), z 1 / ln(far / near), w unused
  float4 depth;
  // the projection's depth rows P22, P23, P32, P33 (ao.cpp: lin)
  float4 lin;
#if ATM_SHAFTS
  float3 shafts_tint;
  uint shafts_slot;
#endif
};

// The soft limit (volumetrics_rules.h SoftLimit / SoftLimitColour is the twin): the largest channel is kept up to the knee (half the cap), then rolls off
// exponentially toward the cap with the same slope; the three channels are scaled alike so that the hue stays the light's.
static const float kSoftKnee = 0.5;
float3 SoftLimit(float3 c, float cap) {
  const float m = max(c.x, max(c.y, c.z));
  if (!(cap > 0.0) || !(m > 0.0)) {
    return c;
  }
  const float knee = cap * kSoftKnee;
  if (m <= knee) {
    return c;
  }
  const float room = cap - knee;
  return c * ((knee + room * (1.0 - exp(-(m - knee) / room))) / m);
}

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<VolApplyPush> g_push;
#else
ConstantBuffer<VolApplyPush> g_push : register(b0, space4);
#endif

static const uint kApplyFlagAllSamples = 0x100u;
static const uint kApplyViewShift = 9u;
static const float kApplyViewGain = 8.0;
static const float kApplyViewGainHigh = 32.0;

// The view depth of a stored depth value; the sky (stored 0) and anything beyond the volume read its far end.
float ViewDepthOf(float stored) {
  float t = g_push.depth.y;
  if (stored > 0.0) {
    const float ndc = 1.0 - saturate(stored);
    const float den = g_push.lin.x - ndc * g_push.lin.y;
    const float z = (g_push.lin.z - ndc * g_push.lin.w) / den;
    if (den != 0.0 && z > 0.0) {
      t = min(z, g_push.depth.y);
    }
  }
  return t;
}

// The volume at the view depth `t` and the picture position `uv`: (the light, soft limited; the transmittance the picture is to keep).
float4 LookUp(float t, float2 uv, uint nx, uint ny, uint nz, uint tiles_x) {
  const bool shortened = g_push.depth.x > 0.5f;
  if (shortened && t <= g_push.depth.x) return float4(0.0, 0.0, 0.0, 1.0);
  const uint tiles_y = nz / tiles_x;
  const float s = float(nz) * log(max(t, g_push.depth.x) / g_push.depth.x) * g_push.depth.z - 0.5;
  const float s_floor = floor(s);
  const float blend = s - s_floor;
  const uint k0 = uint(clamp(s_floor, 0.0, float(nz) - 1.0));
  const uint k1 = min(uint(clamp(s_floor + 1.0, 0.0, float(nz) - 1.0)), nz - 1u);
  const float2 tile_texel = clamp(uv * float2(nx, ny), 0.5, float2(nx, ny) - 0.5);
  const float2 atlas = float2(nx * tiles_x, ny * tiles_y);
  const float2 uv0 = (float2(k0 % tiles_x * nx, k0 / tiles_x * ny) + tile_texel) / atlas;
  const float2 uv1 = (float2(k1 % tiles_x * nx, k1 / tiles_x * ny) + tile_texel) / atlas;
  const Texture2D<float4> volume = g_textures[g_push.slots.y];
  const float4 a = volume.SampleLevel(g_samplers[kSamplerLinearClamp], uv0, 0);
  const float4 b = volume.SampleLevel(g_samplers[kSamplerLinearClamp], uv1, 0);
  const float4 v = shortened && s < 0.0f
      ? lerp(float4(0.0, 0.0, 0.0, 1.0), a, saturate(1.0f + 2.0f * s))
      : lerp(a, b, saturate(blend));
  const float3 light = SoftLimit(max(v.rgb, 0.0), g_push.sizes.w);
  const float transmittance = lerp(1.0, saturate(v.a), g_push.sizes.z);
  return float4(light, transmittance);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const uint flags = g_push.slots.w;
  const uint guest_sample = flags & 0xFFu;
  const uint view = (flags >> kApplyViewShift) & 3u;
  const uint nx = g_push.slots.z & 0xFFu;
  const uint ny = (g_push.slots.z >> 8u) & 0xFFu;
  const uint nz = (g_push.slots.z >> 16u) & 0xFFu;
  const uint tiles_x = (g_push.slots.z >> 24u) & 0xFFu;
  const float2 uv = position.xy / g_push.sizes.xy;
  float4 result;
#if APPLY_MS
  uint width, height, count;
  g_textures_ms[g_push.slots.x].GetDimensions(width, height, count);
  if ((flags & kApplyFlagAllSamples) != 0u && count > 1u) {
    float lo = 1e30;
    float hi = 0.0;
    float sum = 0.0;
    [loop] for (uint i = 0u; i < count; ++i) {
      const float t = ViewDepthOf(g_textures_ms[g_push.slots.x].Load(pixel, i).r);
      lo = min(lo, t);
      hi = max(hi, t);
      sum += t;
    }
    // within half a slice's thickness the volume is as good as linear: one lookup at the mean
    const float tolerance = 0.5 * (exp(1.0 / (g_push.depth.z * float(nz))) - 1.0);
    if (hi - lo <= tolerance * lo) {
      result = LookUp(sum / float(count), uv, nx, ny, nz, tiles_x);
    } else {
      result = 0.0;
      [loop] for (uint j = 0u; j < count; ++j) {
        result += LookUp(ViewDepthOf(g_textures_ms[g_push.slots.x].Load(pixel, j).r), uv, nx, ny, nz, tiles_x);
      }
      result /= float(count);
    }
  } else {
    result = LookUp(ViewDepthOf(g_textures_ms[g_push.slots.x].Load(pixel, guest_sample).r), uv, nx, ny, nz, tiles_x);
  }
#else
  result = LookUp(ViewDepthOf(g_textures[g_push.slots.x].Load(int3(pixel, 0)).r), uv, nx, ny, nz, tiles_x);
#endif
  if (view != 0u) {
    return float4(result.rgb * (view == 2u ? kApplyViewGain : (view == 3u ? kApplyViewGainHigh : 1.0)), 0.0);
  }
#if ATM_SHAFTS
  result.rgb += SampleShaftLight(uv, g_push.shafts_slot, g_push.shafts_tint);
#endif
  return result;
}
