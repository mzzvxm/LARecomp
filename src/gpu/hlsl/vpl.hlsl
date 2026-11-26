// LT-6: camera-space depth tests, world-space history. Only hit placement
// stores history. The current receivers are shaded afresh each frame.
#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "light_laws.hlsli"

#ifndef VPL_MS
#define VPL_MS 0
#endif
#ifndef VPL_RAY
#define VPL_RAY 0
#endif

ByteAddressBuffer g_frame : register(t0, space4);
RWByteAddressBuffer g_hit_write : register(u1, space4);
ByteAddressBuffer g_hit_read : register(t2, space4);

#if VPL_RAY
#define CAR_RT_SPACE 5
#define CITY_HIT_SPACE 5
#define RT_WORLD_SPACE 5
#include "car_rt_common.hlsli"
#include "rt_world_frame.hlsli"
#define CITY_HIT_ALPHA_TEXEL(slot, p, level) g_textures[NonUniformResourceIndex((slot) & 0x7FFFFFFFu)].Load(int3((p), int(level))).w
#include "city_hit_alpha.hlsli"
#endif

static const uint kNoSlot = 0xFFFFFFFFu;
static const float kSky = 60000.0f;
static const uint kBeamSteps = 28u;
// CHECK: calibrate against the RT wall and road reference and inspect thin-wall leaks.
static const float kBounceFraction = 0.25f;
static const float kNearCeiling = 0.35f;
static const float kVehicleShare = 0.35f;

struct View {
  float3 right, up, forward, eye;
  float exposure;
  float4 lens, depth;
  float4 cabin_min, cabin_max;
  uint4 sizes, counts, slots, guides, apply;
};

View VplView() {
  View v;
  v.right = asfloat(g_frame.Load4(0)).xyz;
  v.up = asfloat(g_frame.Load4(16)).xyz;
  v.forward = asfloat(g_frame.Load4(32)).xyz;
  float4 eye = asfloat(g_frame.Load4(48));
  v.eye = eye.xyz;
  v.exposure = eye.w;
  v.lens = asfloat(g_frame.Load4(64));
  v.cabin_min = asfloat(g_frame.Load4(80));
  v.cabin_max = asfloat(g_frame.Load4(96));
  v.sizes = g_frame.Load4(112);
  v.counts = g_frame.Load4(128);
  v.slots = g_frame.Load4(144);
  v.guides = g_frame.Load4(160);
  v.apply = g_frame.Load4(176);
  v.depth = asfloat(g_frame.Load4(192));
  return v;
}

struct Beam {
  float4 origin_range, aim_ground, colour_law;
  uint4 meta;
};

Beam VplBeam(uint i) {
  uint offset = 208 + i * 64;
  Beam b;
  b.origin_range = asfloat(g_frame.Load4(offset));
  b.aim_ground = asfloat(g_frame.Load4(offset + 16));
  b.colour_law = asfloat(g_frame.Load4(offset + 32));
  b.meta = g_frame.Load4(offset + 48);
  return b;
}

float3 WorldVector(View v, float3 p) {
  return p.x * v.right + p.y * v.up + p.z * v.forward;
}

float3 ViewPoint(View v, float3 p) {
  p -= v.eye;
  return float3(dot(p, v.right), dot(p, v.up), dot(p, v.forward));
}

float3 AtDepth(View v, float2 uv, float depth) {
  float2 xy = (uv * float2(2, -2) + float2(-1, 1)) * v.lens.xy;
  return float3(xy, 1) * depth;
}

float2 Project(View v, float3 p) {
  return p.xy / (max(p.z, 0.001f) * v.lens.xy) * float2(0.5f, -0.5f) + 0.5f;
}

bool Inside(float2 uv) {
  return all(uv >= 0.0f) && all(uv < 1.0f);
}

float4 Plane(View v, uint slot, int2 p) {
#if VPL_MS
  return g_textures_ms[slot].Load(p, v.counts.w);
#else
  return g_textures[slot].Load(int3(p, 0));
#endif
}

uint Vehicle(View v, int2 p, float raw) {
  if (v.guides.w == kNoSlot) return asuint(raw) & 1u ? 24u : 0u;
#if VPL_MS
  uint stencil = StencilOf(g_utextures_ms[v.guides.w].Load(p, v.counts.w));
#else
  uint stencil = StencilOf(g_utextures[v.guides.w].Load(int3(p, 0)));
#endif
  uint id = stencil >> 2u;
  return id <= 23u ? id : 0u;
}

