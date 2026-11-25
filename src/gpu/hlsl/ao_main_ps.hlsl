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
//   projected normal; the sum over the slices divided by what the same sum is for a surface nothing
//   blocks (a ratio: an open surface is exactly 1 however few slices there are and whatever angle the
//   view takes to it), to a power, never below a floor.
//
// Two things keep the occlusion off ground that nothing hides the sky from. The slice sum used to be divided
// by the number of slices and clamped to 1: with two or three slices the sum of a flat road at a grazing angle
// is 1 only on average, a frame's value above 1 was cut and the rest was not, so a plain road came out
// 4 to 9% dark (more with the distance) and the power made it worse. And a car, which the depth shows as a
// wall at the road's edge, darkened the road for the whole radius: the nearest metre or two beside it, then
// a tail. The depth pass marks a vehicle's pixels in the lowest bit of their depth (ao_common.hlsli,
// AoIsVehicle); a sample that is a vehicle, seen from a pixel that is not, counts only within a share of the
// radius (g_push.extra.x, 0 or 1: all of it; extra.w is the bounce light's far reach), so the road keeps the dark line where the tyres meet it and
// the car's footprint but not a halo; the car's own body (a pixel that is a vehicle) sees everything at the
// full radius as before.
//
// The pixel's own noise is a BLUE noise tile (blue_noise.hlsli; a fixed spatial pattern with no lattice in it: interleaved gradient noise, the first version's, is
// a diagonal lattice whose error the blur and the history leave as thin lines; mcla_gpu_ao_blue_noise false) plus a shift that
// changes every frame through a lattice of 13 frames (ao_stable.h, AoNoiseShift: the Fibonacci lattice, 13 evenly
// spaced rotations and step offsets, all visited by any 13 consecutive frames; the first version's additive
// recurrence of the R2 sequence left gaps of 0.23 of the rotation in 12 frames), so the two noise values of one
// pixel over successive frames cover the square evenly, whatever the camera does: what the
// occlusion's own accumulation over frames (ao_temporal_ps) integrates, and what the denoise
// takes out of any one frame (g_push.slots2.y = the frame's phase, ao_stable.h NoiseCode).

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

// THE TRACED TERM (both variants, the plain pass and the bounce light's; car_rt_ao.h, car_rt_ao_rules.h). Where the device can trace rays against the
// cars of the top level (the player's and the ones beside it), a compute pass (car_rt_ao_cs.hlsl) has cast a few short rays from the pixels near them
// before this pass and left an image the size of this one (R8G8B8A8_UNORM: r the open share of the pixel's hemisphere within the rays' reach, 1 where
// nothing was traced). With the flag kRtOcclusionFlag in the flags (bit 31) the image's bindless slot is in lin.x, as a number (the main pass does
// not read the projection rows lin holds for the depth pass, and every uint of the bounce light's push is taken by its chains), and the visibility
// is the smaller of the screen-space one and the traced one raised to the same power with the same floor: what the camera does not see (the far
// side of a wheel well, the body over a tyre, the road at the foot of a car, between two cars) darkens by the cars' own triangles. In the bounce
// light's variant that is its x channel, the one the denoise and the history read; the bounce itself (yzw) is the screen's. The pass is the same
// to the bit without the flag.
//
// COMPILE-TIME COUNTS. The same source is compiled for the counts of a quality (ao.cpp: 3 slices of 3 steps, or 2 of 2; the
// bounce light adds the far ring's steps a side, 2 or 1): with AO_SLICES and AO_STEPS defined, and in the bounce light's
// pass AO_FAR_STEPS, the loops run over constants: the loops over the steps (and the ring's) unroll and everything that is a
// function of the counts folds. The slice loop keeps its count as a loop (the bitmask pass unrolled over the slices is three
// times the size: AO_UNROLL_SLICES makes that one). Without the counts they are the push block's (flags bits 8-15 and 26-28),
// as the pass always read them: that compilation stays for a count that no variant has (a debug override). The result is the
// same to a rounding error (ao_fast.h).
// AO_FAST_ACOS (the bounce light's pass only, with the counts): the sectors' angles from the polynomial (AoAngleOf; 0.0014 of a
// sector at most, ao_fast.h) in place of the hardware's acos; ao.cpp picks the pipeline without it when mcla_gpu_ao_fast_math is
// false.
#if defined(AO_SLICES) && defined(AO_STEPS)
#define AO_SPECIALISED 1
#define AO_UNROLL [unroll]
#ifdef AO_UNROLL_SLICES
#define AO_SLICE_LOOP [unroll]
#else
#define AO_SLICE_LOOP [loop]
#endif
#else
#define AO_SPECIALISED 0
#define AO_UNROLL
#define AO_SLICE_LOOP
#endif
#ifdef AO_FAR_STEPS
#define AO_FAR_UNROLL [unroll]
#else
#define AO_FAR_UNROLL
#endif
#ifndef AO_FAST_ACOS
#define AO_FAST_ACOS 0
#endif

