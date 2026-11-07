// MCLA GPU renderer -- ambient occlusion, pass 2: the GTAO visibility (ao.h).
//
// One pixel of the AO image = the cosine-weighted visibility of the hemisphere above the surface
// the pixel shows, from the linear depth chain (ao_depth_ps, ao_mip_ps), R16_FLOAT, 1 = open.
// See ao_common.hlsli for where the technique comes from. Per pixel:
//
//   position and normal from the depth (the neighbour on each axis whose depth is nearer, so that
//   an edge does not bend the normal);
//   `slices` directions across the screen, rotated per pixel by noise; for each, `steps` samples
//   to either side, spread by a squared distribution so that most are near the pixel, each from
//   the depth level that fits its distance;
//   per side, the highest horizon seen (as a cosine), each sample's pull on it weighted down
//   where it is far from the surface (the falloff, which is also what keeps a far wall behind a
//   near edge from occluding it);
//   the arc integral of the two horizons around the projected normal, times the length of the
//   projected normal; the mean over the slices, to a power, never below a floor.
//
// The pixel's own noise is interleaved gradient noise (a fixed spatial pattern) plus a shift that
// changes every frame by the additive recurrence of the R2 sequence, so the two noise values of one
// pixel over successive frames cover the square evenly, whatever the camera does: what the
// occlusion's own accumulation over frames (ao_temporal_ps) integrates, and what the denoise
// takes out of any one frame (g_push.slots2.y = the frame number).

#include "ao_common.hlsli"

static const float kPi = 3.14159265358979f;
static const float kHalfPi = 1.57079632679490f;
// The nearest sample of a direction is at least this many pixels from the centre.
static const float kMinOffsetPixels = 1.3f;
// The farthest a sample may reach on screen, as a share of the image height (a surface almost at
// the camera would ask for the whole screen).
static const float kMaxRadiusShare = 0.3f;

// The linear depth at the continuous pixel position `p` (level-0 pixels), from `level` of the
// chain, nearest texel. Each level's slot comes straight from the push constants (never a computed
// index), so the descriptor index is the same for every pixel.
float ChainDepth(float2 p, uint level) {
  const int2 size0 = int2(g_push.sizes.zw);
  const int shift = int(level);
  const int2 size = max((size0 + int(1u << level) - 1) >> shift, int2(1, 1));
  const int2 q = clamp(int2(floor(p)) >> shift, int2(0, 0), size - 1);
  if (level == 0u) {
    return AoLoadLinear(g_push.slots.z, q);
  }
  if (level == 1u) {
    return AoLoadLinear(g_push.slots.w, q);
  }
  return AoLoadLinear(g_push.slots2.x, q);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const float tc = AoLoadLinear(g_push.slots.z, pixel);
  // The sky, and what is past where the effect fades out entirely: nothing to occlude.
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float2 centre = float2(pixel) + 0.5f;
  const float3 pc = AoViewPos(centre, size, tc);
  const float3 normal = AoNormalFromImage(g_push.slots.z, pixel, isize, tc, pc);
  const float3 view_vec = normalize(-pc);

  const float radius = g_push.lens.z;
  // Pixels per world unit at this distance, from the vertical field of view.
  const float pixels_per_unit = 0.5f * size.y / (g_push.lens.y * tc);
  const float radius_px = min(radius * pixels_per_unit, kMaxRadiusShare * size.y);
  if (radius_px < kMinOffsetPixels + 0.5f) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);  // the effect would be inside the pixel
  }
  const float falloff_ratio = max(g_push.lens.w, 0.01f);
  const float falloff_mul = -1.0f / (falloff_ratio * radius);
  const float falloff_add = 1.0f / falloff_ratio;

  const uint slices = max(AoSlices(), 1u);
  const uint steps = max(AoSteps(), 1u);
  const float2 noise_at = float2(pixel);
  const float2 spatial = float2(AoNoise(noise_at), AoNoise(noise_at.yx + float2(31.0f, 11.0f)));
  // R2 (Roberts): the increments are 1/g and 1/g^2 of the plastic number g; the frame number wraps
  // at 1024, small enough for the product to keep its fraction in a float.
  const float frame = float(g_push.slots2.y & 1023u);
  const float2 noise = frac(spatial + frame * float2(0.75487766624669f, 0.56984029099806f));
  const float min_offset = kMinOffsetPixels / radius_px;

  float visibility = 0.0f;
  for (uint slice = 0u; slice < slices; ++slice) {
    const float phi = (float(slice) + noise.x) / float(slices) * kPi;
    const float2 dir_px = float2(cos(phi), sin(phi));          // pixels, y down
    const float3 dir_view = float3(dir_px.x, -dir_px.y, 0.0f);  // view space, y up
    const float3 ortho = dir_view - dot(dir_view, view_vec) * view_vec;
    const float3 axis = normalize(cross(ortho, view_vec));
    const float3 projected = normal - axis * dot(normal, axis);
    const float projected_len = length(projected);
    const float sign_n = sign(dot(ortho, projected));
    const float cos_n = saturate(dot(projected, view_vec) / max(projected_len, 1e-6f));
    const float n = sign_n * acos(cos_n);  // the normal's angle from the view vector in the slice

    // The lowest horizon of each side: the surface's own tangent (nothing hides below it).
    const float low0 = cos(n + kHalfPi);  // the +direction side
    const float low1 = cos(n - kHalfPi);  // the -direction side
    float hcos0 = low0;
    float hcos1 = low1;
    for (uint s = 0u; s < steps; ++s) {
      const float step_noise = frac(noise.y + float(slice + s * slices) * 0.6180339887f);
      float share = (float(s) + step_noise) / float(steps);
      share = share * share + min_offset;
      const float2 offset = dir_px * (share * radius_px);
      const float level_f = clamp(log2(max(length(offset), 1.0f)) - 3.0f, 0.0f, 2.0f);
      const uint level = uint(level_f);

      const float2 p0 = centre + offset;
      if (all(p0 >= 0.0f) && all(p0 < size)) {
        const float3 sp = AoViewPos(floor(p0) + 0.5f, size, ChainDepth(p0, level));
        const float3 delta = sp - pc;
        const float dist = length(delta);
        const float w = saturate(dist * falloff_mul + falloff_add);
        hcos0 = max(hcos0, lerp(low0, dot(delta / max(dist, 1e-6f), view_vec), w));
      }
      const float2 p1 = centre - offset;
      if (all(p1 >= 0.0f) && all(p1 < size)) {
        const float3 sp = AoViewPos(floor(p1) + 0.5f, size, ChainDepth(p1, level));
        const float3 delta = sp - pc;
        const float dist = length(delta);
        const float w = saturate(dist * falloff_mul + falloff_add);
        hcos1 = max(hcos1, lerp(low1, dot(delta / max(dist, 1e-6f), view_vec), w));
      }
    }

    // The horizon angles, kept within the hemisphere around the normal, and the arc integral.
    float h0 = -acos(clamp(hcos1, -1.0f, 1.0f));
    float h1 = acos(clamp(hcos0, -1.0f, 1.0f));
    h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
    h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
    const float sin_n = sin(n);
    const float arc0 = (cos_n + 2.0f * h0 * sin_n - cos(2.0f * h0 - n)) * 0.25f;
    const float arc1 = (cos_n + 2.0f * h1 * sin_n - cos(2.0f * h1 - n)) * 0.25f;
    visibility += projected_len * (arc0 + arc1);
  }
  visibility = saturate(visibility / float(slices));
  visibility = max(pow(visibility, g_push.shape.x), g_push.shape.y);
  return float4(visibility, 0.0f, 0.0f, 0.0f);
}
