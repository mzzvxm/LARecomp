// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections (CITY-RT package R4; ssr_rt.h, ssr_rt_rules.h, hlsl/ssr_rt_common.hlsli), pass 2: the candidates' rays into the city.
//
// One thread per pixel the select pass listed. For its pixel it
//   1. sets the receiver up exactly as the trace did (hlsl/ssr_trace_ps.hlsl: the Hi-Z's depth, the block's own pixel for the class, the smoothness and the normal, the GGX alpha),
//   2. regenerates the SAME ray (ssr_common.hlsli SsrSampleRay: the trace's ray for this pixel and frame, nothing was stored; at two rays a pixel every ray),
//   3. traces it into the top level (the city's structures, mask 0x80, and the cars' parts) and shades the closest city hit with the city hit library's
//      ResolveHit (hlsl/city_hit_shade.hlsli: "the picture where the hit is seen, the model where it is not": the world frame's light, the sun's ray, the glow of lamps and neon, the class's gain),
//      or shades a car's material on road receivers when car hits are enabled,
//   4. adds what the picture could not give to the trace image: trace + (1 - confidence) * weight * mean over the rays of (the hit's radiance x its distance fade - the road view the trace takes out of a
//      road, x that fade), the trace's alpha (the hit distance the accumulation reprojects the virtual image by, and Ray Reconstruction takes as its hit distance guide) becoming the ray traced hit's
//      where the picture's share is under a half. Optional routes replace the planar road view on upward misses or recover a rejected proxy from raster colour.
//
// The trace image is a storage image here (RGBA16F, typed loads and stores: a D3D12 device with TypedUAVLoadAdditionalFormats, which every ray tracing card has; Vulkan: the image's format is given to SPIR-V).
// The temporal and the apply passes read it afterwards unchanged. The units are the trace's own (the colour target's: the library's shaded colours times the frame's units), capped at the trace's cap.
//
// STATISTICS (a stats frame, SSR_RT_FLAG_STATS: every 20th, the world frame's): the library counts its calibration samples into u1 (the host closes the class gains' window from them) and this pass the census
// words (rays, city hits, car hits, misses, hits without data, hits the picture spoke for, hits shaded, pixels it filled).
//
// Layout (ssr_rt.cpp): sets 0 to 3 the bindless texture set bound four times (set 0 Texture2D, set 1 Texture2DMS: SSR_MS 1 reads the planes as the trace does), set 4 the samplers, the push block at
// (b0, space4); set 5 this pass's own (u0 the trace image, u1 the control and statistics words, u2 the list); set 6 the car scene's ray tracing set (CAR_RT_SPACE 6: the top level t0, the hit data t6..t9,
// the world rows t10 and the lamps t11); set 7 the shared handoff frame. Shader Model 6.5 (DXIL), SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query). Compilations: SSR_MS 0 / 1 (ssr_rt_trace_cs, ssr_rt_trace_ms_cs).

#define CAR_RT_SPACE 6
#define RT_WORLD_SPACE 6
#define CITY_HIT_SPACE 6
#include "car_rt_common.hlsli"
#include "ssr_pass.hlsli"
#define SSR_HANDOFF_RT 1
#include "ssr_handoff.hlsli"
#include "ssr_classify.hlsli"
#include "rt_ray_bounds.hlsli"
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
RWTexture2D<uint> g_reflhit : register(u4, space5);
#define CITY_HIT_RESOLVE_MATERIAL_CLASS 1
#endif

// The library's hooks (hlsl/city_hit_shade.hlsli lists them): the top level, the picture (lin_depth level 0 and colour_post's mip chain, the trace's own images), the class gains, the calibration's counters.
// Car hits use a bounded walk of the world lamp grid.
#define RT_HIT_TLAS gCarRtTlas
#define CITY_HIT_SCREEN_DEPTH(texel) g_textures[SsrLo(SsrW(SSR_W_LIN0))].Load(int3(texel, 0)).x
#define CITY_HIT_SCREEN_COLOUR(uv, mip) SsrColour(uv, max((mip) - (SsrFlag(SSR_FLAG_COL_HALF) ? 1.0f : 0.0f), 0.0f))
#define CITY_HIT_GAIN(cls) RtWorldClassGain(cls)
#define CITY_HIT_STAT_ADD(word, value) g_ctl.InterlockedAdd((SSR_RT_STAT_CAL + (word)) * 4u, (value))
#define CITY_HIT_LAMP_GRID 1
#include "city_hit_shade.hlsli"
#include "car_rt_hit_material.hlsli"

