// MCLA GPU renderer -- ambient occlusion, pass 3b: the accumulation over frames (ao.h).
//
// The main pass's noise changes every frame (ao_main_ps) and one denoise pass takes the worst of it
// out of a frame (ao_denoise_ps); this pass averages what is left over frames. Per pixel it:
//
//   moves the pixel's view position (from this frame's linear depth) into the last frame's view
//   space with the camera's own motion (the matrix in lin, shape, light: ao.cpp FrameMotion), finds
//   it in the history image, RG16_FLOAT = (visibility, linear depth) of the frame before, and takes
//   the four texels round it, each by its bilinear weight times how well it agrees with this
//   pixel's surface: the distance of the texel's own position from this pixel's tangent plane
//   (the same measure as the denoise's), and the depth within 8%. A texel that is another surface
//   (an edge, something that came into view, a car that moved) has no weight. The weights add up to
//   the confidence of the history there;
//   a pixel of a vehicle (its stencil ID is 1 to 23) is found with the vehicle's own motion on top
//   of the camera's: the host makes, for each ID the game's matrices gave a usable motion in the last
//   frame, the 3x4 that moves this frame's view space into the last frame's with that vehicle's step
//   repeated (ao.cpp RecordVehicleTable) and the shader loads the row of the pixel's ID from a small
//   table image (slots.x). A vehicle ID with no row (not usable, or no previous frame's motion) has
//   no history, as before. The pause and map view has nothing that moves by itself, so there a vehicle
//   pixel takes the camera's motion like the world (flag bit 21). The IDs are handed out per frame in
//   draw order (a car has no stable ID): an ID that changed owner, or a car that moved differently
//   from last frame, lands on a texel that fails the agreement tests below and gets the wide blur;
//   takes the history's value clamped to the range of this frame's 3x3 neighbourhood of
//   visibilities plus a margin (the clamp of a temporal anti-aliasing filter): a shadow that
//   moved, a car that left, shows as a ghost in the history only for as long as it takes the
//   weights to catch up, and the clamp cuts that to the margin at once;
//   blends: result = lerp(this frame's, the history's, confidence * history weight). The history
//   weight is 1 - 1/N, N frames up to a cap (ao.cpp), so a new history is the plain mean of the
//   frames it has and then an exponential average;
//   gives a pixel with little or no confidence (an edge that just came into view, a pixel of a
//   vehicle whose motion is not known) the wide blur in place of its own value, a 5x5 of the already
//   denoised image: the second denoise pass, only where there is no history to lean on. A frame with
//   no history at all (a cut, the first frame, the pause view switching, a new size) had its two
//   denoise passes already and passes through.
//
// Output: RG16_FLOAT, (the result, the linear depth of this frame as the sky-capped distance), the
// next frame's history. Debug view 6 is a second run of the pass, into an image of its own, that
// writes the confidence in place of the visibility (the apply pass then shows it); the frame's
// own run, which makes the history, is made with the view cleared.
//
// Both the camera's motion and the lens are the previous frame's, and the depth is rendered with a
// sub-pixel jitter while an upscaler runs: a pixel finds its history within a pixel or so, not
// exactly. The occlusion is a smooth image and the plane test is wider than that.

#include "ao_common.hlsli"

// The plane test's width as a share of the distance (the denoise's), the depth agreement, the
// margin of the neighbourhood clamp, and the confidences at which a history counts for nothing and
// for all of it.
static const float kPlaneShare = 0.012f;
static const float kPlaneFloor = 0.02f;
static const float kDepthShare = 0.08f;
static const float kClampMargin = 0.08f;
static const float kConfidenceLow = 0.15f;
static const float kConfidenceHigh = 0.75f;
// A vehicle's stencil ID is the stencil byte >> 2: 1 to 23 (the UI's draws use 24 and up).
static const uint kFirstVehicleId = 1u;
static const uint kLastVehicleId = 23u;
static const uint kNoSlot = 0xFFFFFFFFu;

bool AoHistoryOn() { return (g_push.slots2.z & (1u << 20)) != 0u; }
// The pause and map view: a vehicle pixel moves with the camera like the world (nothing moves by
// itself there), so it takes no row of the vehicle table.
bool AoVehiclesAsWorld() { return (g_push.slots2.z & (1u << 21)) != 0u; }

// A point of this frame's view space in the last frame's, and a direction (the rotation part).
float3 AoToHistory(float3 p) {
  const float4 q = float4(p, 1.0f);
  return float3(dot(g_push.lin, q), dot(g_push.shape, q), dot(g_push.light, q));
}
float3 AoDirToHistory(float3 n) {
  return float3(dot(g_push.lin.xyz, n), dot(g_push.shape.xyz, n), dot(g_push.light.xyz, n));
}

// The view position of the history's texel centre `centre` at distance `t`, with the lens the
// history was made with.
float3 AoHistoryPos(float2 centre, float2 size, float t, float2 tan_half) {
  const float2 uv = centre / size;
  return float3((uv.x * 2.0f - 1.0f) * tan_half.x * t, (1.0f - uv.y * 2.0f) * tan_half.y * t, t);
}

