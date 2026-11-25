// Half-render-size world AO, inside the AO umbrella after the car/city sun pass.
#define CAR_RT_SPACE 5
#define CITY_HIT_SPACE 5
#define CITY_HIT_ALPHA_TEXEL(slot, texel, level) g_textures[NonUniformResourceIndex((slot) & 0x7FFFFFFFu)].Load(int3(texel, int(level))).w
#include "host_pass_common.hlsli"
#include "car_rt_common.hlsli"
#include "car_rt_refl_common.hlsli"
#include "city_hit_alpha.hlsli"
#include "gbuffer_common.hlsli"
#include "rtao_common.hlsli"
#include "interior_camera_rules.hlsli"
#include "rtao_noise.hlsli"

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_signal : register(u0, space4);
#ifdef __spirv__
[[vk::image_format("r32f")]]
#endif
RWTexture2D<float> g_depth : register(u1, space4);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_normal : register(u2, space4);
#ifdef __spirv__
[[vk::image_format("rg16f")]]
#endif
RWTexture2D<float2> g_id_share : register(u3, space4);
RWByteAddressBuffer g_stats : register(u4, space4);

static const uint kCityMask = 0x80u;
static const uint kInstanceCity = 0x800000u;
static const uint kCabinMask = 0x03u;
static const uint kCabinRays = 64u;
static const float kGolden = 0.6180339887;
static const float kHalfSurfaceShare = 0.04;
static const float kHalfSurfaceFloor = 0.02;

#include "rtao_surface.hlsli"

groupshared uint g_group_stats[7];

void FlushCount(uint word, uint value) {
  uint ignored;
  g_stats.InterlockedAdd(word * 4u, value, ignored);
}

void Count(uint word, uint value) {
  InterlockedAdd(g_group_stats[word], value);
}

// The city shadow pass's PCG and candidate acceptance rule. The sample lane
// decorrelates foliage from ray length, direction and the sub-pixel choice.
uint Pcg(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

bool AcceptCandidate(uint mode, bool known, float alpha, float reference, float hash) {
  if (mode == 3u && known)
    return CityHitAlphaBlocks(alpha, reference);
  if (mode == 0u)
    return false;
  if (mode == 1u)
    return true;
  return hash < 0.5;
}

bool Blocked(float3 origin, float3 direction, float t_min, float t_max, uint mask,
             float footprint, float spread, uint salt, bool own, uint instance, uint primitive, float3 normal) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = t_min;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, mask, ray);
  const uint mode = (g_push.slots.w >> kFlagAlphaShift) & 3u;
  const float reference = float((g_push.slots.w >> kFlagAlphaRefShift) & 255u) * (1.0 / 255.0);
  const bool stats = (g_push.slots.w & kFlagStats) != 0u;
  while (query.Proceed()) {
    if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
      continue;
    bool known = false;
    float alpha = 1.0;
    const uint candidate = query.CandidateInstanceID();
    const uint candidate_primitive = query.CandidatePrimitiveIndex();
    if (mode == 3u && (candidate & kInstanceCity) != 0u) {
      const float width = footprint + query.CandidateTriangleRayT() * spread;
      const CityHitAlpha hit = CityHitAlphaOfCandidate(candidate, query.CandidateGeometryIndex(), candidate_primitive,
                                                        query.CandidateTriangleBarycentrics(), width);
      known = hit.known != 0u;
      alpha = hit.alpha;
    }
    if (stats) {
      Count(kStatCandidates, 1u);
      if (known)
        Count(kStatAlpha, 1u);
    }
    const float hash = float(Pcg(salt ^ Pcg(candidate ^ Pcg(candidate_primitive))) >> 8) * (1.0 / 16777216.0);
    if (AcceptCandidate(mode, known, alpha, reference, hash))
      query.CommitNonOpaqueTriangleHit();
  }
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    return false;
  return !own || !SelfHit(query.CommittedInstanceIndex(), query.CommittedPrimitiveIndex(),
                          query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(),
                          direction, instance, primitive, normal);
}

bool Inside(float3 p, float3 lo, float3 hi, float pad) {
  return all(lo <= hi) && all(p >= lo - pad) && all(p <= hi + pad);
}

bool NearCar(float3 p) {
  const float reach = g_push.lens.z;
  if (Inside(p, g_push.box_min.xyz, g_push.box_max.xyz, reach))
    return true;
  uint row = g_push.trace.w & 0xFFFFFFu;
  const uint count = g_push.trace.w >> 24u;
  for (uint i = 0u; i < count; ++i, row += 2u) {
    if (Inside(p, asfloat(gCarRtInstances[row].xyz), asfloat(gCarRtInstances[row + 1u].xyz), reach + 0.15))
      return true;
  }
  return false;
}