#include "ao_common.hlsli"

#ifndef AO_GI
#define AO_GI 0
#endif

#include "nan_guard.hlsli"
#if AO_GI
#include "ssgi_common.hlsli"
#include "ao_gbuffer_normal.hlsli"
#include "cabin_shadow_rules.hlsli"
#include "interior_camera_rules.hlsli"
#endif

static const float kPi = 3.14159265358979f;
static const float kHalfPi = 1.57079632679490f;
// The nearest sample of a direction is at least this many pixels from the centre.
static const float kMinOffsetPixels = 1.3f;
// The farthest a sample may reach on screen, as a share of the image height (a surface almost at
// the camera would ask for the whole screen).
static const float kMaxRadiusShare = 0.3f;
// The traced term's flag (the flags word's bit 31, car_rt_ao_rules.h kMainFlag): lin.x is the traced image's slot, as a number.
static const uint kRtOcclusionFlag = 1u << 31;

// The traced term's visibility at `pixel` (the flag is set): the traced open share shaped as the visibility is (the final power and the floor).
float AoTracedVisibility(int2 pixel) {
  const float traced = g_textures[uint(g_push.lin.x)].Load(int3(pixel, 0)).x;
  return max(pow(traced, g_push.shape.x), g_push.shape.y);
}

// The linear depth at the continuous pixel position `p` (level-0 pixels), from `level` of the
// chain, nearest texel. Each level's slot comes straight from the push constants (never a computed
// index), so the descriptor index is the same for every pixel.
// The level's size and the shift are constants of each branch (the level's size is a function of the pass's, so the
// compiler works it out once for the pass): the same texel as ever, without the integer arithmetic of a variable shift.
float ChainDepth(float2 p, uint level) {
  const int2 size0 = max(int2(g_push.sizes.zw), int2(1, 1));
  const int2 ip = int2(floor(p));
  if (level == 0u) {
    return AoLoadLinear(g_push.slots.z, clamp(ip, int2(0, 0), size0 - 1));
  }
  if (level == 1u) {
    return AoLoadLinear(g_push.slots.w, clamp(ip >> 1, int2(0, 0), max((size0 + 1) >> 1, int2(1, 1)) - 1));
  }
  return AoLoadLinear(g_push.slots2.x, clamp(ip >> 2, int2(0, 0), max((size0 + 3) >> 2, int2(1, 1)) - 1));
}

// Whether the sample at the continuous pixel position `p` (level-0 pixels) is a vehicle: the bit the depth
// pass leaves in level 0, read at the sample's own pixel whatever level the sample's depth comes from.
bool AoSampleIsVehicle(float2 p) {
  const int2 size0 = int2(g_push.sizes.zw);
  return AoIsVehicle(AoLoadLinear(g_push.slots.z, clamp(int2(floor(p)), int2(0, 0), size0 - 1)));
}

// The falloff of a vehicle's samples as the (mul, add) of the weight saturate(dist * mul + add): the world's
// ramp (the same share of the radius) over `car_share` of the radius instead of all of it.
float2 AoVehicleFalloff(float car_share, float radius, float falloff_ratio) {
  const float car_radius = max(car_share * radius, 0.05f);
  return float2(-1.0f / (falloff_ratio * car_radius), 1.0f / falloff_ratio);
}

#if AO_GI

static const uint kSectors = 32u;

// The colour chain's radiance at the continuous pixel position `p` (level-0 pixels), nearest texel of
// `level`: the slots come straight from the push constants, as the depth chain's do.
float3 ChainColour(float2 p, uint level) {
  const int2 size0 = max(int2(g_push.sizes.zw), int2(1, 1));
  const int2 ip = int2(floor(p));
  if (level == 0u) {
    return g_textures[g_push.slots.x].Load(int3(clamp(ip, int2(0, 0), size0 - 1), 0)).rgb;
  }
  if (level == 1u) {
    return g_textures[g_push.slots.y]
        .Load(int3(clamp(ip >> 1, int2(0, 0), max((size0 + 1) >> 1, int2(1, 1)) - 1), 0))
        .rgb;
  }
  return g_textures[g_push.slots2.w]
      .Load(int3(clamp(ip >> 2, int2(0, 0), max((size0 + 3) >> 2, int2(1, 1)) - 1), 0))
      .rgb;
}

