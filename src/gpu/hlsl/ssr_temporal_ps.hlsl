// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1), pass 2: the accumulation over frames.
//
// One stochastic ray a pixel is noise; this pass averages it over frames. A full-screen triangle into an RGBA16F image of the SSR size (the next frame's history, the previous one is read):
//
//   rgb  the radiance the pixel has accumulated (premultiplied by the fades like the trace's: a ray that missed brings 0, and the average fades a reflection out smoothly), a the pixel's
//        receiver depth (the Hi-Z's, linear), which the next frame's history test and the apply pass's upsample read; 0 where nothing was traced.
//
// THE VIRTUAL IMAGE. A reflection moves on the screen like the MIRROR IMAGE of what it shows, not like the surface it is on: unfold the reflected path into a straight line and the hit stands
// `distance` behind the receiver along the view ray (hlsl/ssr_common.hlsli SsrVirtualPoint, exact for a flat mirror whatever the hit's place). That point, moved into the last frame's view space
// with the camera's own motion (the 3x4 of the occlusion's accumulation pass, ao.h FrameCameraNow) and projected with the last frame's lens, is where the same reflection was last frame; the
// history is read there (bilinear). A ray that found nothing has a distance of kSsrMissLength: the history of the sky, which moves with the camera's rotation alone.
//
// WHAT IS KEPT. Only where the receiver itself was the same surface: its own reprojection (the plain position, not the image) must land on a history texel whose stored depth agrees within 8% (a car
// that passed, a wall that came into view, an edge: no history), and the image's position must be more than a texel inside the picture (GB-I2: the clamped sample of an edge smears it). The history
// is CLIPPED to the neighbourhood of this frame's 3x3 of the trace: the box is the neighbourhood's mean +- 1.25 standard deviations, no wider than its min / max widened by a tenth (the variance
// clip), and the history goes along the line to the box's centre, so a reflection that changed, a light that went out, is a ghost for as long as it takes the clip, and keeps its colour.
//
// GB-I2, THE VEHICLES. A hit on a VEHICLE's pixel (the sign of the trace's alpha, SsrPackHit; the stencil ID in its low bits) moves by the vehicle's own motion, not the world's: where the AO has a
// motion row for that ID (its vehicle table, vehicle_motion.h: the 3x4 that takes this frame's view space into the last frame's with the vehicle's step on top of the camera's, the very rows the AO's
// own history uses) the image of the hit is reprojected with it (hlsl/ssr_common.hlsli SsrVehicleImage: the virtual image undone to the hit, the hit moved by the vehicle's row, mirrored again in
// the surface as it was) and the history keeps 0.8 of its weight; with no row (the AO is off, an ID that was not seen, the pause view) the image is taken as the world's and the history keeps
// 0.35. The weight the history keeps is SSR_W_FRAME as a float (ssr_rules.h NumbersOf), 0 for a frame without a usable history.
//
// Push block: the trace in the low half of SSR_W_SRC and the history in the high half; the Hi-Z; the lens; the last frame's lens in SSR_W_NEAR and SSR_W_MAX_DIST; the motion's rows from SSR_W_MOTION;
// the AO's vehicle table in the low half of SSR_W_VEH_TABLE (0xFFFF: none).