float Linear(float raw) {
  return asfloat(asuint(raw) & ~7u);
}

float MainDepth(View v, int2 p) {
  float stored = Plane(v, v.slots.w, p).x;
  float ndc = 1.0f - saturate(stored);
  float den = v.depth.x - ndc * v.depth.y;
  float t = (v.depth.z - ndc * v.depth.w) / den;
  return stored > 0.0f && t > 0.0f && t < kSky ? t : kSky;
}

float Chain(View v, float2 uv, uint level) {
  int2 size = max(int2(1, 1), (int2(v.counts.xy) + int(1u << level) - 1) >> level);
  int2 p = clamp(int2(uv * size), int2(0, 0), size - 1);
  return g_textures[v.slots[level]].Load(int3(p, 0)).x;
}

float3 GeometricNormal(View v, int2 p, float z) {
  int2 size = int2(v.counts.xy);
  int2 lo = max(p - 1, int2(0, 0)), hi = min(p + 1, size - 1);
  float3 pos = AtDepth(v, (float2(p) + 0.5f) / size, z);
  int2 pl = int2(lo.x, p.y), pr = int2(hi.x, p.y);
  int2 pt = int2(p.x, lo.y), pb = int2(p.x, hi.y);
  float dl = Linear(g_textures[v.slots.x].Load(int3(pl, 0)).x);
  float dr = Linear(g_textures[v.slots.x].Load(int3(pr, 0)).x);
  float dt = Linear(g_textures[v.slots.x].Load(int3(pt, 0)).x);
  float db = Linear(g_textures[v.slots.x].Load(int3(pb, 0)).x);
  float3 dx = abs(dl - z) < abs(dr - z) ? pos - AtDepth(v, (float2(pl) + 0.5f) / size, dl)
                                         : AtDepth(v, (float2(pr) + 0.5f) / size, dr) - pos;
  float3 dy = abs(dt - z) < abs(db - z) ? AtDepth(v, (float2(pt) + 0.5f) / size, dt) - pos
                                         : pos - AtDepth(v, (float2(pb) + 0.5f) / size, db);
  float3 n = cross(dy, dx);
  float len = dot(n, n);
  return len > 1.0e-10f ? n * rsqrt(len) : float3(0, 0, -1);
}

struct Hit {
  float distance, weight;
  float3 normal, albedo;
};

float SurfaceBase(float3 normal) {
  return 0.30f - 0.17f * saturate(normal.y);
}

float3 SourceAlbedo(View v, float2 uv, float z, float3 normal, float3 fallback) {
  if (!Inside(uv)) return fallback;
  int2 base = int2(uv * v.sizes.xy - 0.5f);
  float3 sum = 0.0f;
  float weight = 0.0f;
  [unroll] for (int j = 0; j < 2; ++j) {
    [unroll] for (int i = 0; i < 2; ++i) {
      int2 p = clamp(base + int2(i, j), int2(0, 0), int2(v.sizes.xy) - 1);
      if (abs(MainDepth(v, p) - z) > max(0.12f, z * 0.015f)) continue;
      float4 rt2 = v.guides.x == kNoSlot ? 0.0f : Plane(v, v.guides.x, p);
      float3 c = fallback;
      if (rt2.w > 0.0f) {
        c = GbAlbedoDecode(rt2.rgb);
      } else if (v.guides.y != kNoSlot) {
        float3 lit = max(Plane(v, v.guides.y, p).rgb, 0.0f);
        float peak = max(lit.r, max(lit.g, lit.b));
        c = SurfaceBase(normal) * lerp(1.0f, lit / max(peak, 0.0001f), 0.5f * saturate(peak * 100.0f));
      }
      sum += c;
      weight += 1.0f;
    }
  }
  return weight > 0.0f ? sum / weight : fallback;
}

bool WithinDepthThickness(float beam, float surface) {
  return beam >= surface && beam - surface <= max(0.14f, surface * 0.018f);
}