// The instance mask of the rays: the city's bit and every part of a car.
static const uint kSsrRtCityMask = 0x80u;
static const uint kSsrRtMask = CAR_RT_MASK_ALL | kSsrRtCityMask;
// The picture's trust at a hit (city_refl_rules.h's numbers: the depth tolerance in metres and its share of the depth, the edge band in ndc, the least facing).
static const float4 kSsrRtScreenTolerance = float4(0.05f, 0.01f, 0.04f, 0.05f);
static const float kSsrRtSunTan = 0.01388f;   // tan of three times the sun's disc, as the city's shadow pass and the car reflections
static const float kSsrRtAoMix = 0.5f;

void SsrRtCount(bool stats, uint word) {
  if (stats) {
    g_ctl.InterlockedAdd((SSR_RT_STAT_CENSUS + word) * 4u, 1u);
  }
}

// CHECK: compare recovered proxy silhouettes against the parked and driving captures. A proxy needs a geometric normal,
// a close raster depth match, and sufficient facing and distance from the screen edge.
float4 SsrRtScreenProxy(CityHitScreen s, CityHitRay ray, CityHitInstance inst, CityHit decoded) {
  if (decoded.valid == 0u || !(decoded.worldArea > 0.0f)) {
    return 0.0f;
  }
  s.tolerance.xy = float2(0.03f, 0.002f);
  const float3 hit_point = ray.origin + ray.direction * ray.t;
  const CityHitScreenAt at = CityHitScreenWeight(s, hit_point, CityHitViewNormal(decoded, inst, ray.direction));
  const float coverage = SsrRtProxyWeight(at.weight);
  if (!(coverage > 0.0f)) {
    return 0.0f;
  }
  const float mip = log2(max(ray.footprint / max(at.pixel, 1.0e-6f), 1.0f));
  return float4(CITY_HIT_SCREEN_COLOUR(at.uv, mip), coverage);
}

// CHECK: compare the map's exposure and colour on day, wet night and tunnel captures. Only a known planar
// term can be replaced, and the hit shader leaves the SSR distance guide alone on an environment miss.
float3 SsrRtEnvironmentTerm(RtHitFrame f, float3 dir, float alpha, float units, float visibility, float3 fallback, float cap) {
  const float lod = clamp(0.5f + 3.0f * alpha, 0.5f, 3.0f);
  const float3 colour = min(NgFinite3(RtHitEnvAt(f, dir, lod) * units), float3(cap, cap, cap));
  return colour * visibility - fallback;
}

float3 SsrRtCarLamps(CityHitParams p, RtWorld w, float3 position, float3 normal) {
  float3 sum = 0.0f;
  if (p.lampFactor > 0.0f) {
    RtLampCursor cursor = RtLampBegin(w, position, kSsrRtCarLampRadius);
    [loop] for (uint i = 0u; i < kSsrRtCarLampVisits && cursor.done == 0u; ++i) {
      const CityHitLampEval lamp = CityHitLampAtIndex(p, w, cursor.index, position, normal);
      sum += lamp.radiance * SsrRtCarLampWeight(lamp.distance);
      cursor = RtLampStep(w, cursor);
    }
  }
  return sum * (kCityHitLampSumCap / max(CityHitLuma(sum), kCityHitLampSumCap));
}