// The angle of a cosine, 0 to pi: acos of the cosine kept within -1 to 1 (a NaN counts as 1, as the hardware's min does), or
// with AO_FAST_ACOS the polynomial of Abramowitz and Stegun 4.4.45, sqrt(1 - |c|) times a cubic in |c|, mirrored for a
// negative cosine: 6.8e-5 radians at the most, which moves a sector's edge by 0.0014 of a sector at the most, and one
// square root and three fused multiply-adds where the hardware's acos is a longer sequence. ao_fast.h AcosFast is the
// same function in C++ and ao_fast_test.cpp has its error.
float AoAngleOf(float c) {
#if AO_FAST_ACOS
  const float a = min(abs(c), 1.0f);
  const float r = sqrt(1.0f - a) * (1.5707288f + a * (-0.2121144f + a * (0.0742610f + a * -0.0187293f)));
  return c < 0.0f ? kPi - r : r;
#else
  return acos(clamp(c, -1.0f, 1.0f));
#endif
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
                float facing_weight, bool limit_vehicles, float2 vehicle_fall, float2 clip_scale,
                inout uint occupied, inout float3 light) {
  const float2 size = g_push.sizes.zw;
  if (!(all(p >= 0.0f) && all(p < size))) {
    return;  // off screen: nothing there, the sky share of the hemisphere stays the game's ambient
  }
  const float t_sample = ChainDepth(p, level);
  const float3 sp = AoViewPosInv(floor(p) + 0.5f, clip_scale, t_sample);
  const float3 delta = sp - pc;
  const float dist = length(delta);
  float w = saturate(dist * falloff_mul + falloff_add);
  if (limit_vehicles && w > 0.0f && AoSampleIsVehicle(p)) {
    w = min(w, saturate(dist * vehicle_fall.x + vehicle_fall.y));
  }
  const float inv_dist = 1.0f / max(dist, 1e-6f);
  const float3 dir = delta * inv_dist;
  const float cos_front = max(lerp(low, dot(dir, view_vec), w), low);
  const float u_front = AoSectorPos(side * AoAngleOf(cos_front), n, sin_n, cos_n, g_end, inv_total);
  float u_back = side > 0.0f ? 1.0f : 0.0f;  // a solid: everything below the front horizon
  if (thickness > 0.0f) {
    const float3 behind = delta + sp * (thickness / max(length(sp), 1e-6f));
    const float cos_back = max(lerp(low, dot(normalize(behind), view_vec), w), low);
    u_back = AoSectorPos(side * AoAngleOf(cos_back), n, sin_n, cos_n, g_end, inv_total);
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
  // With the normal image (GB-E; ao_common.hlsli AoNormalAt, ao_gbuffer_normal.hlsli) the second depth is not read: the surface's own normal at the sample's pixel, cut by the slice
  // plane, is the same profile normal on a plane (ao_gbuffer_test.cpp compares them) and it is not another surface across a depth edge; a sample in the sky is taken to face the
  // pixel as before. The image's pixel is the sample's own, at the traced size (the chain's coarser levels average the depth, not the normal).
  float facing = 1.0f;
  if (facing_weight > 0.0f) {
    const uint normal_slot = uint(g_push.lin.y);
    if (normal_slot != 0u) {
      if (t_sample < kAoSky * 0.5f) {
        const int2 at = clamp(int2(floor(p)), int2(0, 0), int2(size) - 1);
        const float3 normal_sample = GbOctDecode(g_textures[normal_slot].Load(int3(at, 0)).xy);
        facing = lerp(1.0f, AoGbFacingFromNormal(normal_sample, axis, sp, dir), facing_weight);
      }
    } else {
      const float2 p2 = p + away * float(1u << level);
      const float t2 = ChainDepth(p2, level);
      const float3 tangent = AoViewPosInv(floor(p2) + 0.5f, clip_scale, t2) - sp;
      if (t2 < kAoSky * 0.5f && dot(tangent, tangent) > 1e-10f) {
        float3 normal_s = cross(tangent, axis);
        const float length_n = length(normal_s);
        if (length_n > 1e-6f) {
          normal_s *= 1.0f / length_n;
          if (dot(normal_s, sp) > 0.0f) {
            normal_s = -normal_s;
          }
          facing = lerp(1.0f, saturate(4.0f * dot(normal_s, -dir)), facing_weight);
        }
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
  const bool player_receiver = g_push.lin.z != 0.0f && CabinMarkedDepth(asuint(tc));
  // The sky, and what is past where the effect fades out entirely: nothing to occlude, nothing blocks.
  if (tc >= kAoSky * 0.5f || tc >= g_push.extra.y) {
    return float4(1.0f, 0.0f, 0.0f, 0.0f);
  }
  const float2 centre = float2(pixel) + 0.5f;
  const float3 pc = AoViewPos(centre, size, tc);
  const float3 normal = AoNormalAt(uint(g_push.lin.y), g_push.slots.z, pixel, isize, tc, pc);
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
  // A vehicle's samples reach only a share of the radius on a pixel that is not a vehicle itself.
  const float car_share = g_push.extra.x;  // (the sun's luma is the apply's: the main pass has no use for it)
  const bool limit_vehicles = car_share > 0.001f && car_share < 0.999f && !AoIsVehicle(tc);
  const float2 vehicle_fall = AoVehicleFalloff(car_share, radius, falloff_ratio);
  const float thickness = max(g_push.shape.z, 0.0f);
  const float facing_weight = saturate(g_push.shape.w);

#if AO_SPECIALISED
  const uint slices = AO_SLICES;
  const uint steps = AO_STEPS;
#else
  const uint slices = max(AoSlices(), 1u);
  const uint steps = max(AoSteps(), 1u);
#endif
  const float2 clip_scale = 2.0f / size;
  const float2 spatial = AoSpatialNoise(pixel);
  const float2 noise = frac(spatial + AoNoiseShift());
  const float min_offset = kMinOffsetPixels / radius_px;

  // The far ring: from where the near samples end to the reach, if it goes meaningfully farther.
  const float far_reach = max(g_push.extra.w, 0.0f);
#ifdef AO_FAR_STEPS
  const uint far_steps = AO_FAR_STEPS;
#else
  const uint far_steps = (g_push.slots2.z >> 26) & 0x7u;
#endif
  const float near_end_px = radius_px * (1.0f + min_offset);
  const float far_px = min(far_reach * pixels_per_unit, kSsgiFarMaxRadiusShare * size.y);
  const bool far_ring = far_steps > 0u && far_px > near_end_px * kSsgiFarMinGrowth + kSsgiFarMinExtraPx;
  const float far_falloff_mul = -1.0f / (falloff_ratio * max(far_reach, 1e-3f));

  float visibility = 0.0f;
  float3 bounce = float3(0.0f, 0.0f, 0.0f);
  float open_sum = 0.0f;  // what the slices' weights add up to: the visibility of a surface nothing blocks
  AO_SLICE_LOOP for (uint slice = 0u; slice < slices; ++slice) {
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
    // The slice's arc: its sine and its measure (AoSectorPos). The surface's own tangent on each side is the
    // cosine of n + or - a quarter turn, which is minus and plus the sine of n.
    const float sin_n = sin(n);
    const float low0 = -sin_n;
    const float low1 = sin_n;
    const float total = cos_n + n * sin_n;
    const float inv_total = 1.0f / max(total, 1e-3f);
    const float g_end = (2.0f * cos_n + (2.0f * n - kPi) * sin_n) * 0.25f;

    uint occupied = 0u;
    float3 light = float3(0.0f, 0.0f, 0.0f);
    AO_UNROLL for (uint s = 0u; s < steps; ++s) {
      const float step_noise = frac(noise.y + float(slice + s * slices) * 0.6180339887f);
      float share = (float(s) + step_noise) / float(steps);
      share = share * share + min_offset;
      // dir_px is a unit vector: the offset's length is the share of the radius, and the level of the depth chain
      // the sample reads is 0 under 16 pixels, 1 under 32, 2 from there (the same as log2(length) - 3 cut to 0 to 2
      // and truncated, without the square root and the logarithm).
      const float length_px = share * radius_px;
      const float2 offset = dir_px * length_px;
      const uint level = SsgiSampleLevel(player_receiver, (length_px >= 16.0f ? 1u : 0u) + (length_px >= 32.0f ? 1u : 0u));
      AoGiSample(centre + offset, level, dir_px, 1.0f, low0, pc, view_vec, axis, falloff_mul,
                 falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight,
                 limit_vehicles, vehicle_fall, clip_scale, occupied, light);
      AoGiSample(centre - offset, level, -dir_px, -1.0f, low1, pc, view_vec, axis, falloff_mul,
                 falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight,
                 limit_vehicles, vehicle_fall, clip_scale, occupied, light);
    }
    // The far ring: more samples of this slice, out to the reach, into the sectors still open. The mask
    // the visibility is counted from is the near samples' (occupied_near).
    const uint occupied_near = occupied;
    if (far_ring) {
      AO_FAR_UNROLL for (uint s = 0u; s < far_steps; ++s) {
        const float step_noise = frac(noise.y + float(slice + (steps + s) * slices) * 0.6180339887f);
        float share = (float(s) + step_noise) / float(far_steps);
        share *= share;
        const float length_px = lerp(near_end_px, far_px, share);
        const float2 offset = dir_px * length_px;
        const uint level = SsgiSampleLevel(player_receiver, length_px >= 32.0f ? 2u : 1u);
        AoGiSample(centre + offset, level, dir_px, 1.0f, low0, pc, view_vec, axis, far_falloff_mul,
                   falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight,
                   limit_vehicles, vehicle_fall, clip_scale, occupied, light);
        AoGiSample(centre - offset, level, -dir_px, -1.0f, low1, pc, view_vec, axis, far_falloff_mul,
                   falloff_add, n, sin_n, cos_n, g_end, inv_total, thickness, facing_weight,
                   limit_vehicles, vehicle_fall, clip_scale, occupied, light);
      }
    }
    // The slice's share of the picture: the arc's measure times the projected normal's length, as the
    // plain pass weighs it. The open sectors are the visibility, the radiance of the closed ones the
    // bounce.
    const float weight = projected_len * total;
    visibility += weight * float(kSectors - countbits(occupied_near)) * (1.0f / float(kSectors));
    bounce += weight * light;
    open_sum += weight;
  }
  // The weighted mean over the slices (the plain pass's ratio): an open surface is 1 exactly, and a
  // surface that is all radiance gets that radiance, whatever the slices' angles are.
  const float inv_open = 1.0f / max(open_sum, 1e-3f);
  visibility = saturate(NgNumberOr(visibility * inv_open, 1.0f));   // a NaN is no occlusion, decided by the bits (nan_guard.hlsli): saturate(NaN) is 0 on D3D12 and open on Vulkan
  visibility = max(pow(visibility, g_push.shape.x), g_push.shape.y);
  if ((g_push.slots2.z & kRtOcclusionFlag) != 0u) {
    // THE TRACED TERM (above): the plain pass's rule on the visibility channel, the darker of the two.
    visibility = min(visibility, AoTracedVisibility(pixel));
  }
  // A half float keeps 65504; the chain's colours are capped well under that, and a NaN is none (decided by the bits, nan_guard.hlsli: max(NaN, 0) is 0 on D3D12 only).
  bounce = NgHeld3(bounce * inv_open, 60000.0f);
  return float4(visibility, bounce);
}

#else  // !AO_GI

// One sample of one side of a slice: the horizon cosine after it, `h` before it, `low` the surface's own
// tangent on that side. The sample pulls the horizon towards the cosine of its direction by its falloff
// weight (nothing far away counts); where `limit_vehicles` is set a sample that is a vehicle counts by
// the shorter falloff `vehicle_fall` (AoVehicleFalloff) if that is the smaller. The vehicle bit is only
// looked up for a sample that would raise the horizon: the others cost no read.
float AoHorizonAfter(float2 p, uint level, float3 pc, float3 view_vec, float low, float h,
                     float falloff_mul, float falloff_add, bool limit_vehicles, float2 vehicle_fall,
                     float2 clip_scale) {
  const float2 size = g_push.sizes.zw;
  if (!(all(p >= 0.0f) && all(p < size))) {
    return h;
  }
  const float3 sp = AoViewPosInv(floor(p) + 0.5f, clip_scale, ChainDepth(p, level));
  const float3 delta = sp - pc;
  const float dist = length(delta);
  const float cos_sample = dot(delta, view_vec) * (1.0f / max(dist, 1e-6f));
  float w = saturate(dist * falloff_mul + falloff_add);
  float pull = lerp(low, cos_sample, w);
  if (limit_vehicles && pull > h && AoSampleIsVehicle(p)) {
    w = min(w, saturate(dist * vehicle_fall.x + vehicle_fall.y));
    pull = lerp(low, cos_sample, w);
  }
  return max(h, pull);
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
  const float3 normal = AoNormalAt(uint(g_push.lin.y), g_push.slots.z, pixel, isize, tc, pc);
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
  // A vehicle's samples reach only a share of the radius on a pixel that is not a vehicle itself.
  const float car_share = g_push.extra.x;  // (the sun's luma is the apply's: the main pass has no use for it)
  const bool limit_vehicles = car_share > 0.001f && car_share < 0.999f && !AoIsVehicle(tc);
  const float2 vehicle_fall = AoVehicleFalloff(car_share, radius, falloff_ratio);

#if AO_SPECIALISED
  const uint slices = AO_SLICES;
  const uint steps = AO_STEPS;
#else
  const uint slices = max(AoSlices(), 1u);
  const uint steps = max(AoSteps(), 1u);
#endif
  const float2 clip_scale = 2.0f / size;
  const float2 spatial = AoSpatialNoise(pixel);
  // The frame's shift: a lattice of 13 frames (ao_common.hlsli AoNoiseShift, ao_stable.h).
  const float2 noise = frac(spatial + AoNoiseShift());
  const float min_offset = kMinOffsetPixels / radius_px;

  float visibility = 0.0f;
  float open_sum = 0.0f;  // what the slices' terms add up to for a surface nothing blocks
  AO_SLICE_LOOP for (uint slice = 0u; slice < slices; ++slice) {
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

    // The lowest horizon of each side: the surface's own tangent (nothing hides below it), the cosine of
    // n + or - a quarter turn, which is minus and plus the sine of n.
    const float sin_n = sin(n);
    const float low0 = -sin_n;  // the +direction side
    const float low1 = sin_n;   // the -direction side
    float hcos0 = low0;
    float hcos1 = low1;
    AO_UNROLL for (uint s = 0u; s < steps; ++s) {
      const float step_noise = frac(noise.y + float(slice + s * slices) * 0.6180339887f);
      float share = (float(s) + step_noise) / float(steps);
      share = share * share + min_offset;
      // The offset's length is the share of the radius (dir_px is a unit vector); the depth chain's level is 0
      // under 16 pixels, 1 under 32, 2 from there (log2(length) - 3 cut to 0 to 2 and truncated, without the
      // square root and the logarithm).
      const float length_px = share * radius_px;
      const float2 offset = dir_px * length_px;
      const uint level = (length_px >= 16.0f ? 1u : 0u) + (length_px >= 32.0f ? 1u : 0u);

      hcos0 = AoHorizonAfter(centre + offset, level, pc, view_vec, low0, hcos0, falloff_mul,
                             falloff_add, limit_vehicles, vehicle_fall, clip_scale);
      hcos1 = AoHorizonAfter(centre - offset, level, pc, view_vec, low1, hcos1, falloff_mul,
                             falloff_add, limit_vehicles, vehicle_fall, clip_scale);
    }

    // The horizon angles, kept within the hemisphere around the normal, and the arc integral.
    float h0 = -acos(clamp(hcos1, -1.0f, 1.0f));
    float h1 = acos(clamp(hcos0, -1.0f, 1.0f));
    h0 = n + clamp(h0 - n, -kHalfPi, kHalfPi);
    h1 = n + clamp(h1 - n, -kHalfPi, kHalfPi);
    const float arc0 = (cos_n + 2.0f * h0 * sin_n - cos(2.0f * h0 - n)) * 0.25f;
    const float arc1 = (cos_n + 2.0f * h1 * sin_n - cos(2.0f * h1 - n)) * 0.25f;
    visibility += projected_len * (arc0 + arc1);
    // With nothing in the way both horizons are the hemisphere's edges and the arcs add up to
    // cos(n) + n sin(n).
    open_sum += projected_len * (cos_n + n * sin_n);
  }
  visibility = saturate(NgNumberOr(visibility / max(open_sum, 1e-3f), 1.0f));   // a NaN is no occlusion, decided by the bits (nan_guard.hlsli)
  visibility = max(pow(visibility, g_push.shape.x), g_push.shape.y);
  if ((g_push.slots2.z & kRtOcclusionFlag) != 0u) {
    // THE TRACED TERM (above): the open share the rays found, shaped as the visibility is, the darker of the two.
    visibility = min(visibility, AoTracedVisibility(pixel));
  }
  return float4(visibility, 0.0f, 0.0f, 0.0f);
}

#endif  // AO_GI
