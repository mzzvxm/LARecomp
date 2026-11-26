// Accumulate the signed reflection correction at half resolution. Motion
// comes from the virtual image behind the reflector. The geometry plane
// validates both the receiver and each tap of the reprojected history.
#include "ssr_pass.hlsli"

// The debug switch still exposes the original centre-sample resolve for A/B.
float4 SsrFirstResolve(float4 position) {
  const int2 pix = int2(position.xy);
  const int2 size = SsrSize();
  const uint trace_slot = SsrLo(SsrW(SSR_W_SRC));
  const uint history_slot = SsrHi(SsrW(SSR_W_SRC));
  const float4 current = g_textures[trace_slot].Load(int3(pix, 0));
  const float z = SsrHizAt(SsrHz(), pix.x, pix.y);
  if (current.w == 0.0f || z >= kSsrSky * 0.5f) {
    return 0.0f;
  }
  const float3 value = NgFinite3(current.rgb);
  if (!SsrFlag(SSR_FLAG_HISTORY)) {
    return float4(value, z);
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 receiver = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(size.x), float(size.y), z, tan_x, tan_y);
  const float4 m0 = float4(SsrF(SSR_W_MOTION + 0u), SsrF(SSR_W_MOTION + 1u), SsrF(SSR_W_MOTION + 2u), SsrF(SSR_W_MOTION + 3u));
  const float4 m1 = float4(SsrF(SSR_W_MOTION + 4u), SsrF(SSR_W_MOTION + 5u), SsrF(SSR_W_MOTION + 6u), SsrF(SSR_W_MOTION + 7u));
  const float4 m2 = float4(SsrF(SSR_W_MOTION + 8u), SsrF(SSR_W_MOTION + 9u), SsrF(SSR_W_MOTION + 10u), SsrF(SSR_W_MOTION + 11u));
  const float prev_tan_x = SsrF(SSR_W_PREV_TAN_X);
  const float prev_tan_y = SsrF(SSR_W_PREV_TAN_Y);
  const float travelled = SsrHitDistance(current.w);
  SsrPrevious image = SsrToPrevious(m0, m1, m2, SsrVirtualPoint(receiver, travelled), prev_tan_x, prev_tan_y);
  float vehicle_scale = 1.0f;
  if (SsrHitIsVehicle(current.w)) {
    vehicle_scale = kSsrVehicleNoRowWeight;
    const uint table = SsrLo(SsrW(SSR_W_VEH_TABLE));
    const uint id = SsrHitVehicleId(current.w);
    if (SsrHasSlot(table) && id != 0u && g_textures[table].Load(int3(3, int(id), 0)).x > 0.5f) {
      const float4 v0 = g_textures[table].Load(int3(0, int(id), 0));
      const float4 v1 = g_textures[table].Load(int3(1, int(id), 0));
      const float4 v2 = g_textures[table].Load(int3(2, int(id), 0));
      const float3 normal = SsrDepthNormal(SsrHz(), pix.x, pix.y, size.x, size.y, z, receiver, tan_x, tan_y);
      image = SsrVehicleImage(m0, m1, m2, v0, v1, v2, receiver, normal, travelled, prev_tan_x, prev_tan_y);
      vehicle_scale = kSsrVehicleRowWeight;
    }
  }
  const SsrPrevious plain = SsrToPrevious(m0, m1, m2, receiver, prev_tan_x, prev_tan_y);
  if (!plain.ok || !image.ok || !SsrHistoryInside(image.u, image.v, float(size.x), float(size.y))) {
    return float4(value, z);
  }
  const int2 q = clamp(int2(float2(plain.u, plain.v) * float2(size)), 0, size - 1);
  const float stored = g_textures[history_slot].Load(int3(q, 0)).w;
  if (stored <= 0.0f || !SsrSameSurface(stored, plain.z)) {
    return float4(value, z);
  }
  float3 lo = value;
  float3 hi = value;
  float3 sum = 0.0f;
  float3 sum2 = 0.0f;
  float count = 0.0f;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const int2 p = clamp(pix + int2(x, y), 0, size - 1);
      const float4 tap = g_textures[trace_slot].Load(int3(p, 0));
      if (tap.w != 0.0f) {
        const float3 colour = NgFinite3(tap.rgb);
        lo = min(lo, colour);
        hi = max(hi, colour);
        sum += colour;
        sum2 += colour * colour;
        count += 1.0f;
      }
    }
  }
  const float3 mean = sum / count;
  const float3 sigma = sqrt(max(sum2 / count - mean * mean, 0.0f));
  const float3 margin = (hi - lo) * kSsrClipMargin;
  const float3 history = NgFinite3(g_textures[history_slot].SampleLevel(g_samplers[kSamplerLinearClamp], float2(image.u, image.v), 0.0f).rgb);
  const float3 clipped = SsrClipToBox(history, max(mean - kSsrClipGamma * sigma, lo - margin), min(mean + kSsrClipGamma * sigma, hi + margin));
  return float4(NgFinite3(lerp(value, clipped, SsrF(SSR_W_HISTORY_WEIGHT) * vehicle_scale)), z);
}

