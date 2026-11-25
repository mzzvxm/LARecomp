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
//   denoised image (one radius wider than the denoise pass's, AoWideRadius: ao_stable.h): the second denoise
//   pass, only where there is no history to lean on. A frame with
//   no history at all (a cut, the first frame, the pause view switching, a new size) had its two
//   denoise passes already and passes through.
//
// THE CONTACT SHADOWS' HISTORY (NOISE-RESOLVE, mcla_gpu_noise_resolve_no_upscaler). With no temporal upscaler the contact march's start noise and the ray traced sun rays' sample change every frame and
// nothing averaged them; ao.cpp runs this same pass over the contact filter's result (plain visibilities: the filter has taken the vehicle march's marks out) with a history of its own, flag bit 22
// (AoContactHistory): the wide blur is not taken where the history does not hold. Everything else, the reprojection, the plane and depth tests, the vehicles' rows, the clamp and the weight, is
// the occlusion's: a moving shadow's ghost is bounded by the same clamp.
//
// Output: RG16_FLOAT, (the result, the linear depth of this frame as the sky-capped distance), the
// next frame's history. Debug view 6 is a second run of the pass, into an image of its own, that
// writes the confidence in place of the visibility (the apply pass then shows it); the frame's
// own run, which makes the history, is made with the view cleared.
//
// THE NORMAL. The pass's normal (the plane test of the history and of the wide blur, the vehicle rows' rotation) is the normal image's where the frame has one: its bindless slot is
// in the high 16 bits of slots.y (the low bits are the host sample the stencil plane is read at, so those reads mask it; 0 in the high half is none: the depth's normal,
// ao_common.hlsli AoNormalAt).
//
// Both the camera's motion and the lens are the previous frame's, and the depth is rendered with a
// sub-pixel jitter while an upscaler runs: a pixel finds its history within a pixel or so, not
// exactly. The occlusion is a smooth image and the plane test is wider than that.
//
// THE BOUNCE LIGHT TEST (AO_GI=1: ao_temporal_gi_ps and ao_temporal_gi_ms_ps, ao.h mcla_gpu_ssgi_test): the
// same pass over four channels, the visibility in x (to the last operation what the plain pass makes of it)
// and the bounce light in yzw. The history is an RGBA16F image of the last frame's result and the depth of
// that frame is not in it but in an R32_FLOAT image of its own (the plain pass keeps it in the second
// channel of an RG16F image, which here has no room): a texel of the history is judged by the plane test
// and the depth test as before, with that depth. The tests, the vehicle rows, the camera's motion and the
// reasons a pixel has no history are the plain pass's; so is the weight the visibility keeps (extra.w).
// What the bounce adds:
//   its own history weight (lens.z; the bounce is noisier than the visibility and the colour of one
//   neighbour, so it averages over more frames: ao.cpp mcla_gpu_ssgi_test_history);
//   its own clamp: the history of the bounce is clipped to a box round the weighted mean of this frame's
//   3x3 neighbourhood (the neighbours that are on the pixel's plane), a width of lens.w standard
//   deviations of that neighbourhood, never less than kBounceBoxFloor of its mean, plus a share of the history's
//   own value on each side (AoClipHistoryShare) (the variance clip of a temporal anti-aliasing filter, a colour
//   at a time: a car that went by and changed the light on a wall shows as a ghost only until the history is
//   clipped to the new light). The box was 1.25 deviations of the BLURRED neighbourhood, whose nine values are
//   alike and whose mean moves with the frame's noise, so it missed the true value in most frames and the clip
//   pulled the history to the frame's value every time: the accumulated bounce was 2.5 to 3.5 times as noisy as
//   without it. Now 2 deviations and 15% of the history: the noise of no clip, and a light that changed is
//   followed to 90% in about 10 frames (ssgi_stable.h has the numbers);
//   the same wide blur (AoWideRadius: one wider than the denoise's, which follows the picture's size: ao_stable.h) in
//   place of this frame's value where the history does not hold.
// Push fields (those of the plain pass but for): slots.z the image of this frame's denoised result (RGBA16F),
// slots2.y the slot of the depth history (R32_FLOAT, the depth image of the last frame), lens.z the weight
// the bounce's history keeps and lens.w the clip's width. Output: RGBA16F, the next frame's history (the
// debug view 6 run writes the confidence in x).