bool CabinRay(uint i) {
  const float2 rotate = BlueNoiseFrame2(31, 7, RtAoNoiseSample(g_push.trace.x, 7u));
  const float z = 1.0 - 2.0 * ((float(i) + rotate.y) / float(kCabinRays));
  const float angle = kTwoPi * frac(float(i) * kGolden + rotate.x);
  const float r = sqrt(max(0.0, 1.0 - z * z));
  RayDesc ray;
  ray.Origin = float3(0.0, 0.0, 0.0);
  ray.Direction = float3(r * cos(angle), r * sin(angle), z);
  ray.TMin = 0.0;
  ray.TMax = 3.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, kCabinMask, ray);
  while (query.Proceed()) {}
  return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

uint VehicleId(int2 p, uint packed, uint raw) {
  if (packed == 0xFFFFFFFFu || (raw & 1u) == 0u)
    return 0u;
  const uint slot = packed & 0xFFFFFFu;
  if ((g_push.slots.w & kFlagDepthHalf) != 0u) {
    const uint source = (raw >> 1u) & 3u;
    uint w, h;
#if RTAO_MS
    uint samples;
    g_utextures_ms[slot].GetDimensions(w, h, samples);
#else
    g_utextures[slot].GetDimensions(w, h);
#endif
    p = min(p * 2 + int2(source & 1u, source >> 1u), int2(w, h) - 1);
  }
#if RTAO_MS
  const uint id = StencilOf(g_utextures_ms[slot].Load(p, packed >> 24u)) >> 2u;
#else
  const uint id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2u;
#endif
  return id <= 23u ? id : 0u;
}

float3 SurfaceNormal(int2 pixel, float depth, float3 p) {
  if (g_push.slots.y != 0xFFFFFFFFu)
    return GbOctDecode(g_textures[g_push.slots.y].Load(int3(pixel, 0)).xy);
  const float3 n = NormalFromDepth(pixel, int2(g_push.sizes.zw), depth, p);
  return dot(n, p) > 0.0 ? -n : n;
}

