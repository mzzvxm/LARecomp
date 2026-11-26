// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections (CITY-RT package R4; ssr_rt.h, ssr_rt_rules.h, hlsl/ssr_rt_common.hlsli), pass 2: the candidates' rays into the city.
//
// One thread an entry of the list pass 1 made (hlsl/ssr_rt_select_cs.hlsl: the pixels of the SSR image whose reflection the picture could not speak for, listed tile by tile). For its pixel it
//   1. sets the receiver up exactly as the trace did (hlsl/ssr_trace_ps.hlsl: the Hi-Z's depth, the block's own pixel for the class, the smoothness and the normal, the GGX alpha),
//   2. regenerates the SAME ray (ssr_common.hlsli SsrSampleRay: the trace's ray for this pixel and frame, nothing was stored; at two rays a pixel every ray),
//   3. traces it into the top level (the city's structures, mask 0x80, and the cars' parts: a hit on a car is not this pass's to shade) and shades the closest city hit with the city hit library's
//      ResolveHit (hlsl/city_hit_shade.hlsli: "the picture where the hit is seen, the model where it is not": the world frame's light, the sun's ray, the glow of lamps and neon, the class's gain),
//   4. adds what the picture could not give to the trace image: trace + (1 - confidence) * weight * mean over the rays of (the hit's radiance x its distance fade - the road view the trace takes out of a
//      road, x that fade), the trace's alpha (the hit distance the accumulation reprojects the virtual image by, and Ray Reconstruction takes as its hit distance guide) becoming the ray traced hit's
//      where the picture's share is under a half. A ray that finds nothing within the reach, a hit on a car, a hit with no data in the pool and a frame without the world's light add nothing: the pixel stays what
//      the trace and the game's own reflection (the road view, the environment map under it) make of it.
//
// The trace image is a storage image here (RGBA16F, typed loads and stores: a D3D12 device with TypedUAVLoadAdditionalFormats, which every ray tracing card has; Vulkan: the image's format is given to SPIR-V).
// The temporal and the apply passes read it afterwards unchanged. The units are the trace's own (the colour target's: the library's shaded colours times the frame's units), capped at the trace's cap.
//
// STATISTICS (a stats frame, SSR_RT_FLAG_STATS: every 20th, the world frame's): the library counts its calibration samples into u1 (the host closes the class gains' window from them) and this pass the census
// words (rays, city hits, car hits, misses, hits without data, hits the picture spoke for, hits shaded, pixels it filled).
//
// Layout (ssr_rt.cpp): sets 0 to 3 the bindless texture set bound four times (set 0 Texture2D, set 1 Texture2DMS: SSR_MS 1 reads the planes as the trace does), set 4 the samplers, the push block at
// (b0, space4); set 5 this pass's own (u0 the trace image, u1 the control and statistics words, u2 the list); set 6 the car scene's ray tracing set (CAR_RT_SPACE 6: the top level t0, the hit data t6..t9,
// the world rows t10 and the lamps t11). Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query). Compilations: SSR_MS 0 / 1 (ssr_rt_trace_cs, ssr_rt_trace_ms_cs).

#define CAR_RT_SPACE 6
#define RT_WORLD_SPACE 6
#define CITY_HIT_SPACE 6
#include "car_rt_common.hlsli"
#include "ssr_pass.hlsli"
#include "ssr_rt_common.hlsli"
#include "rt_world_frame.hlsli"

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_trace : register(u0, space5);
RWByteAddressBuffer g_ctl : register(u1, space5);
RWByteAddressBuffer g_list : register(u2, space5);
#if SSR_REFLHIT
#include "reflhit.hlsli"
#ifdef __spirv__
[[vk::image_format("r32ui")]]
#endif
RWTexture2D<uint> g_reflhit : register(u3, space5);
#define CITY_HIT_RESOLVE_MATERIAL_CLASS 1
#endif

