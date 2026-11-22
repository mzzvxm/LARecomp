// MCLA GPU renderer -- DLSS Ray Reconstruction's guide images (rr_guides.h, perf/DLSS-RR.md, package RR-2): the pure functions the guide pass and the debug view share.
//
// The pass (rr_guides_cs.hlsl) turns the G-buffer's three planes (gbuffer_types.h: RT1 the octahedral world normal, RT2 sqrt(albedo) and the lit weight, RT3 the smoothness and
// the class), the kept depth and the upscaler's masks into the five images Ray Reconstruction reads next to the colour, the depth and the motion vectors (dlss_rr_rules.h):
//
//   diffuse albedo      RT2.rgb squared (the gamma 2 of the plane taken out), never 0 where there is a surface (a floor), 0.5 grey where the plane holds no albedo and in the sky
//   normal + roughness  the WORLD normal of RT1 (a surface RT1 holds none for: the normal of the depth's tangents, turned to face the camera), the LINEAR roughness 1 - smoothness in
//                       alpha (RT3 cleared to 0 is smoothness 0: roughness 1, a chalk, which is what a surface with no material is); (0, 0, 0, 0) in the sky
//   specular albedo     the average reflectance of the surface seen from here: NVIDIA's EnvBRDFApprox2 (the integration guide's 4.2.1) of F0, alpha = roughness^2 and N.V. F0 is
//                       the class table: a dielectric 0.04, a metal or chrome its albedo (0.6 where the frame has none), an emissive surface 0 (nothing reflects off a lamp)
//   hit distance        0 on every surface (the reflection is taken to be at the surface: no virtual parallax, the primary motion is the reflection's), 65504 in the sky; a writer's
//                       distance (a bindless image of the reflection effects, RR-4) wins where it holds a positive value
//   responsivity        the larger of the upscaler's reactive mask and its transparency mask, 0..1: where the new frame is to be followed faster
//
// "The sky" is a stored depth of 0 (the guest's depth is 1 near and 0 far): nothing to denoise there. Every function below is pure; src/gpu/rr_guides_test.cpp compiles THIS FILE as C++ (through a
// shim of the HLSL vector types, as the G-buffer's and the reflections' tests do) and checks each function against the twin in rr_guides_rules.h, so the text keeps to a subset both languages
// read: scalar and float3 arithmetic, .x .y .z .w, no swizzles of more than one component, no `out` parameters, no `static const` arrays.
//
// View space is the AO's (hlsl/ao_common.hlsli): x right, y up, z forward = the linear depth t in the game's world units, pixels y down. The world's axes of the camera are right, up, forward.

#ifndef MCLA_GPU_RR_GUIDES_COMMON_HLSLI
#define MCLA_GPU_RR_GUIDES_COMMON_HLSLI

#include "gbuffer_common.hlsli"
#include "nan_guard.hlsli"

// ---- the views (mcla_gpu_dlss_rr_view; rr_guides_rules.h View) --------------------------------------------------------------------------------------------------------------------------
#define RR_VIEW_OFF 0
#define RR_VIEW_DIFFUSE 1
#define RR_VIEW_NORMAL 2
#define RR_VIEW_ROUGHNESS 3
#define RR_VIEW_SPECULAR 4
#define RR_VIEW_HIT 5
#define RR_VIEW_RESPONSIVITY 6

// ---- numbers ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static const float kRrSky = 60000.0f;              // ao_common.hlsli kAoSky: a linear depth at or above half of it is no surface
static const float kRrSkyDiffuse = 0.5f;           // what the integration guide says the sky's diffuse albedo is
static const float kRrSkyHitDistance = 65504.0f;   // FP16_MAX
static const float kRrDielectricF0 = 0.04f;
static const float kRrMetalF0 = 0.6f;              // a metal or chrome that the frame holds no albedo for (the reflections' number for a metal)
static const float kRrMinAlbedo = 0.02f;           // "never leave a valid pixel at 0": the darkest albedo a surface gets (charcoal)
static const float kRrUnknownAlbedo = 0.5f;        // a surface RT2 holds nothing for
static const float kRrMinTangent = 1.0e-20f;       // the square of the shortest cross product that is a normal

