// MCLA GPU renderer -- one row of the vehicle table (vehicle_motion.h, temporal_mv.h).
//
// The motion vector pass reads, for the vehicle with stencil ID k, the columns x, y and w of the 4x4 that
// takes this frame's clip space to the vehicle's own previous guest pixels (made on the host in double,
// vehicle_motion.cpp BuildVehicleTable). They live in the row k of a 4 x 24 RGBA32F image, written here
// by one draw a vehicle over a viewport of 4 x 1 pixels at (0, k): the pixel x of the viewport is the
// texel x of the row, 0 to 2 the three columns and 3 a flag, (1, 0, 0, 0): the row is usable. The image is
// cleared to zero first, so the row of an ID nobody drew reads as not usable.

#include "host_pass_common.hlsli"

struct TablePush {
  float4 col_x;
  float4 col_y;
  float4 col_w;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<TablePush> g_push;
#else
ConstantBuffer<TablePush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const uint texel = uint(position.x);
  if (texel == 0u) {
    return g_push.col_x;
  }
  if (texel == 1u) {
    return g_push.col_y;
  }
  if (texel == 2u) {
    return g_push.col_w;
  }
  return float4(1.0f, 0.0f, 0.0f, 0.0f);
}
