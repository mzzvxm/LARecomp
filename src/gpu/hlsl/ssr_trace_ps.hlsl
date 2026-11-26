// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1), pass 1: the trace and the colour of what it finds.
//
// A full-screen triangle into an RGBA16F image of the SSR size (half the picture's). One pixel stands for a 2x2 block of the picture's pixels:
//
//   the surface   the Hi-Z (at this size) gives the block's depth; the pixel of the block whose depth is nearest to it is the receiver (its RT3 gives the class, flags and smoothness, its RT1 the
//                 normal). The class gate (ssr_common.hlsli SsrClassGain: roads, sidewalks, metal, glass, water and the tiles the table flags; never a vehicle, never paint or chrome, which keep
//                 their own reflections) and the smoothness decide whether anything is traced: a pixel that reflects nothing writes (0, 0, 0, 0) and costs the gate's reads.
//   the ray       one GGX visible-normal sample of the surface's lobe (two at ultra), the road's smoothness raised by the wetness, the origin moved off the surface; walked through the Hi-Z
//                 (ssr_trace.hlsli). A hit is rejected where the surface faces away from the ray (a back face the screen does not show).
// Current hits use mip-filtered colour_post. At screen edges, occluders
// and backfaces, the previous view can fill part of the missing lobe
// after its depth and normal confirm the reprojected hit. The uncovered
// share uses planar on ground receivers or the roughness-filtered env map.
//
// RGB is the capped mean radiance, expressed as a delta wherever the guest
// scene already includes the fallback. Alpha packs hit distance and
// vehicle identity for reprojection, or 0 for an ineligible receiver.
// SSR_RT also writes the group and the residual per-ray distribution,
// which let RT replace only the still-uncovered fallback before denoising.
//
// Compilations: SSR_MS 0 / 1 (ssr_trace_ps, ssr_trace_ms_ps); with SSR_RT 1 the same two (ssr_trace_rt_ps, ssr_trace_rt_ms_ps).

#ifndef SSR_RT
#define SSR_RT 0
#endif

#include "ssr_pass.hlsli"
#include "ssr_handoff.hlsli"
#include "ssr_classify.hlsli"
#include "ssr_reuse.hlsli"
#if SSR_REFLHIT
#include "reflhit.hlsli"
static uint g_reflhit = 0u;

uint SsrHitClass(int2 p, float z, bool vehicle) {
  if (SsrFlag(SSR_FLAG_STUB)) return ReflHitScreen(GB_CLASS_NONE, 0u, vehicle);
  if (!SsrFlag(SSR_FLAG_HZ)) {
    const int2 last = SsrRender() - 1;
    const int2 a = min(p * 2, last);
    const int2 b = min(a + int2(1, 0), last);
    const int2 c = min(a + int2(0, 1), last);
    const int2 d = min(a + int2(1, 1), last);
    const uint pick = SsrPickInBlock(SsrLinearAt(a), SsrLinearAt(b), SsrLinearAt(c), SsrLinearAt(d), z);
    p = pick == 0u ? a : (pick == 1u ? b : (pick == 2u ? c : d));
  }
  const float2 material = SsrRt3At(p);
  return ReflHitScreen(GbRt3Class(material), GbRt3Flags(material), vehicle);
}
#endif
#if SSR_RT
#include "ssr_rt_common.hlsli"
static float2 g_confidence = float2(0.0f, 0.0f);   // the second target: left at 0 by every early return (nothing traced here)
#endif

bool SsrTileSsrProbe(int2 tile) {
  const uint frame = SsrW(SSR_W_FRAME) & ~SSR_FRAME_LEGACY_NOISE;
  // Spread one probe per tile across each four-frame block. The phase
  // moves between blocks using the same R2 sequence as the SSR rays.
  const float phase = SsrNoise2(tile.x, tile.y, frame >> 2u, 0u, 1u);
  return SsrTileProbe(frame, uint(phase * 4.0f));
}

