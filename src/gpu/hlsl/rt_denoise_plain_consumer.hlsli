// MCLA GPU renderer -- RtDenoise (CITY-RT package R6; hlsl/rt_denoise_common.hlsli has the design): the PLAIN consumer, for a signal whose guides are plain images at the signal's own size. This is the
// shared entry for the effects that have no guides of their own to read in place (the probe field's per-pixel detail pass, CITY-RT M3 G5, is the first): it makes the guides it has into the
// four images below, and every pass of this consumer takes them as they are; hlsl/ssr_denoise_consumer.hlsli is the other way (guides read in place from the effect's own planes).
//
// Images (bindless slots in the push block; all at the signal's size):
//   signal         RGBA16F  rgb the radiance of one sample a pixel (the temporal pass's current frame, the spatial pass's input); the alpha is the consumer's and is passed through
//   depth          R32F     linear depth along the view axis, metres (0 or the sky's: not signal)
//   normal_rough   RGBA16F  xyz the view-space normal facing the camera, w the perceptual roughness
//   hit_share      RG16F    x the ray's length from the surface in metres (0: none), y the share of the pixel's value the filter may replace (0: not signal)
//   history        RGBA16F  (temporal) rgb the last frame's result, a its length in frames
//   history_depth  R32F     (temporal) the receiver depth the history stored at a texel
// Push block (32 words, the layout the host class keeps for every consumer):
//   0 signal | depth << 16       1 normal_rough | hit_share << 16      2 history | history_depth << 16       3 width | height << 16
//   4 iteration (bits 0-3) | has_history << 8                          5 tan x   6 tan y   7 last frame's tan x   8 last frame's tan y   (floats)
//   9..20 the 3 x 4 motion into the last frame's view space (rows of 4 floats)                                   21 float: the history length the spatial pass assumes (the kernel shrinks as it grows)
// Compilations: rt_denoise_plain_spatial_cs, rt_denoise_plain_temporal_cs; RT_DN_PROFILE is the consumer's (the compile defines it: RT_DN_PROFILE_SPECULAR is the default here).
//
// RT_DN_VIS (RT-LIGHTS-D1 part 2, the ray traced light visibility: rt_lights_rules.h "the memory"): the same block and the same words over lighter guides. The signal and the history are RGBA8_UNORM
// (RT_DN_OUT_UNORM8: the history's length is the alpha as n / 32, decoded in RtDnHistoryAt), depth and history_depth are R16F, and the normal image is RGBA8_SNORM with the SHARE in its alpha: word 1's
// low half names it, its high half is not read; the roughness is 1 and the hit distance 0 (a visibility has neither). Word 22 is the frame's number: the temporal pass rounds its 8 bit average at random
// with the shared per-frame blue noise at that sample (RtDnDither3). Without the macro nothing here changes (the compiled shaders are the same).

#ifndef MCLA_GPU_RT_DENOISE_PLAIN_CONSUMER_HLSLI
#define MCLA_GPU_RT_DENOISE_PLAIN_CONSUMER_HLSLI

#include "host_pass_common.hlsli"
#include "host_pass_samplers.hlsli"
#include "rt_denoise_common.hlsli"
#ifdef RT_DN_VIS
#include "rt_denoise_unorm8.hlsli"
#include "blue_noise_frame.hlsli"
#endif

#ifndef RT_DN_PROFILE
#define RT_DN_PROFILE RT_DN_PROFILE_SPECULAR
#endif

struct RtDnPush {
  uint4 r[8];
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtDnPush> g_push;
#else
ConstantBuffer<RtDnPush> g_push : register(b0, space4);
#endif
uint RtDnW(uint i) { return g_push.r[i >> 2u][i & 3u]; }
float RtDnF(uint i) { return asfloat(RtDnW(i)); }
uint RtDnLo(uint word) { return word & 0xFFFFu; }
uint RtDnHi(uint word) { return word >> 16u; }

#define RTDNP_W_SIGNAL 0
#define RTDNP_W_GUIDES 1
#define RTDNP_W_HISTORY 2
#define RTDNP_W_SIZE 3
#define RTDNP_W_PARAMS 4
#define RTDNP_W_TAN_X 5
#define RTDNP_W_TAN_Y 6
#define RTDNP_W_PREV_TAN_X 7
#define RTDNP_W_PREV_TAN_Y 8
#define RTDNP_W_MOTION 9
#define RTDNP_W_LENGTH 21
#define RTDNP_W_FRAME 22   // RT_DN_VIS: the frame's number (the dither's sample)

int2 RtDnSize() { return int2(int(RtDnLo(RtDnW(RTDNP_W_SIZE))), int(RtDnHi(RtDnW(RTDNP_W_SIZE)))); }
uint RtDnIteration() { return RtDnW(RTDNP_W_PARAMS) & 15u; }
bool RtDnHasHistory() { return ((RtDnW(RTDNP_W_PARAMS) >> 8u) & 1u) != 0u; }
float RtDnPixelAngle() { return 2.0f * RtDnF(RTDNP_W_TAN_Y) / float(max(RtDnSize().y, 1)); }
float4 RtDnSignal(int2 p) { return g_textures[RtDnLo(RtDnW(RTDNP_W_SIGNAL))].Load(int3(p, 0)); }
float RtDnDepthAt(int2 p) { return g_textures[RtDnHi(RtDnW(RTDNP_W_SIGNAL))].Load(int3(p, 0)).x; }
float4 RtDnColourAt(int2 p) {
  const float4 s = RtDnSignal(p);
  return float4(s.x, s.y, s.z, RtDnDepthAt(p) > 0.0f ? 1.0f : 0.0f);
}
float RtDnHistoryDepthAt(int2 p) { return g_textures[RtDnHi(RtDnW(RTDNP_W_HISTORY))].Load(int3(p, 0)).x; }
#ifdef RT_DN_VIS
// Three numbers in [0, 1) for the pixel this frame (rt_denoise_unorm8.hlsli RtDnStore8): the shared blue noise tile stepped along the R2 sequence by the frame (blue_noise_frame.hlsli: the pattern stays
// where it is, nothing slides, every frame's numbers differ), the third channel turned by the golden ratio.
float3 RtDnDither3(int2 p) {
  const float2 n = BlueNoiseFrame2(p.x, p.y, RtDnW(RTDNP_W_FRAME));
  return float3(n.x, n.y, frac(n.x + 0.6180339887f));
}
float4 RtDnHistoryAt(float2 uv) {
  const float4 h = g_textures[RtDnLo(RtDnW(RTDNP_W_HISTORY))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f);
  return float4(h.x, h.y, h.z, RtDnDecodeLength(h.w));
}
#else
float4 RtDnHistoryAt(float2 uv) { return g_textures[RtDnLo(RtDnW(RTDNP_W_HISTORY))].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0.0f); }
#endif

