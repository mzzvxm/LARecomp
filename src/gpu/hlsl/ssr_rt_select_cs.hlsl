// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections (CITY-RT package R4; ssr_rt.h, ssr_rt_rules.h, hlsl/ssr_rt_common.hlsli), pass 1: the candidates, listed tile by tile.
//
// An indirect group visits one compacted 8x8 tile containing current screen
// holes. It spends the level's cap on whole tiles and samples marginal holes
// according to their reprojected history. The A/B path visits the entire
// screen and applies the original per-pixel cap. In either path, one group
// reserves adjacent pixels in the trace queue with one atomic.
//
// The counters (u1 of the output set, ssr_rt_common.hlsli SSR_RT_CTL_*): three rotating frames of two words, the candidates counted and the candidates listed. A frame uses slot rt_frame % 3, reads the last
// frame's total from slot (rt_frame + 2) % 3 and clears slot (rt_frame + 1) % 3 for the next: nothing is cleared by the host and nothing is read back for the cap. The first frame ever (SSR_RT_FLAG_RESET)
// only clears all the control words.
//
// Layout (ssr_rt.cpp): the SSR host pass layout (sets 0 to 3 the bindless texture set bound four times, set 4 the samplers, the push block at b0 space4: ssr_pass.hlsli), set 5 this pass's own
// (u0 the trace image, unused here; u1 the control and statistics words; u2 the list), set 6 the ray tracing set (unused here), set 7 the handoff and compacted tiles. Shader Model 6.0: no rays.

#include "ssr_pass.hlsli"
#include "ssr_rt_common.hlsli"
#define SSR_HANDOFF_RT 1
#include "ssr_handoff.hlsli"
#include "ssr_classify.hlsli"

ByteAddressBuffer g_tile_list : register(t1, space7);

RWByteAddressBuffer g_ctl : register(u1, space5);
RWByteAddressBuffer g_list : register(u2, space5);

groupshared uint gs_total;
groupshared uint gs_kept;
groupshared uint gs_base;
groupshared float4 gs_history;
groupshared bool gs_admitted;

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
  const uint conf_word = SsrW(SSR_W_RT_CONF);
  const uint flags = conf_word >> SSR_RT_FLAGS_SHIFT;
  if ((flags & SSR_RT_FLAG_RESET) != 0u) {
    if (gid.x == 0u && gid.y == 0u && gi == 0u) {
      [unroll] for (uint i = 0u; i < SSR_RT_CTL_WORDS; ++i) {
        g_ctl.Store(i * 4u, 0u);
      }
    }
    return;
  }
  const uint rt_frame = SsrW(SSR_W_RT_FRAME);
  const uint slot = rt_frame % 3u;
  const uint previous = (rt_frame + 2u) % 3u;
  const uint next = (rt_frame + 1u) % 3u;
  const bool classified = g_handoff.Load(140u) != 0u;
  const bool clear = SsrW(SSR_W_RT_CAP) == 0u;
  if ((clear || !classified) && gid.x == 0u && gid.y == 0u && gi == 0u) {
    g_ctl.Store((SSR_RT_CTL_TOTAL + next) * 4u, 0u);
    g_ctl.Store((SSR_RT_CTL_LISTED + next) * 4u, 0u);
  }
  if (clear) return;
  const int2 ssr = SsrSize();
  const uint tiles_x = uint(ssr.x + 7) / 8u;
  const uint tiles_y = uint(ssr.y + 7) / 8u;
  int2 tile;
  if (classified) {
    const uint index = gid.x + gid.y * g_handoff.Load(144u);
    if (index >= g_tile_list.Load(0u)) return;
    const uint entry = g_tile_list.Load(4u + index * 4u);
    tile = int2(SsrLo(entry), SsrHi(entry));
  } else {
    const uint entry = SsrRtTileOf(gid.x, gid.y, tiles_x, tiles_x * tiles_y, rt_frame);
    tile = int2(entry % tiles_x, entry / tiles_x);
  }
  const int2 pix = tile * int(kSsrTileSize) + int2(gi & 7u, gi >> 3u);
  const uint capacity = SsrW(SSR_W_RT_CAP);
  const float keep = SsrRtKeep(capacity, g_ctl.Load((SSR_RT_CTL_TOTAL + previous) * 4u));
  if (gi == 0u) {
    if (classified) {
      gs_history = SsrTileHistory(tile);
      gs_admitted = BlueNoiseFrame(tile.x, tile.y, 0u, rt_frame, kBlueNoiseR2StepA) < keep;
    }
    gs_total = 0u;
    gs_kept = 0u;
    gs_base = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  bool candidate = false;
  bool kept = false;
  if (pix.x < ssr.x && pix.y < ssr.y) {
    const float2 conf = g_textures[SsrLo(conf_word)].Load(int3(pix, 0)).xy;
    candidate = SsrRtCandidate(SsrRtGroupIn(conf.x), conf.y, (conf_word >> SSR_RT_CONF_MASK_SHIFT) & 15u, SsrF(SSR_W_RT_THRESHOLD));
    if (classified) {
      const uint mode = SsrTileClass(uint(gs_history.w));
      const float respawn = SsrTileRtProbeShare(mode);
      const bool probe = BlueNoiseFrame(pix.x, pix.y, 1u, rt_frame, kBlueNoiseR2StepB) < respawn;
      candidate = candidate && SsrTileKeepRt(mode, SsrTileAge(uint(gs_history.w)), gs_history.z, conf.y, probe);
      kept = candidate && gs_admitted;
    } else {
      kept = candidate && SsrRtKept(SsrRtThinHash(pix.x, pix.y, rt_frame, (SsrW(SSR_W_FRAME) & SSR_FRAME_LEGACY_NOISE) != 0u), keep);
    }
  }
  uint rank = 0u;
  if (candidate) {
    uint before;
    InterlockedAdd(gs_total, 1u, before);
  }
  if (kept) {
    InterlockedAdd(gs_kept, 1u, rank);
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0u) {
    if (gs_total != 0u) {
      g_ctl.InterlockedAdd((SSR_RT_CTL_TOTAL + slot) * 4u, gs_total);
    }
    if (gs_kept != 0u) {
      uint base;
      g_ctl.InterlockedAdd((SSR_RT_CTL_LISTED + slot) * 4u, gs_kept, base);
      gs_base = base;
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (kept) {
    const uint index = gs_base + rank;
    if (index < capacity) {
      g_list.Store(index * 4u, uint(pix.x) | (uint(pix.y) << 16u));
    }
  }
}
