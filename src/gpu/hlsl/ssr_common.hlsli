// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1 and GB-I2): the pure functions the three passes share, and the push block's words.
//
// The passes (ssr_trace_ps, ssr_temporal_ps, ssr_apply_ps) are full-screen triangles on a pipeline layout of their own: the host pass layout with a push range of 128 bytes (the AO's
// way, ao.cpp), 32 words read through SsrW(). Everything here that does not touch a texture lives in this file and in ssr_trace.hlsli so that src/gpu/ssr_test.cpp can compile them AS C++
// (through a shim of the HLSL types) and check them against analytic scenes: the class gate, the wet road, the environment BRDF, the GGX visible-normal sampling, the noise, the fades, the
// virtual image's reprojection. So the text keeps to a subset both languages read: scalar and small vector arithmetic with .x .y .z .w, no swizzles of more than one component, no `out`
// parameters, no `static const` arrays (but the blue noise tile's, which is hlsl/blue_noise.hlsli: plain enough for both languages), no attributes ([loop] is SSR_LOOP, which the passes define).
//
// View space is the AO's (hlsl/ao_common.hlsli): x right, y up, z forward = the linear depth t, in the game's world units; pixels y down; the guest's depth is 1 near and 0 far. The
// world's y is up. A "texel" is a pixel of the Hi-Z's level 0 (the AO's traced size); "SSR pixel" is a pixel of the SSR images, which are half the picture's size (ssr_rules.h SsrSize).
//
// THE MATERIAL. RT3 (gbuffer_types.h) holds the smoothness s (r) and the class + flags (g); the class table decides what reflects (GB-F1 flags the road, sidewalk, metal, glass, water and
// the tunnel's tiles as SSR eligible), this file's SsrClassGain says how much of the term each class takes and keeps the car's paint and chrome out (they keep their own reflections: the
// game's env map, the self-reflection history, the ray traced ones). The GGX alpha is (1 - s)^2 (gbuffer_rules.h AlphaOfSmoothness).
//
// THE STUB. GB-W2 writes RT3 now, so the frame has a material plane and the stub is only the fallback for a frame without one (and the debug setting mcla_gpu_ssr_stub): SsrStubGain gives every pixel whose
// world normal points up (the road, a flat roof) the class ROAD with the stub's smoothness (a debug number), and the vehicle mark of the depth keeps the cars out. The pictures it makes are not the feature's.

#ifndef MCLA_GPU_SSR_COMMON_HLSLI
#define MCLA_GPU_SSR_COMMON_HLSLI

#include "gbuffer_common.hlsli"
#include "ao_gbuffer_normal.hlsli"
#include "nan_guard.hlsli"
#include "blue_noise.hlsli"

#ifndef SSR_LOOP
#define SSR_LOOP
#endif