// The library's hooks (hlsl/city_hit_shade.hlsli lists them): the top level, the picture (lin_depth level 0 and colour_post's mip chain, the trace's own images), the class gains, the calibration's counters.
// No lamp list (R7's), no haze, no probe field.
#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_SCREEN_DEPTH(texel) g_textures[SsrLo(SsrW(SSR_W_LIN0))].Load(int3(texel, 0)).x
#define CITY_HIT_SCREEN_COLOUR(uv, mip) SsrColour(uv, max((mip) - (SsrFlag(SSR_FLAG_COL_HALF) ? 1.0f : 0.0f), 0.0f))
#define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)
#define CITY_HIT_STAT_ADD(word, value) g_ctl.InterlockedAdd((SSR_RT_STAT_CAL + (word)) * 4u, (value))
#include "city_hit_shade.hlsli"

// The instance mask of the rays: the city's bit and every part of a car.
static const uint kSsrRtMask = CAR_RT_MASK_ALL | 0x80u;
// The picture's trust at a hit (city_refl_rules.h's numbers: the depth tolerance in metres and its share of the depth, the edge band in ndc, the least facing).
static const float4 kSsrRtScreenTolerance = float4(0.05f, 0.01f, 0.04f, 0.05f);
static const float kSsrRtSunTan = 0.01388f;   // tan of three times the sun's disc, as the city's shadow pass and the car reflections
static const float kSsrRtAoMix = 0.5f;

void SsrRtCount(bool stats, uint word) {
  if (stats) {
    g_ctl.InterlockedAdd((SSR_RT_STAT_CENSUS + word) * 4u, 1u);
  }
}

