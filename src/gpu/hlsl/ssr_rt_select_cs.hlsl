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
RWByteAddressBuffer g_trace_dispatch : register(u3, space5);
RWByteAddressBuffer g_ranks : register(u2, space7);

// One uint2 mask per tile, one exclusive prefix per rotated tile, then
// one prefix per block of 64 tiles. Match ssr_rt::RankBytes and PackSelect.
static const uint kSelectShift = 28u;
static const uint kScanStride = 1024u;
static const uint kCount = 1u, kScan = 2u, kBlocks = 3u, kEmit = 4u;
static const uint kTileLanes = 64u;
groupshared uint gs_scan[kTileLanes];
groupshared uint gs_mask[2];

uint ExclusiveScan(uint lane, uint value) {
  gs_scan[lane] = value;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint offset = 1u; offset < kTileLanes; offset <<= 1u) {
    const uint before = lane >= offset ? gs_scan[lane - offset] : 0u;
    GroupMemoryBarrierWithGroupSync();
    gs_scan[lane] += before;
    GroupMemoryBarrierWithGroupSync();
  }
  return gs_scan[lane] - value;
}

void ScanTileBlock(uint block, uint lane, uint tile_count, uint rotation) {
  const uint block_count = (tile_count + kTileLanes - 1u) / kTileLanes;
  if (block >= block_count) return;
  const uint ordered = block * kTileLanes + lane;
  uint samples = 0u;
  if (ordered < tile_count) {
    const uint2 mask = g_ranks.Load2(8u * ((ordered + rotation) % tile_count));
    samples = countbits(mask.x) + countbits(mask.y);
  }
  const uint before = ExclusiveScan(lane, samples);
  if (ordered < tile_count) g_ranks.Store(4u * (2u * tile_count + ordered), before);
  if (lane == kTileLanes - 1u) g_ranks.Store(4u * (3u * tile_count + block), before + samples);
}

void ScanBlocks(uint lane, uint tile_count, uint capacity, uint slot) {
  const uint blocks = (tile_count + kTileLanes - 1u) / kTileLanes;
  const uint per_lane = (blocks + kTileLanes - 1u) / kTileLanes;
  const uint first = lane * per_lane;
  const uint last = min(first + per_lane, blocks);
  uint sum = 0u;
  for (uint block = first; block < last; ++block) {
    sum += g_ranks.Load(4u * (3u * tile_count + block));
  }
  uint before = ExclusiveScan(lane, sum);
  for (uint block = first; block < last; ++block) {
    const uint address = 4u * (3u * tile_count + block);
    const uint count = g_ranks.Load(address);
    g_ranks.Store(address, before);
    before += count;
  }
  if (lane == kTileLanes - 1u) {
    const uint listed = SsrRtListed(before, capacity);
    g_ctl.Store(4u * (SSR_RT_CTL_LISTED + slot), listed);
    g_trace_dispatch.Store3(0u, uint3((listed + kTileLanes - 1u) / kTileLanes, 1u, 1u));
  }
}

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
  const uint pass = SsrW(SSR_W_RT_CAP) >> kSelectShift;
  const uint capacity = SsrW(SSR_W_RT_CAP) & ((1u << kSelectShift) - 1u);
  const bool classified = pass != 0u;
  const bool clear = capacity == 0u;
  if ((clear || !classified) && gid.x == 0u && gid.y == 0u && gi == 0u) {
    g_ctl.Store((SSR_RT_CTL_TOTAL + next) * 4u, 0u);
    g_ctl.Store((SSR_RT_CTL_LISTED + next) * 4u, 0u);
  }
  if (clear) return;
  const int2 ssr = SsrSize();
  const uint tiles_x = uint(ssr.x + 7) / 8u;
  const uint tiles_y = uint(ssr.y + 7) / 8u;
  const uint tile_count = tiles_x * tiles_y;
  const uint rotation = SsrRtTileOf(0u, 0u, tiles_x, tile_count, rt_frame);
  if (pass == kScan) {
    ScanTileBlock(gid.x + gid.y * kScanStride, gi, tile_count, rotation);
    return;
  }
  if (pass == kBlocks) {
    ScanBlocks(gi, tile_count, capacity, slot);
    return;
  }
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
  const uint canonical = uint(tile.x) + uint(tile.y) * tiles_x;
  if (pass == kEmit) {
    const uint2 mask = g_ranks.Load2(8u * canonical);
    const uint bit = 1u << (gi & 31u);
    const uint half_mask = gi < 32u ? mask.x : mask.y;
    if ((half_mask & bit) == 0u) return;
    const uint ordered = SsrRtRankedTile(canonical, rotation, tile_count);
    const uint block = ordered / kTileLanes;
    const uint prefix = g_ranks.Load(4u * (2u * tile_count + ordered))
                      + g_ranks.Load(4u * (3u * tile_count + block));
    const uint rank = (gi < 32u ? 0u : countbits(mask.x)) + countbits(half_mask & (bit - 1u));
    if (prefix + rank < capacity) {
      g_list.Store((prefix + rank) * 4u, uint(pix.x) | (uint(pix.y) << 16u));
    }
    return;
  }
  const float keep = SsrRtKeep(capacity, g_ctl.Load((SSR_RT_CTL_TOTAL + previous) * 4u));
  if (gi == 0u) {
    if (classified) {
      gs_history = SsrTileHistory(tile);
      gs_admitted = BlueNoiseFrame(tile.x, tile.y, 0u, rt_frame, kBlueNoiseR2StepA) < keep;
    }
    gs_total = 0u;
    gs_kept = 0u;
    gs_mask[0] = gs_mask[1] = 0u;
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
    if (pass == kCount) {
      InterlockedOr(gs_mask[gi / 32u], 1u << (gi & 31u));
    } else {
      InterlockedAdd(gs_kept, 1u, rank);
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0u) {
    if (gs_total != 0u) {
      g_ctl.InterlockedAdd((SSR_RT_CTL_TOTAL + slot) * 4u, gs_total);
    }
    if (pass == kCount) {
      g_ranks.Store2(8u * canonical, uint2(gs_mask[0], gs_mask[1]));
    } else if (gs_kept != 0u) {
      uint base;
      g_ctl.InterlockedAdd((SSR_RT_CTL_LISTED + slot) * 4u, gs_kept, base);
      gs_base = base;
    }
  }
  if (pass == kCount) return;
  GroupMemoryBarrierWithGroupSync();
  if (kept) {
    const uint index = gs_base + rank;
    if (index < capacity) {
      g_list.Store(index * 4u, uint(pix.x) | (uint(pix.y) << 16u));
    }
  }
}
