// MCLA GPU renderer -- the ray tracing benchmark's trace (rt_bench.cpp, rt_bench_rules.h).
//
// Every thread traces g_push.info.w rays (kRaysPerThread) from random points inside a small synthetic city (a grid of boxes on a ground plane,
// instanced in a grid: rt_bench_rules.h MakeScene) in random directions, with inline ray queries against the top level structure, and counts
// the rays that hit something. Random, incoherent rays: the traversal cost the city's reflections and bounce light would have, with no shading and
// no texture fetch, so the number is the ray tracing units' and the driver's own. The origin and direction are hashes of the thread, the ray's number and the
// dispatch's seed, the same on every machine (rt_bench_rules.h Hash and Unit have the same numbers; rt_bench_rules_test.cpp checks the host's side).
//
// The two counters (a 16 byte buffer) make the work observable and prove the rays traced something: word 0 the rays traced, word 1 the rays that hit.
// The wave's lanes add up first, so a million rays do not all fight for one address.
//
// Layout (rt_bench.cpp): set 0 the top level (t0, space0), set 1 the counters (u0, space1), the push range at (b0, space2; a Vulkan push constant block).
// Compiled at Shader Model 6.5 (DXIL), and to SPIR-V 1.5 for Vulkan 1.2 (VK_KHR_ray_query), like the car passes (larecomp_gpu_host_shader_rt).

RaytracingAccelerationStructure gTlas : register(t0, space0);
RWByteAddressBuffer gCounters : register(u0, space1);

struct BenchPush {
  // x the dispatch's seed, y the dispatch's number in the run, z the threads along a row of the dispatch, w the rays a thread traces
  uint4 info;
  // x the side of the city in metres (origins are spread over it), y the height range of the origins, z the ray length, w unused
  float4 range;
};
#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<BenchPush> g_push;
#else
ConstantBuffer<BenchPush> g_push : register(b0, space2);
#endif

// rt_bench_rules.h Hash and Unit.
uint Hash(uint x) {
  x = (x ^ 61u) ^ (x >> 16);
  x *= 9u;
  x = x ^ (x >> 4);
  x *= 0x27d4eb2du;
  x = x ^ (x >> 15);
  return x;
}

float Unit(uint h) {
  return float(h >> 8) * (1.0f / 16777216.0f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint thread = id.y * g_push.info.z + id.x;
  uint rays = 0u;
  uint hits = 0u;
  [loop] for (uint r = 0u; r < g_push.info.w; ++r) {
    const uint h = Hash(thread * 4099u + r * 7919u + g_push.info.x + g_push.info.y * 104729u);
    const float3 origin = float3(Unit(h) * g_push.range.x, 1.0f + Unit(Hash(h ^ 0x68bc21ebu)) * g_push.range.y, Unit(Hash(h ^ 0x02e5be93u)) * g_push.range.x);
    // A direction uniform on the sphere.
    const float z = Unit(Hash(h ^ 0x967a889bu)) * 2.0f - 1.0f;
    const float angle = Unit(Hash(h ^ 0x27d4eb2fu)) * 6.2831853f;
    const float s = sqrt(max(0.0f, 1.0f - z * z));
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = float3(s * cos(angle), z, s * sin(angle));
    ray.TMin = 0.01f;
    ray.TMax = g_push.range.z;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(gTlas, RAY_FLAG_NONE, 0xFFu, ray);
    while (query.Proceed()) {
    }
    ++rays;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
      ++hits;
    }
  }
  const uint wave_rays = WaveActiveSum(rays);
  const uint wave_hits = WaveActiveSum(hits);
  if (WaveIsFirstLane()) {
    uint old;
    gCounters.InterlockedAdd(0u, wave_rays, old);
    gCounters.InterlockedAdd(4u, wave_hits, old);
  }
}