// The receiver of an SSR pixel, set up as ssr_trace_ps.hlsl sets it up (change one, change the other: the ray is the trace's own).
struct SsrRtReceiver {
  bool ok;
  float3 pc;
  float3 n;
  float3 to_camera;
  float alpha;
  uint cls;
  int2 texel;   // the pixel of the 2 x 2 block the class and the normal were read at
};
SsrRtReceiver SsrRtReceiverAt(int2 pix) {
  SsrRtReceiver r;
  r.ok = false;
  r.pc = float3(0.0f, 0.0f, 0.0f);
  r.n = float3(0.0f, 1.0f, 0.0f);
  r.to_camera = float3(0.0f, 0.0f, -1.0f);
  r.alpha = 1.0f;
  r.cls = uint(GB_CLASS_NONE);
  r.texel = int2(0, 0);
  const int2 render = SsrRender();
  const int2 ssr = SsrSize();
  const uint hz = SsrHz();
  const float z_block = SsrHizAt(hz, pix.x, pix.y);
  if (z_block >= kSsrSky * 0.5f) {
    return r;
  }
  const int2 last = render - 1;
  const int2 base = pix * 2;
  const int2 t0 = min(base, last);
  const int2 t1 = min(base + int2(1, 0), last);
  const int2 t2 = min(base + int2(0, 1), last);
  const int2 t3 = min(base + int2(1, 1), last);
  const uint pick = SsrPickInBlock(SsrLinearAt(t0), SsrLinearAt(t1), SsrLinearAt(t2), SsrLinearAt(t3), z_block);
  const int2 texel = pick == 0u ? t0 : (pick == 1u ? t1 : (pick == 2u ? t2 : t3));
  const SsrSurface surface = SsrSurfaceAt(texel, false);
  const float gain = SsrClassGain(surface.cls, surface.flags);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  if (!(gain > 0.0f) || smoothness < SsrF(SSR_W_MIN_SMOOTH)) {
    return r;
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), z_block, tan_x, tan_y);
  const float4 g_normal = SsrViewNormalAt(texel, pc);
  float3 n = g_normal.xyz;
  if (g_normal.w < 0.5f) {
    n = SsrDepthNormal(hz, pix.x, pix.y, ssr.x, ssr.y, z_block, pc, tan_x, tan_y);
  }
  const float3 to_camera = pc * (-1.0f / length(pc));
  const float facing = dot(n, to_camera);
  if (facing < 0.05f) {
    n = normalize(n + to_camera * (0.05f - facing));
  }
  r.ok = true;
  r.pc = pc;
  r.n = n;
  r.to_camera = to_camera;
  r.alpha = SsrAlphaOfSmooth(smoothness);
  r.cls = surface.cls;
  r.texel = texel;
  return r;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint conf_word = SsrW(SSR_W_RT_CONF);
  const uint rt_frame = SsrW(SSR_W_RT_FRAME);
  const uint capacity = SsrW(SSR_W_RT_CAP);
  const uint listed = min(g_ctl.Load((SSR_RT_CTL_LISTED + rt_frame % 3u) * 4u), capacity);
  if (id.x >= listed) {
    return;
  }
  const uint entry = g_list.Load(id.x * 4u);
  const int2 pix = int2(int(entry & 0xFFFFu), int(entry >> 16u));
  const int2 ssr = SsrSize();
  if (pix.x >= ssr.x || pix.y >= ssr.y) {
    return;
  }
  const RtWorld w = RtWorldLoad();
  if ((w.flags & RT_WORLD_VALID) == 0u) {
    return;   // the frame has no light yet: the library would answer "no data" for every hit
  }
  const SsrRtReceiver r = SsrRtReceiverAt(pix);
  if (!r.ok) {
    return;
  }
  const uint flags = conf_word >> SSR_RT_FLAGS_SHIFT;
  const bool stats = (flags & SSR_RT_FLAG_STATS) != 0u;
  const float confidence = g_textures[SsrLo(conf_word)].Load(int3(pix, 0)).y;
  const float4 current = g_trace[pix];
  if (current.w == 0.0f) {
    return;   // the trace wrote nothing here (a stale entry)
  }

  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const int2 h0 = SsrHiz0Size();
  const float reach = SsrF(SSR_W_RT_REACH);
  const float cap = SsrF(SSR_W_CAP);
  const uint rays = ((conf_word >> SSR_RT_RAYS_SHIFT) & 3u) + 1u;
  const uint frame = SsrW(SSR_W_FRAME);
  const float receiver_distance = length(r.pc);
  const float pixel_angle = 2.0f * tan_y / float(max(ssr.y, 1));
  const float cone_tan = r.alpha * kSsrConeScale;
  const float lift = SsrRtLift(r.pc.z, 2.0f * tan_y / float(max(h0.y, 1)));
  const float3 origin = r.pc + r.n * lift;
  const float weight = SsrRtWeight(SsrRtKeep(capacity, g_ctl.Load((SSR_RT_CTL_TOTAL + (rt_frame + 2u) % 3u) * 4u)));
  // Ground surfaces already have SSR and the game's planar view. Only replace their reflection where the detailed hit agrees with the rasterized view.
  const bool ground = SsrRtGroupOf(r.cls) == kSsrRtGroupGround;
  if (ground && (flags & SSR_RT_FLAG_SCREEN) == 0u) {
    return;
  }

  // the road view the trace takes out of a road where it finds a reflection (the share the trace was given): taken out here for the ray traced one in the same way
  float3 fallback = float3(0.0f, 0.0f, 0.0f);
  const float share = SsrF(SSR_W_FALLBACK);
  const uint road_view = SsrW(SSR_W_ROAD_VIEW);
  if (share > 0.0f && SsrHasSlot(SsrRoadViewSlot(road_view)) && (r.cls == uint(GB_CLASS_ROAD) || r.cls == uint(GB_CLASS_WET_ROAD))) {
    const float2 screen_uv = (float2(r.texel) + 0.5f) / float2(SsrRender());
    fallback = min(NgFinite3(SsrRoadViewAt(road_view, screen_uv)), float3(cap, cap, cap)) * share;   // (nan_guard.hlsli: a NaN or an infinity is none)
  }

  CityHitParams p;
  p.mode = CITY_HIT_MODE_REFLECT;
  p.flags = CITY_HIT_P_SUN_RAY | CITY_HIT_P_EMISSIVE | ((flags & SSR_RT_FLAG_SCREEN) != 0u ? CITY_HIT_P_SCREEN : 0u) | (stats ? CITY_HIT_P_CALIBRATE : 0u);
  p.units = w.units;
  p.sunReach = 0.0f;
  p.sunTan = kSsrRtSunTan;
  p.lampFactor = w.lampFactor;
  p.wet = w.wet;
  p.aoMix = kSsrRtAoMix;
  p.lodBias = 0.0f;
  p.bounce = 0.0f;
  p.lampFirst = 0u;
  p.lampCount = 0u;
  CityHitScreen s;
  s.lens = float4(tan_x, tan_y, float(h0.x), float(h0.y));
  s.tolerance = kSsrRtScreenTolerance;
  s.cap = 0.0f;
  const RtHitFrame f = RtHitLoadFrame();

  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float nearest = kSsrMissLength;
  bool any_hit = false;