#include "ao_common.hlsli"
#include "nan_guard.hlsli"

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
// NOISE-RESOLVE (ao.cpp RecordContactHistory): the pass runs over the contact shadows' filtered image. A thin contact shadow is lost in the wide blur the occlusion's pixels without a history take, so
// where the history does not hold this frame's own (already filtered) value stands. Bit 22 of slots2.z, which the apply's flags use but this pass does not.
bool AoContactHistory() { return (g_push.slots2.z & (1u << 22)) != 0u; }
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
// At half resolution (AoLow) the pixel stands for a 2x2 block of the depth plane, and the depth pass (ao_depth_ps) took the depth of the
// one of its four samples nearest to the block's mean and marked the pixel a vehicle (the depth's lowest bit, `depth`) by THAT sample's
// stencil. The vehicle is then the first of the block's four samples that is one, and none where the mark says the pixel is not a
// vehicle: the old rule read the block's first sample, which on a silhouette (a car against the road) could be the other surface, so
// the pixel was followed with the car's motion on the road's depth or the world's on the car's.
uint AoVehicleId(int2 pixel, float depth) {
  const uint slot = g_push.slots2.w;
  if (slot == kNoSlot) {
    return 0u;
  }
  if (!AoLow()) {
#if AO_DEPTH_MS
    const uint id = StencilOf(g_utextures_ms[slot].Load(pixel, g_push.slots.y & 0xFFu)) >> 2;
#else
    const uint id = StencilOf(g_utextures[slot].Load(int3(pixel, 0))) >> 2;
#endif
    return (id >= kFirstVehicleId && id <= kLastVehicleId) ? id : 0u;
  }
  if (!AoIsVehicle(depth)) {
    return 0u;
  }
  for (int k = 0; k < 4; ++k) {
    const int2 p = pixel * 2 + int2(k & 1, k >> 1);
#if AO_DEPTH_MS
    const uint id = StencilOf(g_utextures_ms[slot].Load(p, g_push.slots.y & 0xFFu)) >> 2;
#else
    const uint id = StencilOf(g_utextures[slot].Load(int3(p, 0))) >> 2;
#endif
    if (id >= kFirstVehicleId && id <= kLastVehicleId) {
      return id;
    }
  }
  return 0u;
}

#if AO_GI

