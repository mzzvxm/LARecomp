// GI-BOUNCE-RT: the city hit library compiled as C++ a THIRD time, in its own namespace (hlsl_shim::emit), the way a pass that has emitters includes it (hlsl/gi_pixel_trace_cs.hlsl, gi_update_trace_cs.hlsl):
// `CITY_HIT_EMITTER_COUNT`, `CITY_HIT_EMITTER_RADIANCE(i, P, N)` and `CITY_HIT_EMITTER_TO(i, P)` over a list of the test's own, and CityHitEmitterRay (the library's ray query is not compiled in this test) declared
// here. The law of a fake emitter is the game's plateau law with N.L (the same arithmetic the twin's lambda in city_hit_emitter_checks.inc has); the real law is light_laws.hlsli's and is checked by
// rt_emitters_rules_test.cpp over the world frame's rows. Not a header: included by city_hit_shade_rules_test.cpp at namespace scope, after city_hit_lamp_term_lib.inc. The checks are
// city_hit_emitter_checks.inc.

namespace hlsl_shim {
struct EmitterRec {
  float pos[3];      // view space
  float colour[3];
  float range;
  float plateau;
};
std::vector<EmitterRec> gEmitters;
std::vector<RayCall> gEmitterCalls;
int gEmitterRadianceCalls = 0, gEmitterToCalls = 0;
inline float3 EmitterRadiance(uint i, float3 P, float3 N) {
  ++gEmitterRadianceCalls;
  const EmitterRec& e = gEmitters[i];
  const float3 d = float3(e.pos[0], e.pos[1], e.pos[2]) - P;
  const float d2 = dot(d, d);
  if (!(d2 < e.range * e.range)) {
    return float3(0.0f, 0.0f, 0.0f);
  }
  const float inv = 1.0f / sqrt(d2);
  const float ndl = saturate(dot(N, d * inv));
  const float term = d2 <= e.plateau ? 1.0f / e.plateau : 1.0f / d2;
  return float3(e.colour[0], e.colour[1], e.colour[2]) * (term * ndl);
}
inline float3 EmitterTo(uint i, float3 P) {
  ++gEmitterToCalls;
  const EmitterRec& e = gEmitters[i];
  return float3(e.pos[0], e.pos[1], e.pos[2]) - P;
}
}  // namespace hlsl_shim

#undef MCLA_CITY_HIT_COMMON_HLSLI
#undef MCLA_RT_HIT_COMMON_HLSLI
#undef MCLA_RT_HIT_FRAME_HLSLI
#undef MCLA_RT_HIT_SAMPLE_HLSLI
#undef MCLA_CITY_HIT_SHADE_HLSLI
#define CITY_HIT_EMITTER_COUNT (hlsl_shim::gEmitters.size())
#define CITY_HIT_EMITTER_RADIANCE(i, P, N) hlsl_shim::EmitterRadiance((i), (P), (N))
#define CITY_HIT_EMITTER_TO(i, P) hlsl_shim::EmitterTo((i), (P))

namespace hlsl_shim {
namespace emit {
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
inline bool CityHitEmitterRay(float3 origin, float3 direction, float reach, float footprint) {
  (void)footprint;
  gEmitterCalls.push_back({{origin.x, origin.y, origin.z}, {direction.x, direction.y, direction.z}, reach});
  return FakeBlocked(origin.x, origin.y, origin.z, direction.x, direction.y, direction.z);
}
#include "hlsl/city_hit_shade.hlsli"
}  // namespace emit
}  // namespace hlsl_shim

#undef CITY_HIT_EMITTER_COUNT
#undef CITY_HIT_EMITTER_RADIANCE
#undef CITY_HIT_EMITTER_TO
