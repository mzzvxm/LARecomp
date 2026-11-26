#ifndef MCLA_GPU_SPEC_OCCLUSION_HLSLI
#define MCLA_GPU_SPEC_OCCLUSION_HLSLI

// The paint pack builder embeds these declarations in the translated source.
// Host shaders and the CPU twin compile them directly.
#ifndef SPEC_OCCLUSION_SHADER_CODE
#define SPEC_OCCLUSION_SHADER_CODE(...) __VA_ARGS__
#endif

SPEC_OCCLUSION_SHADER_CODE(
static const uint kSpecularOcclusionPaintFlag = 8u;
static const float kSpecularOcclusionPaintRoughness = 0.15f * 0.15f;

float SpecularOcclusion(float no_v, float ao, float roughness) {
  if (!(ao < 1.0f)) {
    return 1.0f;
  }
  if (ao <= 0.0f) {
    return 0.0f;
  }
  const float exponent = exp2(-16.0f * saturate(roughness) - 1.0f);
  const float lobe = pow(saturate(no_v) + ao, exponent);
  return saturate(lobe - 1.0f + ao);
}
)

#undef SPEC_OCCLUSION_SHADER_CODE
#endif
