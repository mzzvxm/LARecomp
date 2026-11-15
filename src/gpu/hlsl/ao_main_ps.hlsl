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

//
// SCREEN-SPACE GLOBAL ILLUMINATION (G7a; compiled a second time as ao_main_gi_ps with AO_GI=1, the
// plain pass above is the source without it, bit for bit): the same slices, steps and horizon search,
// but the horizons are a visibility BITMASK (Therrien, Levesque and Gilet 2023, "Screen Space
// Indirect Lighting with Visibility Bitmask"; written from the paper's idea, not copied): per slice a
// 32 bit mask over the arc of the hemisphere around the normal, one bit a sector of equal measure
// (the slice's cosine weighted arc integral of GTAO, cdf of cos(theta - n) |sin theta|, so that the
// unset bits are the same visibility as the horizons give, within 1/32). A sample sets the bits
// between its front horizon and, with a thickness, its back horizon (the sample's surface moved
// `thickness` away along the view ray; thickness 0 = a solid: everything below the front horizon, which
// is the GTAO horizon search exactly); the bits that are NEW add the sample's radiance, from the
// colour chain (ao_color_ps: the scene colour at the trigger, averaged over the MSAA samples, three
// levels like the depth chain), weighted by its share of the slice and by how much its surface faces
// the pixel (the slope of the depth one texel on, along the slice). The output is one RGBA16F pixel:
//   x   the visibility (the final power and the floor applied, as the plain pass), the same number the
//       GTAO pass gives to within the sector quantisation; the denoise and the history read this channel
//   yzw the bounce: the cosine weighted mean radiance of the blocked part of the hemisphere, in the
//       scene colour's own units (a uniform emitter of colour C over the whole hemisphere gives C): what
//       the game's ambient light (a constant colour for "the average surroundings") would be if the
//       blocked part were the emitter. 0 on the sky and past the fade.
// Push fields of this variant (the others as above): slots.x, slots.y, slots2.w the bindless slots of
// the colour chain's levels 0, 1, 2 (RGBA16F; the plain pass leaves them unused), shape.z the
// thickness in world units (0 = solid), shape.w the weight of the facing test (0 = a sample's surface
// is assumed to face the pixel; 1 = the full test), extra.w the reach of the far ring in world units
// (0 = none), flags bits 26-28 its steps a side of each slice (0 = none).
//
// THE FAR RING (the bounce light's reach): the samples above find what blocks the pixel within the
// occlusion's radius, 1 to 3 m. A shaded street gets its light from the sunlit road and the walls across it,
// 4 to 10 m away, so each slice goes on with a few more samples from the near samples' end out to the reach,
// from the coarser levels of the chains (1 and 2: the ring is the far part of the picture, and the coarse
// levels keep its texture reads in the cache). They are the same AoGiSample, with the horizon's falloff taken
// at the reach (full to 38% of it, none at it, so a sample falls out of the bounce smoothly instead of
// popping), and they add light only into the sectors the near samples left open (the mask carries on from
// theirs, so a far wall behind a near one counts once). They never change the visibility: that is counted
// from the near samples' mask alone (occupied_near), so the occlusion looks as it did. ssgi_light.h has the
// same maths in C++.

#include "ao_common.hlsli"

#ifndef AO_GI
#define AO_GI 0
#endif

#if AO_GI
#include "ssgi_common.hlsli"
#endif

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

#if AO_GI

static const uint kSectors = 32u;

// The colour chain's radiance at the continuous pixel position `p` (level-0 pixels), nearest texel of
// `level`: the slots come straight from the push constants, as the depth chain's do.
float3 ChainColour(float2 p, uint level) {
  const int2 size0 = int2(g_push.sizes.zw);
  const int shift = int(level);
  const int2 size = max((size0 + int(1u << level) - 1) >> shift, int2(1, 1));
  const int2 q = clamp(int2(floor(p)) >> shift, int2(0, 0), size - 1);
  if (level == 0u) {
    return g_textures[g_push.slots.x].Load(int3(q, 0)).rgb;
  }
  if (level == 1u) {
    return g_textures[g_push.slots.y].Load(int3(q, 0)).rgb;
  }
  return g_textures[g_push.slots2.w].Load(int3(q, 0)).rgb;
}