// ---- the push block's words (ssr_rules.h push::, which the test compares with these) -------------------------------------------------------------------------------------------------
// 32 words of 32 bits; a slot is 16 bits (bindless slots are below 32768), two to a word, 0xFFFF for none; a float is a word read with asfloat.
#define SSR_W_DEPTH_RT1 0      // depth plane | RT1 << 16: the guest's depth plane (Texture2DMS when SSR_FLAG_MS) and the G-buffer's normal plane
#define SSR_W_RT2_RT3 1        // RT2 | RT3 << 16; temporal: the AO's vehicle table's slot in the low 16 bits (SSR_W_VEH_TABLE), 0xFFFF none
#define SSR_W_SRC 2            // the pass's own images: trace: the road view (SSR_W_ROAD_VIEW); temporal: the trace | the history << 16; apply: the accumulated image | the trace << 16 (the debug views)
#define SSR_W_HIZ01 3          // the Hi-Z's levels 0 | 1 << 16
#define SSR_W_HIZ23 4
#define SSR_W_HIZ45 5
#define SSR_W_COL01 6          // colour_post's levels 0 | 1 << 16
#define SSR_W_COL23 7
#define SSR_W_RENDER 8         // the picture's size in host pixels: width | height << 16
#define SSR_W_FLAGS 9          // sample0 (4 bits) | hiz levels << 4 | colour levels << 8 | SSR_FLAG_* << 12 | rays - 1 << 18 | view << 20 | max iterations << 24
#define SSR_W_FRAME 10         // trace, apply: the frame's number (the noise) in bits 0-30, bit 31 SSR_FRAME_LEGACY_NOISE; temporal: the weight the history keeps (float)
#define SSR_W_LIN0 11          // the depth chain's level 0 (the vehicle mark) | the depth target's stencil plane << 16 (the trace: the vehicle a hit is on; 0xFFFF none)
#define SSR_W_TAN_X 12         // floats: tan of the half field of view, x then y
#define SSR_W_TAN_Y 13
#define SSR_W_LIN_X 14         // the projection's depth rows, ScreenCamera::lin
#define SSR_W_LIN_Y 15
#define SSR_W_LIN_Z 16
#define SSR_W_LIN_W 17
#define SSR_W_NEAR 18          // float: the near plane's distance; temporal: tan x of the last frame's lens
#define SSR_W_MAX_DIST 19      // float: the longest ray, world units; temporal: tan y of the last frame's lens
#define SSR_W_RIGHT 20         // 3 floats: the camera's right axis in world space; temporal: the motion's rows (12 floats, words 20 to 31)
#define SSR_W_UP 23            // 3 floats: the camera's up axis in world space
#define SSR_W_STRENGTH 26      // float: the term's strength; trace: the share of the road view taken out where a ray finds a reflection (SSR_W_FALLBACK)
#define SSR_W_MIN_SMOOTH 27    // float: pixels rougher than this are not reflected
#define SSR_W_STUB_SMOOTH 28   // float: the stub's smoothness
#define SSR_W_WET 29           // float: the road's wetness 0..1
#define SSR_W_THICK 30         // float: the surface's thickness as a share of its depth
#define SSR_W_CAP 31           // float: the brightest colour a reflection brings (the target's units)
#define SSR_PUSH_WORDS 32
#define SSR_FRAME_LEGACY_NOISE 0x80000000u   // bit 31 of SSR_W_FRAME: the first version's noise (interleaved gradient noise sliding across the picture), for mcla_gpu_ssr_blue_noise false
// the temporal pass's own names for words it reuses (ssr_rules.h push::kPrevTanX ...)
#define SSR_W_PREV_TAN_X SSR_W_NEAR
#define SSR_W_PREV_TAN_Y SSR_W_MAX_DIST
#define SSR_W_HISTORY_WEIGHT SSR_W_FRAME
#define SSR_W_MOTION 20
#define SSR_W_HISTORY_GUIDE SSR_W_COL01    // temporal: previous frame's plane normal, virtual depth and history state
// GB-I2: the words the new parts use, each in a pass that does not read the word's first meaning (ssr_rules.h push::)
#define SSR_W_ROAD_VIEW 2         // trace: the game's road reflection view (256x256 at the guest's size): its slot (bits 0-15, 0xFFFF none) | its host samples - 1 << 16 (3 bits) | its side in host pixels << 19 (10 bits)
#define SSR_W_FALLBACK 26         // trace: float, the share of that view the effect takes out of the road where a ray finds a reflection (0 adds the reflection on top, as GB-I1)
#define SSR_REFLHIT_PLANAR_BIT 0x20000000u  // capture only: the native side view was drawn, independent of the subtraction setting
#define SSR_W_VEH_TABLE 1         // temporal: the AO's vehicle table (4 x 24 texels, a row an ID: three rows of the 3x4 motion, then a flag) in the low 16 bits, 0xFFFF none; the high 16 bits: SSR_TEMPORAL_* options
#define SSR_TEMPORAL_FIRST_RESOLVE 1u   // bit 0 of the high half of SSR_W_VEH_TABLE: the first version's resolve (the trace's centre pixel alone, the clip takes the history to the box whole), for mcla_gpu_ssr_resolve false
#define SSR_TEMPORAL_UPSCALER 2u        // bit 1: a temporal upscaler (FSR, DLSS) resolves this frame itself, so the clip has no floor by default
#define SSR_TEMPORAL_SPARKLE_RESOLVE 4u // bit 2: retain positive history on slow reflections under temporal upscaling
#define SSR_W_STENCIL 11          // trace: the high 16 bits of SSR_W_LIN0: the depth target's stencil plane (0xFFFF none)
#define SSR_W_ROAD_VIEW_APPLY 3   // apply: the same word as SSR_W_ROAD_VIEW, for the debug view that draws the road view (the apply reads no Hi-Z)

#define SSR_FLAG_MS 1u         // the guest's depth and the G-buffer's planes are multisampled (Texture2DMS, read at sample0)
#define SSR_FLAG_HZ 2u          // the Hi-Z level that is the SSR size is 1 (the traced size is the picture's), else 0 (the traced size is half of it)
#define SSR_FLAG_COL_HALF 4u    // colour_post is half the picture's size
#define SSR_FLAG_STUB 8u        // the frame has no RT3: SsrStubGain
#define SSR_FLAG_HISTORY 16u    // the temporal pass has a history for this frame
#define SSR_FLAG_CAMERA 32u     // the camera's rows are the frame's own (else the G-buffer's normal is not used: the depth's normal)
#define SSR_FLAG_SHIFT 12u
#define SSR_RAYS_SHIFT 18u
#define SSR_VIEW_SHIFT 20u
#define SSR_TRACE_STABLE (1u << SSR_VIEW_SHIFT)  // the trace uses the apply's view field for its stability switch
#define SSR_ITER_SHIFT 24u

