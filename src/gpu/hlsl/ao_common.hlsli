// MCLA GPU renderer -- screen-space ambient occlusion (ao.h), shared declarations.
//
// The passes (ao_depth_ps, ao_mip_ps, ao_main_ps, ao_denoise_ps, ao_apply_ps) are full-screen
// triangles on the host pass layout of host_passes.h with a push range of their own: 128 bytes,
// one AoPush for all of them, each pass reading the fields it needs. The technique is GTAO,
// Jimenez, Wu, Pesce and Jarabo, "Practical Realtime Strategies for Accurate Indirect Occlusion"
// (2016), in the arrangement of Intel's XeGTAO (MIT licence, Intel Corporation): a prefiltered
// linear depth chain, slices and steps with a squared step distribution and the horizon falloff,
// the cosine-weighted arc integral, noise rotated per pixel, a depth-aware denoise. The code here
// is written for this renderer from the paper; none of XeGTAO's source is copied.
//
// View space here: x right, y up, z forward (away from the camera, positive in front of it), the
// depth along z in the game's world units ("linear depth", t). Pixels are in the pass's own target
// pixels, y down; the guest's depth is 1 near and 0 far (AO-DESIGN.md CHECK 2).

#ifndef MCLA_GPU_AO_COMMON_HLSLI
#define MCLA_GPU_AO_COMMON_HLSLI

#include "host_pass_common.hlsli"

#ifndef AO_DEPTH_MS
#define AO_DEPTH_MS 0
#endif

struct AoPush {
  // x: the depth plane's bindless slot (the EDRAM depth target: Texture2DMS in space1 when
  //    AO_DEPTH_MS, else Texture2D in space0)
  // y: the host sample that is the guest's sample 0
  // z, w: the images this pass reads (slot of src0, src1)
  uint4 slots;
  // x: slot of src2; y: the frame number the noise is shifted by; z: flags (below); w: reserved
  uint4 slots2;
  // xy: this pass's target size; zw: the size of the image it reads (the depth plane's for the
  // depth pass, the previous level's for a mip pass, the AO images' for the others)
  float4 sizes;
  // x, y: tan of the half field of view (horizontal, vertical); z: the effect radius in world
  // units; w: the falloff range as a share of the radius
  float4 lens;
  // t = (P32 - ndc * P33) / (P22 - ndc * P23), ndc = 1 - stored depth: the projection's z rows
  float4 lin;
  // x: the final power of the visibility (mcla_gpu_ao_power); y: its floor; z: strength; w: the
  // distance fade's start
  float4 shape;
  // xyz: the direction to the sun in view space (unit length; zero when unknown); w: the ambient
  // light's luma
  float4 light;
  // x: the sun light's luma; y: the distance fade's end; z: how far the ambient share moves towards
  // 1 (mcla_gpu_ao_direct: the occlusion also darkens the sun-lit part); w: reserved
  float4 extra;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<AoPush> g_push;
#else
ConstantBuffer<AoPush> g_push : register(b0, space4);
#endif

// flags: bits 0-7 debug view (0 none), 8-11 slices, 12-15 steps, 16 low (half resolution), 17-19
// the denoise radius.
uint AoView() { return g_push.slots2.z & 0xFFu; }
uint AoSlices() { return (g_push.slots2.z >> 8) & 0xFu; }
uint AoSteps() { return (g_push.slots2.z >> 12) & 0xFu; }
bool AoLow() { return (g_push.slots2.z & (1u << 16)) != 0u; }
int AoDenoiseRadius() { return int((g_push.slots2.z >> 17) & 0x7u); }

// What the depth chain holds for the sky and for anything the linearisation cannot place. Also
// the largest value a half float keeps (65504) with room.
static const float kAoSky = 60000.0f;

// The depth plane's stored value at `p` (the guest's sample 0 when multisampled).
float AoLoadStored(int2 p) {
#if AO_DEPTH_MS
  return g_textures_ms[g_push.slots.x].Load(p, g_push.slots.y).r;
#else
  return g_textures[g_push.slots.x].Load(int3(p, 0)).r;
#endif
}

// The distance along the view direction of a stored depth (1 near, 0 far); kAoSky for the sky.
float AoLinearDepth(float stored) {
  const float ndc = 1.0f - saturate(stored);
  const float num = g_push.lin.z - ndc * g_push.lin.w;
  const float den = g_push.lin.x - ndc * g_push.lin.y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kAoSky) ? t : kAoSky;
}

// The view-space position of the pixel centre `centre` (in pixels of an image `size` wide and
// high) at distance `t`.
float3 AoViewPos(float2 centre, float2 size, float t) {
  const float2 uv = centre / size;
  return float3((uv.x * 2.0f - 1.0f) * g_push.lens.x * t, (1.0f - uv.y * 2.0f) * g_push.lens.y * t,
                t);
}

// A linear depth image (R32_FLOAT or R16_FLOAT, bindless slot `slot`) at `p`.
float AoLoadLinear(uint slot, int2 p) {
  return g_textures[slot].Load(int3(p, 0)).x;
}

// The view-space normal from the linear depth image `slot` of size `size` at pixel `p`, whose
// distance is `tc` and position `pc`: from the neighbour on each axis whose depth is nearer to the
// pixel's (so that a silhouette edge does not bend it), facing the camera.
float3 AoNormalFromImage(uint slot, int2 p, int2 size, float tc, float3 pc) {
  const int2 last = size - 1;
  const int2 pl = int2(max(p.x - 1, 0), p.y);
  const int2 pr = int2(min(p.x + 1, last.x), p.y);
  const int2 pt = int2(p.x, max(p.y - 1, 0));
  const int2 pb = int2(p.x, min(p.y + 1, last.y));
  const float tl = AoLoadLinear(slot, pl);
  const float tr = AoLoadLinear(slot, pr);
  const float tt = AoLoadLinear(slot, pt);
  const float tb = AoLoadLinear(slot, pb);
  const float2 fs = float2(size);
  const float3 h = abs(tl - tc) < abs(tr - tc) ? pc - AoViewPos(float2(pl) + 0.5f, fs, tl)
                                               : AoViewPos(float2(pr) + 0.5f, fs, tr) - pc;
  const float3 v = abs(tt - tc) < abs(tb - tc) ? AoViewPos(float2(pt) + 0.5f, fs, tt) - pc
                                               : pc - AoViewPos(float2(pb) + 0.5f, fs, tb);
  const float3 n = cross(v, h);
  const float len = length(n);
  return len > 1e-20f ? n / len : float3(0.0f, 0.0f, -1.0f);
}

// Interleaved gradient noise (Jimenez 2014) of a pixel position.
float AoNoise(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

#endif  // MCLA_GPU_AO_COMMON_HLSLI
