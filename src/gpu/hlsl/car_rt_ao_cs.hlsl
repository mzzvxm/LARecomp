// MCLA GPU renderer -- ambient occlusion and contact shadows around the player's car, ray traced (car_rt_ao.h, car_rt_ao_rules.h).
//
// Run inside the occlusion's passes (ao.cpp Run), right after the depth chain and before the visibility pass, when this frame's acceleration
// structures of the player's car exist (car_rt_scene.h: the top level is in VIEW space, x right, y up, z forward, the AO's: ao_common.hlsli
// AoViewPos). One thread is one pixel of the occlusion's linear depth image (level 0, R32_FLOAT at the traced size, the vehicle bit in its
// lowest bit) and writes the pixel of the output image of the same size (R8G8B8A8_UNORM):
//
//   r  the occlusion's open share: 1 minus the share of the pixel's short rays that hit the car (1 = open)
//   g  the sun's open share: 0 where the ray toward the sun hits the car, else 1
//   b  1 where the pixel was traced, else 0 (with r and g 1)
//   a  1
//
// THE REGION. Only the pixels of the rectangle in the push block are looked at (the host's RegionRect: the hull of the boxes' rectangles, each box
// grown by the reach and extruded toward the sun), and of those only the pixels whose view position is within the reach of a box (the occlusion rays)
// or whose sun ray meets one (the sun ray). The boxes are the player's car's (the push) and the other cars' of the top level (the car RT table's rows
// from noise.w, two a box, the count in bits 12-15 of the flags: car_rt_instances_rules.h List::boxes), so the road between two cars is traced too.
// Everything else is written "open" at once. The occlusion's passes read the image by its slot: the main pass (the plain one and the bounce light's)
// takes min(its own visibility, the pass's r raised to the look's power), the contact march takes the darker of its own shadow and 1 - g.
//
// THE OCCLUSION RAYS: kRays (the push's) cosine weighted directions about the pixel's normal, the radius squared stratified by the ray's number, the
// angle turned by the golden angle a ray, the whole pattern rotated by the pixel's interleaved gradient noise and shifted by the occlusion's own
// 13 frame lattice (ao_stable.h: so that the history, which averages 12 frames, sees each rotation once). Each ray is as long as reach (1 - sqrt(u))
// with u from a stratum of its own (not its direction's), so it is stopped by a blocker at distance d with probability (1 - d / reach)^2: yes or
// no answers (a first hit ends the query) average to an occlusion that falls off with distance. The ray is lifted to at least kMinAbove of cosine
// above the tangent plane.
//
// THE SUN RAY: one ray, from the surface toward the sun spread over the sun's disc (the push's tangent), as long as the far side of the farthest box
// it meets; it is cast only for a surface that faces the sun (kSunFacingLow, as the contact march's) and whose ray meets a box.
//
// THE PARTS THE RAYS HIT are the mask in noise.y (car_rt_ao_rules.h kRayPartsWithOthers: the player's body and tyres and every part of another
// car); the primary ray that snaps a vehicle's pixel takes it too, so another car's pixels are snapped onto its own triangles.
//
// THE PIXEL'S SURFACE. A pixel is placed by the depth (view position, normal from the depth's neighbours, as ao_common.hlsli AoNormalFromImage),
// lifted off its surface by a few footprints. A vehicle's pixel is first snapped onto the car's triangles by a primary ray from the camera
// through the pixel in a window around the depth (car_rt_refl_cs.hlsl's, the same tolerance): the game's depth and the triangles differ by
// a centimetre, and a ray that starts under its surface finds its own panel. A hit there gives the exact point and the mesh's normal, and a ray that
// later hits the same instance's triangle, or a surface that faces within 32 degrees the way the normal does, is the panel itself (a miss).
//
// Shader Model 6.5, inline ray queries (DXR tier 1.1). The layout (car_rt_ao.cpp): set 0 the bindless Texture2D<float4> set (space 0: the depth image),
// set 1 the output (u0, space 1), set 2 the car's acceleration structures (CAR_RT_SPACE 2), the push block (b0, space 3; a push constant on Vulkan).

