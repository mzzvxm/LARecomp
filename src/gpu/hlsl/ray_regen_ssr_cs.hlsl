// MCLA GPU renderer -- AMD FSR Ray Regeneration: the screen-space reflections' guide and signal pass (ray_regen_guides.h, perf/RAY-REGEN.md, packages RG-2 and RG-3).
//
// Recorded at the trigger right after the reflections' trace, only in a frame that Ray Regeneration takes. One thread is one texel of the trace's image (the SSR size: half of the picture, rounded
// up). Ray Regeneration is a decoupled denoiser, so it does not get the colour: it gets the reflection's own radiance as a SIGNAL next to the guides it moves and weighs it by, all at the SSR size
// (the whole dispatch is at that size: a denoiser of a half size trace, which is what one ray a half-size pixel is):
//
//   u0  signal           RGBA16F  the trace's radiance (never below 0), a the ray's hit distance (0: nothing was traced here)
//   u1  linear depth     R32F     the signed view-space z of the receiver (negative ahead), or kRgNoDepth for a texel nothing was traced for: outside the dispatch's bounds, so the denoiser leaves it alone
//   u2  motion           RGBA16F  PreviousUV - CurrentUV of the receiver (the camera's own motion: the class gate keeps vehicles out of the receivers), b the signed linear depth delta
//   u3  normals          RGBA16F  the octahedral world normal, the linear roughness the trace used (the wet road's), material type 0
//   u4  specular albedo  RGBA16F  the Fresnel term the reflection's resolve multiplies the denoised radiance by (linear: the dispatch says so)
//
// The receiver of a texel is the one the trace used: the pixel of the 2x2 block whose depth is the Hi-Z's (ssr_trace_ps.hlsl). A texel the trace wrote nothing for (alpha 0: the sky, a surface that
// reflects nothing, a ray pointing back at the camera) is not traced and is passed through. The class gate is not repeated here: the trace's own alpha is its answer.
//
// The push block is the apply's (ssr_rules.h: the planes, the Hi-Z, the lens, the camera's right and up axes, the wetness, the trace's slot in the low half of SSR_W_SRC; the lens of the LAST frame in
// the near and distance words, as the accumulation's) plus 16 words (ray_regen_guides_rules.h push::): the camera's motion rows, the jitter, the depth sign and the flags. Layout (ray_regen_guides.cpp):
// sets 0 to 3 the bindless texture set four times, set 4 the heap's samplers (not read), set 5 the five images, the push block at (b0, space4). Plain planes only (a frame with host MSAA is not taken).

// CHECK (nothing here ran on a Radeon: perf/RAY-REGEN.md section 8): C3 the motion's sign and units in AMD's debug view; C16 the dispatch's view matrix has no translation; C17 the jitter's sign and units (render
// pixels at the dispatch's half size, +y down); C18 a road's negative difference is floored at 0 (some doubling may remain); C20 a miss's hit distance is the effect's 4096, not FP16_MAX; C21 the specular albedo
// is the resolve's Fresnel without the class gain.

#define SSR_REGEN_EXTRA 1
#include "ssr_pass.hlsli"
#include "ray_regen_common.hlsli"

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_signal : register(u0, space5);
#ifdef __spirv__
[[vk::image_format("r32f")]]
#endif
RWTexture2D<float> g_depth : register(u1, space5);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_motion : register(u2, space5);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_normals : register(u3, space5);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_specular : register(u4, space5);