#if SSR_REFLHIT
  uint rt_id = kReflHitNone;
  uint rejected = kReflHitNone;
  float rt_fades = 0.0f;
  float best_rt_fade = -1.0f;
#endif
  [loop] for (uint k = 0u; k < rays; ++k) {
    const float3 dir = SsrSampleRay(r.n, r.to_camera, r.alpha, pix.x, pix.y, frame, k, rays);
    SsrRtCount(stats, SSR_RT_CENSUS_RAYS);
    const CityHitTraceResult hit = CityHitTrace(origin, dir, 0.0f, reach, kSsrRtMask, pixel_angle * receiver_distance, cone_tan, 0.5f);
    if (hit.hit == 0u) {
      SsrRtCount(stats, SSR_RT_CENSUS_MISS);
#if SSR_REFLHIT
      if (rejected == kReflHitNone) rejected = kReflHitRtMiss;
#endif
      continue;
    }
    if (!CityHitIsCity(hit.inst.instanceId)) {
      SsrRtCount(stats, SSR_RT_CENSUS_CAR);
#if SSR_REFLHIT
      rejected = kReflHitRtCar;
#endif
      continue;   // a car's part: its own passes shade it
    }
    SsrRtCount(stats, SSR_RT_CENSUS_CITY);
#if SSR_REFLHIT
    if (rejected != kReflHitRtCar) rejected = kReflHitRtUnresolved;
#endif
    p.noise = float3(SsrNoise1(pix.x, pix.y, frame, k + 4u, rays), SsrNoise2(pix.x, pix.y, frame, k + 4u, rays), SsrNoise1(pix.y, pix.x, frame, k + 6u, rays));
    CityHitRay ray;
    ray.origin = origin;
    ray.direction = dir;
    ray.t = hit.t;
    ray.footprint = SsrRtFootprint(pixel_angle, receiver_distance, hit.t, cone_tan);
    const CityHitResolved res = CityHitResolve(p, f, s, ray, hit.inst, ground);
    const float coverage = SsrRtHitCoverage(ground, res.screen);
    if (res.source == CITY_HIT_SOURCE_NONE || coverage <= 0.0f) {
      SsrRtCount(stats, SSR_RT_CENSUS_NO_DATA);
      continue;   // keep the SSR/planar result for this ray, including its hit distance
    }
    SsrRtCount(stats, res.screen >= 0.5f ? SSR_RT_CENSUS_SCREEN : SSR_RT_CENSUS_SHADED);
    const float fade = coverage * SsrRtReachFade(hit.t, reach);
    sum += (min(res.colour, float3(cap, cap, cap)) - fallback) * fade;
    if (!ground || fade >= 0.5f) {
      nearest = min(nearest, hit.t);
    }
    any_hit = true;
#if SSR_REFLHIT
    if (fade > best_rt_fade) {
      rt_id = ReflHitCity(res.cls);
      best_rt_fade = fade;
    }
    rt_fades += fade;
#endif
  }
#if SSR_REFLHIT
  const uint packed = g_reflhit[pix];
  const uint screen_id = ReflHitScreenId(packed);
  const uint native = ReflHitFallbackId(packed);
  const float rt_weight = (1.0f - confidence) * weight * rt_fades / float(rays);
  const uint visible = ReflHitDominant(screen_id, confidence, rt_id, rt_weight, native);
  g_reflhit[pix] = ReflHitPack(visible, any_hit ? rt_id : rejected, screen_id, native);
#endif
  if (!any_hit) {
    return;
  }
  float3 rgb = NgFinite3(SsrRtBlend(current.xyz, confidence, sum / float(rays), weight));   // (a NaN or an infinity is none: it must not reach the cap below nor the history)
  const float peak = max(max(abs(rgb.x), abs(rgb.y)), abs(rgb.z));
  if (peak > cap) {
    rgb *= cap / peak;
  }
  float alpha = current.w;
  if (confidence < 0.5f && nearest < kSsrMissLength) {
    alpha = SsrPackHit(clamp(nearest, 0.02f, 60000.0f), false, 0u);   // the reflection is the ray traced one's: its distance is the virtual image's
  }
  g_trace[pix] = float4(rgb, alpha);
  SsrRtCount(stats, SSR_RT_CENSUS_FILLED);
}
