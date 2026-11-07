// MCLA GPU renderer -- the passes of the UI layer split (frame_layers.h, hdr_scene.h).
//
// The split keeps two 8-bit images of an HDR frame: the underlay (the scene as the game's post
// chain finished it, in the game's code values) and the UI layer (what the UI phase drew into a
// target cleared to zero: colour premultiplied by coverage, coverage in alpha, because the draws'
// alpha blend was changed to ONE / INV_SRC_ALPHA). Both are UNORM 8:8:8:8 colour targets of the
// game, so nothing here is linearised: the game blends in code values and so does this.
//
// LAYERS_COMPOSE = 0 (layers_restore_ps): the underlay as it is, drawn into the UI target with the
//   blend (INV_DEST_ALPHA, ONE) on colour and alpha, which makes the target UI + underlay x (1 -
//   coverage) -- what the game's own draws left there, so its resolves and readbacks see its frame.
// LAYERS_COMPOSE = 1 (hdr_compose_ps): the same sum, for the HDR present, into an
//   R16G16B16A16_FLOAT image: the scene and the UI in the game's code values with nothing clamped
//   (the scene may be above 1 here: HDR stage 3's float scene, hdr_tonemap.h, is read in place of the
//   underlay when the frame has one; the UI stays under it). With a debug view
//   (mcla_gpu_hdr_layers_view) the pass shows one layer instead.
//
// Both draw 1:1 from the image's origin: the pixel the shader sees is the texel it reads.

#include "host_pass_common.hlsli"

struct LayersPush {
  uint underlay;  // the scene: R8G8B8A8_UNORM underlay, or (compose) the float scene; a bindless 2D slot
  uint ui;        // LAYERS_COMPOSE only: the UI layer's slot
  uint view;      // LAYERS_COMPOSE only: 0 the frame, 1 the UI layer, 2 the scene, 3 coverage,
                  // 4 where the float scene is above 1, 5 its difference from the 8-bit scene
  uint reference; // LAYERS_COMPOSE only: the 8-bit underlay's slot (views 4 and 5)
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<LayersPush> g_push;
#else
ConstantBuffer<LayersPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  const int3 at = int3(int2(position.xy), 0);
  const float4 underlay = g_textures[g_push.underlay].Load(at);
#if LAYERS_COMPOSE
  const float4 ui = g_textures[g_push.ui].Load(at);
  if (g_push.view == 1u) {
    return float4(ui.rgb, 1.0f);
  }
  if (g_push.view == 2u) {
    return float4(underlay.rgb, 1.0f);
  }
  if (g_push.view == 3u) {
    return float4(ui.a, ui.a, ui.a, 1.0f);
  }
  if (g_push.view >= 4u) {
    const float3 reference = g_textures[g_push.reference].Load(at).rgb;
    if (g_push.view == 4u) {
      // The scene's light above the game's white (code values past the 8-bit 1) in magenta, over
      // the 8-bit scene dimmed.
      const float largest = max(underlay.r, max(underlay.g, underlay.b));
      return largest > 1.002f ? float4(1.0f, 0.0f, 1.0f, 1.0f) : float4(reference * 0.25f, 1.0f);
    }
    // The float scene against the 8-bit one: sixteen times the difference, per channel.
    return float4(saturate(abs(underlay.rgb - reference) * 16.0f), 1.0f);
  }
  // Premultiplied "over", as the game blends the UI: no clamp, so what the scene holds above 1
  // is kept; the UI's own part never exceeds its coverage. The display's table is indexed by what
  // this writes, so it is made finite and non-negative here: a NaN a float16 draw left in the float
  // scene is none, and 65504 is the most a float16 holds.
  const float3 code = ui.rgb + underlay.rgb * (1.0f - ui.a);
  const uint3 bits = asuint(code) & 0x7FFFFFFFu;
  return float4((bits.x <= 0x7F800000u) ? clamp(code.x, 0.0f, 65504.0f) : 0.0f,
                (bits.y <= 0x7F800000u) ? clamp(code.y, 0.0f, 65504.0f) : 0.0f,
                (bits.z <= 0x7F800000u) ? clamp(code.z, 0.0f, 65504.0f) : 0.0f, 1.0f);
#else
  return underlay;
#endif
}
