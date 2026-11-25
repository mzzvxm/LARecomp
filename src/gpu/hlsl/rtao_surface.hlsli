// The vehicle snap and surface frame follow car_rt_ao_cs.hlsl.
#ifndef MCLA_GPU_RTAO_SURFACE_HLSLI
#define MCLA_GPU_RTAO_SURFACE_HLSLI

static const float kSelfCos = 0.85;
static const float kSnapFootprint = 0.75;

float LoadDepth(int2 p)
{
    const float raw = g_textures[g_push.slots.x].Load(int3(p, 0)).x;
    return asfloat(asuint(raw) & ~7u);
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
    first.TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.trace.y, primary);
    while (first.Proceed())
    {
    }
    if (first.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return false;
    }
    // The city is in this query's mask but has no row in the car geometry table.
    if ((first.CommittedInstanceID() & kInstanceCity) != 0u)
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

bool SelfHit(uint instance, uint primitive, float2 bary, float3x4 object_to_world,
             float3 direction, uint own_instance, uint own_primitive, float3 own_normal)
{
    if (instance != own_instance)
        return false;
    if (primitive == own_primitive)
        return true;
    const float3 hit_normal = CarRtHitNormal(instance, primitive, bary, object_to_world, direction);
    return dot(hit_normal, hit_normal) > 0.25 && dot(hit_normal, own_normal) > kSelfCos;
}

float Lift(float depth, float footprint)
{
    return 0.006 + 3.0 * footprint + 0.001 * depth;
}

#endif
