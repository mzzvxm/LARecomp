// MCLA GPU renderer -- the G-buffer's shared HLSL: THE FROZEN INTERFACE (package IF of perf/GBUFFER-SURVEY.md 7.2).
//
// One include for everything that writes or reads the G-buffer: the G-buffer variants of the game's lit pixel shaders (the pack builder
// pastes this text before `void main(`, shaderpack/variants/gbuffer.cpp), the debug view and every consumer pass (hlsl/*.hlsl, `#include
// "gbuffer_common.hlsli"`). The C++ twin is src/gpu/gbuffer_rules.h; src/gpu/gbuffer_rules_test.cpp compiles THIS FILE as C++ (through a small
// shim of the HLSL vector types) and checks it against the twin, and compares every #define below with gbuffer_types.h, so the two cannot drift.
// That is why the text keeps to a subset both languages read: scalar and vector arithmetic, .x .y .z .w, no swizzles of more than one
// component, no intrinsics but abs, sqrt, saturate, dot, cross, normalize, min, max, no `static const` arrays.
//
// The layout (gbuffer_types.h): RT1 R16G16_UNORM octahedral WORLD normal, 0 = no data; RT2 R8G8B8A8_UNORM sqrt(albedo) + lit weight;
// RT3 R8G8_UNORM smoothness + (class | flags << 5).
//
// The pasted text must not use the names of the game's registers (r0..r31, c0..c255, p0, ps, a0, aL): every name here starts with Gb or GB_.

#ifndef MCLA_GBUFFER_COMMON_HLSLI
#define MCLA_GBUFFER_COMMON_HLSLI

// ---- the numbers (gbuffer_types.h) -------------------------------------------------------------------------------------------------------

#define GB_CLASS_NONE 0
#define GB_CLASS_WORLD 1
#define GB_CLASS_ROAD 2
#define GB_CLASS_WET_ROAD 3
#define GB_CLASS_SIDEWALK 4
#define GB_CLASS_TERRAIN 5
#define GB_CLASS_LOD 6
#define GB_CLASS_FOLIAGE 7
#define GB_CLASS_PAINT 8
#define GB_CLASS_CHROME 9
#define GB_CLASS_METAL 10
#define GB_CLASS_GLASS 11
#define GB_CLASS_SKIN 12
#define GB_CLASS_EMISSIVE 13
#define GB_CLASS_INTERIOR 14
#define GB_CLASS_WATER 15
#define GB_CLASS_MASK 31

#define GB_FLAG_SSR 1
#define GB_FLAG_ALPHA_COVERED 2
#define GB_FLAG_VEHICLE 4

#define GB_MATERIAL_CLASS_SHIFT 8
#define GB_MATERIAL_FLAGS_SHIFT 13
#define GB_MATERIAL_LIT_SHIFT 16
#define GB_MATERIAL_METAL_SHIFT 24

#define GB_MODE_WRITE_NORMAL 1
#define GB_MODE_WRITE_ALBEDO 2
#define GB_MODE_WRITE_MATERIAL 4
#define GB_MODE_FLIP_BACKFACE 8
#define GB_MODE_DERIVATIVE_NORMAL 16

// ---- RT1: the normal ---------------------------------------------------------------------------------------------------------------------

// The sign with no zero: 1 for +0 and -0 alike (the octahedral fold needs a side for a component that is exactly 0).
float GbSignNotZero(float v) {
  return v >= 0.0f ? 1.0f : -1.0f;
}

// The octahedral map of a direction (Cigolle et al., "A Survey of Efficient Representations for Independent Unit Vectors"): the direction
// is projected onto the octahedron |x| + |y| + |z| = 1, the lower half folded out over the upper one's corners, and the square put in 0..1.
// `n` need not be unit length (the L1 norm divides); it must not be zero.
float2 GbOctEncode(float3 n) {
  float inv = 1.0f / (abs(n.x) + abs(n.y) + abs(n.z));
  float x = n.x * inv;
  float y = n.y * inv;
  float z = n.z * inv;
  float ex = x;
  float ey = y;
  if (z < 0.0f) {
    ex = (1.0f - abs(y)) * GbSignNotZero(x);
    ey = (1.0f - abs(x)) * GbSignNotZero(y);
  }
  return float2(ex * 0.5f + 0.5f, ey * 0.5f + 0.5f);
}

// The unit direction of a code in 0..1 (what the sampler or Load returns for the R16G16_UNORM texel).
float3 GbOctDecode(float2 f) {
  float x = f.x * 2.0f - 1.0f;
  float y = f.y * 2.0f - 1.0f;
  float z = 1.0f - abs(x) - abs(y);
  float t = saturate(-z);
  x = x + (x >= 0.0f ? -t : t);
  y = y + (y >= 0.0f ? -t : t);
  float il = 1.0f / sqrt(x * x + y * y + z * z);
  return float3(x * il, y * il, z * il);
}

// What a G-buffer shader writes to RT1: the octahedral code, never (0, 0) (that value is "no data", the clear value): a code that would
// round to it in the 16 bit unorm texel is moved by one step in x (0.003 degrees). A direction that is not a direction (zero, or a NaN
// out of a degenerate interpolant) writes the "no data" code on purpose, so the consumer falls back to the depth.
float2 GbRt1Encode(float3 n) {
  float len2 = dot(n, n);
  if (!(len2 > 1.0e-12f)) {
    return float2(0.0f, 0.0f);
  }
  float2 e = GbOctEncode(n);
  if (e.x < 0.5f / 65535.0f && e.y < 0.5f / 65535.0f) {
    e.x = 1.0f / 65535.0f;
  }
  return e;
}