// ---- numbers -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static const float kSsrSky = 60000.0f;          // ao_common.hlsli kAoSky: a depth at or above half of it is no surface
static const float kSsrMinAlpha = 0.005f;       // the GGX alpha's floor (a mirror)
static const float kSsrNormalBias = 0.02f;      // the ray's origin is moved off the surface by this and kSsrNormalBiasShare of its depth
static const float kSsrNormalBiasShare = 0.004f;
static const float kSsrStubUp = 0.85f;          // the stub: a world normal's y at or above this is a road
static const float kSsrWetRoadSmooth = 0.92f;   // what rain makes of a road's smoothness (a wet road is nearly a mirror)
static const float kSsrWetSidewalkSmooth = 0.80f;
static const float kSsrEdgeBorder = 0.08f;      // the hit's fade at the picture's edge: the share of the picture over which the reflection goes to the game's own
static const float kSsrFacingStart = -0.15f;    // a ray that points back at the camera (R.z below this) has no reflection, from this to kSsrFacingEnd it is faded in
static const float kSsrFacingEnd = 0.15f;
static const float kSsrConeScale = 0.25f;       // the colour's blur: the lobe's half width at the hit over a colour texel, times this, is the footprint in texels
static const float kSsrMissLength = 4096.0f;    // what the trace's alpha holds for a ray that found nothing (the virtual image is then nearly at infinity)
// GB-I2
static const float kSsrRampSpan = 0.20f;        // a surface goes from "not reflected" (the smoothness floor) to the whole term over this much smoothness (at most up to the wet road's)
static const float kSsrVehicleRowWeight = 0.8f;     // the history weight kept for a reflected vehicle that has a motion row (the clip does the rest)
static const float kSsrVehicleNoRowWeight = 0.35f;  // ... and for one that has none (its image is taken as the world's: it smears, so it keeps little)
static const float kSsrClipGamma = 1.25f;       // the history is clipped to the neighbourhood's mean +- this many standard deviations (and to its min / max widened by kSsrClipMargin)
static const float kSsrClipMargin = 0.1f;
static const float kSsrGuard = 0.8f;            // the road view taken out may never leave less than this share's complement of the colour that was there (a bad estimate must not black the road)
static const float kSsrClipKeep = 0.75f;       // the clip may take away at most 1 - this of a positive history in one frame (the box of ONE noisy frame is no proof that a bright reflection is gone)
// CHECK: not run on a device: a take of a moving bright reflection (a lamp across a puddle, a tail light) with DLSS on should show no trail of ours now; if the dimming without the floor is the worse of the two, move this one number (0.5 keeps half of the floor's brightness gain with a tenth after 3 frames).
static const float kSsrClipKeepUpscaler = 0.0f; // ... and with a temporal upscaler (no floor: the upscaler's history is on top of ours). ssr_test.cpp's sweep of the Monte Carlo: 0 fades a lamp in 1 / 1 frames (half / a tenth) and keeps 0.83 / 0.48 / 0.90 of the lobe's mean
                                                //   (three small bright things); 0.5: 1 / 3 frames, 0.86 / 0.55 / 0.92; 0.75 (without an upscaler): 2 / 6 frames, 0.89 / 0.66 / 0.94; the flicker is the tent average's in all (0.18 to 0.23)
static const float kSsrHistoryBorder = 1.0f;    // a history position nearer than this many texels to the picture's edge is not read (the clamped sample would smear the edge's texel)
static const uint kSsrVehicleIdMask = 0x1Fu;    // a vehicle's ID (1 to 23) rides in the low five bits of the half float's mantissa in the trace's alpha (SsrPackHit)

// ---- slots and words ---------------------------------------------------------------------------------------------------------------------------------------------------------------
uint SsrLo(uint word) { return word & 0xFFFFu; }
uint SsrHi(uint word) { return word >> 16u; }
bool SsrHasSlot(uint slot16) { return slot16 != 0xFFFFu; }

// ---- the class ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// How much of the reflection a pixel of this class and these flags takes, 0 to 1. The vehicle flag and a class without the SSR flag take none; the car's paint and chrome are not in the
// list on purpose (and carry the vehicle flag): their own reflections stay.
float SsrClassGain(uint cls, uint flags) {
  if ((flags & uint(GB_FLAG_VEHICLE)) != 0u) {
    return 0.0f;
  }
  if ((flags & uint(GB_FLAG_SSR)) == 0u) {
    return 0.0f;
  }
  if (cls == uint(GB_CLASS_ROAD) || cls == uint(GB_CLASS_WET_ROAD) || cls == uint(GB_CLASS_WATER)) {
    return 1.0f;
  }
  if (cls == uint(GB_CLASS_SIDEWALK) || cls == uint(GB_CLASS_METAL)) {
    return 0.8f;
  }
  if (cls == uint(GB_CLASS_GLASS)) {
    return 0.7f;
  }
  if (cls == uint(GB_CLASS_WORLD)) {
    return 0.6f;  // a surface the table flagged (the tunnel's tiles): everything else of the class is matte
  }
  return 0.0f;
}

// The class a stub pixel is: a road where the world normal points up, nothing elsewhere (CLASS_NONE).
uint SsrStubClass(float world_up) {
  return world_up >= kSsrStubUp ? uint(GB_CLASS_ROAD) : uint(GB_CLASS_NONE);
}

// What rain does to a road's and a sidewalk's smoothness: the wetness (0 dry .. 1 pouring) pulls it up to the wet number, never down.
float SsrWetSmooth(uint cls, float smoothness, float wet) {
  if (cls == uint(GB_CLASS_ROAD) || cls == uint(GB_CLASS_WET_ROAD)) {
    return lerp(smoothness, max(smoothness, kSsrWetRoadSmooth), saturate(wet));
  }
  if (cls == uint(GB_CLASS_SIDEWALK)) {
    return lerp(smoothness, max(smoothness, kSsrWetSidewalkSmooth), saturate(wet));
  }
  return smoothness;
}

