// MCLA GPU renderer -- the temporal system's motion vectors (temporal_mv.h).
//
// One pixel of the vector image = where the scene point seen here was one frame ago, minus where it is
// now, in render pixels, +y down, with this frame's jitter taken out (F4-DESIGN.md 2.4):
//
//   id      = (stencil.x * 255 + 0.5) >> 2, 0 when 24 or more   (the vehicle ID the game's draws write)
//   pos     = pixel + 0.5 - jitter                                (render pixels)
//   clip    = (2 pos.x / w - 1, 1 - 2 pos.y / h, 1 - depth, 1)    (the stored depth is 1 near, 0 far)
//   prev    = clip * M                                            (row vector times the 4x4 below: the
//                                                                  previous frame's guest pixels, homogeneous)
//   mv      = prev.xy / prev.w * render_per_guest - pos
//
// M is made on the host in double (temporal_camera.h): this frame's clip space to the previous frame's
// guest pixels, through this renderer's own view-projection history for the world and for every pixel
// that has no vehicle matrix; only its columns x, y and w are used, so only those travel (col_x, col_y,
// col_w, one float4 each). With mode 1 the pass draws again over the same image and keeps only the
// pixels of vehicle `id`, with that vehicle's own matrix (debug: mcla_gpu_temporal_vehicles). Mode 2
// writes zero (no previous frame: the history starts again).
//
// The depth and the stencil view are read through the bindless slots the anchor draw binds, so the
// format, the swizzle and the size are the guest shader's own.

#include "host_pass_common.hlsli"

struct MotionPush {
  uint depth;        // slot: the scene's depth, R32_FLOAT, 1 near / 0 far (the anchor's fetch 2)
  uint stencil;      // slot: the stencil view of the depth, 8_8_8_8 (the anchor's fetch 1)
  uint mode;         // 0 every pixel, 1 only the pixels of `id`, 2 zero
  uint id;
  float2 jitter;     // render pixels this frame's draws were moved by, +y down
  float2 size;       // the image's size, render pixels
  float2 to_render;  // render pixels per guest pixel, each way
  float2 reserved;
  float4 col_x;      // M's column x, y and w: prev.x = dot(clip, col_x), ...
  float4 col_y;
  float4 col_w;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<MotionPush> g_push;
#else
ConstantBuffer<MotionPush> g_push : register(b0, space4);
#endif

float4 main(float4 position : SV_Position) : SV_Target {
  if (g_push.mode == 2u) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const int3 at = int3(int2(position.xy), 0);
  // The texture index may carry the gamma bit (bit 31): not an index.
  const uint raw = uint(g_textures[g_push.stencil & 0x7FFFFFFFu].Load(at).x * 255.0f + 0.5f);
  uint id = raw >> 2;
  if (id >= 24u) {
    id = 0u;
  }
  if (g_push.mode == 1u && id != g_push.id) {
    discard;
  }
  const float depth = g_textures[g_push.depth & 0x7FFFFFFFu].Load(at).x;
  const float2 pos = float2(at.xy) + 0.5f - g_push.jitter;
  const float4 clip = float4(2.0f * pos.x / g_push.size.x - 1.0f, 1.0f - 2.0f * pos.y / g_push.size.y,
                             1.0f - depth, 1.0f);
  const float w = dot(clip, g_push.col_w);
  float2 mv = float2(0.0f, 0.0f);
  if (abs(w) > 1e-6f) {
    const float2 previous = float2(dot(clip, g_push.col_x), dot(clip, g_push.col_y)) / w;
    mv = previous * g_push.to_render - pos;
  }
  // Not finite (or NaN: every comparison with it is false): no motion. Else clamped to the frame.
  mv = all(abs(mv) < 1e30f) ? clamp(mv, -g_push.size, g_push.size) : float2(0.0f, 0.0f);
  return float4(mv, 0.0f, 0.0f);
}