// CHECK: compare the material, glass, lamp coverage and history in moving 4K wet-road captures on both APIs.
// CHECK: measure the enabled Ultra fallback and the complete reflection pass on both APIs.
// Use the world frame's cascades and local lamp list so a car hit needs no secondary rays.
bool SsrRtShadeCar(CityHitParams p, RtWorld w, RtHitFrame f, CityHitTraceResult hit, float3 position,
                   float3 direction, float footprint, float cone_tan, out float3 colour) {
  colour = 0.0f;
  if ((f.flags & RT_HIT_FRAME_SHADE) == 0u) {
    return false;
  }
  const CarMat m = CarMatLoad(hit.instanceIndex);
  if (m.cls == CAR_MAT_CLASS_NONE || m.cls > CAR_MAT_CLASS_OTHER) {
    return false;
  }
  const float3x4 transform = float3x4(float4(hit.inst.row0, 0.0f), float4(hit.inst.row1, 0.0f), float4(hit.inst.row2, 0.0f));
  const float3 normal = CarRtHitNormal(hit.instanceIndex, hit.inst.primitiveIndex, hit.inst.barycentrics, transform, direction);
  const float nv = saturate(-dot(normal, direction));
  const float3 albedo = CarMatAlbedo(m, hit.instanceIndex, hit.inst.primitiveIndex, hit.inst.barycentrics, footprint, nv);

  const bool sun_known = (f.flags & RT_HIT_FRAME_SUN) != 0u;
  const float3 to_sun = sun_known ? f.toSun : f.axisY;
  const float sun = saturate(dot(normal, to_sun)) * (sun_known ? 1.0f - f.night : 1.0f);
  float visible = 1.0f;
  [branch] if (sun > 0.0f && (f.flags & RT_HIT_FRAME_CASCADE) != 0u) {
    visible = RtHitCascadeVisible(f, position, normal);
  }
  const float3 light = RtHitAmbientLight(f, RtHitHemisphere(f, normal)) + f.sun * (sun * visible) + SsrRtCarLamps(p, w, position, normal);
  float3 result = albedo * light * kClassDiffuseGain[m.cls];

  const float3 reflected = reflect(direction, normal);
  float3 env = f.ambient * (0.6f + 0.4f * dot(reflected, f.axisY));
  [branch] if ((f.flags & RT_HIT_FRAME_ENV) != 0u) {
    const float lod = kClassEnvLod[m.cls] + log2(1.0f + kRtHitConeLodScale * cone_tan);
    env = RtHitEnvAt(f, reflected, lod);
  }
  const float3 tint = m.cls == CAR_MAT_CLASS_CHROME ? albedo : float3(1.0f, 1.0f, 1.0f);
  result += env * (CarMatFresnel(m, nv) * kClassSpecGain[m.cls] * f.specGain) * tint;
  if ((m.flags & CAR_MAT_FLAG_EMISSIVE) != 0u) {
    result += albedo * (m.emissive * kRtHitEmissiveGain * CarMatGlowScale(m, w.lampFactor));
  }
  colour = NgHeld3(result * p.units, kCityHitMaxColour);
  return true;
}

// The receiver of an SSR pixel, set up as ssr_trace_ps.hlsl sets it up (change one, change the other: the ray is the trace's own).
struct SsrRtReceiver {
  bool ok;
  float3 pc;
  float3 n;
  float3 to_camera;
  float alpha;
  float smoothness;
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
  r.smoothness = 0.0f;
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
  const float3 to_camera = pc * (-1.0f / length(pc));
  r.ok = true;
  r.pc = pc;
  r.n = SsrReceiverNormal(pix, texel, surface.cls, pc, to_camera);
  r.to_camera = to_camera;
  r.alpha = SsrAlpha(smoothness, SsrPbr());
  r.smoothness = smoothness;
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
  const float2 conf = g_textures[SsrLo(conf_word)].Load(int3(pix, 0)).xy;
  const float confidence = conf.y;
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
  const bool single_ray = (flags & SSR_RT_FLAG_SINGLE_RAY) != 0u && SsrRays() == 2u;
  const uint frame = SsrW(SSR_W_FRAME);
  const bool stable = SsrStableTrace();
  const bool handoff = SsrHandoffEnabled();
  const bool classified = g_handoff.Load(140u) != 0u;
  const uint pattern_rays = (stable || single_ray || handoff || classified) ? SsrRays() : rays;
  const float receiver_distance = length(r.pc);
  const float pixel_angle = 2.0f * tan_y / float(max(ssr.y, 1));
  const float cone_tan = r.alpha * kSsrConeScale;
  const float lift = SsrRtLift(r.pc.z, 2.0f * tan_y / float(max(h0.y, 1)));
  const float3 origin = r.pc + r.n * lift;
  float keep = SsrRtKeep(capacity, g_ctl.Load((SSR_RT_CTL_TOTAL + (rt_frame + 2u) % 3u) * 4u));
  if (classified) {
    const float4 history = SsrTileHistory(pix / int(kSsrTileSize));
    const uint mode = SsrTileClass(uint(history.w));
    if (!SsrTileKeepRt(mode, SsrTileAge(uint(history.w)), history.z, confidence, false)) {
      keep *= SsrTileRtProbeShare(mode);
    }
  }
  // Keep the same fourfold weight bound as the legacy cap.
  float weight = SsrRtWeight(keep);
  if (stable || handoff) {
    weight *= SsrRtHistoryEdge(confidence, SsrF(SSR_W_RT_THRESHOLD));
  }
  // Ground surfaces already have SSR and the game's planar view. Only replace their reflection where the detailed hit agrees with the rasterized view.
  const bool ground = SsrRtGroupOf(r.cls) == kSsrRtGroupGround;
  if (ground && (flags & SSR_RT_FLAG_SCREEN) == 0u) {
    return;
  }

