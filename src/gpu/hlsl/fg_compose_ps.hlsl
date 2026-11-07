// MCLA GPU renderer -- frame generation: the generated frame composed with the UI (frame_generation.h).
//
// The interpolation made the scene of the frame between two real ones, in the codes fg_prepare_ps.hlsl
// wrote. The real frame has its UI over its scene, so the generated one does too: the UI layer of the
// frame layers (frame_layers.h: the UI as the game drew it into a cleared target, colour premultiplied
// by its coverage, coverage in alpha) is read at the generation size and put over it as the game
// blends the UI, in float and unclamped, which is what hdr_compose_ps does for the real frame:
//   code = ui.rgb + scene.rgb * (1 - ui.a)
// The result goes through the present pass like a real frame's picture (the display's table, FSR1,
// the overlay), as an image of code values that may exceed 1.
//
// FLAG_FLOAT_SCENE: the interpolation was given the compressed float scene; it is expanded first (the
// inverse of fg_prepare_ps.hlsl: y = 2 * v; x = y up to 1, 1 / (2 - y) above it).
//
// The UI is read bilinearly at the same positions as the scene was (the generation size over the
// picture's size), so UI and scene scale alike.

#include "host_pass_common.hlsli"

struct ComposePush {
  uint generated;   // the interpolated scene (generation size), bindless slot
  uint ui;          // the UI layer, bindless slot
  uint flags;       // bit 0: the float scene (expand)
  uint src_width;   // the picture's size in the UI layer, texels
  uint src_height;
  uint out_width;   // the generation size
  uint out_height;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<ComposePush> g_push;
#else
ConstantBuffer<ComposePush> g_push : register(b0, space4);
#endif

static const uint kFlagFloatScene = 1u;

float4 UiTexel(int2 at) {
  const int2 limit = int2(g_push.src_width, g_push.src_height) - 1;
  return g_textures[g_push.ui].Load(int3(clamp(at, int2(0, 0), limit), 0));
}

float Expand(float v) {
  const float y = 2.0f * v;
  // Below the asymptote by at least a float16 step, so a value the interpolation pushed to it stays
  // finite.
  return y <= 1.0f ? y : 1.0f / max(2.0f - y, 1.0f / 65504.0f);
}

float4 main(float4 position : SV_Position) : SV_Target {
  float3 scene = g_textures[g_push.generated].Load(int3(int2(position.xy), 0)).rgb;
  if ((g_push.flags & kFlagFloatScene) != 0u) {
    scene = float3(Expand(scene.x), Expand(scene.y), Expand(scene.z));
  }
  const float2 scale = float2(g_push.src_width, g_push.src_height) /
                       float2(g_push.out_width, g_push.out_height);
  const float2 p = position.xy * scale - 0.5f;
  const float2 cell = floor(p);
  const float2 f = p - cell;
  const int2 at = int2(cell);
  float4 ui;
  if (all(f == 0.0f)) {
    ui = UiTexel(at);
  } else {
    const float4 top = lerp(UiTexel(at), UiTexel(at + int2(1, 0)), f.x);
    const float4 bottom = lerp(UiTexel(at + int2(0, 1)), UiTexel(at + int2(1, 1)), f.x);
    ui = lerp(top, bottom, f.y);
  }
  // Premultiplied "over", unclamped above: what the scene holds above 1 is kept. The display's table
  // is indexed by what this writes, so it is made finite and not negative, as hdr_compose_ps does.
  const float3 code = ui.rgb + scene * (1.0f - ui.a);
  const uint3 bits = asuint(code) & 0x7FFFFFFFu;
  return float4((bits.x <= 0x7F800000u) ? clamp(code.x, 0.0f, 65504.0f) : 0.0f,
                (bits.y <= 0x7F800000u) ? clamp(code.y, 0.0f, 65504.0f) : 0.0f,
                (bits.z <= 0x7F800000u) ? clamp(code.z, 0.0f, 65504.0f) : 0.0f, 1.0f);
}