// How much of the term a smoothness takes: 0 at the floor (mcla_gpu_ssr_min_smooth: a dry road is below it), 1 from a little above it (smoothstep), so that the wet road does not jump from nothing
// to a whole reflection at the number the rain's ramp crosses the floor at. The ramp ends no higher than the wet road's smoothness (the floor of a debug run may sit just under it).
float SsrSmoothRamp(float smoothness, float floor_smooth) {
  const float end = max(min(floor_smooth + kSsrRampSpan, kSsrWetRoadSmooth), floor_smooth + 0.01f);
  const float t = saturate((smoothness - floor_smooth) / (end - floor_smooth));
  return t * t * (3.0f - 2.0f * t);
}

float SsrAlphaOfSmooth(float smoothness) {
  const float r = 1.0f - saturate(smoothness);
  return max(r * r, kSsrMinAlpha);
}

// ---- Fresnel: the environment BRDF's scale and bias (Karis, "Physically Based Shading on Mobile", 2014): the reflected colour is F0 * x + y of the pair this returns, `rough` the perceptual
// roughness (1 - smoothness), `nov` the cosine between the normal and the direction to the camera.
float2 SsrEnvBrdf(float rough, float nov) {
  const float rx = rough * -1.0f + 1.0f;
  const float ry = rough * -0.0275f + 0.0425f;
  const float rz = rough * -0.572f + 1.04f;
  const float rw = rough * 0.022f - 0.04f;
  const float a004 = min(rx * rx, exp2(-9.28f * saturate(nov))) * rx + ry;
  return float2(-1.04f * a004 + rz, 1.04f * a004 + rw);
}

// ---- the noise -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
// THE PIXEL'S PART is the blue noise tile (blue_noise.hlsli: 32 x 32, no lattice and no low spatial frequencies; channel 1 is the same tile 13 pixels right and 8 down), THE FRAME'S PART a step along the
// R2 sequence (Roberts 2018: 1/g and 1/g^2 of the plastic number, as 32 bit fractions so that the product wraps exactly), the same for every pixel: value = frac(tile + sample * step), the sample's
// number frame * rays + ray. A pixel's own numbers over the frames are a low discrepancy sequence in the square (the history averages them to the lobe's mean), a frame's picture of the noise is the tile
// moved round the circle, and the pattern STAYS where it is. The first version's noise (SSR_FRAME_LEGACY_NOISE, mcla_gpu_ssr_blue_noise false) was interleaved gradient noise (Jimenez 2014) read at a
// position moved by 5.588 pixels a frame along the diagonal: a lattice, whose few sharp frequencies the history left as a pattern, and that slid across the picture: a position moved along a linear phase is
// the same as the phase moved, so what the history left of it (a function of the phase, advanced every frame) crawled over the road, and on a mirror-smooth puddle read as moving stripes.
float SsrIgn(float x, float y) {
  return frac(52.9829189f * frac(0.06711056f * x + 0.00583715f * y));
}
float SsrLegacyNoise1(int px, int py, uint frame, uint ray) {
  const float f = float(frame & 255u) + float(ray) * 97.0f;
  return SsrIgn(float(px) + 5.588238f * f, float(py) + 5.588238f * f);
}
float SsrLegacyNoise2(int px, int py, uint frame, uint ray) {
  const float f = float(frame & 255u) + float(ray) * 97.0f;
  return SsrIgn(float(py) + 4.4f * f + 31.0f, float(px) + 6.2f * f + 17.0f);
}
static const uint kSsrR2StepA = 3242174889u;   // 0.7548776662466927 * 2^32
static const uint kSsrR2StepB = 2447445413u;   // 0.5698402909980532 * 2^32
float SsrNoiseOf(int px, int py, uint frame, uint ray, uint rays, uint channel, uint step) {
  const uint sample_index = (frame & 0x7FFFFFFFu) * rays + ray;
  return frac(BlueNoiseTileAt(px, py, channel) + float(sample_index * step) * (1.0f / 4294967296.0f));
}
// The ray's two uniform numbers (u1 for the lobe's radius, u2 for its angle) for the pixel (px, py) in the frame word `frame` (the number, and bit 31 for the first version's noise), the ray's number `ray`
// of `rays` a pixel.
float SsrNoise1(int px, int py, uint frame, uint ray, uint rays) {
  if ((frame & SSR_FRAME_LEGACY_NOISE) != 0u) {
    return SsrLegacyNoise1(px, py, frame, ray);
  }
  return SsrNoiseOf(px, py, frame, ray, rays, 0u, kSsrR2StepA);
}
float SsrNoise2(int px, int py, uint frame, uint ray, uint rays) {
  if ((frame & SSR_FRAME_LEGACY_NOISE) != 0u) {
    return SsrLegacyNoise2(px, py, frame, ray);
  }
  return SsrNoiseOf(px, py, frame, ray, rays, 1u, kSsrR2StepB);
}

