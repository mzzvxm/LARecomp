#include "host_pass_common.hlsli"
#include "rtao_denoise_math.hlsli"
#include "ao_rate_rules.hlsli"

#ifndef RTAO_GI
#define RTAO_GI 0
#endif

struct RtAoComposePush {
  uint4 terms;       // signal, traced depth, AO depth, screen-space term
  uint4 guides;      // bounce, raw signal, id/share, stencil
  uint4 sizes;       // traced.xy, plan.ao.xy
  uint4 stencil;     // render.xy, host sample and MS flag, car AO term
  float power;
  float floor_;
  uint flags;        // bit 0: blend with screen space, bit 1: signal has a history length
  uint view;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RtAoComposePush> g_push;
#if RTAO_GI
[[vk::image_format("rgba16f")]]
#else
[[vk::image_format("r16f")]]
#endif
#else
ConstantBuffer<RtAoComposePush> g_push : register(b0, space4);
#endif
#if RTAO_GI
RWTexture2D<float4> g_term : register(u0, space4);
#else
RWTexture2D<float> g_term : register(u0, space4);
#endif

static const uint kMissing = 0xFFFFFFFFu;
bool RtAoIsVehicle(float depth) { return (asuint(depth) & 1u) != 0u; }

uint StencilId(int2 p) {
  const uint slot = g_push.guides.w;
  const uint params = g_push.stencil.z;
  uint id;
  if ((params & 0x100u) != 0u) {
    id = StencilOf(g_utextures_ms[slot].Load(p, params & 0xFFu)) >> 2u;
  } else {
    id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2u;
  }
  return id <= 23u ? id : 0u;
}

uint ReceiverId(int2 p) {
  const bool full = all(g_push.sizes.zw == g_push.stencil.xy);
  if (full) return StencilId(p);
  // AO's half-size depth uses the first vehicle id in the 2x2 block.
  [unroll] for (int k = 0; k < 4; ++k) {
    const int2 q = min(p * 2 + int2(k & 1, k >> 1), int2(g_push.stencil.xy) - 1);
    const uint id = StencilId(q);
    if (id != 0u) return id;
  }
  return 0u;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_push.sizes.zw)) return;

  const int2 p = int2(id.xy);
  const int3 px = int3(p, 0);
  const float z = g_textures[g_push.terms.z].Load(px).x;
  const bool vehicle = RtAoIsVehicle(z);
  const bool compare_id = vehicle && g_push.guides.z != kMissing && g_push.guides.w != kMissing;
  const uint receiver_id = compare_id ? ReceiverId(p) : 0u;
  const float2 uv = (float2(p) + 0.5f) / float2(g_push.sizes.zw);
  const float2 q = all(g_push.sizes.xy == g_push.sizes.zw) ? float2(p) : uv * float2(g_push.sizes.xy) - 0.5f;
  const int2 base = int2(floor(q));
  const float2 f = frac(q);
  float weights = 0.0f, judged = 0.0f, open_sum = 0.0f;
  float debug_sum = 0.0f;
  [unroll] for (int y = 0; y < 2; ++y) {
    [unroll] for (int x = 0; x < 2; ++x) {
      const int3 tap = int3(clamp(base + int2(x, y), 0, int2(g_push.sizes.xy) - 1), 0);
      const float bilinear = (x == 0 ? 1.0f - f.x : f.x) * (y == 0 ? 1.0f - f.y : f.y);
      if (bilinear <= 0.0f) continue;
      const float depth = g_textures[g_push.terms.y].Load(tap).x;
      if (!(z > 0.0f) || !(depth > 0.0f)) continue;
      const bool same_vehicle = RtAoIsVehicle(depth) == vehicle;
      const bool same_id = !compare_id || uint(g_textures[g_push.guides.z].Load(tap).x) == receiver_id;
      const float w = RtAoUpsampleWeight(bilinear, depth, z, same_vehicle && same_id);
      if (w <= 0.0f) continue;
      const float4 signal = g_textures[g_push.terms.x].Load(tap);
      const float4 raw = g_push.guides.y == kMissing ? signal : g_textures[g_push.guides.y].Load(tap);
      const float valid = saturate(raw.g);
      const float covered = w * valid;
      open_sum += signal.r * covered;
      judged += covered;
      weights += w;
      if (g_push.view == 1u) debug_sum += raw.r * covered;
      if (g_push.view == 4u && (g_push.flags & 2u) != 0u) {
        const float age = (g_push.flags & 4u) != 0u ? float(AoHistoryAge(signal.z)) : signal.a;
        debug_sum += age * (covered / 32.0f);
      }
      if (g_push.view == 5u) debug_sum += raw.b * covered;
      if (g_push.view == 6u && g_push.guides.z != kMissing) {
        debug_sum += g_textures[g_push.guides.z].Load(tap).x * (covered / 23.0f);
      }
    }
  }
  const float coverage = weights > 1.0e-8f ? saturate(judged / weights) : 0.0f;
  // Estimate the traced term from judged taps. Coverage mixes in the
  // screen-space term once, where those taps do not cover the receiver.
  const float open = judged > 1.0e-8f ? saturate(open_sum / judged) : 1.0f;
  const float screen = g_push.terms.w == kMissing ? 1.0f : g_textures[g_push.terms.w].Load(px).x;
  float rt = RtAoVisibility(open, g_push.power, g_push.floor_);
  if (vehicle && g_push.stencil.w != kMissing) {
    const float car_open = g_textures[g_push.stencil.w].Load(px).x;
    rt = min(rt, RtAoVisibility(car_open, g_push.power, g_push.floor_));
  }
  float term = RtAoCoverage(rt, coverage, screen, (g_push.flags & 1u) != 0u);
  if (g_push.view == 2u) {
    term = open;
  } else if (g_push.view == 3u) {
    term = coverage;
  } else if (g_push.view != 0u) {
    term = judged > 1.0e-8f ? saturate(debug_sum / judged) : 0.0f;
  }
#if RTAO_GI
  g_term[p] = RtAoWithBounce(term, g_textures[g_push.guides.x].Load(px));
#else
  g_term[p] = term;
#endif
}