// ---- the depth and the view ------------------------------------------------------------------------------------------------------------------------------------------------------------
// The distance along the view direction of a stored depth (1 near, 0 far): lin is the projection's depth rows P22 P23 P32 P33 (ao.cpp FillPush), t = (P32 - ndc P33) / (P22 - ndc P23), ndc = 1 - stored.
// kRrSky for the sky and for anything the linearisation cannot place.
float RrLinearDepth(float stored, float lin_x, float lin_y, float lin_z, float lin_w) {
  const float ndc = 1.0f - saturate(stored);
  const float num = lin_z - ndc * lin_w;
  const float den = lin_x - ndc * lin_y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < kRrSky) ? t : kRrSky;
}

// The view-space point of the pixel centre (cx, cy) of an image width x height at distance t.
float3 RrViewPos(float cx, float cy, float width, float height, float t, float tan_x, float tan_y) {
  const float u = cx / width;
  const float v = cy / height;
  return float3((u * 2.0f - 1.0f) * tan_x * t, (1.0f - v * 2.0f) * tan_y * t, t);
}

// A view-space vector in the world: the camera's axes.
float3 RrWorldOfView(float3 v, float3 right, float3 up, float3 forward) {
  return right * v.x + up * v.y + forward * v.z;
}

// The unit direction in the world from the camera through the pixel centre (cx, cy).
float3 RrViewDir(float cx, float cy, float width, float height, float tan_x, float tan_y, float3 right, float3 up, float3 forward) {
  const float3 d = RrWorldOfView(RrViewPos(cx, cy, width, height, 1.0f, tan_x, tan_y), right, up, forward);
  return normalize(d);
}

// ---- the normal of a surface RT1 holds none for ------------------------------------------------------------------------------------------------------------------------------------
// The tangent of the surface toward increasing pixel coordinates (right for x, down for y): through the neighbour on the side whose depth is nearer the centre's (a step in depth is an edge, the
// tangent must stay on one surface), the other side where only one is a surface; zero where neither is. `pc`, `pm`, `pp` are view-space points of the centre and its neighbours at -1 and +1.
float3 RrTangent(float3 pc, float zc, float3 pm, float zm, float3 pp, float zp) {
  const bool m_ok = zm < kRrSky * 0.5f;
  const bool p_ok = zp < kRrSky * 0.5f;
  if (m_ok && p_ok) {
    return abs(zp - zc) < abs(zm - zc) ? pp - pc : pc - pm;
  }
  if (p_ok) {
    return pp - pc;
  }
  if (m_ok) {
    return pc - pm;
  }
  return float3(0.0f, 0.0f, 0.0f);
}