// ---- the reflection direction: GGX visible normals (Heitz 2018) ---------------------------------------------------------------------------------------------------------------
// `n` the surface's unit normal, `v` the unit direction to the camera, both in view space and `v` on the normal's side; `alpha` the GGX alpha; u1, u2 in 0..1. The result is the unit
// reflection direction R = reflect(-v, h) of a visible microfacet normal h; where it points below the surface (a grazing sample) the caller keeps the mirror direction.
float3 SsrReflect(float3 n, float3 v) {
  return n * (2.0f * dot(n, v)) - v;
}
float3 SsrSampleReflection(float3 n, float3 v, float alpha, float u1, float u2) {
  // an orthonormal basis (Duff et al. 2017)
  const float sgn = n.z >= 0.0f ? 1.0f : -1.0f;
  const float a = -1.0f / (sgn + n.z);
  const float b = n.x * n.y * a;
  const float3 t = float3(1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x);
  const float3 bt = float3(b, sgn + n.y * n.y * a, -n.y);
  const float3 ve = float3(dot(v, t), dot(v, bt), dot(v, n));
  const float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
  const float lensq = vh.x * vh.x + vh.y * vh.y;
  const float3 t1 = lensq > 1.0e-12f ? float3(-vh.y, vh.x, 0.0f) * (1.0f / sqrt(lensq)) : float3(1.0f, 0.0f, 0.0f);
  const float3 t2 = cross(vh, t1);
  const float r = sqrt(u1);
  const float phi = 6.28318530718f * u2;
  const float p1 = r * cos(phi);
  float p2 = r * sin(phi);
  const float s = 0.5f * (1.0f + vh.z);
  p2 = (1.0f - s) * sqrt(max(1.0f - p1 * p1, 0.0f)) + s * p2;
  const float3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(1.0f - p1 * p1 - p2 * p2, 0.0f));
  const float3 ne = normalize(float3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0f)));
  const float3 h = t * ne.x + bt * ne.y + n * ne.z;
  return SsrReflect(h, v);
}

// CITY-RT R4: the reflection ray of an SSR pixel (px, py the SSR image's pixel, `frame` the frame's word (the number, bit 31 for the first version's noise), `ray` the pixel's ray index of `rays`, which the noise's sample number needs: frame * rays + ray), the ONE place both the trace (hlsl/ssr_trace_ps.hlsl) and the ray
// traced fallback (hlsl/ssr_rt_trace_cs.hlsl) take it from: the fallback regenerates the very ray the trace walked, so nothing is stored. A sample that points below the surface is the mirror direction.
float3 SsrSampleRay(float3 n, float3 v, float alpha, int px, int py, uint frame, uint ray, uint rays) {
  float3 dir = SsrSampleReflection(n, v, alpha, SsrNoise1(px, py, frame, ray, rays), SsrNoise2(px, py, frame, ray, rays));
  if (dot(dir, n) < 0.02f) {
    dir = SsrReflect(n, v);
  }
  return dir;
}

// ---- the fades -----------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The reflection goes to nothing (and the game's own reflection stays alone) as the hit nears the picture's edge: u, v in 0..1.
float SsrEdgeFade(float u, float v) {
  const float e = min(min(u, 1.0f - u), min(v, 1.0f - v));
  const float t = saturate(e / kSsrEdgeBorder);
  return t * t * (3.0f - 2.0f * t);
}
// A ray that points back at the camera finds what is behind the surface it hits, which the screen does not show.
float SsrFacingFade(float ray_z) {
  return saturate((ray_z - kSsrFacingStart) / (kSsrFacingEnd - kSsrFacingStart));
}

// The mip of colour_post a hit is read at: the lobe's half width at the hit (alpha times the distance, times kSsrConeScale), in colour texels, as a level (two texels of footprint a level).
float SsrConeLod(float alpha, float hit_length, float hit_z, float tan_x, float colour_width, float max_level) {
  const float texel = 2.0f * tan_x * max(hit_z, 0.01f) / max(colour_width, 1.0f);
  const float footprint = hit_length * alpha * kSsrConeScale / max(texel, 1.0e-5f);
  return clamp(log2(max(footprint, 1.0f)), 0.0f, max_level);
}

// ---- the lens ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The distance along z of a stored depth (1 near, 0 far): ao_common.hlsli AoLinearDepth with the lens given as four numbers; kSsrSky for the sky and what cannot be placed.
float SsrLinearDepth(float stored, float lin_x, float lin_y, float lin_z, float lin_w) {
  const float ndc = 1.0f - saturate(stored);
  const float num = lin_z - ndc * lin_w;
  const float den = lin_x - ndc * lin_y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kSsrSky) ? t : kSsrSky;
}

// The view position of the pixel centre (cx, cy) of an image width x height at distance t.
float3 SsrViewPos(float cx, float cy, float width, float height, float t, float tan_x, float tan_y) {
  const float u = cx / width;
  const float v = cy / height;
  return float3((u * 2.0f - 1.0f) * tan_x * t, (1.0f - v * 2.0f) * tan_y * t, t);
}

