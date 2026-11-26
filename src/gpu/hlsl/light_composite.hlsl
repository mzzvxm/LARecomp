// LT-5. The old full-size evaluators supply the probe field and the vehicle
// branch. The half-size pass carries separate direct and indirect terms so
// only bounce receives the AO multiplier.
#include "vpl.hlsl"
#include "light_composite_common.hlsli"

#define LL_PASS_MS VPL_MS
#define LP_COMPOSITE
#define g_push lp_push
#include "local_light_ps.hlsl"
#undef g_push

#if !LC_IRRADIANCE
SamplerState g_samplers[] : register(s0, space5);
#define GIA_MS VPL_MS
#define GIA_COMPOSITE
#define g_push gia_push
#include "gi_apply_ps.hlsl"
#undef g_push
#endif

static const uint kDirectOffset = 976u;
static const uint kGiPushOffset = 2032u;

uint4 SetDirectPush() {
  lp_push.slots = g_frame.Load4(kDirectOffset);
  const uint4 info = g_frame.Load4(kDirectOffset + 16u);
  lp_push.info = uint4(0u, 0u, info.y, 0u);
  return info;
}

uint4 DirectDraw(uint index) {
  return g_frame.Load4(kDirectOffset + 32u + index * 32u);
}

uint DirectClass(int2 pixel, out bool vehicle) {
  const float2 material = LoadPlane(lp_push.slots.w, pixel).xy;
  vehicle = (GbRt3Flags(material) & GB_FLAG_VEHICLE) != 0u;
  return GbRt3Class(material);
}

bool DirectReceiver(uint material) {
  return material != GB_CLASS_NONE && material != GB_CLASS_EMISSIVE;
}

float4 DirectIrradiance(View v, int2 p, uint count) {
  const LpGlobals g = LpLoadGlobals();
  const int2 pixel = min(p * 2, int2(v.sizes.xy) - 1);
  const float2 uv = (float2(pixel) + 0.5f) / v.sizes.xy;
  const float raw = Chain(v, uv, 0);
  const float depth = MainDepth(v, pixel);
  if (!(depth < kSky)) return 0.0f;

  bool vehicle;
  if (!DirectReceiver(DirectClass(pixel, vehicle)) || vehicle) return 0.0f;

  // CHECK: compare wall, kerb and track-prop lighting against the full-size draw.
  // Use the shared depth on the same surface. A block that straddles a
  // silhouette keeps its anchor's depth and uses the nearest normal samples.
  const float shared_depth = Linear(raw);
  const float tolerance = max(0.20f, 0.018f * depth);
  const float z = abs(shared_depth - depth) < tolerance ? shared_depth : depth;
  const float3 world = LpWorldPosition(g, float(pixel.x) + 0.5f, float(pixel.y) + 0.5f, z);
  float3 normal = 0.0f;
  bool have_normal = false;
  float3 sum = 0.0f;
  [loop] for (uint d = 0u; d < count; ++d) {
    const uint4 draw = DirectDraw(d);
    [loop] for (uint i = draw.x; i < draw.x + draw.y; ++i) {
      const float4 reach = asfloat(g_lights.Load4(LP_LIGHTS_OFFSET + i * LL_LIGHT_BYTES));
      if (!LpReaches(reach.xyz, reach.w, world)) continue;
      if (!have_normal) {
        normal = WorldVector(v, GeometricNormal(v, int2(uv * v.counts.xy), z));
        if (dot(normal, g.position - world) < 0.0f) normal = -normal;
        have_normal = true;
      }
      sum += NgFinite3(LpPixelLight(LlUnpack(LpLoadLight(i)), g, world, normal, 1.0f, 1.0f));
    }
  }
  return float4(sum, depth);
}

#if LC_IRRADIANCE

struct Irradiance {
  float4 bounce : SV_Target0;
  float4 direct : SV_Target1;
};