// The least half width of the bounce's clip box, as a share of the neighbourhood's mean (ssgi_stable.h kClipFloorShare), and how
// much of the history's own value the box adds on each side: the push block's flags, bits 26-29 in units of 0.05
// (ssgi_stable.h ClipHistoryCode; 0.15 in the shader's default, 0 for the first version's box).
static const float kBounceBoxFloor = 0.05f;
float AoClipHistoryShare() { return float((g_push.slots2.z >> 26) & 0xFu) * 0.05f; }

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const uint current_slot = g_push.slots.z;
  const uint depth_slot = g_push.slots.w;
  const float tc = AoLoadLinear(depth_slot, pixel);
  const float4 current = g_textures[current_slot].Load(int3(pixel, 0));
  const uint view = AoView();
  // The sky, and what is past where the effect fades out entirely: nothing to occlude, no bounce.
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  if (!AoHistoryOn()) {
    return view == 6u ? float4(0.0f, 0.0f, 0.0f, 0.0f) : current;
  }

  const float3 pc = AoViewPos(float2(pixel) + 0.5f, size, tc);
  const float3 normal = AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, isize, tc, pc);
  float3 pe = AoToHistory(pc);
  float3 ne = AoDirToHistory(normal);
  // A pixel of a vehicle is found with the vehicle's own motion, as in the plain pass.
  bool no_history = false;
  const uint vehicle = AoVehicleId(pixel, tc);
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
  const uint depth_history_slot = g_push.slots2.y;

  // The history round the place the pixel was: the plain pass's four texels and weights, over four
  // channels.
  float4 sum = float4(0.0f, 0.0f, 0.0f, 0.0f);
  float weight = 0.0f;
  if (!no_history && pe.z > 0.05f) {
    const float2 uv = float2(0.5f + 0.5f * pe.x / (pe.z * tan_half.x),
                             0.5f - 0.5f * pe.y / (pe.z * tan_half.y));
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
        const float4 texel = g_textures[history_slot].Load(int3(q, 0));
        const float texel_depth = AoLoadLinear(depth_history_slot, q);
        const float3 ph = AoHistoryPos(float2(q) + 0.5f, size, texel_depth, tan_half);
        const float off_plane = dot(ne, ph - pe) / sigma;
        float w = exp(-0.5f * off_plane * off_plane);
        w *= abs(texel_depth - pe.z) < kDepthShare * pe.z + 0.05f ? 1.0f : 0.0f;
        w *= (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
        sum += texel * w;
        weight += w;
      }
    }
  }
  float4 history = sum / max(weight, 1e-4f);
  const float confidence = no_history ? 0.0f : smoothstep(kConfidenceLow, kConfidenceHigh, weight);
  if (view == 6u) {
    return float4(confidence, 0.0f, 0.0f, 0.0f);
  }

  // The anti-ghost clamps. The visibility: the range of this frame's 3x3 neighbourhood plus a margin,
  // as the plain pass. The bounce: the box of the neighbourhood's mean and spread, the neighbours that
  // are on the pixel's plane counting (a colour across an edge is not this surface's light).
  float lo = current.x;
  float hi = current.x;
  float3 m1 = float3(0.0f, 0.0f, 0.0f);
  float3 m2 = float3(0.0f, 0.0f, 0.0f);
  float wsum = 0.0f;
  const float inv_plane_sigma = 1.0f / (kPlaneShare * tc + kPlaneFloor);
  const float2 clip_scale = 2.0f / size;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      const int2 q = clamp(pixel + int2(dx, dy), int2(0, 0), isize - 1);
      const float4 v = g_textures[current_slot].Load(int3(q, 0));
      lo = min(lo, v.x);
      hi = max(hi, v.x);
      const float3 pq = AoViewPosInv(float2(q) + 0.5f, clip_scale, AoLoadLinear(depth_slot, q));
      const float off_plane = dot(normal, pq - pc) * inv_plane_sigma;
      const float w = exp2(kAoNegHalfLog2e * off_plane * off_plane);
      m1 += v.yzw * w;
      m2 += v.yzw * v.yzw * w;
      wsum += w;
    }
  }
  m1 /= wsum;
  const float3 spread = sqrt(max(m2 / wsum - m1 * m1, 0.0f));
  const float3 half_box = g_push.lens.w * spread + kBounceBoxFloor * m1 + AoClipHistoryShare() * history.yzw;
  history.x = clamp(history.x, lo - kClampMargin, hi + kClampMargin);
  history.yzw = clamp(history.yzw, max(m1 - half_box, 0.0f), m1 + half_box);

  // Where the history does not hold, the wide blur of this frame instead of its own value.
  float4 fresh = current;
  if (confidence < 1.0f) {
    const float4 wide = AoBilateral4(current_slot, depth_slot, pixel, isize, tc, pc, normal, AoWideRadius());
    fresh = lerp(wide, current, confidence);
  }
  float4 result;
  result.x = lerp(fresh.x, history.x, confidence * g_push.extra.w);
  result.yzw = lerp(fresh.yzw, history.yzw, confidence * g_push.lens.z);
  // A half float keeps 65504; the chain's colours are capped well under that. This image is the next frame's history, so it is always numbers (decided by the bits, nan_guard.hlsli: a NaN is no
  // occlusion and no bounce light; max(NaN, 0) and saturate(NaN) are 0 on D3D12 only).
  return float4(saturate(NgNumberOr(result.x, 1.0f)), NgHeld3(result.yzw, 60000.0f));
}

#else  // !AO_GI

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
  const float3 normal = AoNormalAt(g_push.slots.y >> 16, depth_slot, pixel, isize, tc, pc);
  float3 pe = AoToHistory(pc);
  float3 ne = AoDirToHistory(normal);
  // A pixel of a vehicle is found with the vehicle's own motion (the row of its ID in the vehicle
  // table: the three rows of the 3x4 that moves this frame's view space into the last frame's with the
  // camera's motion and the vehicle's, texel 3 holds 1 in x when the row is usable). No usable row: no
  // history for the pixel.
  bool no_history = false;
  const uint vehicle = AoVehicleId(pixel, tc);
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

  // Where the history does not hold, the wide blur of this frame instead of its own value (not on the contact shadows' image: AoContactHistory).
  float fresh = current;
  if (confidence < 1.0f && !AoContactHistory()) {
    const float wide = AoBilateral(visibility_slot, depth_slot, pixel, isize, tc, pc, normal, AoWideRadius());
    fresh = lerp(wide, current, confidence);
  }
  const float result = lerp(fresh, history, confidence * g_push.extra.w);
  return float4(saturate(NgNumberOr(result, 1.0f)), depth_out, 0.0f, 0.0f);   // the next frame's history: a number (nan_guard.hlsli), a NaN is no occlusion
}

#endif  // AO_GI