struct SsrHistoryOutput {
  float4 colour : SV_Target0;   // signed premultiplied correction; receiver depth for apply
  float4 guide : SV_Target1;    // octahedral plane normal, virtual Z, packed source/miss/sample counts
};

float SsrPreviousPlane(int2 p, float4 guide, float stored_z, float3 plane_point, float3 plane_normal,
                       float2 size, float2 lens) {
  if (stored_z <= 0.0f || SsrGuideSamples(uint(guide.w)) == 0u ||
      dot(SsrDecodePlaneNormal(guide.xy), plane_normal) < 0.85f) {
    return 0.0f;
  }
  const float3 sample_position = SsrViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, size.x, size.y, stored_z, lens.x, lens.y);
  const float footprint = 2.0f * max(lens.x / size.x, lens.y / size.y) * plane_point.z;
  return SsrPlaneWeight(sample_position, plane_point, plane_normal, footprint);
}

SsrHistoryOutput SsrAccumulate(int2 pix, uint options) {
  SsrHistoryOutput out_history = (SsrHistoryOutput)0;
  const int2 size = SsrSize();
  const uint trace_slot = SsrLo(SsrW(SSR_W_SRC));
  const float4 current = g_textures[trace_slot].Load(int3(pix, 0));
  if (current.w == 0.0f) {
    return out_history;
  }
  const uint hz = SsrHz();
  const float z = SsrHizAt(hz, pix.x, pix.y);
  if (z >= kSsrSky * 0.5f) {
    return out_history;
  }

  const float2 lens = float2(SsrF(SSR_W_TAN_X), SsrF(SSR_W_TAN_Y));
  const float3 receiver = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(size.x), float(size.y), z, lens.x, lens.y);
  const float3 normal = SsrDepthNormal(hz, pix.x, pix.y, size.x, size.y, z, receiver, lens.x, lens.y);
  const float footprint = 2.0f * max(lens.x / float(size.x), lens.y / float(size.y)) * z;
  float3 sum = 0.0f;
  float3 sum2 = 0.0f;
  float3 lo = NgFinite3(current.rgb);
  float3 hi = lo;
  float total = 0.0f;
  float hit_weight = 0.0f;
  float representative = current.w;
  float best = 0.0f;
  const bool centre_hit = SsrHistorySource(current.w) != kSsrDistantHit;
  [unroll] for (int dy = -1; dy <= 1; ++dy) {
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
      const int2 p = pix + int2(dx, dy);
      if (any(p < 0) || any(p >= size)) {
        continue;
      }
      const float4 tap = g_textures[trace_slot].Load(int3(p, 0));
      if (tap.w == 0.0f) {
        continue;
      }
      const float tap_z = SsrHizAt(hz, p.x, p.y);
      const float3 sample_position = SsrViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, float(size.x), float(size.y), tap_z, lens.x, lens.y);
      const float w = SsrTapKernel(dx, dy) * SsrPlaneWeight(sample_position, receiver, normal, footprint);
      if (w <= 0.0f) {
        continue;
      }
      const float3 colour = NgFinite3(tap.rgb);
      sum += w * colour;
      sum2 += w * colour * colour;
      lo = min(lo, colour);
      hi = max(hi, colour);
      total += w;
      if (SsrHistorySource(tap.w) != kSsrDistantHit) {
        hit_weight += w;
        // Preserve the identity along with the neighbouring ray's distance.
        if (SsrPreferHistoryHit(centre_hit, tap.w, w, best)) {
          best = w;
          representative = tap.w;
        }
      }
    }
  }
  const float inv_total = 1.0f / max(total, 1.0e-6f);
  const float3 value = sum * inv_total;
  const float support = hit_weight * inv_total;
  uint source = SsrHistorySource(representative);
  float distance_to_hit = SsrHitDistance(representative);
  uint misses = 0u;
  uint samples = 1u;
  float3 result = value;

  if (SsrFlag(SSR_FLAG_HISTORY)) {
    const float4 m0 = float4(SsrF(SSR_W_MOTION + 0u), SsrF(SSR_W_MOTION + 1u), SsrF(SSR_W_MOTION + 2u), SsrF(SSR_W_MOTION + 3u));
    const float4 m1 = float4(SsrF(SSR_W_MOTION + 4u), SsrF(SSR_W_MOTION + 5u), SsrF(SSR_W_MOTION + 6u), SsrF(SSR_W_MOTION + 7u));
    const float4 m2 = float4(SsrF(SSR_W_MOTION + 8u), SsrF(SSR_W_MOTION + 9u), SsrF(SSR_W_MOTION + 10u), SsrF(SSR_W_MOTION + 11u));
    const float2 prev_lens = float2(SsrF(SSR_W_PREV_TAN_X), SsrF(SSR_W_PREV_TAN_Y));
    const SsrPrevious plain = SsrToPrevious(m0, m1, m2, receiver, prev_lens.x, prev_lens.y);
    if (plain.ok && SsrHistoryInside(plain.u, plain.v, float(size.x), float(size.y))) {
      const uint history_slot = SsrHi(SsrW(SSR_W_SRC));
      const uint guide_slot = SsrW(SSR_W_HISTORY_GUIDE);
      const float3 prev_receiver = SsrViewPos(plain.u, plain.v, 1.0f, 1.0f, plain.z, prev_lens.x, prev_lens.y);
      const float3 prev_normal = normalize(float3(SsrDot3(m0, normal), SsrDot3(m1, normal), SsrDot3(m2, normal)));
      const int2 q = int2(float2(plain.u, plain.v) * float2(size));
      const float4 plain_guide = g_textures[guide_slot].Load(int3(q, 0));
      const float plain_z = g_textures[history_slot].Load(int3(q, 0)).w;
      const float receiver_weight = SsrPreviousPlane(q, plain_guide, plain_z, prev_receiver, prev_normal, float2(size), prev_lens);
      if (receiver_weight > 0.0f) {
        const uint plain_state = uint(plain_guide.w);
        if (source == kSsrDistantHit && SsrCanCarryHit(plain_state) && plain_guide.z > plain_z) {
          // No finite ray in this frame's footprint. Reuse a nearby path
          // briefly. A vehicle still needs its own motion row below.
          const float3 q_receiver = SsrViewPos(float(q.x) + 0.5f, float(q.y) + 0.5f, float(size.x), float(size.y),
                                              plain_z, prev_lens.x, prev_lens.y);
          distance_to_hit = SsrCarryDistance(plain_guide.z, plain_z, length(q_receiver));
          source = SsrGuideSource(plain_state);
          misses = SsrGuideMisses(plain_state) + 1u;
        }
        SsrPrevious image = SsrToPrevious(m0, m1, m2, SsrVirtualPoint(receiver, distance_to_hit), prev_lens.x, prev_lens.y);
        float motion_weight = 1.0f;
        if (source > kSsrWorldHit && source <= kSsrUnknownVehicle) {
          // Unknown motion gets current-frame spatial filtering only.
          motion_weight = 0.0f;
          const uint table = SsrLo(SsrW(SSR_W_VEH_TABLE));
          if (source < kSsrUnknownVehicle && SsrHasSlot(table) &&
              g_textures[table].Load(int3(3, int(source), 0)).x > 0.5f) {
            const float4 v0 = g_textures[table].Load(int3(0, int(source), 0));
            const float4 v1 = g_textures[table].Load(int3(1, int(source), 0));
            const float4 v2 = g_textures[table].Load(int3(2, int(source), 0));
            image = SsrVehicleImage(m0, m1, m2, v0, v1, v2, receiver, normal, distance_to_hit, prev_lens.x, prev_lens.y);
            motion_weight = kSsrVehicleRowWeight;
          }
        }
        if (motion_weight > 0.0f && image.ok && SsrHistoryInside(image.u, image.v, float(size.x), float(size.y))) {
          const float2 texels = float2(image.u, image.v) * float2(size) - 0.5f;
          const int2 base = int2(floor(texels));
          const float2 f = frac(texels);
          float3 history_sum = 0.0f;
          float coverage = 0.0f;
          float sample_sum = 0.0f;
          [unroll] for (int y = 0; y <= 1; ++y) {
            [unroll] for (int x = 0; x <= 1; ++x) {
              const int2 p = base + int2(x, y);
              const float4 guide = g_textures[guide_slot].Load(int3(p, 0));
              const uint state = uint(guide.w);
              if (SsrGuideSource(state) != source) {
                continue;
              }
              const float4 old = g_textures[history_slot].Load(int3(p, 0));
              float w = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
              w *= SsrPreviousPlane(p, guide, old.w, prev_receiver, prev_normal, float2(size), prev_lens);
              w *= SsrImageDepthWeight(guide.z, image.z, source);
              history_sum += w * NgFinite3(old.rgb);
              sample_sum += w * float(SsrGuideSamples(state));
              coverage += w;
            }
          }
          if (coverage > 1.0e-4f) {
            const float3 history = history_sum / coverage;
            const float3 sigma = sqrt(max(sum2 * inv_total - value * value, 0.0f));
            const float3 margin = (hi - lo) * kSsrClipMargin;
            const float3 clipped = SsrClipToBox(history, max(value - kSsrClipGamma * sigma, lo - margin),
                                               min(value + kSsrClipGamma * sigma, hi + margin));
            const float validity = coverage * receiver_weight;
            const bool upscaler = (options & SSR_TEMPORAL_UPSCALER) != 0u;
            const bool sparkle = (options & SSR_TEMPORAL_SPARKLE_RESOLVE) != 0u;
            // CHECK: compare wet-road dolly highlights and moving taillight trails in game.
            const float keep = SsrValidatedClipKeep(support, validity, upscaler, sparkle);
            const float3 kept = float3(SsrKeepSigned(history.x, clipped.x, keep), SsrKeepSigned(history.y, clipped.y, keep),
                                       SsrKeepSigned(history.z, clipped.z, keep));
            const uint previous_samples = uint(round(sample_sum / coverage));
            const float weight = SsrHistoryMix(SsrF(SSR_W_HISTORY_WEIGHT), validity * motion_weight, previous_samples);
            result = lerp(value, kept, weight);
            samples = min(1u + uint(round(validity * float(previous_samples))), kSsrMaxHistorySamples);
          }
        }
        if (misses != 0u && samples == 1u) {
          // Keep a carried distance only if the image lookup also validated it.
          misses = 0u;
          source = kSsrDistantHit;
          distance_to_hit = kSsrMissLength;
        }
      }
    }
  }

  out_history.colour = float4(NgFinite3(result), z);
  out_history.guide = float4(SsrEncodePlaneNormal(normal), SsrVirtualPoint(receiver, distance_to_hit).z,
                             float(SsrGuideState(source, misses, samples)));
  return out_history;
}

SsrHistoryOutput main(float4 position : SV_Position) {
  const uint options = SsrHi(SsrW(SSR_W_VEH_TABLE));
  if ((options & SSR_TEMPORAL_FIRST_RESOLVE) != 0u) {
    SsrHistoryOutput legacy = (SsrHistoryOutput)0;
    legacy.colour = SsrFirstResolve(position);
    return legacy;
  }
  return SsrAccumulate(int2(position.xy), options);
}
