// MCLA GPU renderer -- the car shadow map's matrix texture (car_shadow_map.h CarShadowMapView::matrix_slot).
//
// A 4x1 R32G32B32A32_FLOAT image whose texel k is the push block's row k: rows 0 to 2 the map's to_map rows (the AO's view space ->
// u, v, depth), row 3 (texel_m, depth_m, size, 1). It is there for a pass whose own push block has no room for a matrix; one tiny
// draw of the host pass layout writes it each frame the map is recorded.

#include "host_pass_common.hlsli"

struct CarShadowMatrixPush {
  float4 rows[4];
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarShadowMatrixPush> g_push;
#else
ConstantBuffer<CarShadowMatrixPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  return g_push.rows[min(uint(position.x), 3u)];
}