  // Replace only the fallback radiance represented by the uncovered share.
  const SsrFallback lower = SsrSurfaceFallback(r.cls, r.texel, r.pc, r.n, r.to_camera, r.smoothness, handoff);
  const float3 fallback = lower.planar + lower.env;
  const bool has_planar = lower.has_planar;

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
  p.noise = 0.0f;
  p.lampFirst = 0u;
  p.lampCount = 0u;
  CityHitScreen s;
  s.lens = float4(tan_x, tan_y, float(h0.x), float(h0.y));
  s.tolerance = kSsrRtScreenTolerance;
  s.cap = 0.0f;
  const RtHitFrame f = RtHitLoadFrame();
  const bool sky_miss = (flags & SSR_RT_FLAG_SKY_MISS) != 0u && has_planar && (f.flags & RT_HIT_FRAME_ENV) != 0u;
  const bool shade_cars = SsrRtCarReceiver(flags, r.cls);

  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float nearest = kSsrMissLength;
  float hit_coverage = 0.0f;
  bool filled = false;
#if SSR_REFLHIT
  uint rt_id = kReflHitNone;
  uint rejected = kReflHitNone;
  float rt_fades = 0.0f;
  float best_rt_fade = -1.0f;
#endif
  [loop] for (uint sample_index = 0u; sample_index < rays; ++sample_index) {
    const float ray_share = handoff && rays == 2u ? SsrRtRayShare(conf.x, sample_index) : 1.0f / float(rays);
    if (ray_share <= 0.0f) {
      continue;
    }
    // Use a separate temporally stepped blue-noise sample for ray selection. Sampling in proportion to the
    // missing confidence makes the existing mean-confidence blend correct in expectation (up to quantization).
    // CHECK: compare residual grain and edge stability with two-ray RT, including Ray Reconstruction.
    const uint k = (single_ray || (handoff && rays == 1u && SsrRays() == 2u)) ? SsrRtPickRay(conf.x, BlueNoiseFrame(pix.x + 17, pix.y + 23, 1u, frame, 2654435769u)) : sample_index;
    const float3 dir = SsrSampleRay(r.n, r.to_camera, r.alpha, pix.x, pix.y, frame, k, pattern_rays);
    // CHECK: compare retained cars and off-screen hits with the untrimmed path on both APIs.
    const float trace_reach = g_handoff.Load(184u) != 0u
        ? RtClippedLength(asfloat(g_handoff.Load3(172u)), asfloat(g_handoff.Load3(188u)), origin, dir, reach) : reach;
    CityHitTraceResult hit = CityHitTraceNone();
    if (trace_reach >= 0.0f) {
      SsrRtCount(stats, SSR_RT_CENSUS_RAYS);
      hit = CityHitTrace(origin, dir, 0.0f, trace_reach, kSsrRtMask, pixel_angle * receiver_distance, cone_tan, 0.5f);
    }
    const float up = dot(dir, f.axisY);
    if (hit.hit == 0u) {
      SsrRtCount(stats && trace_reach >= 0.0f, SSR_RT_CENSUS_MISS);
#if SSR_REFLHIT
      if (rejected == kReflHitNone) rejected = kReflHitRtMiss;
#endif
      // CHECK: collect the probe census separately from timing. No resident hit at twice the reach is
      // compatible with both sky and missing geometry. Upward is a direction, not a geometry classification.
      if ((flags & SSR_RT_FLAG_PROBE_MISSES) != 0u &&
          BlueNoiseFrame(pix.x + 7, pix.y + 19, 1u, rt_frame * 2u + k, kBlueNoiseR2StepB) < 1.0f / 64.0f) {
        const CityHitTraceResult distant = CityHitTrace(origin, dir, reach, 2.0f * reach, kSsrRtCityMask, pixel_angle * receiver_distance, cone_tan, 0.5f);
        SsrRtCount(stats, SSR_RT_CENSUS_MISS_PROBED);
        SsrRtCount(distant.hit != 0u, SSR_RT_CENSUS_MISS_BEYOND);
        SsrRtCount(distant.hit == 0u && up > 0.1f, SSR_RT_CENSUS_MISS_OPEN_UP);
      }
      const float sky_weight = sky_miss ? SsrRtSkyWeight(up, 0.0f) : 0.0f;
      if (sky_weight > 0.0f) {
        sum += SsrRtEnvironmentTerm(f, dir, r.alpha, w.units, lower.visibility, fallback, cap) * (sky_weight * ray_share);
        filled = true;
#if SSR_REFLHIT
        rt_fades += sky_weight * ray_share;
        if (sky_weight > best_rt_fade) {
          rt_id = kReflHitEnvironment;
          best_rt_fade = sky_weight;
        }
#endif
        SsrRtCount(stats, SSR_RT_CENSUS_ENV_MISS);
      }
      continue;
    }
    if (!CityHitIsCity(hit.inst.instanceId)) {
      SsrRtCount(stats, SSR_RT_CENSUS_CAR);
#if SSR_REFLHIT
      rejected = kReflHitRtCar;   // a car the pass does not shade (or shades to nothing) stays the rejected query
#endif
      [branch] if (shade_cars) {
        const float fade = SsrRtReachFade(hit.t, reach);
        const float footprint = SsrRtFootprint(pixel_angle, receiver_distance, hit.t, cone_tan);
        float3 colour;
        if (fade > 0.0f && SsrRtShadeCar(p, w, f, hit, origin + dir * hit.t, dir, footprint, cone_tan, colour)) {
          sum += (min(colour, float3(cap, cap, cap)) - fallback) * (fade * ray_share);
          const float sky_weight = sky_miss ? SsrRtSkyWeight(up, fade) : 0.0f;
          if (sky_weight > 0.0f) {
            sum += SsrRtEnvironmentTerm(f, dir, r.alpha, w.units, lower.visibility, fallback, cap) * (sky_weight * ray_share);
          }
          nearest = min(nearest, hit.t);
          hit_coverage += fade * ray_share;
          filled = true;
#if SSR_REFLHIT
          rt_fades += (fade + sky_weight) * ray_share;
          if (fade > best_rt_fade) {
            rt_id = kReflHitRtCarShaded;
            best_rt_fade = fade;
          }
#endif
          SsrRtCount(stats, SSR_RT_CENSUS_CAR_SHADED);
        }
      }
      continue;
    }
    SsrRtCount(stats, SSR_RT_CENSUS_CITY);
#if SSR_REFLHIT
    if (rejected != kReflHitRtCar) rejected = kReflHitRtUnresolved;
#endif
    p.noise = float3(SsrNoise1(pix.x, pix.y, frame, k + 4u, pattern_rays), SsrNoise2(pix.x, pix.y, frame, k + 4u, pattern_rays), SsrNoise1(pix.y, pix.x, frame, k + 6u, pattern_rays));
    CityHitRay ray;
    ray.origin = origin;
    ray.direction = dir;
    ray.t = hit.t;
    ray.footprint = SsrRtFootprint(pixel_angle, receiver_distance, hit.t, cone_tan);
    const CityHitResolved res = CityHitResolve(p, f, s, ray, hit.inst, ground);
    const float coverage = SsrRtHitCoverage(ground, res.screen);
    if (res.source == CITY_HIT_SOURCE_NONE || coverage <= 0.0f) {
      if (res.source == CITY_HIT_SOURCE_NONE) {
        const bool lod = (hit.inst.instanceId & 1u) != 0u;
        const bool try_proxy = ground && (flags & SSR_RT_FLAG_SCREEN_PROXY) != 0u && (flags & SSR_RT_FLAG_SCREEN) != 0u;
        if (lod && ground && !try_proxy) {
          SsrRtCount(stats, SSR_RT_CENSUS_NO_DATA_LOD);
        } else if (stats || try_proxy) {
          const CityHit decoded = CityHitDecode(hit.inst.instanceId, hit.inst.geometryIndex, hit.inst.primitiveIndex, hit.inst.barycentrics);
          const bool placeholder = decoded.material == 0u;
          if (try_proxy && (lod || placeholder)) {
            const float4 proxy = SsrRtScreenProxy(s, ray, hit.inst, decoded);
            const float proxy_coverage = proxy.w * SsrRtReachFade(hit.t, reach);
            if (proxy_coverage > 0.0f) {
              sum += (min(proxy.rgb, float3(cap, cap, cap)) - fallback) * (proxy_coverage * ray_share);
              if (proxy_coverage >= 0.5f) {
                nearest = min(nearest, hit.t);
                hit_coverage += proxy_coverage * ray_share;
              }
              filled = true;
#if SSR_REFLHIT
              rt_fades += proxy_coverage * ray_share;
              if (proxy_coverage > best_rt_fade) {
                rt_id = kReflHitSsrOther;
                best_rt_fade = proxy_coverage;
              }
#endif
              SsrRtCount(stats, SSR_RT_CENSUS_PROXY_RECOVERED);
              SsrRtCount(stats, SSR_RT_CENSUS_SCREEN);
              continue;
            }
          }
          SsrRtCount(stats, SsrRtNoDataReason(decoded.valid != 0u, ground, lod, placeholder));
        }
      }
      SsrRtCount(stats, SSR_RT_CENSUS_NO_DATA);
      continue;   // keep the SSR/planar result for this ray, including its hit distance
    }
    SsrRtCount(stats, res.screen >= 0.5f ? SSR_RT_CENSUS_SCREEN : SSR_RT_CENSUS_SHADED);
    const float fade = coverage * SsrRtReachFade(hit.t, reach);
    sum += (min(res.colour, float3(cap, cap, cap)) - fallback) * (fade * ray_share);
    const float sky_weight = sky_miss ? SsrRtSkyWeight(up, fade) : 0.0f;
    if (sky_weight > 0.0f) {
      sum += SsrRtEnvironmentTerm(f, dir, r.alpha, w.units, lower.visibility, fallback, cap) * (sky_weight * ray_share);
    }
    if (!ground || fade >= 0.5f) {
      nearest = min(nearest, hit.t);
      hit_coverage += fade * ray_share;
    }
    filled = true;
#if SSR_REFLHIT
    if (fade > best_rt_fade) {
      rt_id = ReflHitCity(res.cls);
      best_rt_fade = fade;
    }
    rt_fades += (fade + sky_weight) * ray_share;
#endif
  }
#if SSR_REFLHIT
  const uint packed = g_reflhit[pix];
  const uint screen_id = ReflHitScreenId(packed);
  const uint native = ReflHitFallbackId(packed);
  const float screen_fraction = ReflHitScreenFraction(packed);
  const float screen_weight = confidence * screen_fraction;
  const float reuse_weight = confidence - screen_weight;
  const float rt_weight = ReflectionHandoff(confidence, 0.0f, weight * rt_fades, 0.0f).rt;
  const uint visible = ReflHitHandoffDominant(screen_id, screen_weight, reuse_weight, rt_id, rt_weight, native);
  g_reflhit[pix] = ReflHitPackHandoff(visible, rt_id >= kReflHitRtCity ? rt_id : rejected, screen_id, native, screen_fraction) | ReflHitConfidenceBits(packed);
#endif
  if (!filled) {
    return;
  }
  const float rt_weight_scale = ReflectionHandoff(confidence, 0.0f, weight, 0.0f).rt;
  float3 rgb = NgFinite3(current.xyz + sum * rt_weight_scale);   // (a NaN or an infinity is none: it must not reach the cap below nor the history)
  const float peak = max(max(abs(rgb.x), abs(rgb.y)), abs(rgb.z));
  if (peak > cap) {
    rgb *= cap / peak;
  }
  float alpha = current.w;
  // CHECK: validate this hit-distance choice across RR, Regen and native
  // denoising with low-confidence silhouettes and upward RT misses.
  const bool rt_distance = handoff ? (hit_coverage * rt_weight_scale > confidence) : (confidence < 0.5f);
  if (rt_distance && nearest < kSsrMissLength) {
    alpha = SsrPackHit(clamp(nearest, 0.02f, 60000.0f), false, 0u);   // the reflection is the ray traced one's: its distance is the virtual image's
  }
  g_trace[pix] = float4(rgb, alpha);
  SsrRtCount(stats, SSR_RT_CENSUS_FILLED);
}
