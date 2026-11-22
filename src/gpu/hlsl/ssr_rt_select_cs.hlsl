// MCLA GPU renderer -- the ray traced fallback of the screen-space reflections (CITY-RT package R4; ssr_rt.h, ssr_rt_rules.h, hlsl/ssr_rt_common.hlsli), pass 1: the candidates, listed tile by tile.
//
// One group of 8 x 8 threads a tile of the SSR image (the trace's size: half the picture's), over the CONFIDENCE image the trace wrote (R8G8_UNORM: r the class group, g the picture's share of the pixel's
// reflection). A pixel is a CANDIDATE where its group is one the level traces (the push's group mask) and its confidence is under the threshold. The candidates are counted (before the cap) and, after the
// cap's thinning (hlsl/ssr_rt_common.hlsli SsrRtKeep / SsrRtThinHash: a stable share of them, the share from the candidates the LAST frame counted), listed: the group reserves a range of the list with one
// atomic add and writes its survivors' pixels (x | y << 16) into it, so the entries of a tile are neighbours and the trace pass's waves read neighbouring pixels and trace neighbouring rays. The entries
// past the capacity are dropped (the order of the tiles is turned by a prime stride a frame, so it is not the same corner that goes without).
//
// The counters (u1 of the output set, ssr_rt_common.hlsli SSR_RT_CTL_*): three rotating frames of two words, the candidates counted and the candidates listed. A frame uses slot rt_frame % 3, reads the last
// frame's total from slot (rt_frame + 2) % 3 and clears slot (rt_frame + 1) % 3 for the next: nothing is cleared by the host and nothing is read back for the cap. The first frame ever (SSR_RT_FLAG_RESET)
// only clears all the control words.
//
// Layout (ssr_rt.cpp): the SSR host pass layout (sets 0 to 3 the bindless texture set bound four times, set 4 the samplers, the push block at b0 space4: ssr_pass.hlsli), set 5 this pass's own
// (u0 the trace image, unused here; u1 the control and statistics words; u2 the list), set 6 the ray tracing set (unused here). Shader Model 6.0: no rays.

#include "ssr_pass.hlsli"
#include "ssr_rt_common.hlsli"

RWByteAddressBuffer g_ctl : register(u1, space5);
RWByteAddressBuffer g_list : register(u2, space5);

groupshared uint gs_total;
groupshared uint gs_kept;
groupshared uint gs_base;

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
  if (gid.x == 0u && gid.y == 0u && gi == 0u) {
    g_ctl.Store((SSR_RT_CTL_TOTAL + next) * 4u, 0u);
    g_ctl.Store((SSR_RT_CTL_LISTED + next) * 4u, 0u);
  }
  const int2 ssr = SsrSize();
  const uint tiles_x = uint(ssr.x + 7) / 8u;
  const uint tiles_y = uint(ssr.y + 7) / 8u;
  const uint tile = SsrRtTileOf(gid.x, gid.y, tiles_x, tiles_x * tiles_y, rt_frame);
  const int2 pix = int2(int(tile % tiles_x) * 8 + int(gi & 7u), int(tile / tiles_x) * 8 + int(gi >> 3u));
  if (gi == 0u) {
    gs_total = 0u;
    gs_kept = 0u;
    gs_base = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  const uint capacity = SsrW(SSR_W_RT_CAP);
  const float keep = SsrRtKeep(capacity, g_ctl.Load((SSR_RT_CTL_TOTAL + previous) * 4u));
  bool candidate = false;
  bool kept = false;
  if (pix.x < ssr.x && pix.y < ssr.y) {
    const float2 conf = g_textures[SsrLo(conf_word)].Load(int3(pix, 0)).xy;
    candidate = SsrRtCandidate(SsrRtGroupIn(conf.x), conf.y, (conf_word >> SSR_RT_CONF_MASK_SHIFT) & 15u, SsrF(SSR_W_RT_THRESHOLD));
    kept = candidate && SsrRtKept(SsrRtThinHash(pix.x, pix.y, rt_frame, (SsrW(SSR_W_FRAME) & SSR_FRAME_LEGACY_NOISE) != 0u), keep);
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