float4 SsrTraceBody(float4 position) {
  const int2 pix = int2(position.xy);
  const int2 render = SsrRender();
  const int2 ssr = SsrSize();
  if (pix.x >= ssr.x || pix.y >= ssr.y) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const uint hz = SsrHz();
  const float z_block = SsrHizAt(hz, pix.x, pix.y);
  if (z_block >= kSsrSky * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);   // the sky
  }
  // the receiver: the pixel of the 2x2 block whose depth is the Hi-Z's
  const int2 last = render - 1;
  const int2 base = pix * 2;
  const int2 t0 = min(base, last);
  const int2 t1 = min(base + int2(1, 0), last);
  const int2 t2 = min(base + int2(0, 1), last);
  const int2 t3 = min(base + int2(1, 1), last);
  const uint pick = SsrPickInBlock(SsrLinearAt(t0), SsrLinearAt(t1), SsrLinearAt(t2), SsrLinearAt(t3), z_block);
  const int2 texel = pick == 0u ? t0 : (pick == 1u ? t1 : (pick == 2u ? t2 : t3));
  // the surface
  const int2 mark = SsrFlag(SSR_FLAG_HZ) ? texel : pix;    // the depth chain's level 0 has the Hi-Z level 0's size
  const bool vehicle = SsrFlag(SSR_FLAG_STUB) && SsrVehicleAt(mark.x, mark.y);
  const SsrSurface surface = SsrSurfaceAt(texel, vehicle);
#if SSR_REFLHIT
  const bool planar = (SsrW(SSR_W_ROAD_VIEW) & SSR_REFLHIT_PLANAR_BIT) != 0u;
  uint native = ReflHitFallback(surface.cls, surface.flags, planar);
  g_reflhit = ReflHitPack(native, kReflHitNone, kReflHitNone, native);
  float screen_weight = 0.0f;
  float reuse_weight = 0.0f;
  float best_fade = 0.0f;
  uint screen_id = kReflHitNone;
#endif
  const float gain = SsrClassGain(surface.cls, surface.flags);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  if (!(gain > 0.0f) || smoothness < SsrF(SSR_W_MIN_SMOOTH)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
#if SSR_RT
  // the class group the fallback may trace for this pixel: none where the term the apply takes is under a tenth of a whole one (a road just wet enough to cross the smoothness floor)
  const uint rt_group = SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH)) >= kSsrRtMinRamp ? SsrRtGroupOf(surface.cls) : kSsrRtGroupNone;
  float conf_sum = 0.0f;
  float first_confidence = 0.0f;
#endif
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), z_block, tan_x, tan_y);
  const float3 to_camera = pc * (-1.0f / length(pc));
  const float3 n = SsrReceiverNormal(pix, texel, surface.cls, pc, to_camera);
  const float alpha = SsrAlphaOfSmooth(smoothness);

  // the walk's numbers
  SsrTraceParams tp = (SsrTraceParams)0;
  const bool handoff = SsrHandoffEnabled();
  tp.adaptive_thickness = handoff;
  const int2 h0 = SsrHiz0Size();
  tp.width = float(h0.x);
  tp.height = float(h0.y);
  tp.width_i = uint(h0.x);
  tp.height_i = uint(h0.y);
  tp.tan_x = tan_x;
  tp.tan_y = tan_y;
  tp.lin_x = SsrF(SSR_W_LIN_X);
  tp.lin_y = SsrF(SSR_W_LIN_Y);
  tp.lin_z = SsrF(SSR_W_LIN_Z);
  tp.lin_w = SsrF(SSR_W_LIN_W);
  tp.near_plane = SsrF(SSR_W_NEAR);
  tp.max_distance = SsrF(SSR_W_MAX_DIST);
  tp.thick_rel = SsrF(SSR_W_THICK);
  tp.levels = SsrHizLevels();
  tp.max_iter = SsrMaxIter();
  const float colour_width = SsrFlag(SSR_FLAG_COL_HALF) ? float((render.x + 1) / 2) : float(render.x);
  const float colour_levels = float(max(SsrColourLevels(), 1u) - 1u);
  const float bias = kSsrNormalBias + kSsrNormalBiasShare * z_block;
  const float3 origin = pc + n * bias;
  const uint pattern_rays = SsrRays();
  uint rays = pattern_rays;
  const uint classify = SsrClassifyFlags();
  if ((classify & kSsrClassifyEnabled) != 0u) {
    tp.max_iter = SsrTileIterations(tp.max_iter, smoothness);
    const bool sparse = (classify & kSsrClassifySparse) != 0u;
    if (pattern_rays > 1u || sparse) {
      const int2 tile = pix / int(kSsrTileSize);
      const float4 previous = SsrTileHistory(tile);
      rays = SsrTileRays(pattern_rays, SsrTileAge(uint(previous.w)), SsrTileMiss(previous.x, previous.y),
                        previous.z, smoothness, sparse, sparse && SsrTileSsrProbe(tile));
    }
  }
  const uint frame = SsrW(SSR_W_FRAME);
  const bool stable = SsrStableTrace();
  const float cap = SsrF(SSR_W_CAP);

  const SsrFallback fallback = SsrSurfaceFallback(surface.cls, texel, pc, n, smoothness, handoff);
