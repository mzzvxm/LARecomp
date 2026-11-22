// MCLA GPU renderer -- RtDenoise (CITY-RT package R6; rt_denoise_common.hlsli has the design): the two passes' bodies, written against a CONSUMER's hooks.
//
// A shader of a consumer is three lines: it defines the pass it wants (RT_DN_PASS_SPATIAL or RT_DN_PASS_TEMPORAL), includes its consumer header (which defines the hooks below, in HLSL, over its own images and
// push block) and this file, and calls the body from its entry point:
//     #define RT_DN_PASS_SPATIAL 1
//     #include "ssr_denoise_consumer.hlsli"
//     #include "rt_denoise_engine.hlsli"
//     [numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { RtDnSpatialMain(id); }
//
// The hooks (all of them for the spatial pass; the temporal pass needs the lower block too):
//   int2     RtDnSize()                 the signal's size in pixels
//   float4   RtDnSignal(int2 p)         the signal's own texel as it stands (passed through where the pixel is not filtered; its alpha is the consumer's and is kept)
//   float4   RtDnColourAt(int2 p)       the signal's rgb, w 1 where the pixel is part of the signal and 0 where not (the variance's and the clip's neighbourhood)
//   RtDnPixel RtDnLoad(int2 p)          the pixel with its guides (rt_denoise_common.hlsli)
//   uint     RtDnIteration()            the a-trous iteration of this dispatch: 0, 1, 2 (steps 1, 2, 4)
//   float    RtDnPixelAngle()           the angle one pixel of the signal spans, radians (2 tan(half fov y) over the height)
//   temporal only:
//   bool     RtDnHasHistory()           the frame has a history at all
//   RtDnPrevious RtDnToPrevious(float3 p)   a point of this frame's view space in the last frame's
//   float    RtDnHistoryDepthAt(int2 p) the receiver depth the history stored at a texel (0: none)
//   float4   RtDnHistoryAt(float2 uv)   the history's colour (rgb) and length (a), bilinear
// and RT_DN_PROFILE (RT_DN_PROFILE_SPECULAR or _DIFFUSE). The result is written to u0 of space 5, an RGBA16F image: the spatial pass's keeps the signal's alpha, the temporal pass's alpha is the
// history's new length.
// RT_DN_OUT_UNORM8 (RT-LIGHTS-D1 part 2, hlsl/rt_denoise_unorm8.hlsli): the result is an RGBA8_UNORM image holding a signal in 0..1 (a visibility) and the length in the alpha as n / 32; the temporal
// pass rounds the blended average at random (the consumer's float3 RtDnDither3(int2 p): three numbers in [0, 1) that change every frame) so that the 8 bit average neither stalls nor drifts, and the
// consumer decodes the history's length (RtDnHistoryAt). Without the macro nothing here changes.

#ifndef MCLA_GPU_RT_DENOISE_ENGINE_HLSLI
#define MCLA_GPU_RT_DENOISE_ENGINE_HLSLI

#include "rt_denoise_common.hlsli"

#ifdef RT_DN_OUT_UNORM8
#include "rt_denoise_unorm8.hlsli"
#endif

#ifdef __spirv__
#ifdef RT_DN_OUT_UNORM8
[[vk::image_format("rgba8")]]
#else
[[vk::image_format("rgba16f")]]
#endif
#endif
RWTexture2D<float4> g_dn_out : register(u0, space5);

#if defined(RT_DN_PASS_SPATIAL)

void RtDnSpatialMain(uint3 id) {
  const int2 size = RtDnSize();
  const int2 p = int2(id.xy);
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  // a colour that is not a number (see RtDnIsNumber) is black, in the pass-through as well; the alpha is the consumer's
  const float4 raw_signal = RtDnSignal(p);
  const float4 own = float4(RtDnSane(raw_signal.xyz), raw_signal.w);
  RtDnPixel c = RtDnLoad(p);
  c.colour = RtDnSane(c.colour);
  const float amount = RtDnMix(c.share);
  const uint step = 1u << RtDnIteration();
  if (!RtDnGuidesOk(c) || amount <= 0.0f) {
    g_dn_out[p] = own;
    return;
  }
  const float alpha = c.rough * c.rough;
  const float radius = RtDnRadius(RT_DN_PROFILE, alpha, c.hit, c.z, RtDnPixelAngle(), c.length);
  if (!RtDnStepTaken(radius, step)) {
    g_dn_out[p] = own;
    return;
  }
  // the neighbourhood's luminance variance (3 x 3 at this step, the pixels that are signal)
  float sum = 0.0f;
  float sum2 = 0.0f;
  float count = 0.0f;
  [unroll] for (int vy = -1; vy <= 1; ++vy) {
    [unroll] for (int vx = -1; vx <= 1; ++vx) {
      const int2 q = clamp(p + int2(vx, vy) * int(step), int2(0, 0), size - 1);
      const float4 t = RtDnColourAt(q);
      if (t.w > 0.0f && RtDnIsNumber3(t.xyz)) {
        const float l = RtDnLuma(t.x, t.y, t.z);
        sum += l;
        sum2 += l * l;
        count += 1.0f;
      }
    }
  }
  const float variance = RtDnVariance(sum, sum2, count);
  const float luma_centre = RtDnLuma(c.colour.x, c.colour.y, c.colour.z);
  const float depth_sigma = RtDnDepthSigma(RT_DN_PROFILE);
  const float normal_power = RtDnNormalPower(RT_DN_PROFILE);
  const float rough_sigma = RtDnRoughSigma(RT_DN_PROFILE);
  const float luma_sigma = RtDnLumaSigma(RT_DN_PROFILE);
  const float hit_sigma = RtDnHitSigma(RT_DN_PROFILE);

  float3 total = float3(0.0f, 0.0f, 0.0f);
  float weights = 0.0f;
  [loop] for (int dy = -2; dy <= 2; ++dy) {
    [loop] for (int dx = -2; dx <= 2; ++dx) {
      const int2 q = p + int2(dx, dy) * int(step);
      if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) {
        continue;
      }
      float w = RtDnKernel1(dx) * RtDnKernel1(dy);
      RtDnPixel t = c;
      if (dx != 0 || dy != 0) {
        t = RtDnLoad(q);
        if (!t.ok) {
          continue;
        }
        if (!RtDnGuidesOk(t) || !RtDnIsNumber3(t.colour)) {
          continue;   // guides or a colour that are not numbers would make the weights and the sum NaN: the tap is left out
        }
        const float plane = abs(dot(c.n, t.pos - c.pos));
        w *= RtDnDepthWeight(plane, c.z, depth_sigma) * RtDnNormalWeight(dot(c.n, t.n), normal_power) * RtDnRoughWeight(c.rough, t.rough, rough_sigma) *
             RtDnLumaWeight(luma_centre, RtDnLuma(t.colour.x, t.colour.y, t.colour.z), variance, luma_sigma) * RtDnHitWeight(c.hit, t.hit, hit_sigma);
      }
      total += t.colour * w;
      weights += w;
    }
  }
  const float3 filtered = weights > 1.0e-6f ? total / weights : c.colour;
  g_dn_out[p] = float4(lerp(own.xyz, filtered, amount), own.w);
}

