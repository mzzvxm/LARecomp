// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1), pass 1: the trace and the colour of what it finds.
//
// A full-screen triangle into an RGBA16F image of the SSR size (half the picture's). One pixel stands for a 2x2 block of the picture's pixels:
//
//   the surface   the Hi-Z (at this size) gives the block's depth; the pixel of the block whose depth is nearest to it is the receiver (its RT3 gives the class, flags and smoothness, its RT1 the
//                 normal). The class gate (ssr_common.hlsli SsrClassGain: roads, sidewalks, metal, glass, water and the tiles the table flags; never a vehicle, never paint or chrome, which keep
//                 their own reflections) and the smoothness decide whether anything is traced: a pixel that reflects nothing writes (0, 0, 0, 0) and costs the gate's reads.
//   the ray       one GGX visible-normal sample of the surface's lobe (two at ultra), the road's smoothness raised by the wetness, the origin moved off the surface; walked through the Hi-Z
//                 (ssr_trace.hlsli). A hit is rejected where the surface faces away from the ray (a back face the screen does not show).
//   the colour    colour_post at the hit, the mip chosen by the lobe's width at the hit; the sky at the vanishing point of the ray where it ended on the picture without a hit. Multiplied by
//                 the edge fade and the facing fade: a hit near the picture's edge, or a ray that points back at the camera, brings less and less, and what is not brought is the game's own
//                 reflection (the planar view of the road, the env map), which is already in the colour.
//
// Output: rgb the radiance (the rays' mean, premultiplied by the fades, capped at SSR_W_CAP, in the colour target's own units), a the distance to the NEAREST hit of the rays packed with what
// the hit is (hlsl/ssr_common.hlsli SsrPackHit: a vehicle's pixel sets the sign and carries its stencil ID, whose motion is not the world's), kSsrMissLength where none found anything; exactly 0
// where nothing was traced (the temporal pass and the apply read that as "no reflection here"). A ray that points back at the camera is not walked (its fade is 0): the window of a shop front,
// whose mirror ray goes behind the camera, costs the gate only.
//
// GB-I2, THE ROAD VIEW (the off-screen fallback). On a ROAD pixel the game's own reflection, the 256x256 planar view of the whole screen (side_views.h), is already in the colour this term is added to.
// Where a ray finds a reflection the trace therefore brings the DIFFERENCE: radiance minus the road view at the pixel's screen position (times the share SSR_W_FALLBACK takes out), each times the
// ray's fade: the sharp reflection replaces the soft one instead of lying on top of it (no doubling), and where the ray found nothing, or its hit is faded out by the picture's edge or by a surface
// that faces away, the difference is 0 and the game's reflection stays alone (no seam: the fallback IS the colour that is there). The rgb is then signed. Pixels of other classes keep the plain
// radiance (the game's environment map under them stays, only faded: its dual paraboloid mapping is the game's own and not reproduced here).
//
// CITY-RT R4, SSR_RT 1 (ssr_trace_rt_ps, ssr_trace_rt_ms_ps): the same pass for a frame the ray traced fallback follows (ssr_rt.h). It writes a second target, the pixel's CONFIDENCE (hlsl/ssr_rt_common.hlsli: r the class
// group, g the share of the reflection that is the picture's: the mean of its rays' fades), which the fallback reads to find the pixels the picture could not speak for. The first target is what it always
// was; the plain compilations do not contain any of this (their code is the same as before the fallback).
//
// Compilations: SSR_MS 0 / 1 (ssr_trace_ps, ssr_trace_ms_ps); with SSR_RT 1 the same two (ssr_trace_rt_ps, ssr_trace_rt_ms_ps).

#ifndef SSR_RT
#define SSR_RT 0
#endif

#include "ssr_pass.hlsli"
#include "ssr_trace.hlsli"
#if SSR_RT
#include "ssr_rt_common.hlsli"
static float2 g_confidence = float2(0.0f, 0.0f);   // the second target: left at 0 by every early return (nothing traced here)
#endif

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
  const float gain = SsrClassGain(surface.cls, surface.flags);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  if (!(gain > 0.0f) || smoothness < SsrF(SSR_W_MIN_SMOOTH)) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
#if SSR_RT
  // the class group the fallback may trace for this pixel: none where the term the apply takes is under a tenth of a whole one (a road just wet enough to cross the smoothness floor)
  const uint rt_group = SsrSmoothRamp(smoothness, SsrF(SSR_W_MIN_SMOOTH)) >= kSsrRtMinRamp ? SsrRtGroupOf(surface.cls) : kSsrRtGroupNone;
  float conf_sum = 0.0f;