#if SSR_REFLHIT
  if (handoff) {
    native = fallback.has_planar ? kReflHitPlanar : (fallback.has_env ? kReflHitEnvironment : kReflHitNone);
  }
#endif

  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float travelled = kSsrMissLength;   // the nearest hit's distance of the rays (the virtual image's), a miss's where none hit
  bool vehicle_hit = false;
  bool screen_sky = false;
  uint vehicle_id = 0u;
  SSR_LOOP for (uint ray = 0u; ray < rays; ++ray) {
    const float3 dir = SsrSampleRay(n, to_camera, alpha, pix.x, pix.y, frame, ray, pattern_rays);   // (a sample that points below the surface is the mirror direction)
    const float facing = SsrFacingFade(dir.z);
    if (!handoff && facing <= 0.0f) {
      continue;   // a ray that points back at the camera brings nothing (the screen does not show what is behind the surface it hits): not walked
    }
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float conf = 0.0f;   // how much of the ray's reflection is the picture's (its fades), what the game's own reflection gives way for
    float length_to_hit = kSsrMissLength;
    bool is_vehicle = false;
    uint hit_vehicle = 0u;
    SsrHit hit = (SsrHit)0;
    hit.behind_distance = kSsrMissLength;
    if (facing > 0.0f) {
      hit = SsrTrace(tp, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z);
    }
    if (hit.hit) {
      const int hx = clamp(int(hit.px), 0, h0.x - 1);
      const int hy = clamp(int(hit.py), 0, h0.y - 1);
      // a surface that faces away from the ray is a back face: the screen shows its front, which is somewhere else
      const float hit_facing = SsrHitFacing(tp, hx, hy, hit.surface_z, dir.x, dir.y, dir.z);
      if (hit_facing < 0.15f) {
        const float2 uv = float2(hit.px / tp.width, hit.py / tp.height);
        const float edge = SsrEdgeFade(uv.x, uv.y);
        const float fade = (stable || handoff) ? SsrScreenHitConfidence(edge, facing, hit_facing, hit.depth_error_fraction) : edge * facing;
        const float3 surface_point = SsrViewPos(float(hx) + 0.5f, float(hy) + 0.5f, tp.width, tp.height, hit.surface_z, tan_x, tan_y);
        const float distance = max(length(surface_point - pc), 0.02f);
        length_to_hit = (stable || handoff) && fade <= 0.0f ? kSsrMissLength : distance;
        const float lod = SsrConeLod(alpha, distance, hit.surface_z, tan_x, colour_width, colour_levels);
        radiance = SsrColour(uv, lod);
        conf = fade;
        is_vehicle = SsrVehicleAt(hx, hy);
        hit_vehicle = is_vehicle ? SsrVehicleIdAt(hx, hy) : 0u;
#if SSR_REFLHIT
        if (conf > best_fade) screen_id = SsrHitClass(int2(hx, hy), hit.surface_z, is_vehicle);
#endif
      }
    } else if (hit.inside_end) {
      // the ray ended on the picture without meeting anything: where the end is sky, the sky's colour there is the sky of the ray's direction
      const int ex = clamp(int(hit.px), 0, h0.x - 1);
      const int ey = clamp(int(hit.py), 0, h0.y - 1);
      if (SsrHizAt(0u, ex, ey) >= kSsrSky * 0.5f) {
        const float2 uv = float2(hit.px / tp.width, hit.py / tp.height);
        const float fade = SsrEdgeFade(uv.x, uv.y) * SsrFacingFade(dir.z);
        radiance = SsrColour(uv, 0.0f);
        conf = fade;
        screen_sky = screen_sky || fade > 0.1f;
#if SSR_REFLHIT
        if (conf > best_fade) screen_id = kReflHitSsrSky;
#endif
      }
    }
    SsrReusedHit reused = (SsrReusedHit)0;
    if (handoff) {
      reused = SsrReuse(tp, origin, dir, hit, conf);
    }
    const ReflectionWeights weights = ReflectionHandoff(conf, reused.confidence, 0.0f, fallback.has_planar ? 1.0f : 0.0f);
    if (handoff) {
      sum += radiance * weights.screen + reused.colour * weights.reuse +
             fallback.planar * weights.planar + fallback.env * weights.env - fallback.stock;
      if (weights.reuse > weights.screen) {
        length_to_hit = max(reused.distance, 0.02f);
        is_vehicle = false;
        hit_vehicle = 0u;
      }
    } else {
      sum += (radiance - fallback.stock) * conf;
    }
#if SSR_REFLHIT
    best_fade = max(best_fade, conf);
    screen_weight += weights.screen;
    reuse_weight += weights.reuse;
#endif
    conf = weights.screen + weights.reuse;
#if SSR_RT
    conf_sum += conf;
    if (ray == 0u) {
      first_confidence = conf;
    }
#endif
    if (length_to_hit < travelled) {
      travelled = length_to_hit;
      vehicle_hit = is_vehicle;
      vehicle_id = hit_vehicle;
    }
  }
  const float divisor = float(max(rays, 1u));
  if (rays == 0u) {
    sum = (fallback.has_planar ? fallback.planar : fallback.env) - fallback.stock;
  }
  float3 rgb = NgFinite3(sum / divisor);   // a NaN or an infinity must not reach the cap below (infinity times cap / infinity is NaN) nor the history
  const float peak = max(max(abs(rgb.x), abs(rgb.y)), abs(rgb.z));
  if (peak > cap) {
    rgb *= cap / peak;
  }
  float distance_out = clamp(travelled, 0.02f, 60000.0f);
  if (rays == 0u) {
    distance_out = kSsrSkippedDistance;
  } else if (screen_sky && distance_out == kSsrMissLength && (classify & kSsrClassifySparse) != 0u) {
    distance_out = kSsrScreenSkyDistance;
  }