#define CAR_RT_SPACE 2
#include "car_rt_common.hlsli"
#include "car_rt_refl_common.hlsli"

Texture2D<float4> g_textures[] : register(t0, space0);
#ifdef __spirv__
// Vulkan takes the format of a storage image from the shader: the image is R8G8B8A8_UNORM (car_rt_ao.cpp EnsureOutput).
[[vk::image_format("rgba8")]]
#endif
RWTexture2D<float4> g_output : register(u0, space1);

struct CarRtAoPush
{
    // x the linear depth image's bindless slot, y and z the image's width and height, w the flags (car_rt_ao_rules.h kPush*): bit 0 the occlusion
    // rays, bit 1 the sun ray, bits 8-11 the occlusion rays a pixel (1 to 4), bits 12-15 the other cars' boxes (0 to 8)
    uint4 slots;
    // the pixel rectangle that is looked at: x0, y0, x1, y1 (exclusive)
    uint4 rect;
    // x the noise code (ao_stable.h NoiseCode: the main pass's, bit 30 the lattice), y the parts the rays hit (CAR_RT_MASK_*), z the frame the sun
    // ray's spread is shifted by (0 where it stands still on the screen), w the first row of the other cars' boxes in the car RT table (gCarRtInstances)
    uint4 noise;
    // x, y tan of the half field of view (the depth image's lens), z the occlusion rays' reach in metres, w the distance beyond which nothing is traced
    float4 lens;
    // xyz the direction to the sun in view space (unit; 0 = unknown), w the tangent of the sun disc's radius
    float4 light;
    // xyz the player's car's box in view space, w the sun ray's reach in metres
    float4 box_min;
    // xyz the player's car's box in view space, w reserved
    float4 box_max;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarRtAoPush> g_push;
#else
ConstantBuffer<CarRtAoPush> g_push : register(b0, space3);
#endif

// car_rt_ao_rules.h: the numbers and the flags.
static const uint kPushOcclusion = 1u << 0;
static const uint kPushSun = 1u << 1;
static const uint kPushBoxesShift = 12;
static const float kOtherBoxMargin = 0.15;    // what the other cars' boxes are grown by (the player's is grown on the host)
static const float kSky = 60000.0;            // what the depth chain holds for the sky (ao_common.hlsli kAoSky)
static const float kTwoPi = 6.283185307179586;
static const float kGolden = 0.6180339887;
static const float kMinAbove = 0.08;          // a ray is kept at least this far (as a cosine) above the point's tangent plane
static const float kSelfCos = 0.85;           // a hit on the primary ray's instance that faces within 32 degrees of the point's normal is the panel itself
static const float kSunFacingLow = 0.03;
static const float kDepthLiftFloor = 0.006;
static const float kDepthLiftPixels = 3.0;
static const float kSnapFootprint = 0.75;     // car_rt_refl_cs.hlsl's primary ray window
static const float kBoxGrowSun = 0.1;         // the sun ray's box test is a little roomy: the origin is the depth's, not the surface's
static const uint kLatticeFlag = 1u << 30;    // ao_stable.h kLatticeFlag

// Interleaved gradient noise (ao_common.hlsli AoNoise).
float Ign(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// The frame's shift of the noise (ao_common.hlsli AoNoiseShift).
float2 NoiseShift()
{
    const uint code = g_push.noise.x;
    if ((code & kLatticeFlag) != 0u)
    {
        const uint m = code & 0xFFFu;
        return float2(float(m), float((m * 8u) % 13u)) * (1.0 / 13.0);
    }
    return float(code & 1023u) * float2(0.75487766624669, 0.56984029099806);
}

float LoadDepth(int2 p)
{
    return g_textures[g_push.slots.x].Load(int3(p, 0)).x;
}

// The view-space position of the pixel centre `centre` (in pixels of an image `size` wide and high) at distance t (ao_common.hlsli AoViewPos).
float3 ViewPos(float2 centre, float2 size, float t)
{
    const float2 uv = centre / size;
    return float3((uv.x * 2.0 - 1.0) * g_push.lens.x * t, (1.0 - uv.y * 2.0) * g_push.lens.y * t, t);
}

// The view-space normal from the depth image (ao_common.hlsli AoNormalFromImage: the neighbour on each axis whose depth is nearer to the pixel's).
float3 NormalFromDepth(int2 p, int2 size, float tc, float3 pc)
{
    const int2 last = size - 1;
    const int2 pl = int2(max(p.x - 1, 0), p.y);
    const int2 pr = int2(min(p.x + 1, last.x), p.y);
    const int2 pt = int2(p.x, max(p.y - 1, 0));
    const int2 pb = int2(p.x, min(p.y + 1, last.y));
    const float tl = LoadDepth(pl);
    const float tr = LoadDepth(pr);
    const float tt = LoadDepth(pt);
    const float tb = LoadDepth(pb);
    const float2 fs = float2(size);
    const float3 h = abs(tl - tc) < abs(tr - tc) ? pc - ViewPos(float2(pl) + 0.5, fs, tl) : ViewPos(float2(pr) + 0.5, fs, tr) - pc;
    const float3 v = abs(tt - tc) < abs(tb - tc) ? ViewPos(float2(pt) + 0.5, fs, tt) - pc : pc - ViewPos(float2(pb) + 0.5, fs, tb);
    const float3 n = cross(v, h);
    const float len = length(n);
    return len > 1e-20 ? n / len : float3(0.0, 0.0, -1.0);
}

// An orthonormal frame about the unit vector n (Duff et al. 2017).
void Frame(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// The slab test of a ray against the box [bmin, bmax]: true when it meets it in [0, t_max], `t_exit` the parameter where it leaves.
bool RayMeetsBox(float3 origin, float3 direction, float3 bmin, float3 bmax, float t_max, out float t_exit)
{
    const float3 safe = float3(abs(direction.x) < 1e-12 ? 1e-12 : direction.x, abs(direction.y) < 1e-12 ? 1e-12 : direction.y,
                               abs(direction.z) < 1e-12 ? 1e-12 : direction.z);
    const float3 inv = 1.0 / safe;
    const float3 a = (bmin - origin) * inv;
    const float3 b = (bmax - origin) * inv;
    const float3 lo3 = min(a, b);
    const float3 hi3 = max(a, b);
    const float lo = max(max(lo3.x, lo3.y), max(lo3.z, 0.0));
    const float hi = min(min(hi3.x, hi3.y), min(hi3.z, t_max));
    t_exit = hi;
    return lo <= hi;
}

// One yes or no ray: does anything of the car's parts (the push's mask) lie within `t_max` of `origin` along `direction`? A first hit ends the search.
// `own`: the origin is on the car (snapped): the instance and triangle it left, and a surface facing the way its normal does, are not blockers.
bool Blocked(float3 origin, float3 direction, float t_max, bool own, uint own_instance, uint own_primitive, float3 own_normal)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = t_max;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.noise.y, ray);
    while (query.Proceed())
    {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }
    if (own)
    {
        const uint instance = query.CommittedInstanceIndex();
        if (instance == own_instance)
        {
            const uint primitive = query.CommittedPrimitiveIndex();
            if (primitive == own_primitive)
            {
                return false;  // the triangle the ray left
            }
            const float3 hit_normal = CarRtHitNormal(instance, primitive, query.CommittedTriangleBarycentrics(), query.CommittedObjectToWorld3x4(), direction);
            if (dot(hit_normal, hit_normal) > 0.25 && dot(hit_normal, own_normal) > kSelfCos)
            {
                return false;  // a panel that faces the way this one does: the ray ran under its own surface
            }
        }
    }
    return true;
}

// A vehicle's pixel snapped onto the car's triangles: a primary ray from the camera through the pixel in a window around the depth t. True: a car of
// the top level (the player's or another's: the push's mask) is the surface here, `position` the exact point, `normal` the mesh's normal (facing
// the camera).
bool Snap(float2 centre, float2 size, float t, out float3 position, out float3 normal, out uint instance, out uint primitive)
{
    position = float3(0.0, 0.0, 0.0);
    normal = float3(0.0, 0.0, -1.0);
    instance = 0u;
    primitive = 0u;
    const float2 uv = centre / size;
    const float3 direction = float3((uv.x * 2.0 - 1.0) * g_push.lens.x, (1.0 - uv.y * 2.0) * g_push.lens.y, 1.0);
    const float pixel_m = PixelMeters(t, g_push.lens.y, size.y);
    const float window = Tolerance(t, pixel_m, kSnapFootprint, 0.0);
    RayDesc primary;
    primary.Origin = float3(0.0, 0.0, 0.0);
    primary.Direction = direction;
    primary.TMin = max(t - window, 0.001);
    primary.TMax = t + window;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> first;
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.noise.y, primary);
    while (first.Proceed())
    {
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }
    const float hit_t = first.CommittedRayT();
    const uint hit_instance = first.CommittedInstanceIndex();
    const uint hit_primitive = first.CommittedPrimitiveIndex();
    const float3 view_direction = normalize(direction);
    const float3 hit_normal = CarRtHitNormal(hit_instance, hit_primitive, first.CommittedTriangleBarycentrics(), first.CommittedObjectToWorld3x4(), view_direction);
    if (!(dot(hit_normal, hit_normal) > 0.25))
    {
        return false;
    }
    const float nv = abs(dot(hit_normal, view_direction));
    if (abs(hit_t - t) > Tolerance(t, pixel_m, kSnapFootprint, nv))
    {
        return false;  // a surface of the car, but not the one the picture shows here
    }
    position = direction * hit_t;
    normal = hit_normal;
    instance = hit_instance;
    primitive = hit_primitive;
    return true;
}