#endif
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), z_block, tan_x, tan_y);
  const float3 to_camera = pc * (-1.0f / length(pc));
  const float3 n = SsrReceiverNormal(pix, texel, surface.cls, pc, to_camera);
  const float alpha = SsrAlphaOfSmooth(smoothness);

  // the walk's numbers
  SsrTraceParams tp;
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
  const uint rays = SsrRays();
  const uint frame = SsrW(SSR_W_FRAME);
  const bool stable = SsrStableTrace();
  const float cap = SsrF(SSR_W_CAP);

  // the game's own reflection of this pixel (GB-I2): the road view at the pixel's screen position, in the colour's units, the share of it the effect takes out
  float3 fallback = float3(0.0f, 0.0f, 0.0f);
  const float share = SsrF(SSR_W_FALLBACK);
  const uint road_view = SsrW(SSR_W_ROAD_VIEW);
  if ((surface.cls == uint(GB_CLASS_ROAD) || surface.cls == uint(GB_CLASS_WET_ROAD)) && share > 0.0f && SsrHasSlot(SsrRoadViewSlot(road_view))) {
    const float2 screen_uv = (float2(texel) + 0.5f) / float2(render);
    fallback = min(NgFinite3(SsrRoadViewAt(road_view, screen_uv)), float3(cap, cap, cap)) * share;   // (the game's own view: a NaN or an infinity in it is none, nan_guard.hlsli)
  }

  float3 sum = float3(0.0f, 0.0f, 0.0f);
  float travelled = kSsrMissLength;   // the nearest hit's distance of the rays (the virtual image's), a miss's where none hit
  bool vehicle_hit = false;
  uint vehicle_id = 0u;
  SSR_LOOP for (uint ray = 0u; ray < rays; ++ray) {
    const float3 dir = SsrSampleRay(n, to_camera, alpha, pix.x, pix.y, frame, ray, rays);   // (a sample that points below the surface is the mirror direction)
    if (SsrFacingFade(dir.z) <= 0.0f) {
      continue;   // a ray that points back at the camera brings nothing (the screen does not show what is behind the surface it hits): not walked
    }
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float conf = 0.0f;   // how much of the ray's reflection is the picture's (its fades), what the game's own reflection gives way for
    float length_to_hit = kSsrMissLength;
    bool is_vehicle = false;
    uint hit_vehicle = 0u;
    const SsrHit hit = SsrTrace(tp, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z);
    if (hit.hit) {
      const int hx = clamp(int(hit.px), 0, h0.x - 1);
      const int hy = clamp(int(hit.py), 0, h0.y - 1);
      // a surface that faces away from the ray is a back face: the screen shows its front, which is somewhere else
      const float hit_facing = SsrHitFacing(tp, hx, hy, hit.surface_z, dir.x, dir.y, dir.z);
      if (hit_facing < 0.15f) {
        const float2 uv = float2(hit.px / tp.width, hit.py / tp.height);
        const float edge = SsrEdgeFade(uv.x, uv.y);
        const float facing = SsrFacingFade(dir.z);
        const float fade = stable ? SsrScreenHitConfidence(edge, facing, hit_facing, hit.depth_error_fraction) : edge * facing;
        const float3 surface_point = SsrViewPos(float(hx) + 0.5f, float(hy) + 0.5f, tp.width, tp.height, hit.surface_z, tan_x, tan_y);
        const float distance = max(length(surface_point - pc), 0.02f);
        length_to_hit = stable && fade <= 0.0f ? kSsrMissLength : distance;
        const float lod = SsrConeLod(alpha, distance, hit.surface_z, tan_x, colour_width, colour_levels);
        radiance = SsrColour(uv, lod) * fade;
        conf = fade;
        is_vehicle = SsrVehicleAt(hx, hy);
        hit_vehicle = is_vehicle ? SsrVehicleIdAt(hx, hy) : 0u;
      }
    } else if (hit.inside_end) {
      // the ray ended on the picture without meeting anything: where the end is sky, the sky's colour there is the sky of the ray's direction
      const int ex = clamp(int(hit.px), 0, h0.x - 1);
      const int ey = clamp(int(hit.py), 0, h0.y - 1);
      if (SsrHizAt(0u, ex, ey) >= kSsrSky * 0.5f) {
        const float2 uv = float2(hit.px / tp.width, hit.py / tp.height);
        const float fade = SsrEdgeFade(uv.x, uv.y) * SsrFacingFade(dir.z);
        radiance = SsrColour(uv, 0.0f) * fade;
        conf = fade;
      }
    }
    sum += radiance - fallback * conf;
#if SSR_RT
    conf_sum += conf;
#endif
    if (length_to_hit < travelled) {
      travelled = length_to_hit;
      vehicle_hit = is_vehicle;
      vehicle_id = hit_vehicle;
    }
  }
  float3 rgb = NgFinite3(sum / float(rays));   // a NaN or an infinity must not reach the cap below (infinity times cap / infinity is NaN) nor the history
  const float peak = max(max(abs(rgb.x), abs(rgb.y)), abs(rgb.z));
  if (peak > cap) {
    rgb *= cap / peak;
  }
  const float distance_out = clamp(travelled, 0.02f, 60000.0f);
#if SSR_RT
  g_confidence = float2(SsrRtGroupOut(rt_group), saturate(conf_sum / float(rays)));
#endif
  return float4(rgb, SsrPackHit(distance_out, vehicle_hit, vehicle_id));
}

#if SSR_RT
struct SsrTraceOut {
  float4 trace : SV_Target0;
  float2 confidence : SV_Target1;
};
SsrTraceOut main(float4 position : SV_Position) {
  SsrTraceOut o;
  o.trace = SsrTraceBody(position);
  o.confidence = g_confidence;
  return o;
}
#else
float4 main(float4 position : SV_Position) : SV_Target {
  return SsrTraceBody(position);
}
#endif
