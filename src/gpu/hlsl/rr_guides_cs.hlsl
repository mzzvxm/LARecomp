// MCLA GPU renderer -- DLSS Ray Reconstruction's guide pass (rr_guides.h, perf/DLSS-RR.md, package RR-2).
//
// One compute pass at the anchor (the game's motion blur draw, where the upscaler runs): one thread is one pixel of the render-size images. It reads the G-buffer's three planes
// (gbuffer_types.h, at the host sample that is the guest's sample 0 when the planes are multisampled), the kept depth (R32_FLOAT, 1 near / 0 far) and the upscaler's two masks, and writes the
// five guides of hlsl/rr_guides_common.hlsli (read its header for what each one holds):
//
//   u0  diffuse albedo      RGBA16F, alpha 1
//   u1  specular albedo     RGBA16F, alpha 1
//   u2  normal + roughness  RGBA16F: the world normal, the linear roughness in alpha
//   u3  hit distance        R16F: 0 on a surface, 65504 in the sky, a writer's distance where one is given (RR-4's reflections: the screen-space reflections' image and the cars' ray traced one; the larger wins;
//                           CITY-RT G6: where neither wrote one, the bounce light's per-pixel pass's ray length, from its own image at its own size)
//   u4  responsivity        R16F: the larger of the reactive and the transparency masks
//
// A pixel whose stored depth is 0 is the sky (the defaults of the integration guide). A surface pixel whose RT1 holds no normal (a shader with no G-buffer variant, a mirror) gets the normal of the
// depth's tangents; the five depth reads for it are made only for such a pixel.
//
// Layout (rr_guides.cpp): sets 0 to 3 the bindless texture set four times (space 0 Texture2D<float4>[], space 1 Texture2DMS<float4>[], ...: host_pass_common.hlsli), set 4 the five images (u0..u4,
// space 4), the push block at (b0, space 5). RR_MS 0 reads single-sample planes, RR_MS 1 the host's MSAA level.

#include "host_pass_common.hlsli"
#include "rr_guides_common.hlsli"

#ifndef RR_MS
#define RR_MS 0
#endif

