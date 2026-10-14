// MCLA GPU renderer -- one triangle covering the viewport, from SV_VertexID.
//
// Vertices (-1, 1), (3, 1), (-1, -3) in D3D clip space; on Vulkan dxc flips the
// output y (-fvk-invert-y), so the same pixels are covered on both backends.
// The pixel shaders work from SV_Position alone.

float4 main(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
}
