// MCLA GPU renderer -- DLSS Ray Reconstruction's guide images, the debug view (rr_guides.h, mcla_gpu_dlss_rr_view).
//
// One compute pass over the UPSCALER'S OUTPUT image (R16G16B16A16_FLOAT, the output size), run right after the upscaler wrote it: one guide (rr_guides_rules.h View) of the frame, read at the
// render size's pixel that covers each output pixel (nearest: a blocky picture, which is what it is), written over the picture the upscaler made. The game's tone map, bloom and the UI are
// drawn over it as usual, so what shows is the guide after the tone map. The views are in hlsl/rr_guides_common.hlsli RrViewColour: diffuse albedo, the normal (0.5 + 0.5 n), the roughness,
// the specular albedo, the hit distance (black on a surface, blue in the sky) and the responsivity.
//
// Layout (rr_guides.cpp): sets 0 to 3 the bindless texture set four times, set 4 the output image (u0, space 4), the push block at (b0, space 5).

#include "host_pass_common.hlsli"
#include "rr_guides_common.hlsli"

struct RrGuidesViewPush
{
    // x the diffuse albedo image's bindless slot, y the normal + roughness image's, z the specular albedo's, w the hit distance's
    uint4 slots;
    // x the responsivity image's slot, y the view (rr_guides_rules.h View)
    uint4 slots2;
    // x, y the render size, z, w the output size
    uint4 sizes;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RrGuidesViewPush> g_push;
#else
ConstantBuffer<RrGuidesViewPush> g_push : register(b0, space5);
#endif

#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space4);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 render = int2(g_push.sizes.xy);
    const int2 output = int2(g_push.sizes.zw);
    const int2 p = int2(id.xy);
    if (any(p >= output))
    {
        return;
    }
    // the render pixel under the output pixel's centre
    const float2 uv = (float2(p) + 0.5f) / float2(output);
    const int2 q = clamp(int2(uv * float2(render)), int2(0, 0), render - int2(1, 1));
    const float4 diffuse = g_textures[g_push.slots.x].Load(int3(q, 0));
    const float4 normal_roughness = g_textures[g_push.slots.y].Load(int3(q, 0));
    const float4 specular = g_textures[g_push.slots.z].Load(int3(q, 0));
    const float hit = g_textures[g_push.slots.w].Load(int3(q, 0)).x;
    const float responsivity = g_textures[g_push.slots2.x].Load(int3(q, 0)).x;
    const float3 colour = RrViewColour(g_push.slots2.y, diffuse.xyz, normal_roughness, specular.xyz, hit, responsivity);
    g_output[p] = float4(colour, 1.0f);
}