// The near steps are denser. Coarse minima only nominate a step; exact
// depth at level zero must bracket an intersection before it is accepted.
bool ScreenHit(View v, float3 origin, float3 direction, float range, out float dist, out float3 normal) {
  float previous = 0.0f;
  bool was_in_front = true;
  dist = 0.0f;
  normal = float3(0, 1, 0);
  [loop] for (uint i = 1; i <= kBeamSteps; ++i) {
    float f = float(i) / float(kBeamSteps);
    float current = max(0.04f, range * f * f);
    float3 p = origin + direction * current;
    float2 uv = Project(v, p);
    if (p.z <= 0.04f || !Inside(uv)) {
      previous = current;
      was_in_front = true;
      continue;
    }
    float2 previous_uv = Project(v, origin + direction * previous);
    float span = length((uv - previous_uv) * v.counts.xy);
    uint level = span >= 4.0f ? 2u : span >= 2.0f ? 1u : 0u;
    if (v.slots[level] == kNoSlot) level = 0u;
    float coarse = Chain(v, uv, level);
    float z = coarse <= p.z + 0.02f ? Linear(Chain(v, uv, 0)) : kSky;
    if (was_in_front && p.z >= z) {
      float lo = previous, hi = current;
      [unroll] for (uint step = 0; step < 5u; ++step) {
        float mid = 0.5f * (lo + hi);
        float3 q = origin + direction * mid;
        float2 at = Project(v, q);
        if (Inside(at) && q.z >= Linear(Chain(v, at, 0))) hi = mid;
        else lo = mid;
      }
      float3 q = origin + direction * hi;
      float2 at = Project(v, q);
      float hit_depth = Linear(Chain(v, at, 0));
      int2 pixel = clamp(int2(at * v.sizes.xy), int2(0, 0), int2(v.sizes.xy) - 1);
      float raw = Chain(v, at, 0);
      if (Inside(at) && hi >= 0.06f && hit_depth < kSky && WithinDepthThickness(q.z, hit_depth) &&
          Vehicle(v, pixel, raw) == 0u) {
        normal = WorldVector(v, GeometricNormal(v, int2(at * v.counts.xy), hit_depth));
        if (dot(normal, WorldVector(v, direction)) > 0.0f) normal = -normal;
        dist = hi;
        return true;
      }
    }
    previous = current;
    was_in_front = p.z < z;
  }
  return false;
}

#if VPL_RAY
bool RayHit(float3 origin, float3 aim, float range, inout Hit hit) {
  RtWorld world = RtWorldLoad();
  float3 p = origin - world.camera;
  float3 local_origin = p.x * world.axisX + p.y * world.axisY + p.z * world.axisZ;
  float3 local_aim = aim.x * world.axisX + aim.y * world.axisY + aim.z * world.axisZ;
  RayDesc ray;
  ray.Origin = local_origin;
  ray.Direction = local_aim;
  ray.TMin = 0.045f;
  ray.TMax = range;
  RayQuery<RAY_FLAG_NONE> query;
  query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, 0x80u, ray);
  while (query.Proceed()) {
    if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      CityHitAlpha a = CityHitAlphaOfCandidate(query.CandidateInstanceID(), query.CandidateGeometryIndex(),
          query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics(), 0.08f);
      if (!a.known || CityHitAlphaBlocks(a.alpha, 0.5f)) query.CommitNonOpaqueTriangleHit();
    }
  }
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
  CityHit surface = CityHitDecode(query.CommittedInstanceID(), query.CommittedGeometryIndex(),
                                  query.CommittedPrimitiveIndex(), query.CommittedTriangleBarycentrics());
  if (surface.valid) {
    hit.normal = dot(surface.normal, aim) > 0.0f ? -surface.normal : surface.normal;
    hit.albedo = saturate(CityHitLoadMaterial(surface.material).average);
  }
  hit.distance = query.CommittedRayT();
  hit.weight = 1.0f;
  return true;
}
#endif

float GroundDistance(float height, float down, float range) {
  return min(range, height / max(down, 0.001f));
}

float3 Energy(float3 incident, float3 albedo, float footprint) {
  return max(incident, 0.0f) * saturate(albedo) * (kBounceFraction * footprint * footprint);
}

float Falloff(float distance_sq, float footprint) {
  float size_sq = footprint * footprint;
  float window = saturate(1.0f - distance_sq / (9.0f * size_sq));
  return window * window / (distance_sq + size_sq);
}