// Whether an RT1 texel holds a normal (a consumer's first test; the texel is what Load returns, a unorm value).
bool GbRt1Valid(float2 rt1) {
  return rt1.x > 0.0f || rt1.y > 0.0f;
}

float3 GbRt1Decode(float2 rt1) {
  return GbOctDecode(rt1);
}

// ---- RT2: the albedo and the lit weight --------------------------------------------------------------------------------------------------

float3 GbAlbedoEncode(float3 albedo) {
  return sqrt(saturate(albedo));
}

float3 GbAlbedoDecode(float3 stored) {
  return stored * stored;
}

// ---- RT3 and the material word -----------------------------------------------------------------------------------------------------------

// The word of a draw (gbuffer_types.h): smoothness 0..1, the class, the flags (GB_FLAG_*), the lit weight 0..1.
uint GbMaterialWord(float smoothness, uint cls, uint flags, float lit) {
  uint s = uint(saturate(smoothness) * 255.0f + 0.5f);
  uint l = uint(saturate(lit) * 255.0f + 0.5f);
  return s | ((cls & uint(GB_CLASS_MASK)) << uint(GB_MATERIAL_CLASS_SHIFT)) | ((flags & 7u) << uint(GB_MATERIAL_FLAGS_SHIFT)) |
         (l << uint(GB_MATERIAL_LIT_SHIFT));
}

// RT3 of a word: r = smoothness, g = class | flags << 5, both as unorm8.
float2 GbMaterialRt3(uint word) {
  return float2(float(word & 255u), float((word >> uint(GB_MATERIAL_CLASS_SHIFT)) & 255u)) * (1.0f / 255.0f);
}

// RT2.a of a word.
float GbMaterialLit(uint word) {
  return float((word >> uint(GB_MATERIAL_LIT_SHIFT)) & 255u) * (1.0f / 255.0f);
}

// The consumer's side: what the RT3 texel (a unorm value) says.
float GbRt3Smoothness(float2 rt3) {
  return rt3.x;
}
uint GbRt3Class(float2 rt3) {
  return uint(rt3.y * 255.0f + 0.5f) & uint(GB_CLASS_MASK);
}
uint GbRt3Flags(float2 rt3) {
  return (uint(rt3.y * 255.0f + 0.5f) >> 5u) & 7u;
}

// ---- the gloss: smoothness, the GGX alpha, the game's Blinn-Phong power (the twin: gbuffer_rules.h part 3) --------------------------------------

// RT3.r and the word's bits 0..7 hold the PERCEPTUAL smoothness s of the lobe: its GGX (Trowbridge-Reitz) alpha is (1 - s)^2. The game's own Blinn-Phong lobe, (N.H)^n, has the
// alpha sqrt(2 / (n + 2)) (the usual fit: the lobes' widths agree), so a shader that has a power puts GbSmoothnessOfPower(n) in the word and a consumer that shades with GGX takes
// GbRt3Alpha(rt3) back out. A power that is not above 0 (a NaN too) is a chalk: alpha 1.
float GbGgxAlphaOfPower(float power) {
  float n = power > 0.0f ? power : 0.0f;
  return sqrt(2.0f / (n + 2.0f));
}

float GbSmoothnessOfAlpha(float alpha) {
  return 1.0f - sqrt(saturate(alpha));
}

float GbAlphaOfSmoothness(float smoothness) {
  float r = 1.0f - saturate(smoothness);
  return r * r;
}

float GbSmoothnessOfPower(float power) {
  return GbSmoothnessOfAlpha(GbGgxAlphaOfPower(power));
}

// The GGX alpha a consumer shades with, from the RT3 texel (a unorm value).
float GbRt3Alpha(float2 rt3) {
  return GbAlphaOfSmoothness(GbRt3Smoothness(rt3));
}

// The metalness byte of the word (bits 24..31; no plane carries it yet, see gbuffer_rules.h kMaterialMetalShift) and the word with it set.
float GbMaterialMetal(uint word) {
  return float((word >> uint(GB_MATERIAL_METAL_SHIFT)) & 255u) * (1.0f / 255.0f);
}

uint GbMaterialWithMetal(uint word, float metal) {
  uint m = uint(saturate(metal) * 255.0f + 0.5f);
  return (word & 0x00FFFFFFu) | (m << uint(GB_MATERIAL_METAL_SHIFT));
}

// ---- the derivative normal ---------------------------------------------------------------------------------------------------------------

// Rotates `v` by the unit quaternion `q` (x, y, z, w).
float3 GbQuatRotate(float4 q, float3 v) {
  float3 u = float3(q.x, q.y, q.z);
  float3 t = cross(u, v) * 2.0f;
  return v + t * q.w + cross(u, t);
}

// The point a fragment is, relative to the camera, in WORLD space: `ray` is GBufferConstants::view_ray, `q` the camera's rotation, `px, py`
// SV_Position.xy and `w` SV_Position.w (1 / the view-space depth). Its derivatives (ddx, ddy in the shader) are the surface's tangents.
float3 GbWorldPoint(float4 ray, float4 q, float px, float py, float w) {
  float z = 1.0f / w;
  float3 view = float3((px * ray.x + ray.y) * z, (py * ray.z + ray.w) * z, z);
  return GbQuatRotate(q, view);
}

// The geometric normal of the surface from the two tangents, turned to face the camera: `to_camera` is the direction from the surface to the
// camera (minus GbWorldPoint's result). Exact on a flat face, faceted on a round one.
float3 GbFaceNormal(float3 dpdx, float3 dpdy, float3 to_camera) {
  float3 n = normalize(cross(dpdx, dpdy));
  if (dot(n, to_camera) < 0.0f) {
    n = n * -1.0f;
  }
  return n;
}

#endif  // MCLA_GBUFFER_COMMON_HLSLI
