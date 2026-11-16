// MCLA GPU renderer -- the HDR frame's picture: the UI layer over the scene (frame_layers.h,
// hdr_scene.h).
//
// The game blends its UI in code values, with the alpha blend ONE / INV_SRC_ALPHA over a target
// cleared to zero, so the UI layer holds colour premultiplied by coverage and the coverage in alpha.
// The frame is the sum: ui + scene x (1 - coverage), per channel, with nothing clamped to 1 (the scene
// may be above it: HDR stage 3's float scene). The display's table is indexed by what this returns, so
// it is made finite and non-negative here: a NaN a float16 draw left in the float scene is none, and
// 65504 is the most a float16 holds.
//
// Two shaders use it, so they cannot differ: the compose pass (frame_layers_ps.hlsl, LAYERS_COMPOSE=1)
// that writes the sum into a float16 image, and the present itself (present_ps.hlsl,
// kComposeLayers) that reads the two layers and takes the sum in place, which saves the image. The
// scalar function is plain C++ as well: hdr_compose_test.cpp includes this file as it is.

#ifndef MCLA_GPU_HDR_COMPOSE_HLSLI
#define MCLA_GPU_HDR_COMPOSE_HLSLI

float ComposeCode(float scene, float ui, float coverage) {
  const float code = ui + scene * (1.0f - coverage);
  const uint bits = asuint(code) & 0x7FFFFFFFu;
  return (bits <= 0x7F800000u) ? clamp(code, 0.0f, 65504.0f) : 0.0f;
}

#ifdef __HLSL_VERSION
// `ui`: the UI layer's texel (premultiplied colour, coverage in alpha).
float3 ComposeLayers(float3 scene, float4 ui) {
  return float3(ComposeCode(scene.x, ui.x, ui.w), ComposeCode(scene.y, ui.y, ui.w),
                ComposeCode(scene.z, ui.z, ui.w));
}
#endif

#endif  // MCLA_GPU_HDR_COMPOSE_HLSLI