// The extra words (ray_regen_guides_rules.h push::).
#define RG_X_MOTION 0      // 12 floats: the three rows of the camera's 3x4 motion (this frame's view space into the last frame's)
#define RG_X_JITTER_X 12   // float: this frame's jitter in render pixels, +y down (0 when the frame is not jittered)
#define RG_X_JITTER_Y 13
#define RG_X_DEPTH_SIGN 14 // float: -1 (the game's view space looks along -z), +1 for the mirrored convention
#define RG_X_FLAGS 15      // bit 0: the motion rows are usable (a previous frame exists)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 pix = int2(id.xy);
  const int2 ssr = SsrSize();
  if (pix.x >= ssr.x || pix.y >= ssr.y) {
    return;
  }
  const float depth_sign = SsrXF(RG_X_DEPTH_SIGN);
  const uint trace_slot = SsrLo(SsrW(SSR_W_SRC));
  const float4 trace = g_textures[trace_slot].Load(int3(pix, 0));
  const float z_block = SsrHizAt(SsrHz(), pix.x, pix.y);
  const bool traced = trace.w != 0.0f && z_block < kSsrSky * 0.5f;
  if (!traced) {
    g_signal[pix] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_depth[pix] = RgDepthTexel(false, 0.0f, depth_sign);
    g_motion[pix] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_normals[pix] = RgNormalsTexel(float3(0.0f, 1.0f, 0.0f), 1.0f, 0u);
    g_specular[pix] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    return;
  }

  // the receiver: the pixel of the 2x2 block whose depth is the Hi-Z's, as the trace picks it
  const int2 render = SsrRender();
  const int2 last = render - 1;
  const int2 base = pix * 2;
  const int2 t0 = min(base, last);
  const int2 t1 = min(base + int2(1, 0), last);
  const int2 t2 = min(base + int2(0, 1), last);
  const int2 t3 = min(base + int2(1, 1), last);
  const uint pick = SsrPickInBlock(SsrLinearAt(t0), SsrLinearAt(t1), SsrLinearAt(t2), SsrLinearAt(t3), z_block);
  const int2 texel = pick == 0u ? t0 : (pick == 1u ? t1 : (pick == 2u ? t2 : t3));
  const SsrSurface surface = SsrSurfaceAt(texel, false);
  const float smoothness = SsrWetSmooth(surface.cls, surface.smoothness, SsrF(SSR_W_WET));
  const float roughness = 1.0f - smoothness;

  // the normal and the Fresnel term, as the reflection's resolve has them: RT1's world normal turned into the view space for N.V
  const float tan_x = SsrF(SSR_W_TAN_X);
  const float tan_y = SsrF(SSR_W_TAN_Y);
  const float3 pc = SsrViewPos(float(pix.x) + 0.5f, float(pix.y) + 0.5f, float(ssr.x), float(ssr.y), z_block, tan_x, tan_y);
  const float2 rt1 = SsrRt1At(texel);
  const float3 n_world = GbRt1Valid(rt1) ? GbRt1Decode(rt1) : float3(0.0f, 1.0f, 0.0f);
  const float4 g_normal = SsrViewNormalAt(texel, pc);
  const float nov = g_normal.w < 0.5f ? 0.0f : saturate(dot(g_normal.xyz, pc * (-1.0f / length(pc))));
  float3 f0 = float3(0.04f, 0.04f, 0.04f);
  if (surface.cls == uint(GB_CLASS_METAL)) {
    const float4 rt2 = SsrRt2At(texel);
    f0 = (rt2.x + rt2.y + rt2.z) > 0.0f ? GbAlbedoDecode(rt2.xyz) : float3(0.6f, 0.6f, 0.6f);
  }
  const float2 ab = SsrEnvBrdf(roughness, nov);

  // the motion of the receiver: its unjittered place, moved into the last frame by the camera's own motion (the class gate keeps vehicles out of the receivers)
  const float2 jitter = float2(SsrXF(RG_X_JITTER_X), SsrXF(RG_X_JITTER_Y));
  const float2 pos = float2(texel) + 0.5f - jitter;
  const float2 cur = pos / float2(render);
  float4 motion = float4(0.0f, 0.0f, 0.0f, 0.0f);
  if ((SsrXW(RG_X_FLAGS) & 1u) != 0u) {
    const float t_texel = SsrLinearAt(texel);
    const float3 point_view = SsrViewPos(pos.x, pos.y, float(render.x), float(render.y), t_texel, tan_x, tan_y);
    const float4 m0 = float4(SsrXF(0u), SsrXF(1u), SsrXF(2u), SsrXF(3u));
    const float4 m1 = float4(SsrXF(4u), SsrXF(5u), SsrXF(6u), SsrXF(7u));
    const float4 m2 = float4(SsrXF(8u), SsrXF(9u), SsrXF(10u), SsrXF(11u));
    const SsrPrevious previous = SsrToPrevious(m0, m1, m2, point_view, SsrF(SSR_W_PREV_TAN_X), SsrF(SSR_W_PREV_TAN_Y));
    motion = RgMotionTexel(cur, previous.u, previous.v, previous.z, t_texel, previous.z > 1.0e-3f, depth_sign);
    // not finite: no motion
    if (!all(abs(motion.xyz) < float3(1.0e30f, 1.0e30f, 1.0e30f))) {
      motion = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
  }

  g_signal[pix] = RgSignalTexel(trace.xyz, SsrHitDistance(trace.w), true);
  g_depth[pix] = RgDepthTexel(true, z_block, depth_sign);
  g_motion[pix] = motion;
  g_normals[pix] = RgNormalsTexel(n_world, roughness, 0u);
  g_specular[pix] = float4(RgSpecularAlbedo(f0, ab), 1.0f);
}
