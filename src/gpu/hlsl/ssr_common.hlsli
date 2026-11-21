// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1): the pure functions the three passes share, and the push block's words.
//
// The passes (ssr_trace_ps, ssr_temporal_ps, ssr_apply_ps) are full-screen triangles on a pipeline layout of their own: the host pass layout with a push range of 128 bytes (the AO's
// way, ao.cpp), 32 words read through SsrW(). Everything here that does not touch a texture lives in this file and in ssr_trace.hlsli so that src/gpu/ssr_test.cpp can compile them AS C++
// (through a shim of the HLSL types) and check them against analytic scenes: the class gate, the wet road, the environment BRDF, the GGX visible-normal sampling, the noise, the fades, the
// virtual image's reprojection. So the text keeps to a subset both languages read: scalar and small vector arithmetic with .x .y .z .w, no swizzles of more than one component, no `out`
// parameters, no `static const` arrays, no attributes ([loop] is SSR_LOOP, which the passes define).
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

#ifndef SSR_LOOP
#define SSR_LOOP
#endif

// ---- the push block's words (ssr_rules.h push::, which the test compares with these) -------------------------------------------------------------------------------------------------
// 32 words of 32 bits; a slot is 16 bits (bindless slots are below 32768), two to a word, 0xFFFF for none; a float is a word read with asfloat.
#define SSR_W_DEPTH_RT1 0      // depth plane | RT1 << 16: the guest's depth plane (Texture2DMS when SSR_FLAG_MS) and the G-buffer's normal plane
#define SSR_W_RT2_RT3 1        // RT2 | RT3 << 16
#define SSR_W_SRC 2            // the pass's own images: trace: -; temporal: the trace | the history << 16; apply: the accumulated image | the trace << 16 (the debug views)
#define SSR_W_HIZ01 3          // the Hi-Z's levels 0 | 1 << 16
#define SSR_W_HIZ23 4
#define SSR_W_HIZ45 5
#define SSR_W_COL01 6          // colour_post's levels 0 | 1 << 16
#define SSR_W_COL23 7
#define SSR_W_RENDER 8         // the picture's size in host pixels: width | height << 16
#define SSR_W_FLAGS 9          // sample0 (4 bits) | hiz levels << 4 | colour levels << 8 | SSR_FLAG_* << 12 | rays - 1 << 18 | view << 20 | max iterations << 24
#define SSR_W_FRAME 10         // trace, apply: the frame's number (the noise); temporal: the weight the history keeps (float)
#define SSR_W_LIN0 11          // the depth chain's level 0 (the vehicle mark) | spare << 16
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
#define SSR_W_STRENGTH 26      // float: the term's strength
#define SSR_W_MIN_SMOOTH 27    // float: pixels rougher than this are not reflected
#define SSR_W_STUB_SMOOTH 28   // float: the stub's smoothness
#define SSR_W_WET 29           // float: the road's wetness 0..1
#define SSR_W_THICK 30         // float: the surface's thickness as a share of its depth
#define SSR_W_CAP 31           // float: the brightest colour a reflection brings (the target's units)
#define SSR_PUSH_WORDS 32
// the temporal pass's own names for words it reuses (ssr_rules.h push::kPrevTanX ...)
#define SSR_W_PREV_TAN_X SSR_W_NEAR
#define SSR_W_PREV_TAN_Y SSR_W_MAX_DIST
#define SSR_W_HISTORY_WEIGHT SSR_W_FRAME
#define SSR_W_MOTION 20

#define SSR_FLAG_MS 1u          // the guest's depth and the G-buffer's planes are multisampled (Texture2DMS, read at sample0)
#define SSR_FLAG_HZ 2u          // the Hi-Z level that is the SSR size is 1 (the traced size is the picture's), else 0 (the traced size is half of it)
#define SSR_FLAG_COL_HALF 4u    // colour_post is half the picture's size
#define SSR_FLAG_STUB 8u        // the frame has no RT3: SsrStubGain
#define SSR_FLAG_HISTORY 16u    // the temporal pass has a history for this frame
#define SSR_FLAG_CAMERA 32u     // the camera's rows are the frame's own (else the G-buffer's normal is not used: the depth's normal)
#define SSR_FLAG_SHIFT 12u
#define SSR_RAYS_SHIFT 18u
#define SSR_VIEW_SHIFT 20u
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
// Interleaved gradient noise (Jimenez 2014): a value in 0..1 per pixel, well spread in space; the frame moves it by a step that keeps the sequence of one pixel low discrepancy.
float SsrIgn(float x, float y) {
  return frac(52.9829189f * frac(0.06711056f * x + 0.00583715f * y));
}
float SsrNoise1(int px, int py, uint frame, uint ray) {
  const float f = float(frame & 255u) + float(ray) * 97.0f;
  return SsrIgn(float(px) + 5.588238f * f, float(py) + 5.588238f * f);
}
float SsrNoise2(int px, int py, uint frame, uint ray) {
  const float f = float(frame & 255u) + float(ray) * 97.0f;
  return SsrIgn(float(py) + 4.4f * f + 31.0f, float(px) + 6.2f * f + 17.0f);
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

// The colour a clamp keeps: the history's value pulled into the box of the current neighbourhood, widened by a share of its size.
float SsrClampToBox(float value, float lo, float hi) {
  const float margin = 0.1f * (hi - lo);
  return clamp(value, lo - margin, hi + margin);
}

#endif  // MCLA_GPU_SSR_COMMON_HLSLI
