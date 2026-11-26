// MCLA GPU renderer -- volumetric light (volumetrics.h), shared declarations of the two compute passes.
//
// The binding model of the compute passes (volumetrics.cpp, one layout for both), the same on D3D12 and Vulkan:
//   set 0  t0, space0   Texture2D<float4>[]   the bindless texture heap: the history scatter, the scatter the integrate reads, the cascade
//                                             atlas, the headlights' shadow atlas (all read with Load or the sampler below)
//   set 1  s0, space1   SamplerState[]        the heap's samplers (descriptors.h: 0 linear clamp, 2 point clamp)
//   set 2  t0, space2   ByteAddressBuffer     VolGpuConstants (volumetrics_types.h), one per frame slot, uploaded each frame
//   set 3  u0, space3   RWTexture2D<float4>   the volume the pass writes (RGBA16F, the 2D slice atlas)
//   push   b0, space4   VolPush, 32 bytes (Vulkan: the one push-constant block)
// The writes are typed stores of RGBA16F (the format is given to SPIR-V explicitly: no shaderStorageImageWriteWithoutFormat needed).
//
// Everything the passes read of the constants is at the byte offsets below, which are volumetrics_types.h's VolGpuConstants byte for byte
// (volumetrics_rules_test.cpp parses the offset lines below and checks them against offsetof). The math is
// volumetrics_rules.h's twin: change one, change the other.

#ifndef MCLA_GPU_VOLUMETRICS_COMMON_HLSLI
#define MCLA_GPU_VOLUMETRICS_COMMON_HLSLI

Texture2D<float4> g_textures[] : register(t0, space0);
SamplerState g_samplers[] : register(s0, space1);
ByteAddressBuffer g_vol : register(t0, space2);
#ifdef __spirv__
[[vk::image_format("rgba16f")]] RWTexture2D<float4> g_out : register(u0, space3);
#else
RWTexture2D<float4> g_out : register(u0, space3);
#endif

struct VolPush {
  // x the history volume's bindless slot (the scatter of last frame), y the scatter volume's slot (the integrate reads it), z flags (bit 0:
  // history in use, bit 1: shadows of the headlights in use), w the inject's flags (bit 0: the lamps' term averaged over the slice and sampled at the voxel's centre,
  // bit 1: the steady headlights, VOL-STABLE-2)
  uint4 slots;
  // unused for now (room for a debug view)
  float4 data;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<VolPush> g_push;
#else
ConstantBuffer<VolPush> g_push : register(b0, space4);
#endif

static const uint kSamplerLinearClamp = 0;
static const uint kSamplerPointClamp = 2;

// Byte offsets into VolGpuConstants.
static const uint kO_grid = 0;
static const uint kO_depth = 16;
static const uint kO_history = 32;
static const uint kO_jitter = 48;
static const uint kO_cam_pos = 64;
static const uint kO_prev_pos = 128;
static const uint kO_world = 192;
static const uint kO_sun = 304;
static const uint kO_cascades = 336;
static const uint kO_sun_atlas = 656;
static const uint kO_sun_size = 672;
static const uint kO_counts = 688;
static const uint kO_shadow_atlas = 704;
static const uint kO_lamps = 720;
static const uint kO_headlights = 3792;
static const uint kO_shadow_views = 4048;
static const uint kO_prev_counts = 4816;
static const uint kO_prev_headlights = 4832;
static const uint kO_lamp_changes = 5088;
static const uint kO_retired_lamps = 5104;

float4 VolF4(uint offset) { return asfloat(g_vol.Load4(offset)); }
uint4 VolU4(uint offset) { return g_vol.Load4(offset); }

static const float kPi = 3.14159265358979323846;

// --- the grid ---------------------------------------------------------------------------------------------------------------------

// The atlas texel of voxel (x, y) of slice k.
uint2 AtlasTexel(uint2 xy, uint k, uint4 grid) {
  const uint tx = k % grid.w;
  const uint ty = k / grid.w;
  return uint2(tx * grid.x + xy.x, ty * grid.y + xy.y);
}
// The atlas uv of (u, v) in 0..1 of slice k, half a texel inside the tile.
float2 AtlasUv(float2 uv, uint k, uint4 grid) {
  const uint tx = k % grid.w;
  const uint ty = k / grid.w;
  const uint tiles_y = grid.z / grid.w;
  const float2 texel = clamp(uv * float2(grid.xy), 0.5, float2(grid.xy) - 0.5);
  return (float2(tx * grid.x, ty * grid.y) + texel) / float2(grid.x * grid.w, grid.y * tiles_y);
}

#endif  // MCLA_GPU_VOLUMETRICS_COMMON_HLSLI