[numthreads(16, 1, 1)]
void HitMain(uint3 thread : SV_DispatchThreadID) {
  View v = VplView();
  if (thread.x >= v.counts.z) return;
  Beam beam = VplBeam(thread.x);
  float3 origin = beam.origin_range.xyz;
  float range = beam.origin_range.w;
  float3 aim = normalize(beam.aim_ground.xyz);
  float3 origin_view = ViewPoint(v, origin);
  float3 aim_view = float3(dot(aim, v.right), dot(aim, v.up), dot(aim, v.forward));
  Hit hit;
  hit.distance = GroundDistance(beam.aim_ground.w, -aim.y, range * 0.9f);
  hit.normal = float3(0, 1, 0);
  hit.albedo = float3(0.13f, 0.13f, 0.13f);
  hit.weight = 0.55f;
  bool found = false;
#if VPL_RAY
  found = RayHit(origin, aim, range, hit);
#endif
  if (!found) {
    float distance;
    float3 normal;
    if (ScreenHit(v, origin_view, aim_view, range, distance, normal)) {
      hit.distance = distance;
      hit.normal = normal;
      hit.weight = 1.0f;
      hit.albedo = SurfaceBase(normal);
    }
  }
  float3 view_hit = origin_view + aim_view * hit.distance;
  if (view_hit.z > 0.04f) hit.albedo = SourceAlbedo(v, Project(v, view_hit), view_hit.z, hit.normal, hit.albedo);

  LlLight lamp = (LlLight)0;
  lamp.pos = origin;
  lamp.range = range;
  lamp.dir = aim;
  lamp.cos_outer = 0.786f;
  lamp.cos_inner = 0.978f;
  lamp.law = beam.meta.w & 3u;
  lamp.law_a = beam.colour_law.w;
  float3 landing = origin + aim * hit.distance;
  float incident = LlLightScale(lamp, landing, hit.normal, 1.0f);
  float footprint = lamp.law == LL_LAW_HEADLIGHT ? clamp(hit.distance * 0.18f, 0.65f, 3.5f)
                                               : (beam.meta.w & 4u) != 0u ? 1.6f : 0.95f;
  float3 flux = Energy(beam.colour_law.xyz * (incident * hit.weight), hit.albedo, footprint);
  landing += hit.normal * 0.12f;

  uint offset = beam.meta.x * 48u;
  if (beam.meta.y == 0u) {
    float4 previous = asfloat(g_hit_write.Load4(offset));
    float3 previous_normal = asfloat(g_hit_write.Load4(offset + 16)).xyz;
    float3 previous_flux = asfloat(g_hit_write.Load4(offset + 32)).xyz;
    // Bound the spatial lag at high speed, and prevent the interpolated
    // normal flipping through zero when a beam changes walls.
    float3 delta = previous.xyz - landing;
    float old_weight = 1.0f - v.lens.z;
    landing += delta * (old_weight * min(1.0f, 1.5f / max(length(delta), 0.001f)));
    footprint = lerp(footprint, previous.w, old_weight);
    flux = lerp(flux, previous_flux, old_weight);
    if (dot(hit.normal, previous_normal) > 0.0f) hit.normal = normalize(lerp(hit.normal, previous_normal, old_weight));
  }
  g_hit_write.Store4(offset, asuint(float4(landing, footprint)));
  g_hit_write.Store4(offset + 16, asuint(float4(hit.normal, 0)));
  g_hit_write.Store4(offset + 32, asuint(float4(flux, 0)));
}

bool InCabin(View v, float3 p, uint id) {
  return v.apply.z != 0u &&
         ((v.apply.y != 0u && id == v.apply.y) ||
          (v.cabin_min.w != 0.0f ? all(p >= v.cabin_min.xyz) && all(p <= v.cabin_max.xyz)
                                 : dot(p - v.eye, p - v.eye) < 2.25f));
}

float ReceiverShare(uint receiver, uint source, bool cabin) {
  if (cabin || (receiver != 0u && receiver == source)) return 0.0f;
  return receiver != 0u ? kVehicleShare : 1.0f;
}

bool Occluded(View v, float3 from, float3 to) {
  // Test the receiver-to-hit segment away from its end points. Requiring a
  // finite depth difference keeps the receiving plane from shadowing itself.
  [unroll] for (uint i = 1; i <= 4; ++i) {
    float3 p = lerp(from, to, float(i) / 5.0f);
    float2 uv = Project(v, p);
    if (p.z > 0.04f && Inside(uv)) {
      float z = Linear(Chain(v, uv, 0));
      if (z + max(0.12f, p.z * 0.012f) < p.z) return true;
    }
  }
  return false;
}

