// MCLA GPU renderer -- what every ray traced hit shading shares (CITY-RT M2 package H4): the frame's rows and the hit's light (rt_hit_frame.hlsli), the texture, environment, cascade and
// sun ray reads (rt_hit_sample.hlsli). The C++ twin is src/gpu/rt_hit_frame.h.
//
// The car hit shading (hlsl/car_rt_hit_shade.hlsli, RT-HIT-SHADING) held, beside what is about a car, the pieces every hit shading needs. The city hit library (hlsl/city_hit_shade.hlsli)
// shades a hit on the city from the same frame, so they moved to these two files by name (CarMat* became RtHit*, bodies unchanged: tools/check_rt_hit_identity.py compiles the car shaders
// from before and after the move and compares them); the car file keeps only the car's own (its classes, its instance records, its uv buffer, CarHitShade, the clear coat).
//
// THE INCLUDER defines RT_HIT_FRAME_ROW(i), declares `Texture2D<float4> g_textures[]` and, unless it defines RT_HIT_NO_RAYQUERY, RT_HIT_TLAS (see the two files).

#ifndef MCLA_RT_HIT_COMMON_HLSLI
#define MCLA_RT_HIT_COMMON_HLSLI

#include "rt_hit_frame.hlsli"
#include "rt_hit_sample.hlsli"

#endif  // MCLA_RT_HIT_COMMON_HLSLI