// Where the signed angle `theta` (from the view vector, positive towards the slice's +direction) falls
// in the slice's 32 sectors, 0 to 1: the share of the arc's measure below it. The arc is the half
// circle around the normal (angle n; sin_n, cos_n are its sine and cosine) and the measure is GTAO's,
// cos(theta - n) |sin theta| d theta, whose integral from 0 to theta is
//   G(theta) = (cos n - cos(2 theta - n) + 2 theta sin n) / 4
// (the 'arc' of the plain pass); `g_end` = G(n - pi/2) is the measure of the arc below the view
// vector and `inv_total` is 1 over the measure of the whole arc, cos n + n sin n.
float AoSectorPos(float theta, float n, float sin_n, float cos_n, float g_end, float inv_total) {
  const float t = n + clamp(theta - n, -kHalfPi, kHalfPi);
  const float g = (cos_n - cos(2.0f * t - n) + 2.0f * t * sin_n) * 0.25f;
  const float below = t < 0.0f ? g_end - g : g_end + g;
  return saturate(below * inv_total);
}

// The sectors from `lo` to `hi` (0 to 1, lo <= hi) as a mask: rounded to the nearest sector at each end
// (no bias towards more or less occlusion than the horizon search), and a span that exists is at least
// one sector wide (a thin thing still blocks something).
uint AoSectorBits(float lo, float hi) {
  const uint start = uint(lo * float(kSectors) + 0.5f);
  const uint stop = uint(hi * float(kSectors) + 0.5f);
  uint width = stop > start ? stop - start : 0u;
  if (width == 0u && hi > lo + 1e-4f) {
    width = 1u;
  }
  if (start >= kSectors || width == 0u) {
    return 0u;
  }
  width = min(width, kSectors - start);
  return (width >= kSectors ? 0xFFFFFFFFu : ((1u << width) - 1u)) << start;
}

// One sample of one side of a slice: the front horizon (as the plain pass takes it, with its falloff),
// the sectors it blocks, and for the sectors that are new the sample's radiance. `side` is +1 for the
// slice's +direction and -1 for the other; `away` is the screen direction (pixels, y down) from the
// pixel through the sample; `low` is the cosine of the tangent horizon on that side; `axis` is the
// slice plane's normal. `occupied` and `light` are the slice's mask and radiance so far.
void AoGiSample(float2 p, uint level, float2 away, float side, float low, float3 pc,
                float3 view_vec, float3 axis, float falloff_mul, float falloff_add, float n,
                float sin_n, float cos_n, float g_end, float inv_total, float thickness,
                float facing_weight, inout uint occupied, inout float3 light) {
  const float2 size = g_push.sizes.zw;
  if (!(all(p >= 0.0f) && all(p < size))) {
    return;  // off screen: nothing there, the sky share of the hemisphere stays the game's ambient
  }
  const float3 sp = AoViewPos(floor(p) + 0.5f, size, ChainDepth(p, level));
  const float3 delta = sp - pc;
  const float dist = length(delta);
  const float w = saturate(dist * falloff_mul + falloff_add);
  const float3 dir = delta / max(dist, 1e-6f);
  const float cos_front = max(lerp(low, dot(dir, view_vec), w), low);
  const float u_front = AoSectorPos(side * acos(clamp(cos_front, -1.0f, 1.0f)), n, sin_n, cos_n,
                                    g_end, inv_total);
  float u_back = side > 0.0f ? 1.0f : 0.0f;  // a solid: everything below the front horizon
  if (thickness > 0.0f) {
    const float3 behind = delta + sp * (thickness / max(length(sp), 1e-6f));
    const float cos_back = max(lerp(low, dot(normalize(behind), view_vec), w), low);
    u_back = AoSectorPos(side * acos(clamp(cos_back, -1.0f, 1.0f)), n, sin_n, cos_n, g_end,
                         inv_total);
  }
  const uint bits = AoSectorBits(min(u_front, u_back), max(u_front, u_back));
  const uint fresh = bits & ~occupied;
  occupied |= bits;
  if (fresh == 0u) {
    return;
  }
  // Its radiance, for the share of the slice it newly covers. A diffuse surface radiates the same in
  // every direction, so the sector's measure already is the solid angle it fills (a wall seen from
  // the ground at a steep angle is a thin sliver of the sky because it is one, not because of a
  // second cosine); what is left to ask is whether the surface faces the pixel at all: the slope of
  // its depth one texel further along the slice gives its normal in the slice plane (towards the
  // camera), and the weight is 0 for a surface that faces away (a roof seen from the ground, a wall
  // seen from behind), 1 from a quarter of the way to facing straight at the pixel, a ramp between.
  // No second depth (the sky, the image's edge): it is taken to face the pixel.
  float facing = 1.0f;
  if (facing_weight > 0.0f) {
    const float2 p2 = p + away * float(1u << level);
    const float t2 = ChainDepth(p2, level);
    const float3 tangent = AoViewPos(floor(p2) + 0.5f, size, t2) - sp;
    if (t2 < kAoSky * 0.5f && dot(tangent, tangent) > 1e-10f) {
      float3 normal_s = cross(tangent, axis);
      const float length_n = length(normal_s);
      if (length_n > 1e-6f) {
        normal_s /= length_n;
        if (dot(normal_s, sp) > 0.0f) {
          normal_s = -normal_s;
        }
        facing = lerp(1.0f, saturate(4.0f * dot(normal_s, -dir)), facing_weight);
      }
    }
  }
  light += (float(countbits(fresh)) * (1.0f / float(kSectors)) * facing) * ChainColour(p, level);
}