#ifndef RT_DN_SHADOW
RtDnPixel RtDnLoad(int2 pix) {
  RtDnPixel r;
  r.ok = false;
  r.colour = float3(0.0f, 0.0f, 0.0f);
  r.z = 0.0f;
  r.pos = float3(0.0f, 0.0f, 0.0f);
  r.n = float3(0.0f, 0.0f, -1.0f);
  r.rough = 1.0f;
  r.hit = 0.0f;
  r.share = 0.0f;
  r.length = RtDnF(RTDNP_W_LENGTH);
  const int2 size = RtDnSize();
  if (pix.x < 0 || pix.y < 0 || pix.x >= size.x || pix.y >= size.y) {
    return r;
  }
#ifdef RT_DN_VIS
  const float z = RtDnDepthAt(pix);
  const float4 nr = g_textures[RtDnLo(RtDnW(RTDNP_W_GUIDES))].Load(int3(pix, 0));
  const float2 hs = float2(0.0f, nr.w);   // no hit distance; the share is the normal image's alpha
  if (!(z > 0.0f) || !(hs.y > 0.0f)) {
    return r;
  }
#else
  const float z = RtDnDepthAt(pix);
  const float2 hs = g_textures[RtDnHi(RtDnW(RTDNP_W_GUIDES))].Load(int3(pix, 0)).xy;
  if (!(z > 0.0f) || !(hs.y > 0.0f)) {
    return r;
  }
  const float4 nr = g_textures[RtDnLo(RtDnW(RTDNP_W_GUIDES))].Load(int3(pix, 0));
#endif
  const float tan_x = RtDnF(RTDNP_W_TAN_X);
  const float tan_y = RtDnF(RTDNP_W_TAN_Y);
  r.ok = true;
  r.colour = RtDnSignal(pix).xyz;
  r.z = z;
  r.pos = float3(((float(pix.x) + 0.5f) / float(size.x) * 2.0f - 1.0f) * tan_x * z, (1.0f - (float(pix.y) + 0.5f) / float(size.y) * 2.0f) * tan_y * z, z);
  r.n = nr.xyz;
#ifdef RT_DN_VIS
  r.rough = 1.0f;
#else
  r.rough = nr.w;
#endif
  r.hit = hs.x;
  r.share = hs.y;
  return r;
}
#endif

RtDnPrevious RtDnToPrevious(float3 p) {
  RtDnPrevious r;
  const float4 m0 = float4(RtDnF(RTDNP_W_MOTION + 0u), RtDnF(RTDNP_W_MOTION + 1u), RtDnF(RTDNP_W_MOTION + 2u), RtDnF(RTDNP_W_MOTION + 3u));
  const float4 m1 = float4(RtDnF(RTDNP_W_MOTION + 4u), RtDnF(RTDNP_W_MOTION + 5u), RtDnF(RTDNP_W_MOTION + 6u), RtDnF(RTDNP_W_MOTION + 7u));
  const float4 m2 = float4(RtDnF(RTDNP_W_MOTION + 8u), RtDnF(RTDNP_W_MOTION + 9u), RtDnF(RTDNP_W_MOTION + 10u), RtDnF(RTDNP_W_MOTION + 11u));
  const float4 q = float4(p.x, p.y, p.z, 1.0f);
  const float x = dot(m0, q);
  const float y = dot(m1, q);
  const float z = dot(m2, q);
  r.z = z;
  r.u = 0.0f;
  r.v = 0.0f;
  r.ok = false;
  if (z > 1.0e-3f) {
    const float tan_x = RtDnF(RTDNP_W_PREV_TAN_X);
    const float tan_y = RtDnF(RTDNP_W_PREV_TAN_Y);
    r.u = 0.5f + 0.5f * x / (tan_x * z);
    r.v = 0.5f - 0.5f * y / (tan_y * z);
    r.ok = r.u >= 0.0f && r.u <= 1.0f && r.v >= 0.0f && r.v <= 1.0f;
  }
  return r;
}

#endif  // MCLA_GPU_RT_DENOISE_PLAIN_CONSUMER_HLSLI
