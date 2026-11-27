#include "host_pass_common.hlsli"
#include "pbr_screen_common.hlsli"
#include "gbuffer_common.hlsli"
#include "ray_regen_common.hlsli"

struct RayRegenAoPush {
  uint4 inputs;
  uint4 gbuffer;
  uint4 sizes;
  float4 lens;
  float4 motion[3];
  float2 jitter;
  float depth_sign;
  uint vehicle_ids;  // IDs 1-23, bit 31 selects PBR guides.
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RayRegenAoPush> g_push;
[[vk::image_format("r16f")]]
#else
ConstantBuffer<RayRegenAoPush> g_push : register(b0, space4);
#endif
RWTexture2D<float> g_signal : register(u0, space1);
#ifdef __spirv__
[[vk::image_format("r32f")]]
#endif
RWTexture2D<float> g_depth : register(u1, space1);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_motion : register(u2, space1);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_normals : register(u3, space1);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_specular : register(u4, space1);

float4 Motion(float2 uv, float depth, uint vehicle, bool on_vehicle) {
  float4 m0 = g_push.motion[0], m1 = g_push.motion[1], m2 = g_push.motion[2];
  if (on_vehicle) {
    const uint slot = g_push.gbuffer.w;
    if (vehicle == 0u || vehicle > 23u || (g_push.vehicle_ids & (1u << vehicle)) == 0u ||
        g_textures[slot].Load(int3(3, vehicle, 0)).x <= 0.5f) return 0.0f;
    m0 = g_textures[slot].Load(int3(0, vehicle, 0));
    m1 = g_textures[slot].Load(int3(1, vehicle, 0));
    m2 = g_textures[slot].Load(int3(2, vehicle, 0));
  }
  const float2 xy = (uv * 2.0f - 1.0f) * float2(g_push.lens.x, -g_push.lens.y) * depth;
  const float4 p = float4(xy, depth, 1.0f);
  const float3 prev = float3(dot(m0, p), dot(m1, p), dot(m2, p));
  if (!(prev.z > 1.0e-3f) || !all(g_push.lens.zw > 0.0f)) return 0.0f;
  const float2 prev_uv = float2(0.5f, -0.5f) * prev.xy / (prev.z * g_push.lens.zw) + 0.5f;
  const float4 motion = RgMotionTexel(uv, prev_uv.x, prev_uv.y, prev.z, depth, true, g_push.depth_sign);
  return all(abs(motion.xyz) < 60000.0f) ? motion : 0.0f;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_push.sizes.xy)) return;
  const int2 p = int2(id.xy);
  const int3 tap = int3(p, 0);
  const float4 signal = g_textures[g_push.inputs.x].Load(tap);
  const float depth = g_textures[g_push.inputs.y].Load(tap).x;
  const float4 guide = g_textures[g_push.inputs.z].Load(tap);
  const float2 vehicle_share = g_textures[g_push.inputs.w].Load(tap).xy;
  const bool judged = signal.g > 0.5f && depth > 0.0f && depth < kRgNoDepth && vehicle_share.y > 0.0f;
  g_signal[p] = judged ? saturate(signal.r) : 1.0f;
  g_depth[p] = RgDepthTexel(judged, depth, g_push.depth_sign);
  g_motion[p] = 0.0f;
  g_normals[p] = RgNormalsTexel(float3(0.0f, 1.0f, 0.0f), 1.0f, 0u);
  g_specular[p] = 0.0f;
  if (!judged) return;

  const float2 uv = (float2(p) + 0.5f) / float2(g_push.sizes.xy);
  const int3 gb = int3(min(int2(uv * float2(g_push.sizes.zw)), int2(g_push.sizes.zw) - 1), 0);
  const float2 normal = g_textures[g_push.gbuffer.x].Load(gb).xy;
  const float3 material = g_textures[g_push.gbuffer.z].Load(gb).xyz;
  const uint cls = GbRt3Class(material.xy);
  const float smoothness = GbRt3Smoothness(material.xy);
  const bool pbr = (g_push.vehicle_ids & (1u << 31)) != 0u && cls != GB_CLASS_NONE;
  const float roughness = cls == GB_CLASS_NONE ? guide.w : (pbr ? PbrRoughnessFromSmoothness(smoothness) : 1.0f - smoothness);
  const float3 world_normal = GbRt1Valid(normal) ? GbRt1Decode(normal) : float3(0.0f, 1.0f, 0.0f);
  const float3 stored_albedo = g_textures[g_push.gbuffer.y].Load(gb).xyz;
  float3 specular = cls == GB_CLASS_METAL ? GbAlbedoDecode(stored_albedo) : float3(0.04f, 0.04f, 0.04f);
  if (pbr) {
    const float f0 = PbrF0OfClass(cls);
    specular = lerp(f0.xxx, GbAlbedoDecode(stored_albedo), PbrMetalOfScreen(cls, GbRt3MetalFlag(material)));
  }
  g_normals[p] = RgNormalsTexel(world_normal, roughness, 0u);
  g_specular[p] = float4(specular, 1.0f);
  g_motion[p] = Motion(uv - g_push.jitter / float2(g_push.sizes.xy), depth, uint(vehicle_share.x), (asuint(depth) & 1u) != 0u);
}
