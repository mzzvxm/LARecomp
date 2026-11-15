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
// The HUD fade (ui_fade.h, the trailer toolkit) scales the premultiplied layer, colour and coverage
// alike, by `fade` (0..1, clamped here; FrameLayers::UiAlphaFor gives 1 to a pause view's frame):
// ui x fade + scene x (1 - coverage x fade). At 1 that is the plain sum to the bit (a product with 1
// is exact), so a frame without a fade is what it was.
//
// Two shaders use it, so they cannot differ: the compose pass (frame_layers_ps.hlsl, LAYERS_COMPOSE=1)
// that writes the sum into a float16 image, and the present itself (present_ps.hlsl,
// kComposeLayers) that reads the two layers and takes the sum in place, which saves the image. Both
// are given the same fade (the compose pass's push ui_alpha, the present's push ui_alpha), so a
// fading HUD looks the same with mcla_gpu_hdr_compose_in_present on and off. The scalar function is
// plain C++ as well: hdr_compose_test.cpp includes this file as it is.

#ifndef MCLA_GPU_HDR_COMPOSE_HLSLI
#define MCLA_GPU_HDR_COMPOSE_HLSLI

float ComposeCode(float scene, float ui, float coverage, float fade) {
  const float f = clamp(fade, 0.0f, 1.0f);
  const float code = ui * f + scene * (1.0f - coverage * f);
  const uint bits = asuint(code) & 0x7FFFFFFFu;
  return (bits <= 0x7F800000u) ? clamp(code, 0.0f, 65504.0f) : 0.0f;
}

#ifdef __HLSL_VERSION
// `ui`: the UI layer's texel (premultiplied colour, coverage in alpha); `fade`: the UI's share of the
// frame, 1 for no fade.
float3 ComposeLayers(float3 scene, float4 ui, float fade) {
  return float3(ComposeCode(scene.x, ui.x, ui.w, fade), ComposeCode(scene.y, ui.y, ui.w, fade),
                ComposeCode(scene.z, ui.z, ui.w, fade));
}
#endif

#endif  // MCLA_GPU_HDR_COMPOSE_HLSLI
