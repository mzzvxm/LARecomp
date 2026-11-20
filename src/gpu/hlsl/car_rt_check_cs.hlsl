// MCLA GPU renderer -- the player's car as ray tracing structures: the check pass (car_rt_scene.h, mcla_gpu_car_rt_check).
//
// Proves the structures on the first run. For every vehicle pixel of the ambient occlusion's linear depth image (the level 0 image,
// R32_FLOAT, whose lowest bit is the vehicle bit: ao_contact_ps.hlsl's header, ao_common.hlsli AoIsVehicle) one primary ray goes from
// the view-space origin through the pixel, in the lens the image was made with (ao_common.hlsli AoViewPos: the direction is
// (ndc.x * tan_x, ndc.y * tan_y, 1), so the committed hit's distance t is the view-space z of the hit, the image's own number). The
// committed hit is compared with the pixel's distance:
//
//   - the pixel is a vehicle's and the ray hit a part of the player's car: covered, with the relative difference |hit - image| / image
//   - the pixel is a vehicle's and nothing was hit: a miss (the other cars, the driver, a part not built yet, a wrong transform)
//
// 16 words are counted (car_rt_scene_rules.h has the numbers and the decoder; the wave's lanes add up first so that a million vehicle
// pixels do not all fight for one address). With a view mode the pass also writes an RGBA8 picture the size of the image:
//   1  the error as colour: green below 2%, yellow at 2%, red from 10%; magenta for a vehicle pixel with no hit
//   2  the hit's view-space normal (turned to face the ray) as colour: its x (right), y (up) and z (forward) times 0.5 plus 0.5
//   a  0.85 on a vehicle's pixel, 0 elsewhere (the display pass blends the picture into the scene)
//
// Layout (car_rt_scene.cpp), the same shape the ray traced reflection pass has: set 0 the bindless texture set (t0, space0: the linear depth
// image by its slot), set 1 this pass's own (u0 the picture, u1 the counters, space1), set 2 the car RT set (hlsl/car_rt_common.hlsli,
// CAR_RT_SPACE 2), the push range at (b0, space3; a Vulkan push constant block). Compiled at Shader Model 6.5 (DXIL), and to SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> gLinearDepth[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the picture is R8G8B8A8_UNORM.
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<unorm float4> gView : register(u0, space1);
RWByteAddressBuffer gStats : register(u1, space1);

struct CheckPush {
  // x the linear depth image's bindless slot, y and z its width and height, w the flags: bit 0 write the picture, bits 8-15 the view mode
  uint4 slots;
  // x and y tan of the half field of view the image was made with, z the ray's start (distance), w its end
  float4 lens;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CheckPush> g_push;
#else
ConstantBuffer<CheckPush> g_push : register(b0, space3);
#endif

// car_rt_scene_rules.h: the counters' places, the unit of the error sum and the histogram's edges.
static const uint kStatsWords = 16;
static const uint kStatVehicle = 0;
static const uint kStatCovered = 1;
static const uint kStatErrorSum = 2;
static const uint kStatNearer = 3;
static const uint kStatFarther = 4;
static const uint kStatHistogram = 5;
static const uint kStatGlass = 13;
static const uint kStatTyres = 14;
static const uint kStatBody = 15;
static const float kErrorUnit = 1000.0f;
static const float kNearFarTolerance = 0.02f;
// The error of one pixel is counted up to this (a larger one is a wrong surface, not a measure).
static const float kErrorCap = 0.5f;
// What the depth chain holds for the sky (ao_common.hlsli kAoSky): a vehicle's pixel never has it.
static const float kSky = 60000.0f;

uint BucketOf(float error) {
  return error < 0.0025f ? 0u
         : error < 0.005f ? 1u
         : error < 0.01f  ? 2u
         : error < 0.02f  ? 3u
         : error < 0.05f  ? 4u
         : error < 0.1f   ? 5u
         : error < 0.25f  ? 6u
                          : 7u;
}

// The wave's lanes add up first, one lane adds to the counter.
void Count(uint word, uint value) {
  if (WaveIsFirstLane() && value != 0u) {
    uint ignored;
    gStats.InterlockedAdd(word * 4u, value, ignored);
  }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 size = uint2(g_push.slots.y, g_push.slots.z);
  const bool inside = id.x < size.x && id.y < size.y;
  float depth = 0.0f;
  if (inside) {
    depth = gLinearDepth[g_push.slots.x].Load(int3(id.xy, 0)).x;
  }
  const bool vehicle = inside && (asuint(depth) & 1u) != 0u;
  const float t_image = asfloat(asuint(depth) & ~1u);

  // The pixel's ray: the same view-space position the depth pass's pixel has at distance 1.
  const float2 uv = (float2(id.xy) + 0.5f) / float2(size);
  const float3 direction = float3((uv.x * 2.0f - 1.0f) * g_push.lens.x, (1.0f - uv.y * 2.0f) * g_push.lens.y, 1.0f);

  CarRtHit hit = (CarRtHit)0;
  if (vehicle && t_image > 0.0f && t_image < kSky) {
    hit = CarRtTrace(float3(0.0f, 0.0f, 0.0f), direction, g_push.lens.z, g_push.lens.w, CAR_RT_MASK_ALL);
  }
  const bool covered = vehicle && hit.hit;
  const float relative = covered ? (hit.t - t_image) / t_image : 0.0f;
  const float error = abs(relative);
  uint mask = 0u;
  if (covered) {
    mask = CarRtInstanceMask(CarRtLoadInstance(hit.instance_index));
  }

  // The counters, one add a wave.
  Count(kStatVehicle, WaveActiveCountBits(vehicle));
  Count(kStatCovered, WaveActiveCountBits(covered));
  Count(kStatErrorSum, WaveActiveSum(covered ? uint(min(error, kErrorCap) * kErrorUnit + 0.5f) : 0u));
  Count(kStatNearer, WaveActiveCountBits(covered && relative < -kNearFarTolerance));
  Count(kStatFarther, WaveActiveCountBits(covered && relative > kNearFarTolerance));
  const uint bucket = BucketOf(error);
  [unroll] for (uint b = 0u; b < 8u; ++b) {
    Count(kStatHistogram + b, WaveActiveCountBits(covered && bucket == b));
  }
  Count(kStatGlass, WaveActiveCountBits(covered && (mask & CAR_RT_MASK_GLASS) != 0u));
  Count(kStatTyres, WaveActiveCountBits(covered && (mask & CAR_RT_MASK_TYRES) != 0u));
  Count(kStatBody, WaveActiveCountBits(covered && (mask & CAR_RT_MASK_BODY) != 0u));

  // The picture.
  if (inside && (g_push.slots.w & 1u) != 0u) {
    float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (vehicle) {
      const uint mode = (g_push.slots.w >> 8) & 0xFFu;
      color = float4(1.0f, 0.0f, 1.0f, 0.85f);  // a vehicle pixel no part of the car covers
      if (covered) {
        if (mode == 2u) {
          const float3 n = CarRtHitNormal(hit.instance_index, hit.primitive_index, hit.barycentrics, hit.object_to_world, direction);
          color = float4(n * 0.5f + 0.5f, 0.85f);
        } else {
          const float3 ramp = error < 0.02f ? lerp(float3(0.0f, 1.0f, 0.0f), float3(1.0f, 1.0f, 0.0f), error / 0.02f)
                                            : lerp(float3(1.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f), saturate((error - 0.02f) / 0.08f));
          color = float4(ramp, 0.85f);
        }
      }
    }
    gView[id.xy] = color;
  }
}
