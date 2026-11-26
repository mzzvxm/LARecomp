// MCLA GPU renderer -- the player's car as ray tracing structures: the check pass (car_rt_scene.h, mcla_gpu_car_rt_check).
//
// Proves the structures on the first run. For every vehicle pixel of the ambient occlusion's linear depth image (the level 0 image,
// R32_FLOAT, whose lowest bit is the vehicle bit: ao_contact_ps.hlsl's header, ao_common.hlsli AoIsVehicle) that the scene is meant to
// hold (the scope, below) one primary ray goes from the view-space origin through the pixel, in the lens the image was made with
// (ao_common.hlsli AoViewPos: the direction is (ndc.x * tan_x, ndc.y * tan_y, 1), so the committed hit's distance t is the view-space z
// of the hit, the image's own number). The committed hit is compared with the pixel's distance:
//
//   - the pixel is a vehicle's and the ray hit a part of a car in the scene: covered, with the relative difference |hit - image| / image
//   - the pixel is a vehicle's and nothing was hit: a miss (the driver, a part not built yet, a wrong transform)
//
// THE SCOPE. The scene holds the player's car and the other cars the frame's selection kept (the class's radius and count, the cars 0
// switch: car_rt_instances.h), and leaves the driver out, so a vehicle pixel of any other car is not a miss: it is "not in the scene by
// design" and is counted apart (kStatOutside), not traced. The image says only "vehicle", not which car, so the pixel's view-space
// position (direction * image distance) is placed in the kept cars' boxes: the player's (the push block's, grown by the host) and each
// kept other car's (the car RT table's rows from scope.x, two a box, scope.y of them: the same rows the ray traced occlusion reads,
// grown by kScopeBoxMargin). Outside every box: not in the scene by design (without a box of the player's car, nothing is: a pixel in no
// other car's box counts as the player's). The driver is inside the player's box and the depth image
// has no mask for it, so its pixels stay in the player's count (seen through glass they are hit as glass, nearer than the image).
//
// 20 words are counted (car_rt_scene_rules.h has the numbers and the decoder; the wave's lanes add up first so that a million vehicle
// pixels do not all fight for one address). With a view mode the pass also writes an RGBA8 picture the size of the image:
//   1  the error as colour: green below 2%, yellow at 2%, red from 10%; magenta for a vehicle pixel with no hit; grey-blue for one not in
//      the scene by design
//   2  the hit's view-space normal (turned to face the ray) as colour: its x (right), y (up) and z (forward) times 0.5 plus 0.5
//   3  another car's PAINT as the reflection pass reads it (RT-MODEL-CARS-4): a pixel whose hit is on another car's part shows the HUE of that car's record's second row
//      (CarRtLoadCar, the table the reflection shader reads; its largest channel at 1: the picture is 8 bits, the strength is in the probe); a car whose record has no paint is
//      purple, the player's own car dark grey
//   a  0.85 on a vehicle's pixel (0.6 where not in the scene by design), 0 elsewhere (the display pass blends the picture into the scene)
//
// THE PROBE (RT-MODEL-CARS-4): the thread (0, 0) also copies the first kProbeCars cars' records (two rows each, from the row g_push.scope.w, as many as bits 16 to 23 of slots.w say)
// as they are in the table into the words from kStatProbe, raw: the host compares them with what it wrote (car_rt_scene_rules.h DecodeProbe) and logs whether the shader read
// the paint the host sent.
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
  // x the linear depth image's bindless slot, y and z its width and height, w the flags: bit 0 write the picture, bits 8-15 the view mode, bits 16-23 how many cars' records
  // the table holds (the probe reads the first kProbeCars of them)
  uint4 slots;
  // x and y tan of the half field of view the image was made with, z the ray's start (distance), w its end
  float4 lens;
  // x the first row of the other kept cars' boxes in the car RT table, y how many boxes (0 to kScopeMaxOtherBoxes), z the flags (bit 0 the
  // player's box is known), w the first row of the cars' records (two rows a car: CarRow, CarPaintRow)
  uint4 scope;
  // xyz the player's car's box in view space (grown by the host)
  float4 box_min;
  float4 box_max;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CheckPush> g_push;
#else
ConstantBuffer<CheckPush> g_push : register(b0, space3);
#endif

// car_rt_scene_rules.h: the counters' places, the unit of the error sum and the histogram's edges.
static const uint kStatsWords = 52;
static const uint kStatVehicle = 0;
static const uint kStatCovered = 1;
static const uint kStatErrorSum = 2;
static const uint kStatNearer = 3;
static const uint kStatFarther = 4;
static const uint kStatHistogram = 5;
static const uint kStatGlass = 13;
static const uint kStatTyres = 14;
static const uint kStatBody = 15;
static const uint kStatOutside = 16;
static const uint kStatOthers = 17;
// The probe (car_rt_scene_rules.h): the first cars' records as the shader reads them, 8 words a car.
static const uint kStatProbe = 20;
static const uint kProbeCars = 4;
static const uint kProbeWordsPerCar = 8;
// The scope (car_rt_scene_rules.h): what the other cars' boxes are grown by, how many there may be, the flag of the player's box.
static const float kScopeBoxMargin = 0.15f;
static const uint kScopeMaxOtherBoxes = 16;
static const uint kScopeFlagPlayerBox = 1u;
static const uint kScopeOutside = 0u;
static const uint kScopePlayer = 1u;
static const uint kScopeOther = 2u;
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

