// The additive light is the same in the standalone and merged blend.
float SampleShaftValue(float2 uv, uint slot) {
  return g_textures[slot].SampleLevel(g_samplers[kSamplerLinearClamp], uv, 0).x;
}

float3 SampleShaftLight(float2 uv, uint slot, float3 tint) {
  const float3 light = tint * SampleShaftValue(uv, slot);
  return max(light.x, max(light.y, light.z)) > 1.0f / 4096.0f ? light : 0.0f;
}