#include "ssr_pass.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pix = int2(position.xy);
  const int2 ssr = SsrSize();
  if (pix.x >= ssr.x || pix.y >= ssr.y) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const uint trace_slot = SsrLo(SsrW(SSR_W_SRC));
  const uint history_slot = SsrHi(SsrW(SSR_W_SRC));
  const float4 current = g_textures[trace_slot].Load(int3(pix, 0));
  if (current.w == 0.0f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);   // nothing was traced here
  }
  const float z_block = SsrHizAt(SsrHz(), pix.x, pix.y);
  if (z_block >= kSsrSky * 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 receiver = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), z_block, tan_x, tan_y);
  const float travelled = SsrHitDistance(current.w);
  const bool vehicle_hit = SsrHitIsVehicle(current.w);
  const uint vehicle_id = SsrHitVehicleId(current.w);

  float weight = 0.0f;
  float3 history = float3(0.0f, 0.0f, 0.0f);
  if (SsrFlag(SSR_FLAG_HISTORY)) {
    const float4 m0 = float4(SsrF(SSR_W_MOTION + 0u), SsrF(SSR_W_MOTION + 1u), SsrF(SSR_W_MOTION + 2u), SsrF(SSR_W_MOTION + 3u));
    const float4 m1 = float4(SsrF(SSR_W_MOTION + 4u), SsrF(SSR_W_MOTION + 5u), SsrF(SSR_W_MOTION + 6u), SsrF(SSR_W_MOTION + 7u));
    const float4 m2 = float4(SsrF(SSR_W_MOTION + 8u), SsrF(SSR_W_MOTION + 9u), SsrF(SSR_W_MOTION + 10u), SsrF(SSR_W_MOTION + 11u));
    const float prev_tan_x = SsrF(SSR_W_PREV_TAN_X);
    const float prev_tan_y = SsrF(SSR_W_PREV_TAN_Y);
    SsrPrevious image = SsrToPrevious(m0, m1, m2, SsrVirtualPoint(receiver, travelled), prev_tan_x, prev_tan_y);
    float vehicle_scale = 1.0f;
    if (vehicle_hit) {
      // a reflected vehicle: its own row where the AO has one, else the world's (and little of the history)
      vehicle_scale = kSsrVehicleNoRowWeight;
      const uint table = SsrLo(SsrW(SSR_W_VEH_TABLE));
      if (SsrHasSlot(table) && vehicle_id != 0u && g_textures[table].Load(int3(3, int(vehicle_id), 0)).x > 0.5f) {
        const float4 v0 = g_textures[table].Load(int3(0, int(vehicle_id), 0));
        const float4 v1 = g_textures[table].Load(int3(1, int(vehicle_id), 0));
        const float4 v2 = g_textures[table].Load(int3(2, int(vehicle_id), 0));
        const float3 n = SsrDepthNormal(SsrHz(), pix.x, pix.y, ssr.x, ssr.y, z_block, receiver, tan_x, tan_y);
        image = SsrVehicleImage(m0, m1, m2, v0, v1, v2, receiver, n, travelled, prev_tan_x, prev_tan_y);
        vehicle_scale = kSsrVehicleRowWeight;
      }
    }
    const SsrPrevious plain = SsrToPrevious(m0, m1, m2, receiver, prev_tan_x, prev_tan_y);
    if (image.ok && plain.ok && SsrHistoryInside(image.u, image.v, float(ssr.x), float(ssr.y))) {
      const int2 q = clamp(int2(plain.u * float(ssr.x), plain.v * float(ssr.y)), int2(0, 0), ssr - 1);
      const float stored = g_textures[history_slot].Load(int3(q, 0)).w;
      if (stored > 0.0f && SsrSameSurface(stored, plain.z)) {
        weight = SsrF(SSR_W_HISTORY_WEIGHT) * vehicle_scale;
        history = g_textures[history_slot].SampleLevel(g_samplers[kSamplerLinearClamp], float2(image.u, image.v), 0.0f).rgb;
      }
    }
  }
  // the clip's box: the mean, the deviation and the range of the trace's neighbourhood (the pixels that were traced)
  if (weight > 0.0f) {
    float3 lo = current.xyz;
    float3 hi = current.xyz;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float3 sum2 = float3(0.0f, 0.0f, 0.0f);
    float count = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
      [unroll] for (int dx = -1; dx <= 1; ++dx) {
        const int2 p = clamp(pix + int2(dx, dy), int2(0, 0), ssr - 1);
        const float4 t = g_textures[trace_slot].Load(int3(p, 0));
        if (t.w != 0.0f) {
          lo = min(lo, t.xyz);
          hi = max(hi, t.xyz);
          sum += t.xyz;
          sum2 += t.xyz * t.xyz;
          count += 1.0f;
        }
      }
    }
    const float3 mean = sum / max(count, 1.0f);
    const float3 sigma = sqrt(max(sum2 / max(count, 1.0f) - mean * mean, float3(0.0f, 0.0f, 0.0f)));
    const float3 box_lo = float3(SsrBoxLoChannel(mean.x, sigma.x, lo.x, hi.x), SsrBoxLoChannel(mean.y, sigma.y, lo.y, hi.y), SsrBoxLoChannel(mean.z, sigma.z, lo.z, hi.z));
    const float3 box_hi = float3(SsrBoxHiChannel(mean.x, sigma.x, lo.x, hi.x), SsrBoxHiChannel(mean.y, sigma.y, lo.y, hi.y), SsrBoxHiChannel(mean.z, sigma.z, lo.z, hi.z));
    history = SsrClipToBox(history, box_lo, box_hi);
  }
  return float4(lerp(current.xyz, history, weight), z_block);
}
