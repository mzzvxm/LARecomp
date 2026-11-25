// MCLA GPU renderer -- screen-space global illumination, the colour chain and its debug views (ao.h).
//
// The visibility bitmask pass (ao_main_ps.hlsl, AO_GI=1) takes the radiance of what blocks a pixel's
// hemisphere from the scene colour as it is at the game's mid-scene depth copy: every opaque draw of
// the main view is in, the fog, the particles and the post chain are not. This file makes that colour
// readable the way the depth chain is, in four compilations (AO_COLOR_PASS, AO_COLOR_MS):
//
//   0  ao_color_ps / ao_color_ms_ps   level 0: the main view's colour target (Texture2D, or
//                                     Texture2DMS when the game's MSAA is on) to an RGBA16F image at
//                                     the AO's size. One texel is the mean of the pixels it covers
//                                     (2x2 when the passes run at half the picture's size) and of
//                                     every host sample of each (one of them above 1080 lines, where
//                                     the image is half size by the height rule: flag bit 21, below),
//                                     after each sample was cleaned and
//                                     capped (below). The sky is in it as any other colour: the main
//                                     pass never reads a sample whose depth says sky.
//   1  ao_color_mip_ps                a coarser level: the mean of the 2x2 texels below it. Run twice,
//                                     level 0 to 1 and 1 to 2, as the depth chain's levels.
//   2  ao_color_view_ps               a debug picture (ao.h mcla_gpu_ssgi_test_view): an RGBA16F image
//                                     of the chain or the main pass's bounce, put into the colour
//                                     target in place of the picture (the pipeline's blend replaces the
//                                     rgb). Never made for a player.
//
// Cleaning and the cap (pass 0): a sample is a radiance only if it is a finite non-negative colour, and
// what it carries into the bounce is its luma through a soft clip at the cap the push block gives (a multiple
// of the frame's ambient + sun, ao.cpp: a street lamp, neon, the sun's disc and a specular glint are sources
// of the game's own light, not surfaces that bounce it, and one of them would light its whole neighbourhood
// white; ssgi_common.hlsli SsgiSoftClip: linear up to half the cap, bending to the cap, never over it). The
// cap is on luma and keeps the hue: the colour is scaled down, never clipped per channel. Level 0's alpha is
// the share of the texel's samples that are a source of light (0 a surface, 1 a source): a ramp of the
// sample's own luma from shape.y to 1.5 times it (SsgiSourceShare), which is not the cap: a surface may carry
// more than a source starts at when the cap is high. The apply pass (ao_apply_ps.hlsl, AO_GI) gives such a
// pixel no bounce, as it has no albedo to estimate.
//
// THE LIGHTS' SHARE (GI-BOUNCE-SS, flags bit 23, pass 0 only; ssgi_lights.h has the reasons and the C++ twin, perf/GI-BOUNCE.md the design). This chain is made at the trigger BEFORE the deferred light pass of the
// same frame (neon, headlights, street lamps), so it never held their light. With the flag the texel also takes what that pass added in the LAST frame at the place the texel was: the host captured the colour
// before and after the light pass (screen_shared.h CaptureLightsBefore / After, RGBA16F at this image's size, the MSAA samples averaged) and this pass reprojects the texel into the last frame with the AO's own
// camera motion and the depth history (the tests of ao_temporal_ps.hlsl: the tangent plane, the depth within 8%, a vehicle or a sky texel of the history rejected, the confidence smoothstep), takes the
// difference of the two captures there (>= 0) and adds the part of its luma that fits under the cap, soft-clipped to the room the capped colour leaves (SsgiCarryLights). A vehicle's own texel takes none (its own
// neon is no radiance for the road a frame behind it), nor a texel that is a source of light. Without the flag, or with no light in the last frame, the texel is what it was, to the bit.
//   push fields with the flag: slots.y high 16 bits the normal image's slot (0: none: the depth's normal), slots.z the last frame's colour AFTER the lights, slots.w the colour BEFORE them, slots2.y this frame's
//   linear depth (level 0 of the depth chain, with the vehicle mark), slots2.w the last frame's (the depth history), lens.xy this frame's tan half field of view (as the other passes; lens.zw the last frame's),
//   lin / light / extra the three rows of the 3x4 that moves this frame's view space into the last frame's (the AO's accumulation pass's matrix), shape.z the gain of the lights' share.
//
// Push block (ao_common.hlsli AoPush), the fields of these passes:
//   pass 0   slots.x the colour plane's bindless slot, slots.y its host samples, sizes.xy this image's
//            size, sizes.zw the colour plane's size (the picture), shape.x the cap (luma), shape.y where a
//            source of light starts (luma), flags: low, bit 21 one sample of each pixel (slots2.x its host sample)
//   pass 1   slots.z the level below's slot, sizes.xy this level's size, sizes.zw the level below's
//   pass 2   slots.z the image shown, sizes.xy the colour target's size, flags: bits 0-7 the view (1 the
//            image's rgb as it is, 2 the image's yzw (the bounce), 3 the image's x as a grey, 4 the
//            bounce cleaned by a 5x5 mean), shape.x the display gain (the other views write the value
//            times 0.3 so that 1.0 reads below the tone map's white; the gain multiplies that)

