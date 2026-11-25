// Per-pixel RT bounce is already irradiance without receiver albedo.
// Pack its hit distance and build AMD's guides at the trace's own size.
#include "host_pass_common.hlsli"
#include "gbuffer_common.hlsli"
#include "ray_regen_common.hlsli"

struct BouncePush {
  uint4 inputs;       // signal, depth, hit/coverage, RT1
  uint4 planes_size;  // RT2, RT3, width, height
  uint3 render_step;  // render width, render height, pixels per trace step
  float depth_sign;
  float4 lens;        // current and previous tan-half-FOV pairs
  float4 motion[3];
  float2 jitter;      // render pixels
  uint have_motion;
  uint padding;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<BouncePush> g_push;
[[vk::image_format("rgba16f")]]
#else
ConstantBuffer<BouncePush> g_push : register(b0, space4);
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
RWTexture2D<float4> g_diffuse : register(u4, space5);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_push.planes_size.zw)) return;
  const int3 tap = int3(id.xy, 0);
  const float t = g_textures[g_push.inputs.y].Load(tap).x;
  const float2 hit = g_textures[g_push.inputs.z].Load(tap).xy;
  const bool traced = hit.y > 0.0f && t > 0.0f && t < kRgNoDepth;
  g_depth[id.xy] = RgDepthTexel(traced, t, g_push.depth_sign);
  if (!traced) {
    g_signal[id.xy] = 0.0f;
    g_motion[id.xy] = 0.0f;
    g_normals[id.xy] = RgNormalsTexel(float3(0.0f, 1.0f, 0.0f), 1.0f, 0u);
    g_diffuse[id.xy] = 0.0f;
    return;
  }

  // Same representative render pixel as gi_pixel_trace_cs, including
  // the governor's divisor and the last texel of odd-sized targets.
  const uint2 render = g_push.render_step.xy;
  const uint2 pixel = min(id.xy * g_push.render_step.z, render - 1u);
  const int3 gb = int3(pixel, 0);
  const float2 normal = g_textures[g_push.inputs.w].Load(gb).xy;
  const float2 material = g_textures[g_push.planes_size.y].Load(gb).xy;
  const uint cls = GbRt3Class(material);
  const float roughness = cls == GB_CLASS_NONE ? 1.0f : 1.0f - GbRt3Smoothness(material);
  const float3 albedo = GbAlbedoDecode(g_textures[g_push.planes_size.x].Load(gb).xyz);
  g_normals[id.xy] = RgNormalsTexel(GbRt1Decode(normal), roughness, 0u);
  g_diffuse[id.xy] = float4(cls == GB_CLASS_METAL ? float3(0.0f, 0.0f, 0.0f) : albedo, 1.0f);
  g_signal[id.xy] = RgSignalTexel(g_textures[g_push.inputs.x].Load(tap).xyz, hit.x, true);

  float4 motion = 0.0f;
  if (g_push.have_motion != 0u) {
    const float2 uv = (float2(pixel) + 0.5f - g_push.jitter) / float2(render);
    const float2 xy = (uv * 2.0f - 1.0f) * float2(g_push.lens.x, -g_push.lens.y) * t;
    const float4 view_point = float4(xy, t, 1.0f);
    const float3 prev = float3(dot(g_push.motion[0], view_point), dot(g_push.motion[1], view_point), dot(g_push.motion[2], view_point));
    if (prev.z > 1.0e-3f) {
      const float2 prev_uv = float2(0.5f, -0.5f) * prev.xy / (prev.z * g_push.lens.zw) + 0.5f;
      motion = RgMotionTexel(uv, prev_uv.x, prev_uv.y, prev.z, t, true, g_push.depth_sign);
    }
  }
  g_motion[id.xy] = all(abs(motion.xyz) < 60000.0f) ? motion : 0.0f;
}
