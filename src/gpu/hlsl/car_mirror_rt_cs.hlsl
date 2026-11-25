// MCLA GPU renderer -- the player's side mirrors show the real view behind, ray traced (car_mirror_rt.h; src/gpu/car_mirror_rt_rules.h has the design, the numbers and the tests).
//
// Run at the car ray tracing's trigger (the game's mid-scene depth copy), right after this frame's top level was built (car_rt_scene.h: the player's car and the cars beside it, in the frame's VIEW
// space, x right, y up, z forward: a ray from the origin through a pixel with the direction (ndc.x tanX, ndc.y tanY, 1) hits at RayT = the view z). One thread is one pixel of a mirror's
// rectangle (the push's rect: first pixel and size, in render pixels). It writes the image (u0, the push's image.xy is the rectangle's first texel; R16G16B16A16_FLOAT): rgb the colour of what
// the mirror shows in the scene colour's units, alpha the share of the game's glass it replaces (the coverage of the glass in the pixel times the strength; 0 off the glass).
//
//   1. THE GLASS: four primary rays (a rotated grid inside the pixel) against the player's parts (the push's image.z). Each goes on through the panes of see-through glass (the car's windows:
//      CAR_RT_FLAG_SEE_THROUGH in the instance's row), so a cockpit camera sees the mirror through the side window as it does in the game; the nearest other part decides. The share of the rays whose
//      nearest part is a mirror glass (CAR_RT_FLAG_MIRROR) is the pixel's coverage: the glass's edge is as soft as the pixel and a pillar in front of it covers it exactly. The first such ray gives the
//      glass's point and its smooth mesh normal (the glass is slightly convex: the normals vary over it, as a real mirror's view widens).
//   2. THE MIRROR RAY: one ray from the glass along reflect(view, normal), the start lifted off it along the normal, against the player's parts and the cars beside (the push's image.w; the glass's own
//      panes of see-through glass are looked through as for the primary ray). A hit on a car is shaded from its material (car_rt_hit_shade.hlsli CarHitShade: the paint's colour, the diffuse texture, the
//      sun and its visibility, the sky's openness, the environment by the fresnel, the lamps at night), and a hit on ANOTHER car fades into the world's colour over the last quarter of the cars' reach
//      (the scene holds cars only within it: a car is not cut off at the edge of the scene). What the ray misses is MirrorWorld().
//   3. THE WORLD: MirrorWorld() is the game's environment map at the mirror direction (a dual paraboloid made around the camera: the sky and the LOD city, read as the game's own glass reads it,
//      times the colour target's units). It is THE HOOK for the city: where the city is in the same top level (feat/city-rt-all, CityRefl's ResolveHit, "the screen or the shaded city hit"), a hit on
//      it replaces the environment here, and the environment is what is left beyond the city's reach.
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_mirror_rt.cpp): set 0 the bindless Texture2D<float4> set (space 0: the environment map and the diffuse textures of the
// materials, read with Load), set 1 the output (u0, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2), the push block (b0, space 3).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader (DXC would say rgba32f for a float4): the image is R16G16B16A16_FLOAT (car_mirror_rt.cpp EnsureImage).
[[vk::image_format("rgba16f")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);

struct CarMirrorPush
{
    // x, y the first pixel of the rectangle in the picture, z, w its size in pixels
    uint4 rect;
    // x, y the rectangle's first texel in the image, z the primary ray's parts (CAR_RT_MASK_*), w the mirror ray's parts
    uint4 image;
    // x, y tan of half the horizontal and the vertical field of view, z, w the picture's size in pixels
    float4 lens;
    // x the strength (the share of the game's glass the picture replaces), y the cars' reach in metres (the mirror ray's length), z the share of it up to which a hit on another car weighs 1,
    // w the units: the colour target's over the shaders' output, 2^exp_bias (16 at the game's bias of 4): a colour made from the materials' constants is multiplied by it
    float4 tune;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarMirrorPush> g_push;
#else
ConstantBuffer<CarMirrorPush> g_push : register(b0, space3);
#endif

#include "car_rt_hit_shade.hlsli"

// car_mirror_rt_rules.h: the same numbers
static const float kMirrorEnvLod = 0.5;          // the environment map's level for a mirror
static const float kMirrorLift = 0.003;          // m: the mirror ray starts this far off the glass along the normal ...
static const float kMirrorLiftPerMetre = 0.001;  // ... plus this a metre of the glass's view distance
static const float kMirrorMinNv = 0.03;          // a glass seen edge on is not traced
static const float kMirrorPrimaryReach = 40.0;   // m
static const uint kMirrorSeeThroughSteps = 3;    // panes of see-through glass a ray goes on through
static const uint kMirrorTaps = 4;               // the coverage's rays a pixel
static const float kMirrorPaneSkip = 0.002;      // m: the ray goes on this far past a pane it looked through
// the four taps: a rotated grid inside the pixel (offsets from the pixel's centre, in pixels)
static const float2 kMirrorTap[4] = {float2(-0.125, -0.375), float2(0.375, -0.125), float2(0.125, 0.375), float2(-0.375, 0.125)};

// The nearest part of the scene along a ray whose parts meet `mask`, going on through the panes of see-through glass (the car's windows), at most kMirrorSeeThroughSteps of them. `t` of the result is
// the distance from the ray's ORIGIN in the direction's own units (the sum over the steps). `flags` is the hit instance's flags word.
bool MirrorTrace(float3 origin, float3 direction, float reach, uint mask, out CarRtHit hit, out uint flags)
{
    hit = (CarRtHit)0;
    flags = 0u;
    float travelled = 0.0;
    float3 from = origin;
    [loop]
    for (uint step = 0u; step <= kMirrorSeeThroughSteps; ++step)
    {
        const CarRtHit h = CarRtTrace(from, direction, 0.0, max(reach - travelled, 0.0), mask);
        if (!h.hit)
        {
            return false;
        }
        const uint f = gCarRtInstances[h.instance_index].z;
        if ((f & CAR_RT_FLAG_SEE_THROUGH) != 0u && step < kMirrorSeeThroughSteps)
        {
            const float skip = h.t + kMirrorPaneSkip;
            from = from + direction * skip;
            travelled += skip;
            continue;
        }
        hit = h;
        hit.t = travelled + h.t;
        flags = f;
        return true;
    }
    return false;
}

// The colour of the world at a direction (view space): THE HOOK for the city (see the header). The game's environment map at the direction by the world's axes, read as the paint's sharp read
// is (rt_hit_sample.hlsli RtHitEnvAt: the map's own paraboloid) and in the colour target's units; without the map (a frame whose draws read none) the car's ambient and a sky gradient.
// `known` is false when the frame holds neither the map nor the shading's rows (the ambient is then zero, which would paint the glass black): the caller leaves the game's glass alone.
float3 MirrorWorld(float3 direction, out bool known)
{
    const RtHitFrame f = RtHitLoadFrame();
    known = (f.flags & (RT_HIT_FRAME_ENV | RT_HIT_FRAME_SHADE)) != 0u;
    if ((f.flags & RT_HIT_FRAME_ENV) != 0u)
    {
        return RtHitEnvAt(f, direction, kMirrorEnvLod) * g_push.tune.w;
    }
    return f.ambient * (0.6 + 0.4 * dot(direction, f.axisY)) * (f.gain * g_push.tune.w);
}

// The pixel: rgb what the mirror shows, alpha the share of the game's glass it replaces (0 where the pixel is not mirror glass).
float4 MirrorPixel(uint2 pixel)
{
    const float2 render = g_push.lens.zw;
    uint hits = 0u;
    bool have = false;
    CarRtHit glass = (CarRtHit)0;
    float3 glassDirection = float3(0.0, 0.0, 1.0);
    [unroll]
    for (uint k = 0u; k < kMirrorTaps; ++k)
    {
        const float2 uv = (float2(pixel) + 0.5 + kMirrorTap[k]) / render;
        const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
        CarRtHit h;
        uint flags;
        if (MirrorTrace(float3(0.0, 0.0, 0.0), direction, kMirrorPrimaryReach, g_push.image.z, h, flags) && (flags & CAR_RT_FLAG_MIRROR) != 0u)
        {
            hits += 1u;
            if (!have)
            {
                have = true;
                glass = h;
                glassDirection = direction;
            }
        }
    }
    if (!have)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float coverage = float(hits) / float(kMirrorTaps);
    const float3 viewDirection = normalize(glassDirection);
    const float3 normal = CarRtHitNormal(glass.instance_index, glass.primitive_index, glass.barycentrics, glass.object_to_world, viewDirection);
    if (!(dot(normal, normal) > 0.25) || abs(dot(normal, viewDirection)) < kMirrorMinNv)
    {
        return float4(0.0, 0.0, 0.0, 0.0);  // no usable normal, or the glass seen edge on: the game's own picture stays
    }

    // the mirror ray
    const float glassT = glass.t;  // along the (non unit) direction, z = 1: the view z of the point
    const float3 glassPoint = glassDirection * glassT;
    const float3 mirror = reflect(viewDirection, normal);
    const float3 origin = glassPoint + normal * (kMirrorLift + kMirrorLiftPerMetre * glassT);
    const float reach = g_push.tune.y;
    bool worldKnown;
    const float3 world = MirrorWorld(mirror, worldKnown);
    float3 colour = world;
    bool shown = worldKnown;  // something real to show: the world, or a part of the player's own car
    CarRtHit r;
    uint rflags;
    [branch]
    if (MirrorTrace(origin, mirror, reach, g_push.image.w, r, rflags))
    {
        const float3 hitNormal = CarRtHitNormal(r.instance_index, r.primitive_index, r.barycentrics, r.object_to_world, mirror);
        if (dot(hitNormal, hitNormal) > 0.25)
        {
            const bool other = (CarRtInstanceMask(CarRtLoadInstance(r.instance_index)) & CAR_RT_MASK_OTHERS) != 0u;
            const float3 hitPoint = origin + mirror * r.t;
            // the footprint of the pixel at the hit: the pixel's angle times the distance travelled (the camera to the glass, then the ray)
            const float pixelAngle = 2.0 * g_push.lens.y / max(g_push.lens.w, 1.0);
            const float footprint = max(pixelAngle * (glassT + r.t), 1e-3);
            float3 shaded;
            if (CarHitShade(r.instance_index, r.primitive_index, r.barycentrics, hitNormal, mirror, hitPoint, footprint, 0.0, g_push.tune.w, -1.0, shaded))
            {
                const float fade = (other && worldKnown) ? 1.0 - smoothstep(g_push.tune.z * reach, reach, r.t) : 1.0;
                colour = lerp(world, shaded, fade);
                shown = shown || fade >= 1.0;
            }
        }
    }
    if (!shown || any(isnan(colour)) || any(isinf(colour)))
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return float4(min(max(colour, 0.0), float3(60000.0, 60000.0, 60000.0)), coverage * saturate(g_push.tune.x));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_push.rect.z || id.y >= g_push.rect.w)
    {
        return;
    }
    g_output[id.xy + g_push.image.xy] = MirrorPixel(id.xy + g_push.rect.xy);
}