#if SSR_REFLHIT
  const uint visible = ReflHitHandoffDominant(screen_id, screen_weight / divisor, reuse_weight / divisor, kReflHitNone, 0.0f, native);
  const float screen_fraction = screen_weight / max(screen_weight + reuse_weight, 1.0e-5f);
  g_reflhit = ReflHitPackHandoff(visible, kReflHitNone, screen_id, native, screen_fraction);
#endif
#if SSR_RT
  g_confidence = float2(SsrRtGroupOut(rt_group, SsrRtFirstRayShare(first_confidence, conf_sum, rays)), saturate(conf_sum / divisor));
#endif
  return float4(rgb, SsrPackHit(distance_out, vehicle_hit, vehicle_id));
}

#if SSR_RT || SSR_REFLHIT
struct SsrTraceOut {
  float4 trace : SV_Target0;
#if SSR_RT
  float2 confidence : SV_Target1;
#endif
#if SSR_REFLHIT
#if SSR_RT
  uint reflhit : SV_Target2;
#else
  uint reflhit : SV_Target1;
#endif
#endif
};
SsrTraceOut main(float4 position : SV_Position) {
  SsrTraceOut o;
  o.trace = SsrTraceBody(position);
#if SSR_RT
  o.confidence = g_confidence;
#endif
#if SSR_REFLHIT
  o.reflhit = g_reflhit;
#endif
  return o;
}
#else
float4 main(float4 position : SV_Position) : SV_Target {
  return SsrTraceBody(position);
}
#endif