// The vehicle (stencil ID 1 to 23) the depth target's stencil plane says the pixel `pixel` (of this
// image) shows; 0 for the world and for anything else (no stencil plane, the UI's IDs of 24 and up).
// At half resolution (AoLow) the pixel stands for the 2x2 block whose first sample this is.
uint AoVehicleId(int2 pixel) {
  const uint slot = g_push.slots2.w;
  if (slot == kNoSlot) {
    return 0u;
  }
  const int2 p = AoLow() ? pixel * 2 : pixel;
#if AO_DEPTH_MS
  const uint id = StencilOf(g_utextures_ms[slot].Load(p, g_push.slots.y)) >> 2;
#else
  const uint id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2;
#endif
  return (id >= kFirstVehicleId && id <= kLastVehicleId) ? id : 0u;
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const uint visibility_slot = g_push.slots.z;
  const uint depth_slot = g_push.slots.w;
  const float tc = AoLoadLinear(depth_slot, pixel);
  const float depth_out = min(tc, kAoSky);
  const float current = g_textures[visibility_slot].Load(int3(pixel, 0)).x;
  const uint view = AoView();
  // The sky, and what is past where the effect fades out entirely: nothing to occlude.
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, depth_out, 0.0f, 0.0f);
  }
  if (!AoHistoryOn()) {
    return float4(view == 6u ? 0.0f : current, depth_out, 0.0f, 0.0f);
  }

  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalFromImage(depth_slot, pixel, isize, tc, pc);
  float3 pe = AoToHistory(pc);
  float3 ne = AoDirToHistory(normal);
  // A pixel of a vehicle is found with the vehicle's own motion (the row of its ID in the vehicle
  // table: the three rows of the 3x4 that moves this frame's view space into the last frame's with the
  // camera's motion and the vehicle's, texel 3 holds 1 in x when the row is usable). No usable row: no
  // history for the pixel.
  bool no_history = false;
  const uint vehicle = AoVehicleId(pixel);
  if (vehicle != 0u && !AoVehiclesAsWorld()) {
    no_history = true;
    const uint table = g_push.slots.x;
    if (table != kNoSlot && g_textures[table].Load(int3(3, int(vehicle), 0)).x > 0.5f) {
      const float4 row0 = g_textures[table].Load(int3(0, int(vehicle), 0));
      const float4 row1 = g_textures[table].Load(int3(1, int(vehicle), 0));
      const float4 row2 = g_textures[table].Load(int3(2, int(vehicle), 0));
      const float4 q = float4(pc, 1.0f);
      pe = float3(dot(row0, q), dot(row1, q), dot(row2, q));
      ne = float3(dot(row0.xyz, normal), dot(row1.xyz, normal), dot(row2.xyz, normal));
      no_history = false;
    }
  }
  const float2 tan_half = float2(g_push.extra.x, g_push.extra.z);
  const uint history_slot = g_push.slots2.x;

  // The history round the place the pixel was.
  float sum = 0.0f;
  float weight = 0.0f;
  if (!no_history && pe.z > 0.05f) {
    const float2 uv = float2(0.5f + 0.5f * pe.x / (pe.z * tan_half.x),
                             0.5f - 0.5f * pe.y / (pe.z * tan_half.y));
    // The texel-centre coordinates of the place, kept where an int can hold them; a texel outside
    // the image has no weight.
    const float2 hp = clamp(uv * size - 0.5f, float2(-2.0f, -2.0f), size + 2.0f);
    const int2 h0 = int2(floor(hp));
    const float2 f = hp - float2(h0);
    const float sigma = kPlaneShare * pe.z + kPlaneFloor;
    for (int j = 0; j < 2; ++j) {
      for (int i = 0; i < 2; ++i) {
        const int2 q = h0 + int2(i, j);
        if (q.x < 0 || q.y < 0 || q.x >= isize.x || q.y >= isize.y) {
          continue;
        }
        const float2 texel = g_textures[history_slot].Load(int3(q, 0)).xy;
        const float3 ph = AoHistoryPos(float2(q) + 0.5f, size, texel.y, tan_half);
        const float off_plane = dot(ne, ph - pe) / sigma;
        float w = exp(-0.5f * off_plane * off_plane);
        w *= abs(texel.y - pe.z) < kDepthShare * pe.z + 0.05f ? 1.0f : 0.0f;
        w *= (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
        sum += texel.x * w;
        weight += w;
      }
    }
  }
  float history = sum / max(weight, 1e-4f);
  const float confidence = no_history ? 0.0f : smoothstep(kConfidenceLow, kConfidenceHigh, weight);
  if (view == 6u) {
    return float4(confidence, depth_out, 0.0f, 0.0f);
  }

  // The anti-ghost clamp: the history may not be farther from this frame's neighbourhood than a
  // margin.
  float lo = current;
  float hi = current;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1);
      const float v = g_textures[visibility_slot].Load(int3(q, 0)).x;
      lo = min(lo, v);
      hi = max(hi, v);
    }
  }
  history = clamp(history, lo - kClampMargin, hi + kClampMargin);

  // Where the history does not hold, the wide blur of this frame instead of its own value.
  float fresh = current;
  if (confidence < 1.0f) {
    const float wide = AoBilateral(visibility_slot, depth_slot, pixel, isize, tc, pc, normal, 2);
    fresh = lerp(wide, current, confidence);
  }
  const float result = lerp(fresh, history, confidence * g_push.extra.w);
  return float4(saturate(result), depth_out, 0.0f, 0.0f);
}