// ---- the receiver --------------------------------------------------------------------------------------------------------------------------------------------------------------------
// An SSR pixel stands for a 2x2 block of the picture's pixels. The Hi-Z (at the SSR size) says the depth of the block; the material and the normal come from the block's pixel whose depth
// is nearest to it (the same surface). `z0..z3` are the four pixels' linear depths in reading order, `z_block` the Hi-Z's. The result is the index 0..3.
uint SsrPickInBlock(float z0, float z1, float z2, float z3, float z_block) {
  uint best = 0u;
  float best_d = abs(z0 - z_block);
  const float d1 = abs(z1 - z_block);
  if (d1 < best_d) {
    best = 1u;
    best_d = d1;
  }
  const float d2 = abs(z2 - z_block);
  if (d2 < best_d) {
    best = 2u;
    best_d = d2;
  }
  const float d3 = abs(z3 - z_block);
  if (d3 < best_d) {
    best = 3u;
  }
  return best;
}

// ---- temporal: the virtual image's reprojection ----------------------------------------------------------------------------------------------------------------------------------------
// The mirror image of a hit seen from the receiver: unfold the reflected path into a straight line, the hit stands `length` behind the receiver along the view ray. Exact for a flat mirror
// whatever the hit's place; the position of that point in the last frame says where the same reflection was then.
float3 SsrVirtualPoint(float3 receiver, float length_to_hit) {
  const float d = sqrt(max(dot(receiver, receiver), 1.0e-12f));
  return receiver * ((d + length_to_hit) / d);
}

struct SsrPrevious {
  bool ok;     // the point is in front of the last frame's camera and on its picture
  float u;     // in 0..1
  float v;
  float z;     // its distance in the last frame's view space
};
// A point of this frame's view space in the last frame's: `m0, m1, m2` the three rows of the 3x4 motion (ao.h's: row vectors, this renderer's view space), `tan_x, tan_y` the last frame's lens.
SsrPrevious SsrToPrevious(float4 m0, float4 m1, float4 m2, float3 p, float tan_x, float tan_y) {
  SsrPrevious r;
  const float4 q = float4(p.x, p.y, p.z, 1.0f);
  const float x = dot(m0, q);
  const float y = dot(m1, q);
  const float z = dot(m2, q);
  r.z = z;
  r.u = 0.0f;
  r.v = 0.0f;
  r.ok = false;
  if (z > 1.0e-3f) {
    r.u = 0.5f + 0.5f * x / (tan_x * z);
    r.v = 0.5f - 0.5f * y / (tan_y * z);
    r.ok = r.u >= 0.0f && r.u <= 1.0f && r.v >= 0.0f && r.v <= 1.0f;
  }
  return r;
}

// Whether the history's texel is still the same surface: its stored receiver depth against the one this frame's receiver has in the last frame's view space.
bool SsrSameSurface(float stored_z, float expected_z) {
  return abs(stored_z - expected_z) <= 0.08f * expected_z + 0.05f;
}

// ---- GB-I2: the history's clip -----------------------------------------------------------------------------------------------------------------------------------------------------
// One ray a pixel is noise, so the neighbourhood the history is held to is the 3x3 of the TRACE (the pixels that were traced): its mean, its standard deviation and its min and max, per channel.
// The box is mean +- kSsrClipGamma sigma, no wider than min / max widened by a tenth of their range (the variance clip, Salvi 2016, with the plain clamp's box as its limit), and the history
// is brought to its inside along the line to its centre, not channel by channel (a clamp turns a colour; the clip keeps it).
float SsrBoxLoChannel(float mean, float sigma, float lo, float hi) {
  return max(mean - kSsrClipGamma * sigma, lo - kSsrClipMargin * (hi - lo));
}
float SsrBoxHiChannel(float mean, float sigma, float lo, float hi) {
  return min(mean + kSsrClipGamma * sigma, hi + kSsrClipMargin * (hi - lo));
}
// The history `value` held to the box [box_lo, box_hi]: unchanged inside it, else the point of the box's surface on the line from the box's centre to the value.
float3 SsrClipToBox(float3 value, float3 box_lo, float3 box_hi) {
  const float3 centre = float3(0.5f * (box_lo.x + box_hi.x), 0.5f * (box_lo.y + box_hi.y), 0.5f * (box_lo.z + box_hi.z));
  const float3 half_size = float3(max(0.5f * (box_hi.x - box_lo.x), 1.0e-3f), max(0.5f * (box_hi.y - box_lo.y), 1.0e-3f), max(0.5f * (box_hi.z - box_lo.z), 1.0e-3f));
  const float3 v = value - centre;
  const float t = max(max(abs(v.x) / half_size.x, abs(v.y) / half_size.y), abs(v.z) / half_size.z);
  return t > 1.0f ? centre + v * (1.0f / t) : value;
}

// Whether a history position (u, v in 0..1 of an image of width x height texels) is far enough from the picture's edge to be read.
bool SsrHistoryInside(float u, float v, float width, float height) {
  return u * width >= kSsrHistoryBorder && (1.0f - u) * width >= kSsrHistoryBorder && v * height >= kSsrHistoryBorder && (1.0f - v) * height >= kSsrHistoryBorder;
}