#include "ao_common.hlsli"
#include "ssgi_common.hlsli"
#include "nan_guard.hlsli"

#ifndef AO_COLOR_PASS
#define AO_COLOR_PASS 0
#endif
#ifndef AO_COLOR_MS
#define AO_COLOR_MS 0
#endif

#if AO_COLOR_PASS == 2
#include "host_pass_samplers.hlsli"
#endif

static const float3 kLuma = float3(0.2126f, 0.7152f, 0.0722f);
// What a half float can carry with room.
static const float kColourMax = 64.0f;
static const float kViewScale = 0.3f;

#if AO_COLOR_PASS == 0

// The sample `s` of the pixel `p`, finite, non-negative, its luma through the soft clip at the cap (never over
// it); w is how much of a source of light it is, 0 to 1 (its own luma against where a source starts).
float4 TakeColour(int2 p, uint s) {
#if AO_COLOR_MS
  float3 colour = g_textures_ms[g_push.slots.x].Load(p, s).rgb;
#else
  float3 colour = g_textures[g_push.slots.x].Load(int3(p, 0)).rgb;
#endif
  colour = NgHeld3(colour, kColourMax);  // a NaN is black, decided by the bits (nan_guard.hlsli): max(NaN, 0) is 0 on D3D12 only
  const float luma = dot(colour, kLuma);
  const float cap = max(g_push.shape.x, 1e-3f);
  const float3 carried = luma > 1e-6f ? colour * (SsgiSoftClip(luma, cap) / luma) : colour;
  return float4(carried, SsgiSourceShare(luma, g_push.shape.y));
}

// Flags bit 21 (the passes' own flag word, ao.cpp kColourOneSampleFlag): the texel takes ONE sample of each pixel it covers, the host sample
// in slots2.x (the depth's own), instead of every sample of them. Only set where the image is half size by the height rule (ao_size.h:
// above 1080 lines), so the texel is still the mean of 4 samples (a 2x2 block of pixels): the bounce light's source is a smooth signal that
// the bounce's denoise, history and a-trous passes blur over many pixels after this, and the colour target's reads are halved at MSAA 2.
bool AoOneSample() { return (g_push.slots2.z & (1u << 21)) != 0u; }

// THE LIGHTS' SHARE (the header above): flags bit 23.
bool AoLights() { return (g_push.slots2.z & (1u << 23)) != 0u; }