Irradiance LightIrradianceMain(float4 position : SV_Position) {
  const View v = VplView();
  Irradiance out_value;
  out_value.bounce = v.counts.z != 0u ? IrradianceMain(position) : 0.0f;
  const uint4 direct = SetDirectPush();
  out_value.direct = DirectIrradiance(v, int2(position.xy), direct.x);
  return out_value;
}

#else

float3 DirectFullSize(float4 position, int2 pixel, uint4 info) {
  float3 sum = 0.0f;
  [loop] for (uint d = 0u; d < info.x; ++d) {
    const int4 rect = asint(g_frame.Load4(kDirectOffset + 48u + d * 32u));
    if (any(pixel < rect.xy) || any(pixel >= rect.zw)) continue;
    const uint4 draw = DirectDraw(d);
    lp_push.info = uint4(draw.xy, info.y, draw.z);
    sum += LpEvaluateDraw(position).rgb;
  }
  return sum;
}

float3 DirectApply(View v, float4 position) {
  const uint4 info = SetDirectPush();
  const int2 pixel = int2(position.xy);
  bool vehicle;
  const uint material = DirectClass(pixel, vehicle);
  if (!DirectReceiver(material)) return 0.0f;
  // Cabin materials under the cockpit camera take no direct light.
  // CHECK: compare hands, wheel and dash in cockpit and paint in chase.
  if (v.apply.z != 0u && (material == GB_CLASS_INTERIOR || material == GB_CLASS_SKIN)) {
    const LpGlobals g = LpLoadGlobals();
    float3 world;
    if (LpWorldAt(g, pixel, world) && InCabin(v, world, Vehicle(v, pixel, 0.0f))) return 0.0f;
  }
  // Reuse the original draw groups and contact donor math on vehicles.
  if (vehicle) return DirectFullSize(position, pixel, info);

  const float2 grid = float2(pixel) * 0.5f;
  const int2 base = int2(grid);
  const float2 t = frac(grid);
  float4 taps[4];
  float3 total = 0.0f;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    const int2 at = min(base + int2(i & 1u, i >> 1u), int2(v.sizes.zw) - 1);
    taps[i] = g_textures[v.apply.w].Load(int3(at, 0));
    total += taps[i].rgb;
  }
  if (!any(total > 0.0f) && taps[0].a > 0.0f && taps[1].a > 0.0f && taps[2].a > 0.0f && taps[3].a > 0.0f) return 0.0f;
  const float depth = MainDepth(v, pixel);
  if (!(depth < kSky)) return 0.0f;
  const float4 material_colour = LoadPlane(lp_push.slots.z, pixel);
  if (!(material_colour.w > 0.0f)) return 0.0f;
  float3 sum = 0.0f;
  float weights = 0.0f;
  [unroll] for (uint j = 0u; j < 4u; ++j) {
    const float4 tap = taps[j];
    const float bilinear = ((j & 1u) ? t.x : 1.0f - t.x) * ((j & 2u) ? t.y : 1.0f - t.y);
    const float weight = LcTapWeight(depth, tap.a, false, bilinear);
    sum += tap.rgb * weight;
    weights += weight;
  }
  // Sparse geometry may not have a same-surface tap. Avoid pulling light
  // across the edge by evaluating the original draw on this pixel.
  if (!(weights > 0.0001f)) return DirectFullSize(position, pixel, info);
  return GbAlbedoDecode(material_colour.rgb) * material_colour.w * (sum / weights);
}

float4 LightCompositeMain(float4 position : SV_Position) : SV_Target {
  const View v = VplView();
  float3 add = 0.0f;
  if (all(int2(position.xy) < int2(v.sizes.xy))) {
    if (v.counts.z != 0u) add += VplComposite(position).rgb;
    if (v.apply.w != kNoSlot) add += DirectApply(v, position);
  }
  gia_push.slots = g_frame.Load4(kGiPushOffset);
  if (gia_push.slots.x != kNoSlot) {
    gia_push.info = g_frame.Load4(kGiPushOffset + 16u);
    add += GiaApply(position).rgb;
  }
  if (!any(add > 0.0f)) discard;
  return float4(add, 0.0f);
}

#endif
