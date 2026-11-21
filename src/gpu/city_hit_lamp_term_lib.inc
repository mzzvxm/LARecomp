// CITY-RT R7: the city hit library compiled as C++ a SECOND time, in its own namespace (hlsl_shim::grid), the way a pass that takes the world frame's lamp grid and glow schedule includes it
// (hlsl/rt_world_lamp_light_cs.hlsl): `#define CITY_HIT_LAMP_GRID`, `CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)`, a counter for the census (CITY_HIT_CENSUS_ADD). Not a header: it is
// included by city_hit_shade_rules_test.cpp at namespace scope, right after the first inclusion (the flat lamp range, in hlsl_shim::flat), so the two paths are compiled side by side and the
// flat one is shown to be unchanged. The checks are city_hit_lamp_term_checks.inc.

namespace hlsl_shim {
StructuredBuffer<uint4> gRtWorld;   // the world frame's rows (t10) and lamps (t11)
StructuredBuffer<uint4> gRtLamps;
std::vector<uint32_t> gCensus(8, 0);  // CITY_HIT_CENSUS_ADD's counters (words 0..7)
}  // namespace hlsl_shim

#undef MCLA_CITY_HIT_COMMON_HLSLI
#undef MCLA_RT_HIT_COMMON_HLSLI
#undef MCLA_RT_HIT_FRAME_HLSLI
#undef MCLA_RT_HIT_SAMPLE_HLSLI
#undef MCLA_CITY_HIT_SHADE_HLSLI
#define RT_WORLD_NO_RESOURCES
#define RT_WORLD_LOOP
#define CITY_HIT_LAMP_GRID
#define CITY_HIT_GLOW_FACTOR(kind) RtWorldGlowFactor(kind)
#define CITY_HIT_CENSUS_ADD(word, value) (hlsl_shim::gCensus[(word)] += (value))

namespace hlsl_shim {
namespace grid {
#include "hlsl/rt_world_frame.hlsli"
#include "hlsl/city_hit_common.hlsli"
#include "hlsl/rt_hit_common.hlsli"
inline bool CityHitSunRay(float3 origin, float3 direction, float reach, float footprint, float sunTan) {
  (void)footprint;
  (void)sunTan;
  gSunCalls.push_back({{origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, reach});
  return FakeBlocked(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
}
inline bool CityHitLampRay(float3 origin, float3 direction, float reach, float footprint) {
  (void)footprint;
  gLampCalls.push_back({{origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, reach});
  return FakeBlocked(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
}
#include "hlsl/city_hit_shade.hlsli"
}  // namespace grid
}  // namespace hlsl_shim

#undef CITY_HIT_LAMP_GRID
#undef CITY_HIT_GLOW_FACTOR
#undef CITY_HIT_CENSUS_ADD