struct RrGuidesPush
{
    // x RT1's bindless slot, y RT2's, z RT3's, w the kept depth's
    uint4 slots;
    // x the reactive mask's slot, y the transparency mask's (0xFFFFFFFF: none), z a hit distance writer's image, w a second writer's (0xFFFFFFFF: none; R16F at the render size, the distance in x)
    uint4 slots2;
    // x the width, y the height of the render size, z the host sample that is the guest's sample 0, w the bounce light's hit image's slot (0xFFFFFFFF: none; RG16F at the per-pixel pass's size, the distance in x)
    uint4 info;
    // xyz the camera's right axis in world space, w tan of half the horizontal field of view
    float4 right;
    // xyz the up axis, w tan of half the vertical field of view
    float4 up;
    // xyz the forward axis, w the pass's flags as bits (RR_FLAG_PBR: the shared physically based model, mcla_gpu_pbr; PBR level 1C)
    float4 forward;
    // the projection's depth rows P22 P23 P32 P33
    float4 lin;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<RrGuidesPush> g_push;
#else
ConstantBuffer<RrGuidesPush> g_push : register(b0, space5);
#endif

// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4 and r32f for a float).
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_diffuse : register(u0, space4);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_specular : register(u1, space4);
#ifdef __spirv__
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_normal : register(u2, space4);
#ifdef __spirv__
[[vk::image_format("r16f")]]
#endif
RWTexture2D<float> g_hit : register(u3, space4);
#ifdef __spirv__
[[vk::image_format("r16f")]]
#endif
RWTexture2D<float> g_responsivity : register(u4, space4);

static const uint kNone = 0xFFFFFFFFu;

float4 LoadPlane(uint slot, int2 p)
{
#if RR_MS
    return g_textures_ms[slot].Load(p, g_push.info.z);
#else
    return g_textures[slot].Load(int3(p, 0));
#endif
}

// The linear depth at a pixel (kRrSky for the sky and for a pixel outside the image: it is not a surface).
float LinearAt(int2 p, int2 size)
{
    if (any(p < int2(0, 0)) || any(p >= size))
    {
        return kRrSky;
    }
    const float stored = g_textures[g_push.slots.w].Load(int3(p, 0)).x;
    return RrLinearDepth(stored, g_push.lin.x, g_push.lin.y, g_push.lin.z, g_push.lin.w);
}

// The view-space point of pixel `p` at distance `t`.
float3 PointAt(int2 p, int2 size, float t)
{
    return RrViewPos(float(p.x) + 0.5f, float(p.y) + 0.5f, float(size.x), float(size.y), t, g_push.right.w, g_push.up.w);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 size = int2(g_push.info.xy);
    const int2 p = int2(id.xy);
    if (any(p >= size))
    {
        return;
    }
    const float stored = g_textures[g_push.slots.w].Load(int3(p, 0)).x;
    float4 reactive = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 transparency = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (g_push.slots2.x != kNone)
    {
        reactive = g_textures[g_push.slots2.x].Load(int3(p, 0));
    }
    if (g_push.slots2.y != kNone)
    {
        transparency = g_textures[g_push.slots2.y].Load(int3(p, 0));
    }
    float source = 0.0f;
    if (g_push.slots2.z != kNone)
    {
        source = g_textures[g_push.slots2.z].Load(int3(p, 0)).x;
    }
    if (g_push.slots2.w != kNone)
    {
        // RR-4: a second writer (the cars' ray traced reflections beside the screen-space ones): where both hold a distance the larger stays (the two never trace the same pixel: a vehicle's pixel takes
        // none of the screen-space effect). The max keeps the other one where a source is NaN (NgMax: decided by the bits, for Vulkan's FMax leaves which one open).
        source = NgMax(source, g_textures[g_push.slots2.w].Load(int3(p, 0)).x);
    }
    if (g_push.info.w != kNone && !(source > 0.0f))
    {
        // CITY-RT G6: the last choice: the bounce light's per-pixel pass's ray length (its image is the occlusion's traced size over a divisor: the texel under this pixel is the pixel scaled by the sizes).
        uint hitWidth;
        uint hitHeight;
        g_textures[g_push.info.w].GetDimensions(hitWidth, hitHeight);
        const int2 q = int2(int(RrBounceTexel(uint(p.x), uint(size.x), hitWidth)), int(RrBounceTexel(uint(p.y), uint(size.y), hitHeight)));
        source = RrWithBounce(source, g_textures[g_push.info.w].Load(int3(q, 0)).x);
    }
    const bool sky = !(stored > 0.0f);
    RrPixel px = RrSkyPixel();
    if (!sky)
    {
        const float3 right = g_push.right.xyz;
        const float3 up = g_push.up.xyz;
        const float3 forward = g_push.forward.xyz;
        const float3 view_dir = RrViewDir(float(p.x) + 0.5f, float(p.y) + 0.5f, float(size.x), float(size.y), g_push.right.w, g_push.up.w, right, up, forward);
        const float2 rt1 = LoadPlane(g_push.slots.x, p).xy;
        const float4 rt2 = LoadPlane(g_push.slots.y, p);
        const float3 rt3 = LoadPlane(g_push.slots.z, p).xyz;
        float3 depth_normal = float3(0.0f, 0.0f, 0.0f);
        if (!GbRt1Valid(rt1))
        {
            const float zc = RrLinearDepth(stored, g_push.lin.x, g_push.lin.y, g_push.lin.z, g_push.lin.w);
            const float zl = LinearAt(p + int2(-1, 0), size);
            const float zr = LinearAt(p + int2(1, 0), size);
            const float zu = LinearAt(p + int2(0, -1), size);
            const float zd = LinearAt(p + int2(0, 1), size);
            const float3 pc = PointAt(p, size, zc);
            const float3 tx = RrTangent(pc, zc, PointAt(p + int2(-1, 0), size, zl), zl, PointAt(p + int2(1, 0), size, zr), zr);
            const float3 ty = RrTangent(pc, zc, PointAt(p + int2(0, -1), size, zu), zu, PointAt(p + int2(0, 1), size, zd), zd);
            const float3 view_normal = RrNormalOfTangents(tx, ty);
            depth_normal = RrWorldOfView(view_normal, right, up, forward);
        }
        if ((asuint(g_push.forward.w) & RR_FLAG_PBR) != 0u)
        {
            px = RrSurfacePixelPbr(rt1, rt2, rt3, depth_normal, view_dir);
        }
        else
        {
            px = RrSurfacePixel(rt1, rt2, rt3.xy, depth_normal, view_dir);
        }
    }
    g_diffuse[p] = float4(px.diffuse, 1.0f);
    g_specular[p] = float4(px.specular, 1.0f);
    g_normal[p] = float4(px.normal, px.roughness);
    g_hit[p] = RrHitDistance(sky, source);
    g_responsivity[p] = sky ? 0.0f : RrResponsivity(reactive.x, transparency.x);
}
