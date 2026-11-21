// MCLA GPU renderer -- car self-reflections, the history pass (car_selfrefl.h).
//
// Run at the game's mid-scene depth copy, when the scene's opaque draws and the car are all in the colour target. One pixel
// of the image (the picture's size over the push's divisor, 1 or 2; R16G16B16A16_FLOAT) is
//
//   rgb  the scene colour of a vehicle's pixel: the mean over the host samples of the target (a multisampled scene:
//        Texture2DMS), clamped to what a half float keeps, a NaN counted as black, in the target's own units (the shaders'
//        output times 2^exp_bias); 0 for everything else
//   a    the view-space distance of the pixel when the depth target's stencil ID says a vehicle (the byte >> 2, 1 to 23, of
//        the guest's sample 0), else 0: the paint variant treats a > 0 as a car pixel it may hit and reads the rest as
//        nothing. The distance is the lens' linearisation of the stored depth (1 near, 0 far), the formula of
//        ao_common.hlsli AoLinearDepth, with the lens the push gives.
//
// Only a vehicle's pixel has anything the paint reads (the ray's hit test reads the distance, the hit's colour is read at a
// vehicle's pixel), so the stencil is read first and the depth and the colour's samples only where it says a vehicle: a
// car is a tenth of the picture or less, and the pass reads one value of the other nine tenths instead of all of them. At a
// divisor of 2 one pixel of the image stands for the pixel at the lower right of its 2x2 block (the image is a quarter of
// the size: the reflection of the car's own body is blurred by the paint's noise anyway).
//
// The planes are the EDRAM depth target's (depth and stencil views) and the main colour target's, read as textures.
//
// Three compilations (CAR_HISTORY_MS): 0 plain planes, 1 multisampled planes, 2 multisampled depth and stencil planes with the colour read from one single sample image of the picture's size
// (slots.y is that image's slot: the shared screen products' colour_post, which already holds the mean of the host samples, clamped as below; the same bytes as variant 1 would make).

#include "host_pass_common.hlsli"

#ifndef CAR_HISTORY_MS
#define CAR_HISTORY_MS 0
#endif

struct CarHistoryPush {
  // x the main depth plane's bindless slot, y the main colour plane's, z the depth target's stencil plane's
  // (0xFFFFFFFF: none, the image then has no car pixels), w the host samples of the planes (1: plain textures)
  uint4 slots;
  // x the host sample that is the guest's sample 0
  uint4 slots2;
  // xy the picture in host pixels, z the divisor (1: the image is the picture's size, 2: half of it in each direction)
  float4 sizes;
  // the projection's rows P22, P23, P32, P33 (view space to clip, row vectors, the camera looking down -z)
  float4 lin;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarHistoryPush> g_push;
#else
ConstantBuffer<CarHistoryPush> g_push : register(b0, space4);
#endif

static const uint kNoSlot = 0xFFFFFFFFu;
static const uint kFirstVehicleId = 1u;
static const uint kLastVehicleId = 23u;
// The largest distance a half float keeps with room, and the largest colour.
static const float kMaxDistance = 60000.0f;
static const float kMaxColour = 60000.0f;

// A colour a half float image keeps, held the same way on both APIs: not negative, at most kMaxColour, and a NaN (any sign or payload) black. The NaN is decided by the bits and the clamp only sees
// numbers: min(max(NaN, 0), x) is 0 on D3D12 (the DXIL max returns the other operand) but SPIR-V's FMax / FMin leave which operand a NaN gives open, so Vulkan must not lean on it.
float HeldChannel(float v) {
  return ((asuint(v) & 0x7FFFFFFFu) <= 0x7F800000u) ? min(max(v, 0.0f), kMaxColour) : 0.0f;
}
float3 HeldColour(float3 colour) {
  return float3(HeldChannel(colour.x), HeldChannel(colour.y), HeldChannel(colour.z));
}

// The distance along the view direction of a stored depth; 0 for the sky and for anything the linearisation cannot place.
float LinearDistance(float stored) {
  const float ndc = 1.0f - saturate(stored);
  const float num = g_push.lin.z - ndc * g_push.lin.w;
  const float den = g_push.lin.x - ndc * g_push.lin.y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kMaxDistance) ? t : 0.0f;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const float divisor = max(g_push.sizes.z, 1.0f);
  const int2 p = min(int2(position.xy * divisor), int2(g_push.sizes.xy) - 1);
  uint id = 0u;
  if (g_push.slots.z != kNoSlot) {
#if CAR_HISTORY_MS
    id = StencilOf(g_utextures_ms[g_push.slots.z].Load(p, g_push.slots2.x)) >> 2;
#else
    id = StencilOf(g_utextures[g_push.slots.z].Load(int3(p, 0))) >> 2;
#endif
  }
  if (id < kFirstVehicleId || id > kLastVehicleId) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);  // not a vehicle: nothing the paint reads
  }
#if CAR_HISTORY_MS == 2
  // The depth and stencil planes are multisampled, the colour is the shared screen products' colour_post at the picture's size (screen_shared.h; hlsl/screen_colour_ps.hlsl made it with the
  // loop below, in the same order): one load of the pixel.
  const float stored = g_textures_ms[g_push.slots.x].Load(p, g_push.slots2.x).r;
  const float3 colour = g_textures[g_push.slots.y].Load(int3(p, 0)).rgb;
#elif CAR_HISTORY_MS
  const uint samples = max(g_push.slots.w, 1u);
  const float stored = g_textures_ms[g_push.slots.x].Load(p, g_push.slots2.x).r;
  float3 sum = float3(0.0f, 0.0f, 0.0f);
  [loop] for (uint s = 0u; s < samples; ++s) {
    sum += g_textures_ms[g_push.slots.y].Load(p, s).rgb;
  }
  const float3 colour = sum / float(samples);
#else
  const float stored = g_textures[g_push.slots.x].Load(int3(p, 0)).r;
  const float3 colour = g_textures[g_push.slots.y].Load(int3(p, 0)).rgb;
#endif
  return float4(HeldColour(colour), LinearDistance(stored));
}