// The last frame's light at the place the texel `pixel` was: rgb the light (the difference of the two captures at the texel, held >= 0, the confidence applied), 0 where the texel takes none (the sky, a vehicle, no
// place in the history) or where nothing agrees. The reprojection is ao_temporal_ps.hlsl's: the position in this frame's view space moved into the last frame's by the 3x4 (lin, light, extra), the four texels round
// where it lands, each by its bilinear weight times its agreement with this texel's tangent plane and depth.
float3 LightsHistory(int2 pixel) {
  const float2 size = g_push.sizes.xy;
  const int2 isize = int2(size);
  const uint depth_slot = g_push.slots2.y;
  const uint prev_depth_slot = g_push.slots2.w;
  const uint post_slot = g_push.slots.z;
  const uint pre_slot = g_push.slots.w;
  const float tc = AoLoadLinear(depth_slot, pixel);
  if (tc >= kAoSky * 0.5f || AoIsVehicle(tc)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, isize, tc, pc);
  const float4 q = float4(pc, 1.0f);
  const float3 pe = float3(dot(g_push.lin, q), dot(g_push.light, q), dot(g_push.extra, q));
  const float3 ne = float3(dot(g_push.lin.xyz, normal), dot(g_push.light.xyz, normal), dot(g_push.extra.xyz, normal));
  if (!(pe.z > kSsgiLightsNearest)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const float2 tan_half = g_push.lens.zw;
  const float2 uv = float2(0.5f + 0.5f * pe.x / (pe.z * tan_half.x), 0.5f - 0.5f * pe.y / (pe.z * tan_half.y));
  const float2 hp = clamp(uv * size - 0.5f, float2(-2.0f, -2.0f), size + 2.0f);
  const int2 h0 = int2(floor(hp));
  const float2 f = hp - float2(h0);
  const float sigma = kSsgiLightsPlaneShare * pe.z + kSsgiLightsPlaneFloor;
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float weight = 0.0f;
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      const int2 t = h0 + int2(i, j);
      if (t.x < 0 || t.y < 0 || t.x >= isize.x || t.y >= isize.y) {
        continue;
      }
      const float texel_depth = AoLoadLinear(prev_depth_slot, t);
      if (texel_depth >= kAoSky * 0.5f || AoIsVehicle(texel_depth)) {
        continue;  // the sky, or a car a frame ago: the light on it was the car's own
      }
      const float2 centre = float2(t) + 0.5f;
      const float2 tuv = centre / size;
      const float3 ph = float3((tuv.x * 2.0f - 1.0f) * tan_half.x * texel_depth, (1.0f - tuv.y * 2.0f) * tan_half.y * texel_depth, texel_depth);
      const float off_plane = dot(ne, ph - pe) / sigma;
      float w = exp(-0.5f * off_plane * off_plane);
      w *= abs(texel_depth - pe.z) < kSsgiLightsDepthShare * pe.z + kSsgiLightsDepthFloor ? 1.0f : 0.0f;
      w *= (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
      const float3 post = g_textures[post_slot].Load(int3(t, 0)).rgb;
      const float3 pre = g_textures[pre_slot].Load(int3(t, 0)).rgb;
      sum += max(post - pre, 0.0f) * w;
      weight += w;
    }
  }
  const float confidence = smoothstep(kSsgiLightsConfidenceLow, kSsgiLightsConfidenceHigh, weight);
  return sum * (confidence / max(weight, 1e-4f));
}

float4 main(float4 position : SV_Position) : SV_Target {
  const bool low = AoLow();
  const int2 base = int2(position.xy) * (low ? 2 : 1);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const int across = low ? 2 : 1;
#if AO_COLOR_MS
  const uint host_samples = max(g_push.slots.y & 0xFFu, 1u);  // (the high half is the lights' normal image slot: AoLights)
  const bool one = AoOneSample();
  const uint samples = one ? 1u : host_samples;
  const uint first = one ? min(g_push.slots2.x, host_samples - 1u) : 0u;
#else
  const uint samples = 1u;
  const uint first = 0u;
#endif
  float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  for (int j = 0; j < across; ++j) {
    for (int i = 0; i < across; ++i) {
      const int2 p = min(base + int2(i, j), last);
      for (uint s = 0u; s < samples; ++s) {
        sum += TakeColour(p, first + s);
      }
    }
  }
  float4 result = sum / float(uint(across * across) * samples);
  if (AoLights()) {
    result.rgb = SsgiCarryLights(result.rgb, LightsHistory(int2(position.xy)), g_push.shape.z, max(g_push.shape.x, 1e-3f), result.a);
  }
  return result;
}

#elif AO_COLOR_PASS == 1

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const int2 last = int2(g_push.sizes.zw) - 1;
  const uint slot = g_push.slots.z;
  float3 sum = g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 0), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(0, 1), last), 0)).rgb;
  sum += g_textures[slot].Load(int3(min(pixel * 2 + int2(1, 1), last), 0)).rgb;
  return float4(sum * 0.25f, 1.0f);
}

#else  // AO_COLOR_PASS == 2

float4 main(float4 position : SV_Position) : SV_Target {
  const uint view = AoView();
  const float2 uv = position.xy / g_push.sizes.xy;
  const uint slot = g_push.slots.z;
  float4 texel = g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f);
  float3 shown = texel.rgb;
  if (view == 2u) {
    shown = texel.yzw;
  } else if (view == 3u) {
    shown = texel.xxx;
  } else if (view == 4u) {
    // The bounce, cleaned by a 5x5 mean (of the image's own texels, nothing cleverer: a preview of
    // what a denoise will make of it, not the denoise).
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (int j = -2; j <= 2; ++j) {
      for (int i = -2; i <= 2; ++i) {
        sum += g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp],
                                            uv + float2(i, j) / g_push.sizes.zw, 0.0f).yzw;
      }
    }
    shown = sum / 25.0f;
  }
  const float gain = g_push.shape.x > 0.0f ? g_push.shape.x : 1.0f;
  return float4(max(shown, 0.0f) * (kViewScale * gain), 1.0f);
}

#endif