float4 main(float4 position : SV_Position) : SV_Target {
  const int2 pixel = int2(position.xy);
  const float2 size = g_push.sizes.zw;
  const int2 isize = int2(size);
  const float tc = AoLoadLinear(g_push.slots.z, pixel);
  // The sky, and what is past where the effect fades out entirely: nothing to occlude, nothing blocks.
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float2 centre = float2(pixel) + 0.5f;
  const float3 pc = AoViewPos(centre, size, tc);
  const float3 normal = AoNormalFromImage(g_push.slots.z, pixel, isize, tc, pc);
  const float3 view_vec = normalize(-pc);

  const float radius = g_push.lens.z;
  const float pixels_per_unit = 0.5f * size.y / (g_push.lens.y * tc);
  const float radius_px = min(radius * pixels_per_unit, kMaxRadiusShare * size.y);
  if (radius_px < kMinOffsetPixels + 0.5f) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);  // the effect would be inside the pixel
  }
  const float falloff_ratio = max(g_push.lens.w, 0.01f);
  const float falloff_mul = -1.0f / (falloff_ratio * radius);
  const float falloff_add = 1.0f / falloff_ratio;
  const float thickness = max(g_push.shape.z, 0.0f);
  const float facing_weight = saturate(g_push.shape.w);

  const uint slices = max(AoSlices(), 1u);
  const uint steps = max(AoSteps(), 1u);
  const float2 noise_at = float2(pixel);
  const float2 spatial = float2(AoNoise(noise_at), AoNoise(noise_at.yx + float2(31.0f, 11.0f)));
  const float frame = float(g_push.slots2.y & 1023u);
  const float2 noise = frac(spatial + frame * float2(0.75487766624669f, 0.56984029099806f));
  const float min_offset = kMinOffsetPixels / radius_px;

  // The far ring: from where the near samples end to the reach, if it goes meaningfully farther.
  const float far_reach = max(g_push.extra.w, 0.0f);
  const uint far_steps = (g_push.slots2.z >> 26) & 0x7u;
  const float near_end_px = radius_px * (1.0f + min_offset);
  const float far_px = min(far_reach * pixels_per_unit, kSsgiFarMaxRadiusShare * size.y);
  const bool far_ring = far_steps > 0u && far_px > near_end_px * kSsgiFarMinGrowth + kSsgiFarMinExtraPx;
  const float far_falloff_mul = -1.0f / (falloff_ratio * max(far_reach, 1e-3f));

  float visibility = 0.0f;
  float3 bounce = float3(0.0f, 0.0f, 0.0f);
  for (uint slice = 0u; slice < slices; ++slice) {
    const float phi = (float(slice) + noise.x) / float(slices) * kPi;
    const float2 dir_px = float2(cos(phi), sin(phi));
    const float3 dir_view = float3(dir_px.x, -dir_px.y, 0.0f);
    const float3 ortho = dir_view - dot(dir_view, view_vec) * view_vec;
    const float3 axis = normalize(cross(ortho, view_vec));
    const float3 projected = normal - axis * dot(normal, axis);
    const float projected_len = length(projected);
    const float sign_n = sign(dot(ortho, projected));
    const float cos_n = saturate(dot(projected, view_vec) / max(projected_len, 1e-6f));
    const float n = sign_n * acos(cos_n);
    const float low0 = cos(n + kHalfPi);
    const float low1 = cos(n - kHalfPi);
    // The slice's arc: its sine and its measure (AoSectorPos).
    const float sin_n = sin(n);
    const float total = cos_n + n * sin_n;
    const float inv_total = 1.0f / max(total, 1e-3f);
    const float g_end = (2.0f * cos_n + (2.0f * n - kPi) * sin_n) * 0.25f;

    uint occupied = 0u;
    float3 light = float3(0.0f, 0.0f, 0.0f);
    for (uint s = 0u; s < steps; ++s) {
      const float step_noise = frac(noise.y + float(slice + s * slices) * 0.6180339887f);
      float share = (float(s) + step_noise) / float(steps);
      share = share * share + min_offset;
      const float2 offset = dir_px * (share * radius_px);
      const float level_f = clamp(log2(max(length(offset), 1.0f)) - 3.0f, 0.0f, 2.0f);
      const uint level = uint(level_f);
      AoGiSample(centre + offset, level, dir_px, 1.0f, low0, pc, view_vec, axis, falloff_mul,
                 falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight, occupied,
                 light);
      AoGiSample(centre - offset, level, -dir_px, -1.0f, low1, pc, view_vec, axis, falloff_mul,
                 falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight, occupied,
                 light);
    }
    // The far ring: more samples of this slice, out to the reach, into the sectors still open. The mask
    // the visibility is counted from is the near samples' (occupied_near).
    const uint occupied_near = occupied;
    if (far_ring) {
      for (uint s = 0u; s < far_steps; ++s) {
        const float step_noise = frac(noise.y + float(slice + (steps + s) * slices) * 0.6180339887f);
        float share = (float(s) + step_noise) / float(far_steps);
        share *= share;
        const float length_px = lerp(near_end_px, far_px, share);
        const float2 offset = dir_px * length_px;
        const uint level = uint(clamp(log2(max(length_px, 1.0f)) - 3.0f, 1.0f, 2.0f));
        AoGiSample(centre + offset, level, dir_px, 1.0f, low0, pc, view_vec, axis, far_falloff_mul,
                   falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight, occupied,
                   light);
        AoGiSample(centre - offset, level, -dir_px, -1.0f, low1, pc, view_vec, axis, far_falloff_mul,
                   falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight, occupied,
                   light);
      }
    }
    // The slice's share of the picture: the arc's measure times the projected normal's length, as the
    // plain pass weighs it. The open sectors are the visibility, the radiance of the closed ones the
    // bounce.
    const float weight = projected_len * total;
    visibility += weight * float(kSectors - countbits(occupied_near)) * (1.0f / float(kSectors));
    bounce += weight * light;
  }
  visibility = saturate(visibility / float(slices));
  visibility = max(pow(visibility, g_push.shape.x), g_push.shape.y);
  // A half float keeps 65504; the chain's colours are capped well under that, and a NaN counts as none.
  bounce = min(max(bounce / float(slices), 0.0f), 60000.0f);
  return float4(visibility, bounce);
}

#else  // !AO_GI

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

#endif  // AO_GI