// ---- the spatial resolve and the clip's floor (2026-11-22, SSR-STRIPES-2) --------------------------------------------------------------------------------------------------------
// WHAT WAS LEFT. One ray a pixel through a lobe with a few pixels' reach finds a small bright thing (a lamp, a tail light, a splash) in a few of the frames, in a few of the pixels round its image:
// a hit or nothing. The history's exponential mean (0.9: about ten frames) cannot average that out, and the clip made it worse. Its box is the mean +- 1.25 deviations of the 3x3 of ONE
// frame's trace, and where those nine pixels hold no hit at all (the commonest case round a small thing's image) the box is a point: the history, a mean of earlier hits, is thrown to the road's
// colour, so the next hit starts from nothing (the flicker stays near the raw noise) and the reflection comes out dimmer than it is. In a Monte Carlo of the shader's own numbers (the noise, the
// clip, the weights: ssr_test.cpp TestResolve) a lamp's reflection flickered by 37 to 44 per cent of its brightness and came out 7 to 52 per cent too dim. The measured take of 2026-11-22
// (BLACK-LINES-NOISE.txt) shows the same: the flicker the reflections add (d2 0.52 to 0.58 levels over 0.28 without them) sits in 5% of the blocks, on the images of lamps and tail lights.
// THE FIX, two parts. (1) The trace's 3x3 is averaged with a tent (1 2 1 / 2 4 2 / 1 2 1, SsrTapKernel) before the history takes it: the nine rays of a neighbourhood are nine samples of the same
// lobe, so the average is nearer the lobe's mean, and a blue noise's energy sits at the highest frequencies, so a 3 x 3 kernel covers it; only pixels of the SAME surface join
// (SsrTapSameSurface: a road's average must not take a kerb's or a car's rays). (2) The clip may not take more than 1 - kSsrClipKeep of a positive history away in a frame (SsrKeepClip): a
// reflection that really went away (a light out, a car gone) fades to half in 2 frames and to a tenth in 6, a frame whose nine rays all missed does not wipe it. Together: the flicker is
// 51 to 54 per cent of the first version's and the brightness is 1 to 17 points nearer the truth.
// Both are the first version's behaviour when SSR_TEMPORAL_FIRST_RESOLVE is set. THE FLOOR ONLY WHERE OUR RESOLVE IS THE ONLY TEMPORAL FILTER (NOISE-FOLLOW, 2026-11-22): with a temporal upscaler the upscaler's own
// history averages what is left, and cannot reject a reflection that moved (its vectors describe the surface the reflection is on, not the mirror image): a floor on top of it is a trail of 6 frames at 60 fps
// (a tenth of a second) on a moving lamp or a pedestrian's light. SsrClipKeepFor gives the floor 0 with an upscaler (SSR_TEMPORAL_UPSCALER): the clip is whole there and the history trails as the first version's did.
// The tent average (1) stays on in both. The sparkle resolve can restore a motion-tapered floor under the upscaler.
float SsrTapKernel(int dx, int dy) {
  return (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
}
// A neighbour of the trace joins the centre's average only on the same surface: the history's own rule (SsrSameSurface, 8% and 5 centimetres of receiver depth). Not tighter: the depth of a road at a
// grazing angle runs away by metres from one SSR row to the next at 50 metres, and a tighter rule would take the vertical neighbours away there.
bool SsrTapSameSurface(float z_tap, float z_centre) {
  return SsrSameSurface(z_tap, z_centre);
}
// The floor's share this frame: kSsrClipKeep where the spatial resolve runs and no temporal upscaler does (our history is the only temporal filter); kSsrClipKeepUpscaler (0: no floor, the clip whole) with an upscaler;
// none with the first version's resolve.
float SsrClipKeepFor(bool resolve, bool upscaler) {
  return resolve ? (upscaler ? kSsrClipKeepUpscaler : kSsrClipKeep) : 0.0f;
}
// A noisy 3x3 can miss a small bright reflection for a frame. Keep a fraction of its positive history and taper the floor as the image moves.
float SsrSparkleClipKeep(float dx, float dy) {
  return kSsrClipKeep * saturate(1.0f - 0.5f * max(abs(dx), abs(dy)));
}
// One channel of the history after the clip: the clip's own result, except that a positive history is not taken below `keep` of itself (a history that is raised, or is not positive, is the clip's; keep 0: the clip's).
float SsrKeepChannel(float history, float clipped, float keep) {
  return (keep > 0.0f && history > 0.0f && clipped < history) ? max(clipped, history * keep) : clipped;
}
float3 SsrKeepClip(float3 history, float3 clipped, float keep) {
  return float3(SsrKeepChannel(history.x, clipped.x, keep), SsrKeepChannel(history.y, clipped.y, keep), SsrKeepChannel(history.z, clipped.z, keep));
}
// The distance the reprojection of the virtual image uses (SsrVirtualPoint): the pixel's own when its ray found something; where its ray found nothing (a pixel of the lobe's tail that went
// by the lamp) the NEAREST hit of the same-surface pixels of its 3x3 (`nearest_tap`, kSsrMissLength when none hit): the history of the pixels round a reflection is then looked up with the
// reflection's parallax, not by the sky's, and does not jump between two places with the noise from frame to frame.
float SsrReprojectDistance(float own, float nearest_tap) {
  return own < kSsrMissLength - 1.0f ? own : min(own, nearest_tap);
}

// ---- GB-I2: what the trace says of a hit, in its alpha -----------------------------------------------------------------------------------------------------------------------------------
// The alpha of the trace is a half float: the distance from the receiver to the hit (the virtual image's), and where the hit is on a VEHICLE's pixel, the sign bit set and the vehicle's ID (the
// stencil's, 1 to 23; 0 = a vehicle whose ID is not known) in the low five bits of the half's ten mantissa bits (float bits 13 to 17), leaving five bits (3%) for the distance: a 3% error in the length
// of a reflected vehicle's image moves its reprojection by far less than a texel (the error is second order: the image is hardly parallax at the lengths a car is reflected from). A hit that is no
// vehicle's keeps its ten bits. The float written has its lower 13 bits clear, so the half conversion is exact.
float SsrPackHit(float distance, bool vehicle, uint vehicle_id) {
  if (!vehicle) {
    return distance;
  }
  const uint bits = (asuint(distance) & 0x7FFFE000u & ~(kSsrVehicleIdMask << 13u)) | ((vehicle_id & kSsrVehicleIdMask) << 13u) | 0x80000000u;
  return asfloat(bits);
}
bool SsrHitIsVehicle(float alpha) {
  return (asuint(alpha) & 0x80000000u) != 0u;
}
uint SsrHitVehicleId(float alpha) {
  return SsrHitIsVehicle(alpha) ? ((asuint(alpha) >> 13u) & kSsrVehicleIdMask) : 0u;
}
// The distance of a packed hit: the vehicle's is rounded to the middle of the 3% the ID took.
float SsrHitDistance(float alpha) {
  const uint bits = asuint(alpha) & 0x7FFFFFFFu;
  return SsrHitIsVehicle(alpha) ? asfloat((bits & 0xFFFC0000u) | 0x00020000u) : asfloat(bits);
}

// ---- GB-I2: the image of a reflected vehicle -----------------------------------------------------------------------------------------------------------------------------------------
// A point mirrored across the plane through `plane_point` with the unit normal `n`.
float3 SsrMirror(float3 p, float3 plane_point, float3 n) {
  return p - n * (2.0f * dot(p - plane_point, n));
}
float SsrDot3(float4 row, float3 v) {
  return row.x * v.x + row.y * v.y + row.z * v.z;
}
// Where the reflection of a point of a MOVING vehicle was in the last frame. The virtual image of SsrVirtualPoint is the mirror image of the hit across the receiver's surface; undone with the
// surface's normal it gives the hit itself (this frame's view space), which the vehicle's own row (v0..v2: the AO's vehicle table, the 3x4 that takes a point of this frame's view space to
// the last frame's view space where THAT vehicle's point was, camera and vehicle motion together) moves to where it was; the receiver is the world's (m0..m2: the camera's motion alone) and so is
// the mirror: the last frame's image is the mirror of the old hit across the old surface. Exact for a flat mirror; the receiver's normal is the depth's (the pass has no camera axes).
SsrPrevious SsrVehicleImage(float4 m0, float4 m1, float4 m2, float4 v0, float4 v1, float4 v2, float3 receiver, float3 n, float length_to_hit, float tan_x, float tan_y) {
  const float3 virt = SsrVirtualPoint(receiver, length_to_hit);
  const float3 hit = SsrMirror(virt, receiver, n);
  const float4 qh = float4(hit.x, hit.y, hit.z, 1.0f);
  const float3 hit_prev = float3(dot(v0, qh), dot(v1, qh), dot(v2, qh));
  const float4 qr = float4(receiver.x, receiver.y, receiver.z, 1.0f);
  const float3 receiver_prev = float3(dot(m0, qr), dot(m1, qr), dot(m2, qr));
  const float3 n_prev = normalize(float3(SsrDot3(m0, n), SsrDot3(m1, n), SsrDot3(m2, n)));
  const float3 image = SsrMirror(hit_prev, receiver_prev, n_prev);
  return SsrToPrevious(float4(1.0f, 0.0f, 0.0f, 0.0f), float4(0.0f, 1.0f, 0.0f, 0.0f), float4(0.0f, 0.0f, 1.0f, 0.0f), image, tan_x, tan_y);
}

// ---- GB-I2: the road view's share and the guard ------------------------------------------------------------------------------------------------------------------------------------------
// The apply adds the term to a colour that already holds the game's own reflection, so where the trace took the road view out of the term (the term can then be negative) a bad estimate must not
// turn the road black: each channel of the term keeps at least kSsrGuard of the colour the pixel had before (colour_post's, at the apply's position) taken away.
float3 SsrGuardTerm(float3 term, float3 base) {
  return float3(max(term.x, -kSsrGuard * max(base.x, 0.0f)), max(term.y, -kSsrGuard * max(base.y, 0.0f)), max(term.z, -kSsrGuard * max(base.z, 0.0f)));
}
// The road view's side, samples and slot out of the trace's SSR_W_ROAD_VIEW word (ssr_rules.h PackRoadView).
uint SsrRoadViewSlot(uint word) { return word & 0xFFFFu; }
uint SsrRoadViewSamples(uint word) { return ((word >> 16u) & 7u) + 1u; }
uint SsrRoadViewSide(uint word) { return (word >> 19u) & 1023u; }

#endif  // MCLA_GPU_SSR_COMMON_HLSLI