float4 IrradianceMain(float4 position : SV_Position) : SV_Target {
  View v = VplView();
  int2 pixel = min(int2(position.xy) * 2, int2(v.sizes.xy) - 1);
  float2 uv = (float2(pixel) + 0.5f) / v.sizes.xy;
  float depth = MainDepth(v, pixel);
  if (depth >= kSky) return 0.0f;
  float raw = Chain(v, uv, 0);
  uint id = Vehicle(v, pixel, raw);
  float3 p = AtDepth(v, uv, depth);
  float3 world = v.eye + WorldVector(v, p);
  if (InCabin(v, world, id)) return 0.0f;
  float3 n = 0.0f;
  bool have_normal = false;
  float3 sum = 0.0f;
  [loop] for (uint i = 0; i < v.counts.z; ++i) {
    Beam beam = VplBeam(i);
    uint offset = beam.meta.x * 48u;
    float4 landed = asfloat(g_hit_read.Load4(offset));
    float3 delta = world - landed.xyz;
    float d2 = dot(delta, delta);
    float radius2 = 9.0f * landed.w * landed.w;
    if (d2 >= radius2) continue;
    float share = ReceiverShare(id, beam.meta.z, false);
    if (id != 0u && beam.meta.z == 0u && (beam.meta.w & 8u) == 0u && dot(world - beam.origin_range.xyz, world - beam.origin_range.xyz) < 5.76f) share = 0.0f;
    if (share == 0.0f) continue;
    float3 travel = delta * rsqrt(max(d2, 1.0e-6f));
    float3 normal = asfloat(g_hit_read.Load4(offset + 16)).xyz;
    float emit = saturate(dot(normal, travel));
    if (emit <= 0.0f) continue;
    if (!have_normal) {
      n = WorldVector(v, GeometricNormal(v, int2(uv * v.counts.xy), Linear(raw)));
      have_normal = true;
    }
    float receive = saturate(dot(n, -travel));
    if (receive <= 0.0f || (d2 < radius2 * 0.25f && Occluded(v, p, ViewPoint(v, landed.xyz)))) continue;
    float3 flux = asfloat(g_hit_read.Load4(offset + 32)).xyz;
    sum += min(flux * (emit * receive * Falloff(d2, landed.w)), kNearCeiling) * share;
  }
  return float4(sum, id != 0u ? -depth : depth);
}

float4 VplComposite(float4 position) {
  View v = VplView();
  int2 pixel = int2(position.xy);
  int2 half_size = int2(v.sizes.zw);
  float2 centre = float2(pixel) * 0.5f;
  int2 base = int2(centre);
  float2 part = frac(centre);
  float4 taps[4];
  float3 total = 0.0f;
  [unroll] for (int i = 0; i < 4; ++i) {
    int2 p = min(base + int2(i & 1, i >> 1), half_size - 1);
    taps[i] = g_textures[v.apply.x].Load(int3(p, 0));
    total += taps[i].rgb;
  }
  if (!any(total > 0.0f)) return 0.0f;
  float z = MainDepth(v, pixel);
  if (z >= kSky) return 0.0f;
  float2 uv = (float2(pixel) + 0.5f) / v.sizes.xy;
  float raw = Chain(v, uv, 0);
  uint id = Vehicle(v, pixel, raw);
  float3 world = v.eye + WorldVector(v, AtDepth(v, uv, z));
  if (InCabin(v, world, id)) return 0.0f;
  float3 sum = 0.0f;
  float weights = 0.0f;
  [unroll] for (int j = 0; j < 4; ++j) {
    float4 tap = taps[j];
    if ((tap.a < 0.0f) != (id != 0u) || tap.a == 0.0f) continue;
    float w = saturate(1.0f - abs(abs(tap.a) - z) / max(0.20f, z * 0.018f));
    w *= ((j & 1) ? part.x : 1.0f - part.x) * ((j & 2) ? part.y : 1.0f - part.y);
    sum += tap.rgb * w;
    weights += w;
  }
  if (weights <= 0.0001f) return 0.0f;
  float4 rt2 = v.guides.x == kNoSlot ? 0.0f : Plane(v, v.guides.x, pixel);
  float3 albedo = (rt2.w > 0.0f || any(rt2.rgb > 0.0f)) ? GbAlbedoDecode(rt2.rgb) * saturate(rt2.w) :
    SurfaceBase(WorldVector(v, GeometricNormal(v, int2(uv * v.counts.xy), Linear(raw))));
  float ao = 1.0f;
  if (v.guides.z != kNoSlot) {
    float a = g_textures[v.guides.z].Load(int3(int2(uv * v.counts.xy), 0)).x;
    if (a < -0.5f) a += 2.0f;
    ao = lerp(1.0f, saturate(a), saturate(v.lens.w));
  }
  return float4(albedo * (ao * v.exposure) * (sum / weights), 0.0f);
}

float4 CompositeMain(float4 position : SV_Position) : SV_Target {
  const float4 add = VplComposite(position);
  if (!any(add.rgb > 0.0f)) discard;
  return add;
}