#elif defined(RT_DN_PASS_TEMPORAL)

void RtDnTemporalMain(uint3 id) {
  const int2 size = RtDnSize();
  const int2 p = int2(id.xy);
  if (p.x >= size.x || p.y >= size.y) {
    return;
  }
  const float4 raw_signal = RtDnSignal(p);
  const float4 current = float4(RtDnSane(raw_signal.xyz), raw_signal.w);   // a colour that is not a number is black
  const RtDnPixel c = RtDnLoad(p);
  if (!RtDnGuidesOk(c)) {
    g_dn_out[p] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }
  // the last frame's result where it is the same reflection: moved by the reflection's own motion, accepted only where the surface itself was the same one
  float3 history = float3(0.0f, 0.0f, 0.0f);
  float length = 0.0f;
  bool used = false;
  if (RtDnHasHistory()) {
    const RtDnPrevious image = RtDnToPrevious(RtDnReprojectionPoint(c.pos, c.hit, c.rough));
    const RtDnPrevious plain = RtDnToPrevious(c.pos);
    if (image.ok && plain.ok) {
      const int2 q = clamp(int2(plain.u * float(size.x), plain.v * float(size.y)), int2(0, 0), size - 1);
      const float stored = RtDnHistoryDepthAt(q);
      if (stored > 0.0f && RtDnSameSurface(stored, plain.z)) {
        const float4 h = RtDnHistoryAt(float2(image.u, image.v));
        // a history that is not a number (a NaN texel in the bilinear footprint, an infinity a half float made of a colour too large) is no history: the pixel starts again from this frame, or it would stay for ever
        if (RtDnIsNumber3(h.xyz) && RtDnIsNumber(h.w)) {
          history = h.xyz;
          length = h.w;
          used = length > 0.0f;
        }
      }
    }
  }
  float3 colour = current.xyz;
  if (used) {
    // the variance clip: the box of the signal's own 3 x 3
    float3 lo = current.xyz;
    float3 hi = current.xyz;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float3 sum2 = float3(0.0f, 0.0f, 0.0f);
    float count = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
      [unroll] for (int dx = -1; dx <= 1; ++dx) {
        const float4 t = RtDnColourAt(clamp(p + int2(dx, dy), int2(0, 0), size - 1));
        if (t.w > 0.0f && RtDnIsNumber3(t.xyz)) {
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
    const float3 box_lo = float3(RtDnBoxLo(mean.x, sigma.x, lo.x, hi.x), RtDnBoxLo(mean.y, sigma.y, lo.y, hi.y), RtDnBoxLo(mean.z, sigma.z, lo.z, hi.z));
    const float3 box_hi = float3(RtDnBoxHi(mean.x, sigma.x, lo.x, hi.x), RtDnBoxHi(mean.y, sigma.y, lo.y, hi.y), RtDnBoxHi(mean.z, sigma.z, lo.z, hi.z));
    history = RtDnClipToBox(history, box_lo, box_hi);
    colour = lerp(current.xyz, history, RtDnHistoryWeight(length));
#ifdef RT_DN_OUT_UNORM8
    const float3 dither = RtDnDither3(p);
    colour = float3(RtDnStore8(colour.x, dither.x), RtDnStore8(colour.y, dither.y), RtDnStore8(colour.z, dither.z));
#endif
  }
#ifdef RT_DN_OUT_UNORM8
  g_dn_out[p] = float4(colour, RtDnEncodeLength(RtDnNextLength(length, used)));
#else
  g_dn_out[p] = float4(colour, RtDnNextLength(length, used));
#endif
}

#endif

#endif  // MCLA_GPU_RT_DENOISE_ENGINE_HLSLI