// The unit normal of the two tangents (x toward the right, y toward the bottom of the picture), toward the camera of a view looking along +z; zero when they do not span a surface.
float3 RrNormalOfTangents(float3 tx, float3 ty) {
  const float3 n = cross(tx, ty);
  const float l2 = dot(n, n);
  if (!(l2 > kRrMinTangent)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  return n * (1.0f / sqrt(l2));
}

// `n` turned to face the camera: `view_dir` is the direction from the camera to the surface.
float3 RrFacing(float3 n, float3 view_dir) {
  return dot(n, view_dir) > 0.0f ? n * -1.0f : n;
}

// ---- the material ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The linear roughness Ray Reconstruction asks for from the smoothness byte of RT3 (gbuffer_rules.h: the GGX alpha is (1 - s)^2, so 1 - s is the linear roughness).
float RrLinearRoughness(float smoothness) {
  return 1.0f - saturate(smoothness);
}

// F0 of a class: the reflectance at normal incidence. A dielectric 0.04 (paint under its clear coat, glass, asphalt, skin ...), a metal or chrome its albedo (RT2) where the frame has one, an
// emissive surface 0.
float3 RrClassF0(uint cls, float3 albedo, bool have_albedo) {
  if (cls == uint(GB_CLASS_CHROME) || cls == uint(GB_CLASS_METAL)) {
    return have_albedo ? albedo : float3(kRrMetalF0, kRrMetalF0, kRrMetalF0);
  }
  if (cls == uint(GB_CLASS_EMISSIVE)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  return float3(kRrDielectricF0, kRrDielectricF0, kRrDielectricF0);
}

// ---- NVIDIA's specular albedo (DLSS-RR Integration Guide, "Specular Albedo Generation": EnvBRDFApprox2, Ray Tracing Gems chapter 32) -------------------------------------------------------------
// The bias and the scale of the fit, with the matrices of the guide's text multiplied out: nov = |N.V|, alpha = linear roughness squared.
float RrEnvBias(float nov, float alpha) {
  const float x1 = nov;
  const float x2 = nov * nov;
  const float x3 = nov * x2;
  const float a3 = alpha * alpha * alpha;
  const float num = (0.99044f - 1.28514f * x1) + alpha * (1.29678f - 0.755907f * x1);
  const float den = (1.0f + 2.92338f * x1 + 59.4188f * x3) + alpha * (20.3225f - 27.0302f * x1 + 222.592f * x3) + a3 * (121.563f + 626.13f * x1 + 316.627f * x3);
  return num / den;
}

float RrEnvScale(float nov, float alpha) {
  const float x1 = nov;
  const float x2 = nov * nov;
  const float x3 = nov * x2;
  const float a3 = alpha * alpha * alpha;
  const float num = (0.0365463f + 3.32707f * x1) + alpha * (9.0632f - 9.04756f * x1);
  const float den = (1.0f + 3.59685f * x2 - 1.36772f * x3) + alpha * (9.04401f - 16.3174f * x2 + 9.22949f * x3) + a3 * (5.56589f + 19.7886f * x2 - 20.2123f * x3);
  return num / den;
}

// The specular albedo: F0 * scale + bias, the bias taken to 0 where F0 is 0 (the guide's hack for a surface that does not reflect), neither below 0.
float3 RrSpecularAlbedo(float3 f0, float alpha, float nov_signed) {
  const float nov = abs(nov_signed);
  const float bias = RrEnvBias(nov, alpha) * saturate(f0.y * 50.0f);
  const float scale = max(0.0f, RrEnvScale(nov, alpha));
  const float b = max(0.0f, bias);
  return float3(f0.x * scale + b, f0.y * scale + b, f0.z * scale + b);
}

// ---- the planes' texels ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// RT2 holds an albedo where any of its four components is above 0 (gbuffer_view_ps.hlsl: all zero is "no albedo"; an emissive pixel has its albedo and a lit weight of 0).
bool RrHasAlbedo(float4 rt2) {
  return rt2.x > 0.0f || rt2.y > 0.0f || rt2.z > 0.0f || rt2.w > 0.0f;
}

// The diffuse albedo guide: the albedo squared (never under the floor), 0.5 where the plane holds none.
float3 RrDiffuseAlbedo(float4 rt2) {
  if (!RrHasAlbedo(rt2)) {
    return float3(kRrUnknownAlbedo, kRrUnknownAlbedo, kRrUnknownAlbedo);
  }
  const float3 a = GbAlbedoDecode(float3(rt2.x, rt2.y, rt2.z));
  return float3(max(a.x, kRrMinAlbedo), max(a.y, kRrMinAlbedo), max(a.z, kRrMinAlbedo));
}

// ---- one pixel ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct RrPixel {
  float3 diffuse;
  float3 normal;       // world, unit (0 in the sky)
  float roughness;     // linear
  float3 specular;
  float hit;           // the hit distance guide, before a writer's
};

RrPixel RrSkyPixel() {
  RrPixel px;
  px.diffuse = float3(kRrSkyDiffuse, kRrSkyDiffuse, kRrSkyDiffuse);
  px.normal = float3(0.0f, 0.0f, 0.0f);
  px.roughness = 0.0f;
  px.specular = float3(0.0f, 0.0f, 0.0f);
  px.hit = kRrSkyHitDistance;
  return px;
}

// A pixel with a surface. `depth_normal` is the world normal of the depth's tangents (zero: none); `view_dir` the unit direction from the camera to the pixel. The normal is RT1's where it holds
// one (as the writer drew it: a back face's was turned to the viewer there), else the depth's turned to face the camera, else the one that faces the camera. N.V is taken as an absolute value
// (the guide's rule), so a normal that points away from the camera still gives a sensible specular albedo.
RrPixel RrSurfacePixel(float2 rt1, float4 rt2, float2 rt3, float3 depth_normal, float3 view_dir) {
  RrPixel px;
  float3 n = view_dir * -1.0f;
  if (GbRt1Valid(rt1)) {
    n = GbRt1Decode(rt1);
  } else if (dot(depth_normal, depth_normal) > 0.5f) {
    n = RrFacing(depth_normal, view_dir);
  }
  const bool have_albedo = RrHasAlbedo(rt2);
  const float3 albedo = GbAlbedoDecode(float3(rt2.x, rt2.y, rt2.z));
  const float roughness = RrLinearRoughness(GbRt3Smoothness(rt3));
  const float3 f0 = RrClassF0(GbRt3Class(rt3), albedo, have_albedo);
  px.diffuse = RrDiffuseAlbedo(rt2);
  px.normal = n;
  px.roughness = roughness;
  px.specular = RrSpecularAlbedo(f0, roughness * roughness, dot(n, view_dir * -1.0f));
  px.hit = 0.0f;
  return px;
}

// ---- the hit distance and the responsivity ---------------------------------------------------------------------------------------------------------------------------------------------
// The guide's value: the sky's 65504; else a writer's distance where it holds a positive finite value below the sky's, else 0 (the reflection is at the surface). `source` is 0 where the pass has none.
float RrHitDistance(bool sky, float source) {
  if (sky) {
    return kRrSkyHitDistance;
  }
  return (source > 0.0f && source < kRrSkyHitDistance) ? source : 0.0f;
}

// CITY-RT G6: the bounce light's per-pixel pass hands over its ray's length as the LAST choice of the hit distance: where no reflection wrote a positive distance (`source`), the bounce light's `bounce`
// stands (0 where its pass traced nothing, so the pixel stays the surface's). The reflections' distances are the specular's own and keep the pixel; the bounce light's image is the pass's own size (the
// occlusion's traced size over a divisor), so its texel under a render pixel is the pixel scaled by the two sizes, one axis at a time, clamped into the image.
float RrWithBounce(float source, float bounce) {
  return source > 0.0f ? source : bounce;
}
uint RrBounceTexel(uint pixel, uint size, uint hitSize) {
  return min(pixel * hitSize / max(size, 1u), max(hitSize, 1u) - 1u);
}

// (a NaN in either source is dropped and a NaN in both is 0, decided by the bits: nan_guard.hlsli; max and saturate of a NaN are the API's to say)
float RrResponsivity(float reactive, float transparency) {
  return NgSaturate(NgMax(reactive, transparency));
}

// ---- the debug views (what the view pass draws, 0..1 per channel) ------------------------------------------------------------------------------------------------------------------------
// diffuse and specular albedo as they are; the normal as 0.5 + 0.5 n (black where it is 0: the sky); the roughness as a grey; the hit distance on a log scale (black on a surface, blue in the
// sky, a grey for a writer's distance); the responsivity as a grey.
float3 RrViewColour(uint view, float3 diffuse, float4 normal_roughness, float3 specular, float hit, float responsivity) {
  if (view == uint(RR_VIEW_DIFFUSE)) {
    return diffuse;
  }
  if (view == uint(RR_VIEW_NORMAL)) {
    const float3 n = float3(normal_roughness.x, normal_roughness.y, normal_roughness.z);
    return dot(n, n) > 0.0f ? n * 0.5f + float3(0.5f, 0.5f, 0.5f) : float3(0.0f, 0.0f, 0.0f);
  }
  if (view == uint(RR_VIEW_ROUGHNESS)) {
    return float3(normal_roughness.w, normal_roughness.w, normal_roughness.w);
  }
  if (view == uint(RR_VIEW_SPECULAR)) {
    return specular;
  }
  if (view == uint(RR_VIEW_HIT)) {
    if (hit >= kRrSkyHitDistance * 0.99f) {
      return float3(0.0f, 0.0f, 1.0f);
    }
    const float g = saturate(log2(hit + 1.0f) * (1.0f / 12.0f));
    return float3(g, g, g);
  }
  if (view == uint(RR_VIEW_RESPONSIVITY)) {
    return float3(responsivity, responsivity, responsivity);
  }
  return float3(0.0f, 0.0f, 0.0f);
}

#endif  // MCLA_GPU_RR_GUIDES_COMMON_HLSLI