// Which car's box the view-space position `p` of a vehicle pixel lies in: the player's (kScopePlayer), another kept car's (kScopeOther) or none
// (kScopeOutside: a car outside the frame's selection). The mirror of car_rt_scene_rules.h ScopeOf.
uint ScopeOf(float3 p) {
  if ((g_push.scope.z & kScopeFlagPlayerBox) != 0u && all(p >= g_push.box_min.xyz) && all(p <= g_push.box_max.xyz)) {
    return kScopePlayer;
  }
  const uint boxes = min(g_push.scope.y, kScopeMaxOtherBoxes);
  [loop] for (uint b = 0u; b < boxes; ++b) {
    const uint row = g_push.scope.x + b * 2u;
    const float3 box_min = asfloat(gCarRtInstances[row].xyz) - kScopeBoxMargin;
    const float3 box_max = asfloat(gCarRtInstances[row + 1u].xyz) + kScopeBoxMargin;
    if (all(p >= box_min) && all(p <= box_max)) {
      return kScopeOther;
    }
  }
  // No box of the player's car (none of its meshes had one): nothing can be told from its pixels, they all count.
  return (g_push.scope.z & kScopeFlagPlayerBox) != 0u ? kScopeOutside : kScopePlayer;
}

// The wave's lanes add up first, one lane adds to the counter.
void Count(uint word, uint value) {
  if (WaveIsFirstLane() && value != 0u) {
    uint ignored;
    gStats.InterlockedAdd(word * 4u, value, ignored);
  }
}

// The probe: the cars' records as the shader's own loads read them (CarRtLoadCar's two rows), raw, for the host to compare with what it wrote.
void ProbeCars() {
  const uint count = min((g_push.slots.w >> 16) & 0xFFu, kProbeCars);
  [loop] for (uint k = 0u; k < count; ++k) {
    const uint row = g_push.scope.w + k * 2u;
    const uint4 centre = gCarRtInstances[row];
    const uint4 paint = gCarRtInstances[row + 1u];
    gStats.Store4((kStatProbe + k * kProbeWordsPerCar) * 4u, centre);
    gStats.Store4((kStatProbe + k * kProbeWordsPerCar + 4u) * 4u, paint);
  }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0u && id.y == 0u) {
    ProbeCars();
  }
  const uint2 size = uint2(g_push.slots.y, g_push.slots.z);
  const bool inside = id.x < size.x && id.y < size.y;
  float depth = 0.0f;
  if (inside) {
    depth = gLinearDepth[g_push.slots.x].Load(int3(id.xy, 0)).x;
  }
  const bool vehicle_pixel = inside && (asuint(depth) & 1u) != 0u;
  const float t_image = asfloat(asuint(depth) & ~1u);
  const bool usable = t_image > 0.0f && t_image < kSky;

  // The pixel's ray: the same view-space position the depth pass's pixel has at distance 1.
  const float2 uv = (float2(id.xy) + 0.5f) / float2(size);
  const float3 direction = float3((uv.x * 2.0f - 1.0f) * g_push.lens.x, (1.0f - uv.y * 2.0f) * g_push.lens.y, 1.0f);

  // The scope: a vehicle pixel with no usable distance (never the case) stays counted, as a miss; the others go by the box their position is in.
  uint scope = kScopePlayer;
  if (vehicle_pixel && usable) {
    scope = ScopeOf(direction * t_image);
  }
  const bool outside = vehicle_pixel && scope == kScopeOutside;
  const bool vehicle = vehicle_pixel && !outside;

  CarRtHit hit = (CarRtHit)0;
  if (vehicle && usable) {
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
  Count(kStatOutside, WaveActiveCountBits(outside));
  Count(kStatOthers, WaveActiveCountBits(vehicle && scope == kScopeOther));
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
    if (outside) {
      color = float4(0.25f, 0.35f, 0.5f, 0.6f);  // a vehicle pixel of a car the scene is not meant to hold
    } else if (vehicle) {
      const uint mode = (g_push.slots.w >> 8) & 0xFFu;
      color = float4(1.0f, 0.0f, 1.0f, 0.85f);  // a vehicle pixel no part of the car covers
      if (covered) {
        if (mode == 3u) {
          const CarRtInstance hit_instance = CarRtLoadInstance(hit.instance_index);
          if ((CarRtInstanceMask(hit_instance) & CAR_RT_MASK_OTHERS) != 0u) {
            const CarRtCar car = CarRtLoadCar(hit_instance);
            // the picture is 8 bits (0..1): the paint's HUE at full strength (its largest channel 1); its strength is in the probe's words
            const float strongest = max(max(car.paint.x, car.paint.y), max(car.paint.z, 1e-4f));
            color = car.has_paint ? float4(saturate(car.paint / strongest), 0.9f) : float4(0.5f, 0.0f, 0.5f, 0.9f);
          } else {
            color = float4(0.1f, 0.1f, 0.1f, 0.5f);  // the player's own car: not another car's paint
          }
        } else if (mode == 2u) {
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
