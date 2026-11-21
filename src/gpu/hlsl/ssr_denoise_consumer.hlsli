// MCLA GPU renderer -- RtDenoise (CITY-RT package R6; hlsl/rt_denoise_common.hlsli has the design) for the screen-space reflections' ray traced fallback: the consumer's hooks (hlsl/rt_denoise_engine.hlsli lists them).
//
// The signal is the reflections' ACCUMULATED image (ssr_temporal_ps.hlsl: rgb the reflection averaged over frames, alpha the receiver's depth, 0 where nothing was traced), at the SSR size. The guides come
// from what the effect already has: the accumulated image's alpha is the pixel's depth (so no depth image is read), RT1's world normal and RT3's smoothness at the pixel's 2 x 2 block (the block's first
// pixel: a pixel across an edge differs in normal and roughness and the weights say so), the trace image's alpha for the hit distance (the virtual image's, SsrHitDistance: a miss is kSsrMissLength), and
// the confidence image for the share: the signal a pixel takes from the ray traced fallback is 1 - the picture's confidence (a pixel of the picture's own hit, confidence 1, is not touched; a pixel the
// picture could not speak for is filtered by the whole share). A class group of 0 (a pixel no level traces) is no share.
//
// Push block: the SSR block (ssr.cpp OnTrigger's `push`, the lens, the camera's axes, the planes, the Hi-Z, the wetness and the smoothness floor) with three words of this pass's own set over words the
// pass does not read for their first meaning:
//   SSR_W_SRC   the signal's slot (low) | the trace image's slot << 16 (the iteration's input: the accumulated image, then the previous iteration's output)
//   SSR_W_COL01 the confidence image's slot (low 16 bits)
//   SSR_W_COL23 the iteration (bits 0-3): 0, 1, 2 for steps 1, 2, 4
//   SSR_W_THICK float: the history's length in frames the accumulation stands for (the kernel shrinks as it grows)
// Compilation: SSR_MS 0 (plain planes; a frame over a multisampled colour keeps the plain pass: ssr.cpp).

#ifndef MCLA_GPU_SSR_DENOISE_CONSUMER_HLSLI
#define MCLA_GPU_SSR_DENOISE_CONSUMER_HLSLI

#include "ssr_pass.hlsli"
#include "rt_denoise_common.hlsli"

#define RT_DN_PROFILE RT_DN_PROFILE_SPECULAR
#define SSRDN_W_CONF SSR_W_COL01
#define SSRDN_W_ITERATION SSR_W_COL23
#define SSRDN_W_LENGTH SSR_W_THICK

int2 RtDnSize() { return SsrSize(); }
uint RtDnIteration() { return SsrW(SSRDN_W_ITERATION) & 15u; }
float RtDnPixelAngle() { return 2.0f * SsrF(SSR_W_TAN_Y) / float(max(SsrSize().y, 1)); }
float4 RtDnSignal(int2 p) { return g_textures[SsrLo(SsrW(SSR_W_SRC))].Load(int3(p, 0)); }
float4 RtDnColourAt(int2 p) {
  const float4 s = RtDnSignal(p);
  return float4(s.x, s.y, s.z, s.w > 0.0f ? 1.0f : 0.0f);
}

RtDnPixel RtDnLoad(int2 pix) {
  RtDnPixel r;
  r.ok = false;
  r.colour = float3(0.0f, 0.0f, 0.0f);
  r.z = 0.0f;
  r.pos = float3(0.0f, 0.0f, 0.0f);
  r.n = float3(0.0f, 0.0f, -1.0f);
  r.rough = 1.0f;
  r.hit = 0.0f;
  r.share = 0.0f;
  r.length = SsrF(SSRDN_W_LENGTH);
  const int2 ssr = SsrSize();
  if (pix.x < 0 || pix.y < 0 || pix.x >= ssr.x || pix.y >= ssr.y) {
    return r;
  }
  const float4 signal = RtDnSignal(pix);
  if (!(signal.w > 0.0f) || signal.w >= kSsrSky * 0.5f) {
    return r;   // nothing was traced here (the accumulated image's alpha is the receiver's depth)
  }
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), signal.w, tan_x, tan_y);
  const int2 texel = min(pix * 2, SsrRender() - 1);
  const SsrSurface surface = SsrSurfaceAt(texel, false);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  float3 n = float3(0.0f, 0.0f, -1.0f);
  const float4 g_normal = SsrViewNormalAt(texel, pc);
  if (g_normal.w >= 0.5f) {
    n = g_normal.xyz;
  } else {
    n = SsrDepthNormal(SsrHz(), pix.x, pix.y, ssr.x, ssr.y, signal.w, pc, tan_x, tan_y);
  }
  const float4 trace = g_textures[SsrHi(SsrW(SSR_W_SRC))].Load(int3(pix, 0));
  const float2 conf = g_textures[SsrLo(SsrW(SSRDN_W_CONF))].Load(int3(pix, 0)).xy;
  const uint group = uint(conf.x * 3.0f + 0.5f);
  r.ok = true;
  r.colour = signal.xyz;
  r.z = signal.w;
  r.pos = pc;
  r.n = n;
  r.rough = 1.0f - saturate(smoothness);
  r.hit = trace.w != 0.0f ? SsrHitDistance(trace.w) : 0.0f;
  r.share = group != 0u ? saturate(1.0f - conf.y) : 0.0f;
  return r;
}

#endif  // MCLA_GPU_SSR_DENOISE_CONSUMER_HLSLI