// One pixel.
float4 TracePixel(uint2 pixel, uint2 size)
{
    const float4 open = float4(1.0, 1.0, 0.0, 1.0);
    const float raw = LoadDepth(int2(pixel));
    const bool vehicle = (asuint(raw) & 1u) != 0u;
    const float t = asfloat(asuint(raw) & ~1u);
    if (!(t > 0.0) || t >= kSky * 0.5 || t >= g_push.lens.w)
    {
        return open;  // the sky, or past where the occlusion has faded out
    }
    const float2 fsize = float2(size);
    const float2 centre = float2(pixel) + 0.5;
    float3 p = ViewPos(centre, fsize, t);

    // Which terms this pixel can have: the occlusion rays when it is near a box of the region, the sun ray when it meets one. The region is the
    // player's car's box (the push) and the other cars' (the car RT table, two rows each from noise.w: car_rt_instances_rules.h List::boxes).
    const uint flags = g_push.slots.w;
    const float reach = g_push.lens.z;
    const float3 to_sun = g_push.light.xyz;
    const bool ao_wanted = (flags & kPushOcclusion) != 0u;
    const bool sun_wanted = (flags & kPushSun) != 0u && dot(to_sun, to_sun) > 0.5;
    const uint box_count = 1u + ((flags >> kPushBoxesShift) & 0xFu);
    bool want_ao = false;
    bool want_sun = false;
    float sun_exit = 0.0;
    [loop]
    for (uint b = 0u; b < box_count; ++b)
    {
        float3 box_min = g_push.box_min.xyz;
        float3 box_max = g_push.box_max.xyz;
        if (b != 0u)
        {
            const uint row = g_push.noise.w + (b - 1u) * 2u;
            box_min = asfloat(gCarRtInstances[row].xyz) - kOtherBoxMargin;
            box_max = asfloat(gCarRtInstances[row + 1u].xyz) + kOtherBoxMargin;
        }
        want_ao = want_ao || (ao_wanted && all(p >= box_min - reach) && all(p <= box_max + reach));
        if (sun_wanted)
        {
            float t_exit;
            if (RayMeetsBox(p, to_sun, box_min - kBoxGrowSun, box_max + kBoxGrowSun, g_push.box_min.w, t_exit))
            {
                want_sun = true;
                sun_exit = max(sun_exit, t_exit);
            }
        }
    }
    if (!want_ao && !want_sun)
    {
        return open;
    }

    // The surface: the depth's, or for a vehicle's pixel the car's own triangles.
    float3 n = NormalFromDepth(int2(pixel), int2(size), t, p);
    if (dot(n, p) > 0.0)
    {
        n = -n;  // facing the camera
    }
    const float footprint_unit = 2.0 * g_push.lens.y / fsize.y;
    bool own = false;
    uint own_instance = 0u;
    uint own_primitive = 0u;
    float lift = kDepthLiftFloor + kDepthLiftPixels * footprint_unit * t;
    if (vehicle)
    {
        float3 snapped_p;
        float3 snapped_n;
        uint instance;
        uint primitive;
        if (Snap(centre, fsize, t, snapped_p, snapped_n, instance, primitive))
        {
            own = true;
            own_instance = instance;
            own_primitive = primitive;
            p = snapped_p;
            n = snapped_n;
            lift = 0.004 + 0.002 * snapped_p.z;
        }
    }
    const float3 origin = p + n * lift;

    // The occlusion rays.
    float open_ao = 1.0;
    if (want_ao)
    {
        const uint rays = clamp((flags >> 8) & 0xFu, 1u, 4u);
        const float2 spatial = float2(Ign(float2(pixel)), Ign(float2(pixel).yx + float2(31.0, 11.0)));
        const float2 noise = frac(spatial + NoiseShift());
        float3 tangent;
        float3 bitangent;
        Frame(n, tangent, bitangent);
        // The pairing of the rays' directions with their length strata is turned by the pixel's noise (car_rt_ao_rules.h LengthRotation).
        const uint rotation = min(uint(frac(noise.x * 13.7 + noise.y * 5.3) * float(rays)), rays - 1u);
        float blocked = 0.0;
        [unroll]
        for (uint k = 0u; k < 4u; ++k)
        {
            if (k >= rays)
            {
                break;
            }
            const float r2 = (float(k) + min(noise.y, 0.9999)) / float(rays);
            const float angle = kTwoPi * (noise.x + kGolden * float(k));
            const float r = sqrt(r2);
            float3 d = tangent * (r * cos(angle)) + bitangent * (r * sin(angle)) + n * sqrt(max(1.0 - r2, 0.0));
            const float above = dot(d, n);
            if (above < kMinAbove)
            {
                d = normalize(d + (kMinAbove - above) * n);
            }
            const float jitter = frac(noise.y * 5.0 + noise.x * 3.0 + float(k) * 0.4142135);
            const float u = (float((k * 3u + 1u + rotation) % rays) + min(jitter, 0.9999)) / float(rays);
            const float length_k = reach * (1.0 - sqrt(u));
            if (Blocked(origin, d, length_k, own, own_instance, own_primitive, n))
            {
                blocked += 1.0;
            }
        }
        open_ao = 1.0 - blocked / float(rays);
    }

    // The sun ray: from a surface that faces the sun, over the sun's disc, to the car's far side.
    float open_sun = 1.0;
    if (want_sun && dot(n, to_sun) > kSunFacingLow)
    {
        const float frame_shift = float(g_push.noise.z & 1023u);
        const float u1 = frac(Ign(float2(pixel) + float2(17.0, 5.0)) + frame_shift * 0.75487766624669);
        const float u2 = frac(Ign(float2(pixel).yx + float2(3.0, 29.0)) + frame_shift * 0.56984029099806);
        float3 sx;
        float3 sy;
        Frame(to_sun, sx, sy);
        const float radius = sqrt(u1) * g_push.light.w;
        const float disc_angle = kTwoPi * u2;
        const float3 direction = normalize(to_sun + sx * (radius * cos(disc_angle)) + sy * (radius * sin(disc_angle)));
        if (Blocked(origin, direction, min(g_push.box_min.w, sun_exit + 0.05), own, own_instance, own_primitive, n))
        {
            open_sun = 0.0;
        }
    }
    return float4(open_ao, open_sun, 1.0, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 size = uint2(g_push.slots.y, g_push.slots.z);
    if (any(id.xy >= size))
    {
        return;
    }
    float4 result = float4(1.0, 1.0, 0.0, 1.0);
    // Most of the image is outside the rectangle: one compare and one write.
    if (all(id.xy >= g_push.rect.xy) && all(id.xy < g_push.rect.zw))
    {
        result = TracePixel(id.xy, size);
    }
    g_output[id.xy] = result;
}
