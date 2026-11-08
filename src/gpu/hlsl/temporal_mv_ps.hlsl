// MCLA GPU renderer -- the temporal system's motion vectors (temporal_mv.h).
//
// One pixel of the vector image = where the scene point seen here was one frame ago, minus where it is
// now, in render pixels, +y down, with this frame's jitter taken out (F4-DESIGN.md 2.4):
//
//   pos     = pixel + 0.5 - jitter                                (render pixels)
//   clip    = (2 pos.x / w - 1, 1 - 2 pos.y / h, 1 - depth, 1)    (the stored depth is 1 near, 0 far)
//   prev    = clip * M                                            (row vector times the 4x4 below: the
//                                                                  previous frame's guest pixels, homogeneous)
//   mv      = prev.xy / prev.w * render_per_guest - pos
//
// M is made on the host in double (temporal_camera.h): this frame's clip space to the previous frame's
// guest pixels, through this renderer's own view-projection history, the same for every pixel; only
// its columns x, y and w are used, so only those travel (col_x, col_y, col_w, one float4 each).
//
// A pixel that shows a vehicle takes its own M instead (feature-parity/GHOSTING-DESIGN.md 3 and 9.5): the
// game's stencil ID of the pixel (temporal_common.hlsli: the byte >> 2 the main view's draws write, 0 the
// world) picks a row of the vehicle table, one 4 x 24 RGBA32F image (vehicle_motion.h): texels 0, 1 and 2
// are that vehicle's columns x, y and w (the camera's motion and the vehicle's own, one 4x4 made on the
// host), texel 3 holds 1 in x when the row is usable. A row that is not usable, an ID without a row, and
// a vehicle vector more than `guard` pixels away from the camera's (a car placed or teleported in this
// frame: its slot holds garbage) keep the camera's. Mode 2 writes zero (no previous frame: the history
// starts again).
//
// The depth and the stencil view are read through the bindless slots the anchor draw binds, so the format
// and the size are the guest shader's own. The stencil's component is not `.x`: the view is swizzled and the
// byte sits where the swizzle puts the product's red (temporal_common.hlsli).

#include "temporal_common.hlsli"

struct MotionPush {
  uint depth;        // slot: the scene's depth, R32_FLOAT, 1 near / 0 far (the anchor's fetch 2)
  uint stencil;      // stencil reference (temporal_common.hlsli): the stencil view's slot and component
  uint mode;         // 0 every pixel, 2 zero
  uint table;        // slot of the vehicle table, 0xFFFFFFFF: none (every pixel gets the camera's)
  float2 jitter;     // render pixels this frame's draws were moved by, +y down
  float2 size;       // the image's size, render pixels
  float2 to_render;  // render pixels per guest pixel, each way
  float guard;       // render pixels: the most a vehicle's vector may differ from the camera's
  float reserved;
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
  const float depth = g_textures[g_push.depth & 0x7FFFFFFFu].Load(at).x;
  const float2 pos = float2(at.xy) + 0.5f - g_push.jitter;
  const float4 clip = float4(2.0f * pos.x / g_push.size.x - 1.0f, 1.0f - 2.0f * pos.y / g_push.size.y,
                             1.0f - depth, 1.0f);

  // The camera's motion, for every pixel.
  float2 mv = float2(0.0f, 0.0f);
  const float w = dot(clip, g_push.col_w);
  if (abs(w) > 1e-6f) {
    const float2 previous = float2(dot(clip, g_push.col_x), dot(clip, g_push.col_y)) / w;
    mv = previous * g_push.to_render - pos;
  }
  // Not finite (or NaN: every comparison with it is false): no motion.
  mv = all(abs(mv) < 1e30f) ? mv : float2(0.0f, 0.0f);

  // A vehicle's own. The ID is read only when there is a table; a vehicle's row is read only for a
  // pixel that has an ID.
  if (g_push.table != 0xFFFFFFFFu) {
    const uint id = VehicleId(g_push.stencil, at);
    if (id != 0u) {
      const uint slot = g_push.table & 0x7FFFFFFFu;
      const int row = int(id);
      if (g_textures[slot].Load(int3(3, row, 0)).x > 0.5f) {
        const float4 col_x = g_textures[slot].Load(int3(0, row, 0));
        const float4 col_y = g_textures[slot].Load(int3(1, row, 0));
        const float4 col_w = g_textures[slot].Load(int3(2, row, 0));
        const float wk = dot(clip, col_w);
        if (abs(wk) > 1e-6f) {
          const float2 previous_k = float2(dot(clip, col_x), dot(clip, col_y)) / wk;
          const float2 mv_k = previous_k * g_push.to_render - pos;
          if (all(abs(mv_k) < 1e30f) && all(abs(mv_k - mv) < g_push.guard)) {
            mv = mv_k;
          }
        }
      }
    }
  }
  // Clamped to the frame.
  mv = clamp(mv, -g_push.size, g_push.size);
  return float4(mv, 0.0f, 0.0f);
}
