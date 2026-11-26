#include "ssr_pass.hlsli"
#include "ssr_handoff.hlsli"
#include "ssr_classify.hlsli"
#include "ssr_rt_common.hlsli"

#if SSR_CLASSIFY_REPROJECT

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_plan : register(u3, space6);

[numthreads(4, 4, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint2 tiles = (uint2(SsrSize()) + kSsrTileSize - 1u) / kSsrTileSize;
  if (all(id.xy < tiles)) g_plan[id.xy] = SsrTileReproject(int2(id.xy));
}

#else

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_tiles : register(u0, space6);
RWByteAddressBuffer g_active_tiles : register(u1, space6);
RWByteAddressBuffer g_dispatch : register(u2, space6);
RWByteAddressBuffer g_ranks : register(u4, space6);

groupshared float4 gs_feedback[16];
groupshared uint gs_candidates[16];
static const uint kSkippedSample = 1u << 16u;

[numthreads(4, 4, 1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID, uint index : SV_GroupIndex) {
  const uint stride = g_handoff.Load(144u);
  if (SsrW(SSR_W_FLAGS) == 0u) {
    if (index == 0u) {
      g_active_tiles.Store(0u, 0u);
      g_dispatch.Store3(0u, uint3(stride, 0u, 1u));
    }
    return;
  }

  const uint word = g_handoff.Load(136u);
  const uint trace_slot = SsrLo(word);
  const uint conf_slot = SsrHi(word);
  const uint group_mask = g_handoff.Load(140u);
  const bool rt = SsrHasSlot(conf_slot);
  const int2 base = int2(group.xy * kSsrTileSize + 2u * lane.xy);
  float4 count = 0.0f;
  uint candidates = 0u;
  [unroll] for (uint y = 0u; y < 2u; ++y) {
    [unroll] for (uint x = 0u; x < 2u; ++x) {
      const int2 pixel = base + int2(x, y);
      if (any(pixel >= SsrSize())) continue;
      const float4 sample = g_textures[trace_slot].Load(int3(pixel, 0));
      if (sample.w == 0.0f) continue;
      if (sample.w == kSsrSkippedDistance) {
        candidates += kSkippedSample;
        continue;
      }
      // Compress the signed correction's magnitude before calculating
      // variance so that exposure and one bright texel cannot dominate it.
      const float luma = dot(abs(sample.rgb), float3(0.2126f, 0.7152f, 0.0722f));
      const float value = luma / (1.0f + luma);
      float miss = SsrHitDistance(sample.w) >= kSsrMissLength - 1.0f &&
          sample.w != kSsrScreenSkyDistance ? 1.0f : 0.0f;
      if (rt) {
        const float2 conf = g_textures[conf_slot].Load(int3(pixel, 0)).xy;
        miss = 1.0f - conf.y;
        candidates += uint(SsrRtCandidate(SsrRtGroupIn(conf.x), conf.y, group_mask, 0.97f));
      }
      count += float4(miss, 1.0f, value, value * value);
    }
  }
  gs_feedback[index] = count;
  gs_candidates[index] = candidates;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint step = 8u; step > 0u; step >>= 1u) {
    if (index < step) {
      gs_feedback[index] += gs_feedback[index + step];
      gs_candidates[index] += gs_candidates[index + step];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (index != 0u) return;

  const float4 previous = SsrTileHistory(int2(group.xy));
  const uint age = SsrTileAge(uint(previous.w));
  const float4 totals = gs_feedback[0];
  if (totals.y == 0.0f) {
    // A skipped walk is not a miss. Retain the counters until the next
    // probe, or forget the tile when no receiver remains in it.
    g_tiles[group.xy] = gs_candidates[0] >= kSkippedSample ? previous : SsrEmptyTile();
  } else {
    const float measured_miss = totals.x / totals.y;
    const float prior_miss = SsrTileMiss(previous.x, previous.y);
    const float mean = totals.z / totals.y;
    const float variance = max(totals.w / totals.y - mean * mean, 0.0f);
    const float hits = SsrTileFeedback(totals.y - totals.x, previous.x, age);
    const float misses = SsrTileFeedback(totals.x, previous.y, age);
    const float noise = SsrTileFeedback(max(variance, abs(measured_miss - prior_miss) * 0.12f), previous.z, age);
    const uint mode = SsrTileMode(SsrTileClass(uint(previous.w)), SsrTileMiss(hits, misses));
    g_tiles[group.xy] = float4(hits, misses, noise, float(SsrTileState(min(age + 1u, 15u), mode)));
  }

  if (rt) {
    const uint columns = (uint(SsrSize().x) + kSsrTileSize - 1u) / kSsrTileSize;
    g_ranks.Store2(8u * (group.x + group.y * columns), uint2(0u, 0u));
  }
  if ((gs_candidates[0] & (kSkippedSample - 1u)) != 0u) {
    uint entry;
    g_active_tiles.InterlockedAdd(0u, 1u, entry);
    const uint tile = group.x | (group.y << 16u);
    g_active_tiles.Store(4u + entry * 4u, tile);
    if (entry % stride == 0u) {
      g_dispatch.InterlockedMax(4u, entry / stride + 1u);
    }
  }
}

#endif