void TracePixel(uint2 pixel) {
  const uint flags = g_push.slots.w;
  const uint2 size = g_push.sizes.zw;
  const uint2 traced = g_push.sizes.xy;
  const uint2 first = all((size + 1u) / 2u == traced) ? pixel * 2u : (pixel * size) / traced;
  const uint2 span = uint2(size.x > traced.x, size.y > traced.y);
  uint bits[4];
  int2 coords[4];
  float4 depths;
  if (all(span == 0u)) {
    const uint raw = asuint(g_textures[g_push.slots.x].Load(int3(first, 0)).x);
    [unroll]
    for (uint i = 0u; i < 4u; ++i) {
      coords[i] = int2(first);
      bits[i] = raw;
      depths[i] = asfloat(raw & ~7u);
    }
  } else {
    [unroll]
    for (uint k = 0u; k < 4u; ++k) {
      coords[k] = int2(min(first + uint2(k & 1u, k >> 1u) * span, size - 1u));
      bits[k] = asuint(g_textures[g_push.slots.x].Load(int3(coords[k], 0)).x);
      depths[k] = asfloat(bits[k] & ~7u);
    }
  }
  const float mean = dot(depths, 0.25);
  const float spread = max(max(depths.x, depths.y), max(depths.z, depths.w)) -
                       min(min(depths.x, depths.y), min(depths.z, depths.w));
  uint guide = 0u;
  [unroll]
  for (uint j = 1u; j < 4u; ++j) {
    if (abs(depths[j] - mean) < abs(depths[guide] - mean))
      guide = j;
  }
  const float guide_depth = spread <= kHalfSurfaceShare * mean + kHalfSurfaceFloor ? mean : depths[guide];
  const float3 guide_position = ViewPos(float2(coords[guide]) + 0.5, float2(size), guide_depth);
  const uint id = VehicleId(coords[guide], g_push.slots.z, bits[guide]);
  g_depth[pixel] = asfloat((asuint(guide_depth) & ~1u) | (bits[guide] & 1u));
  g_normal[pixel] = float4(SurfaceNormal(coords[guide], guide_depth, guide_position), 1.0);

  const uint sample_index = RtAoSubpixel(int(pixel.x), int(pixel.y), g_push.trace.x);
  uint trace_sample = sample_index;
  if ((flags & kRtAoMatchGuide) != 0u && sample_index != guide) {
    // Keep rotating through taps that represent the guide's surface.
    const uint sample_id = VehicleId(coords[sample_index], g_push.slots.z, bits[sample_index]);
    const float tolerance = kHalfSurfaceShare * depths[guide] + kHalfSurfaceFloor;
    trace_sample = RtAoSampleOf(sample_index, guide, sample_id, id, abs(depths[sample_index] - depths[guide]), tolerance);
  }
  const int2 sample_pixel = coords[trace_sample];
  const float t = depths[trace_sample];
  const float2 centre = float2(sample_pixel) + 0.5;
  float3 p = ViewPos(centre, float2(size), t);
  const bool cabin = (flags & kFlagInterior) != 0u && g_push.box_max.w != 0.0 &&
                     (Inside(p, g_push.box_min.xyz, g_push.box_max.xyz, g_push.look.w) ||
                      Inside(guide_position, g_push.box_min.xyz, g_push.box_max.xyz, g_push.look.w));
  // SafeRange is a world-space sphere bounded by the nearest missing hi sector.
  const bool judged = t > 0.0 && length(p) <= g_push.lens.w && length(guide_position) <= g_push.lens.w && !cabin;
  const bool stats = (flags & kFlagStats) != 0u;
  g_id_share[pixel] = float2(float(id), judged ? 1.0 : 0.0);
  if (!judged) {
    g_signal[pixel] = float4(1.0, 0.0, 0.0, 1.0);
    if (stats)
      Count(kStatUncovered, 1u);
    return;
  }

  float3 n = SurfaceNormal(sample_pixel, t, p);
  const float footprint = 2.0 * g_push.lens.y * t / float(size.y);
  float lift = Lift(t, footprint);
  uint instance = 0u, primitive = 0u;
  bool own = false;
  if ((bits[trace_sample] & 1u) != 0u) {
    float3 snapped_p, snapped_n;
    // A hit on the city has no car normal, so it cannot snap a vehicle pixel.
    own = Snap(centre, float2(size), t, snapped_p, snapped_n, instance, primitive);
    if (own) {
      p = snapped_p;
      n = snapped_n;
      lift = 0.004 + 0.002 * snapped_p.z;
    }
  }
  const float3 origin = p + n * lift;
  const bool shorten_car = !own && NearCar(p);
  const uint rays = clamp((flags >> kFlagRaysShift) & 7u, 1u, 4u);
  const float reach = g_push.lens.z;
  const float car_reach = g_push.look.z;
  const float alpha_noise = BlueNoiseFrame(int(pixel.x), int(pixel.y), 0u,
                                          RtAoNoiseSample(g_push.trace.x, 3u), kBlueNoiseR2StepA);
  const uint alpha_seed = uint(alpha_noise * 16777216.0) ^ asuint(g_push.box_min.w);
  float3 tangent, bitangent;
  Frame(n, tangent, bitangent);
  uint hits = 0u, queries = 0u;
  for (uint k = 0u; k < rays; ++k) {
    const float2 noise = RtAoRayNoise(int(pixel.x), int(pixel.y), g_push.trace.x, k);
    const float r2 = (float(k) + noise.y) / float(rays);
    const float angle = kTwoPi * (noise.x + kGolden * float(k));
    const float above = max(kMinAbove, sqrt(1.0 - r2));
    const float radius = sqrt(1.0 - above * above);
    const float3 direction = radius * (tangent * cos(angle) + bitangent * sin(angle)) + above * n;
    const float u = (float((k * 3u + 1u) % rays) + frac(noise.y * 5.0 + noise.x * 3.0)) / float(rays);
    const float ray_length = reach * (1.0 - sqrt(u));
    const uint salt = alpha_seed ^ Pcg(k);
    const float limit = shorten_car ? min(ray_length, car_reach) : ray_length;
    bool hit = limit > 0.0 && Blocked(origin, direction, 0.0, limit, g_push.trace.y, footprint, 0.08,
                                      salt, own, instance, primitive, n);
    queries += limit > 0.0 ? 1u : 0u;
    if (!hit && ray_length > limit) {
      hit = Blocked(origin, direction, limit, ray_length, kCityMask, footprint, 0.08, salt, false, 0u, 0u, n);
      ++queries;
    }
    hits += hit ? 1u : 0u;
  }
  if (stats) {
    Count(kStatRays, queries);
    Count(kStatHits, hits);
  }
  float open = 1.0 - float(hits) / float(rays);
  if ((flags & kFlagCheckOpen) != 0u)
    open = 1.0;
  if ((flags & kFlagCheckClosed) != 0u)
    open = 0.0;
  g_signal[pixel] = float4(open, 1.0, float(rays) / 4.0, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
  const bool stats = (g_push.slots.w & kFlagStats) != 0u;
  if (stats) {
    if (lane < 7u)
      g_group_stats[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();
  }
  if (all(group.xy == 0u) && (g_push.slots.w & kFlagProbe) != 0u) {
    if (lane == 0u)
      FlushCount(kStatCabinRays, kCabinRays);
    if (CabinRay(lane))
      FlushCount(kStatCabinHits, 1u);
  }
  if (all(thread.xy < g_push.sizes.xy))
    TracePixel(thread.xy);
  if (stats) {
    GroupMemoryBarrierWithGroupSync();
    if (lane < 7u && g_group_stats[lane] != 0u)
      FlushCount(lane, g_group_stats[lane]);
  }
}
