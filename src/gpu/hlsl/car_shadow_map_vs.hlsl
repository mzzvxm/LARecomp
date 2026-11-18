// MCLA GPU renderer -- the player car's sun shadow map, the depth-only vertex shader (car_shadow_map.h).
//
// One draw of the car's own geometry, from the sun: the vertex view of the draw itself (the host's byte-swapped copy, the same
// buffers the main view's draw read), with POSITION as the one input, and no pixel shader. The position is three floats (a float4
// stream is read as its first three) or four halves; the pipeline's input element says which, and this reads three components of
// either. A draw's push block is the model -> map clip matrix, the rows of a row-vector matrix (car_shadow_map_rules.h ModelToClip):
//   clip = p.x rows[0] + p.y rows[1] + p.z rows[2] + rows[3]
// with x and y in [-1, 1] across the map (y up), z the depth in [0, 1] (0 towards the sun) and w 1: an orthographic view, so there is
// no perspective divide to speak of. On Vulkan dxc flips the output y (-fvk-invert-y), the same pixels on both backends.

struct CarShadowPush {
  float4 rows[4];
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<CarShadowPush> g_push;
#else
ConstantBuffer<CarShadowPush> g_push : register(b0, space0);
#endif

struct VsInput {
  [[vk::location(0)]] float3 position : POSITION;
};

float4 main(VsInput input) : SV_Position {
  return input.position.x * g_push.rows[0] + input.position.y * g_push.rows[1] + input.position.z * g_push.rows[2] +
         g_push.rows[3];
}
