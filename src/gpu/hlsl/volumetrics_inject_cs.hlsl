// MCLA GPU renderer -- volumetric light, pass 1: the inject (volumetrics.h, volumetrics_types.h).
//
// One thread per voxel of the camera aligned grid (x, y over the picture, z in log spaced slices): the medium's extinction at the voxel's
// centre, the light that scatters toward the camera there, and a blend with last frame's volume. It writes (in-scattered radiance per metre,
// extinction) as RGBA16F into the 2D slice atlas (one tile a slice). The integrate pass (volumetrics_integrate_cs.hlsl) turns that into the
// accumulated scatter and transmittance the composite reads.
//
//   extinction  sigma_t (1 + ground_k exp(-(y - ground_y) / ground_h)) (1 + noise_k (2 n - 1)): the height layer of the producer's world (a hill
//               above the layer is clear air) and a two octave value noise scrolled by the wind (VolWorld).
//   sun         sun colour x sun strength x Henyey-Greenstein(g_sun) x its visibility: the game's own cascades (the collector's lookup: the cascade
//               is 4 minus the number of boxes that hold the point, the atlas uv and depth from that cascade's light matrix, a bilinear 2x2
//               compare), visibility lerp(lit, 1, sun_fade). No cascade data: the sun is unshadowed.
//   lamps       the street lamps of the frame's list: colour x spot cone x 1 / (d^2 + near^2), windowed to zero at the reach, Henyey-Greenstein(g_lamp).
//               near^2 is the lamp's own (the fourth row's second float, volumetrics_lamps.h: the game's lamps saturate at their plateau's radius, so the
//               producer gives the plateau's radius squared times 0.618); 0 means the 0.8 m of a lamp that says nothing (the test lights).
//   headlights  the same for the cars' spot lights with Henyey-Greenstein(g_head), shadowed by their shadow view in the headlights' atlas: the lookup stays
//               inside the view's tile (the atlas holds the views side by side) and a point outside the tile, behind the lamp or outside the map's depth range is lit.
//   haze        the game's own fog colour x sigma: a thickened fog has the game's colour and nothing else is invented.
//   history     the previous frame's volume at the voxel's world position (the previous camera's grid), trilinear, blended in by the weight
//               the host gives (0: never read). The voxel's sample is jittered inside the voxel by the frame's offsets so that the history
//               averages the noise.
//   slice lamps VOL-STABLE: a street lamp's pool (1 / (d^2 + near^2), near^2 = 0.618 x the game's intensity: 6 to 8 m) is narrower than a slice past about
//               40 m, so ONE sample of it per voxel pulsed as the camera moved a slice and turned into noise under the jitter. With the flag (the push's
//               slots.w bit 0) a lamp's term is the exact AVERAGE of the kernel over the slice along the voxel's ray (an arc tangent), in up to three pieces where
//               a piece would be wider than the pool, the cone, the window and the phase function taken at each piece's point nearest the lamp, and the lamp
//               takes no jitter in depth (LampSlice; volumetrics_stable.h LampSliceFactor is the twin). The headlights and the sun keep their one jittered sample.
//   steady heads  VOL-STABLE-2 (the push's slots.w bit 1): a headlight that reads a shadow view takes kHeadlightShadowTaps taps of it along the voxel's slice, one in each part of
//               it with the frame's one offset, and averages them (an edge of a shadow inside a slice was one random tap, lit or dark by the jitter); and the history's beam is
//               taken to the light's place (VOL-HL-STEADY): the history holds the headlights' old beam (colour x the term of the light's OLD place x its visibility), so its
//               headlight part is scaled by the ratio of the term now to the term then at the voxel's centre (the constants carry both sets of lights: kO_prev_headlights) and the
//               history keeps its weight: the beam follows a moving light with no trail and the history still averages the noise (the second pass lowered the weight where the term
//               changed instead, which took the trail and the smoothing away). Where the beam is new at the voxel (the term then is under a quarter of the term now) the history
//               is dropped (volumetrics_stable.h 4b: CompensateHeadlightHistory, StratifiedDepth are the twins). The lamps of the slice average are sampled at the voxel's centre
//               in x and y: their term is smooth over a voxel and the jitter only made noise.
// The shadow lookups are the stubs of the first version: a sun tap and a headlight tap; VOL-SHADERS refines them (calibration against the road's
// pool of light, CHECK V7, and the shadow taps, V9).
//
// Shader Model 6.0, 8 x 8 x 1 threads: x, y the voxel in a tile, the dispatch's z the slice.

#include "volumetrics_common.hlsli"
#include "nan_guard.hlsli"
#include "volumetrics_noise.hlsli"

// --- noise ------------------------------------------------------------------------------------------------------------------------

uint Pcg(uint v) {
  const uint state = v * 747796405u + 2891336453u;
  const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Hash01(int3 i) {
  const uint h = Pcg(uint(i.x) + Pcg(uint(i.y) + Pcg(uint(i.z))));
  return float(h & 0xFFFFFFu) * (1.0 / 16777215.0);
}
float ValueNoise(float3 x) {
  const float3 f = floor(x);
  float3 t = x - f;
  t = t * t * (3.0 - 2.0 * t);
  const int3 i = int3(f);
  const float n000 = Hash01(i + int3(0, 0, 0));
  const float n100 = Hash01(i + int3(1, 0, 0));
  const float n010 = Hash01(i + int3(0, 1, 0));
  const float n110 = Hash01(i + int3(1, 1, 0));
  const float n001 = Hash01(i + int3(0, 0, 1));
  const float n101 = Hash01(i + int3(1, 0, 1));
  const float n011 = Hash01(i + int3(0, 1, 1));
  const float n111 = Hash01(i + int3(1, 1, 1));
  return lerp(lerp(lerp(n000, n100, t.x), lerp(n010, n110, t.x), t.y), lerp(lerp(n001, n101, t.x), lerp(n011, n111, t.x), t.y), t.z);
}

// --- the lights ---------------------------------------------------------------------------------------------------------------------

// The scene units of the lights' colours: a lamp's or a headlight's colour is the game's (the lamp's colour times its intensity and its fade, the headlight's
// times the lit shaders' peak and falloff distance squared: volumetrics_lamps.h MakeVolLamp, volumetrics_headlights.h ApplyHeadlightUnits), so that the term
// at a distance is the light the road gets there; the scattered light is that times this and the haze's extinction. 60 = pi (the physical gain from the
// game's units, a surface's radiance over its albedo, to a medium's in-scatter) x 19.1 (how much thicker than its extinction the haze is shown, so that a
// lamp shows in a clear night's 0.0012 per metre): volumetrics_lamps.h, the calibration; mcla_gpu_vol_lamp_strength and _head_strength are the debug values
// (volumetrics_rules.h kLightGain is the twin).
static const float kLightGain = 60.0;

float HenyeyGreenstein(float cos_theta, float g) {
  const float g2 = g * g;
  const float d = 1.0 + g2 - 2.0 * g * cos_theta;
  return (1.0 - g2) / (4.0 * kPi * d * sqrt(d));
}

float SpotFactor(float cos_axis, float cos_outer, float cos_inner) {
  if (cos_outer <= -0.999) {
    return 1.0;
  }
  const float t = saturate((cos_axis - cos_outer) / max(cos_inner - cos_outer, 1e-4));
  return t * t * (3.0 - 2.0 * t);
}

// A car's headlights are low beams (headlight_rig_rules.h LampScale, volumetrics_rules.h HeadlightCutoff / HeadlightNearFade are the twins): no light above the lamp's
// own height by a ramp in the sine of the angle a point is above the lamp, none in the lamp's first 0.8 m.
static const float kHeadCutLow = 0.10;
static const float kHeadCutHigh = 0.38;
static const float kHeadNearStart = 0.8;
static const float kHeadNearSpan = 1.5;
float HeadlightShape(float sin_above, float dist) {
  return (1.0 - smoothstep(kHeadCutLow, kHeadCutHigh, sin_above)) * saturate((dist - kHeadNearStart) / kHeadNearSpan);
}

float LampFalloff(float dist, float range, float near2) {
  if (!(dist < range)) {
    return 0.0;
  }
  const float r = dist / range;
  const float w = saturate(1.0 - r * r * r * r);
  return w * w / (dist * dist + near2);
}

// A depth texel of a bindless texture, clamped to its size.
float LoadDepth(uint slot, int2 texel, uint2 size) {
  const int2 t = clamp(texel, int2(0, 0), int2(size) - 1);
  return g_textures[slot].Load(int3(t, 0)).x;
}
// A depth texel clamped into the texel box [lo, hi] (a tile of the headlights' atlas).
float LoadDepthIn(uint slot, int2 texel, int2 lo, int2 hi) {
  return g_textures[slot].Load(int3(clamp(texel, lo, hi), 0)).x;
}

// The fraction of the sun's light that reaches `p` through the game's cascades: the collector's lookup with a bilinear weighted compare
// (lit when stored >= receiver depth, the game's own compare). 1 when the frame has no cascade data or the point is in none of the boxes.
float CascadeLit(float3 p) {
  const uint4 info = VolU4(kO_sun_atlas);
  const uint cascades = info.y;
  if (cascades == 0u || info.x == 0xFFFFFFFFu) {
    return 1.0;
  }
  const float3 right = asfloat(g_vol.Load3(kO_sun));
  const float3 top = asfloat(g_vol.Load3(kO_sun + 16u));
  const float r = dot(p, right);
  const float t = dot(p, top);
  uint count = 0u;
  [unroll] for (uint i = 0u; i < 4u; ++i) {
    if (i < cascades) {
      const float4 box = VolF4(kO_cascades + i * 80u + 64u);
      count += (abs(r + box.x) < box.y && abs(t + box.z) < box.w) ? 1u : 0u;
    }
  }
  if (count == 0u) {
    return 1.0;  // outside every cascade: no shadow data
  }
  const uint index = cascades - count;
  const uint base = kO_cascades + index * 80u;
  const float4 r0 = VolF4(base);
  const float4 r1 = VolF4(base + 16u);
  const float4 r2 = VolF4(base + 32u);
  const float4 r3 = VolF4(base + 48u);
  const float u = dot(p, float3(r0.x, r1.x, r2.x)) + r3.x;
  const float v = dot(p, float3(r0.y, r1.y, r2.y)) + r3.y;
  const float depth = (dot(p, float3(r0.z, r1.z, r2.z)) + r3.z) * (dot(p, float3(r0.w, r1.w, r2.w)) + r3.w) + VolF4(kO_sun_size).z;
  uint w, h;
  g_textures[info.x].GetDimensions(w, h);
  const uint2 size = uint2(w, h);
  const float2 tc = float2(u, v) * float2(size) - 0.5;
  const int2 i0 = int2(floor(tc));
  const float2 fr = tc - float2(i0);
  const float s00 = LoadDepth(info.x, i0, size) >= depth ? 1.0 : 0.0;
  const float s10 = LoadDepth(info.x, i0 + int2(1, 0), size) >= depth ? 1.0 : 0.0;
  const float s01 = LoadDepth(info.x, i0 + int2(0, 1), size) >= depth ? 1.0 : 0.0;
  const float s11 = LoadDepth(info.x, i0 + int2(1, 1), size) >= depth ? 1.0 : 0.0;
  return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);
}

// The fraction of a headlight's beam that reaches `p` (1 without a shadow view): the published matrix (column vectors, the atlas uv and the depth z / w in
// it), lit when stored >= depth - bias, a stored 1 meaning nothing drawn there (local_shadow_rules.h). The view's tile is u0 v0 u1 v1 of the atlas (the
// published rectangle: its far corner, not its size): a point outside it, or outside the map's depth range, is lit, and the four taps of the bilinear compare stay
// inside the tile (the neighbour tile is another car's view). The atlas is a depth plane read with Load (the point sampler's compare, weighted by the
// fraction of the texel, so that the shadow's edge is smooth in the air).
float HeadlightLit(float3 p, int view_index) {
  if (view_index < 0 || (g_push.slots.z & 2u) == 0u) {
    return 1.0;
  }
  const uint base = kO_shadow_views + uint(view_index) * 96u;
  const float4 m0 = VolF4(base);
  const float4 m1 = VolF4(base + 16u);
  const float4 m2 = VolF4(base + 32u);
  const float4 m3 = VolF4(base + 48u);
  const float4 rect = VolF4(base + 64u);  // u0, v0, u1, v1
  const float4 info = VolF4(base + 80u);  // x depth bias, y texel size, zw the view's light index and its valid flag (bits)
  if (VolU4(base + 80u).w == 0u) {
    return 1.0;
  }
  const float4 pp = float4(p, 1.0);
  const float4 o = float4(dot(m0, pp), dot(m1, pp), dot(m2, pp), dot(m3, pp));
  if (!(o.w > 0.0)) {
    return 1.0;
  }
  const float2 uv = o.xy / o.w;
  const float depth = o.z / o.w;
  if (!(depth > 0.0 && depth < 1.0) || uv.x < rect.x || uv.y < rect.y || uv.x > rect.z || uv.y > rect.w) {
    return 1.0;
  }
  const uint slot = VolU4(kO_counts).w;
  uint w, h;
  g_textures[slot].GetDimensions(w, h);
  const float2 size = float2(w, h);
  const int2 lo = int2(floor(rect.xy * size + 0.5));
  const int2 hi = max(int2(floor(rect.zw * size + 0.5)) - 1, lo);
  const float2 tc = uv * size - 0.5;
  const int2 i0 = int2(floor(tc));
  const float2 fr = tc - float2(i0);
  const float ref = depth - info.x;
  const float a = LoadDepthIn(slot, i0, lo, hi);
  const float b = LoadDepthIn(slot, i0 + int2(1, 0), lo, hi);
  const float c = LoadDepthIn(slot, i0 + int2(0, 1), lo, hi);
  const float d = LoadDepthIn(slot, i0 + int2(1, 1), lo, hi);
  const float s00 = (a >= 1.0 || a >= ref) ? 1.0 : 0.0;
  const float s10 = (b >= 1.0 || b >= ref) ? 1.0 : 0.0;
  const float s01 = (c >= 1.0 || c >= ref) ? 1.0 : 0.0;
  const float s11 = (d >= 1.0 || d >= ref) ? 1.0 : 0.0;
  return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);
}

// A car's light has a view for each of its two headlamps (HL-PER-LAMP; local_shadow_rules.h PlanViews): its share of the beam at `p` is the mean of the two lamps' (a pole
// both lamps light has two shadows, each half as dark where one is blocked); a light with one view (a car's, not its lamps') takes that; none, lit.
float HeadlightLit2(float3 p, int view_a, int view_b) {
  if (view_b < 0) {
    return HeadlightLit(p, view_a);
  }
  if (view_a < 0) {
    return HeadlightLit(p, view_b);
  }
  return 0.5 * (HeadlightLit(p, view_a) + HeadlightLit(p, view_b));
}

// One spot light (a lamp or a headlight) of the constants at `offset`: its scattered radiance at `p` seen along the unit ray `ray` (camera to p). `shaped`: the low beam's
// shape of a headlight; `shadowed`: and its shadow map (only with the shape).
float3 SpotLightEx(uint offset, float3 p, float3 ray, float g, float strength, bool shaped, bool shadowed) {
  const float4 a = VolF4(offset);
  const float4 b = VolF4(offset + 16u);
  const float4 c = VolF4(offset + 32u);
  const float3 delta = p - a.xyz;
  const float dist2 = dot(delta, delta);
  if (!(dist2 < a.w * a.w)) {
    return 0.0;
  }
  const float dist = sqrt(dist2);
  const float3 travel = delta / max(dist, 1e-4);  // the light's direction of travel
  const float spot = SpotFactor(dot(travel, b.xyz), b.w, c.w);
  if (!(spot > 0.0)) {
    return 0.0;
  }
  float lit = 1.0;
  if (shaped) {
    // the headlights (the only shaped lights): the low beam's shape, then the shadow map
    lit = HeadlightShape(travel.y, dist);
    if (!(lit > 0.0)) {
      return 0.0;
    }
    if (shadowed) {
      lit *= HeadlightLit2(p, int(g_vol.Load(offset + 48u)), int(g_vol.Load(offset + 56u)));  // the two views: VolLamp shadow_view (48), shadow_view_b (56)
    }
  }
  const float near_sq = asfloat(g_vol.Load(offset + 52u));  // the light's near radius squared (0: the default 0.8 m)
  return c.xyz * (strength * spot * lit * LampFalloff(dist, a.w, near_sq > 0.0 ? near_sq : 0.64) * HenyeyGreenstein(dot(travel, -ray), g));
}
float3 SpotLight(uint offset, float3 p, float3 ray, float g, float strength, bool shadowed) { return SpotLightEx(offset, p, ray, g, strength, shadowed, shadowed); }

// --- the lamps' term averaged over a slice (volumetrics_stable.h: SliceAverageKernel, LampStretchFactor, LampSliceFactor are the twins) -----------------------------

// The push's slots.w flags (volumetrics_stable.h kInjectFlag*).
static const uint kInjectFlagSliceLamps = 1u;
static const uint kInjectFlagSteadyHeads = 2u;
static const uint kInjectFlagBlueNoise = 4u;   // the voxel's own sample offset is the blue noise tile's (mcla_gpu_noise_blue; volumetrics_noise.hlsli), else the first version's gradient noise
// A shadowed headlight's taps along a slice (volumetrics_stable.h kHeadlightShadowTaps) and the history's beam compensation (kHeadCompFullRatio, kHeadCompMaxRatio, kHeadCompOldBound,
// kHeadCompSignificance): up to the full ratio of the term now to the term then the history keeps its weight, from the max ratio it is dropped; the old beam is at most the bound times the old
// term; a beam under the significance's share of the voxel's other light does not lower the weight.
static const uint kHeadlightShadowTaps = 3u;
static const float kHeadCompFullRatio = 2.0;
static const float kHeadCompMaxRatio = 4.0;
static const float kHeadCompOldBound = 1.25;
static const float kHeadCompSignificance = 0.1;
// A piece of a slice is at most this many pool widths (h = sqrt(dmin^2 + near^2)) long; a slice has at most this many pieces.
static const float kPieceReach = 0.6;
static const uint kMaxPieces = 3u;

// What a lamp scatters toward the camera through the stretch [s0, s1] of the unit ray `ray` from `cam`, per unit of its colour x strength: 1 / (d^2 + near^2)
// averaged over the stretch, times the window, the cone and the phase function at the stretch's point nearest the lamp. `a` = pos, range; `b` = dir, cos_outer;
// `c` = colour, cos_inner (the lamp's three float4).
float StretchFactor(float4 a, float4 b, float4 c, float near_sq, float3 cam, float3 ray, float s0, float s1, float g) {
  const float3 m = a.xyz - cam;
  const float s_c = dot(m, ray);
  const float dmin_sq = max(dot(m, m) - s_c * s_c, 0.0);
  const float s_near = clamp(s_c, s0, s1);
  const float3 delta = cam + ray * s_near - a.xyz;
  const float dist_sq = dot(delta, delta);
  if (!(dist_sq < a.w * a.w)) {
    return 0.0;
  }
  const float dist = sqrt(dist_sq);
  const float3 travel = delta / max(dist, 1e-4);
  const float spot = SpotFactor(dot(travel, b.xyz), b.w, c.w);
  if (!(spot > 0.0)) {
    return 0.0;
  }
  const float r = dist / a.w;
  const float window = saturate(1.0 - r * r * r * r);
  const float h = sqrt(dmin_sq + near_sq);
  const float len = max(s1 - s0, 1e-6);
  // atan(a) - atan(b) = atan2(a - b, 1 + a b): exact where the two arc tangents are nearly equal (a thin slice far from the closest approach)
  const float ka = (s1 - s_c) / h;
  const float kb = (s0 - s_c) / h;
  const float kernel = atan2(len / h, 1.0 + ka * kb) / (h * len);
  return window * window * kernel * spot * HenyeyGreenstein(dot(travel, -ray), g);
}

float3 LampSlice(uint offset, float3 cam, float3 ray, float s0, float s1, float g, float strength) {
  const float4 a = VolF4(offset);
  const float4 b = VolF4(offset + 16u);
  const float4 c = VolF4(offset + 32u);
  const float raw_near = asfloat(g_vol.Load(offset + 52u));  // the lamp's near radius squared (0: the default 0.8 m)
  const float near_sq = raw_near > 0.0 ? raw_near : 0.64;
  const float3 m = a.xyz - cam;
  const float s_c = dot(m, ray);
  // out of reach of the whole stretch: out of reach of every piece of it
  const float3 e = cam + ray * clamp(s_c, s0, s1) - a.xyz;
  if (!(dot(e, e) < a.w * a.w)) {
    return 0.0;
  }
  const float h = sqrt(max(dot(m, m) - s_c * s_c, 0.0) + near_sq);
  const float len = s1 - s0;
  const uint pieces = clamp(uint(ceil(len / max(kPieceReach * h, 1e-3))), 1u, kMaxPieces);
  float total = 0.0;
  [loop] for (uint i = 0u; i < pieces; ++i) {
    const float pa = s0 + len * float(i) / float(pieces);
    const float pb = s0 + len * float(i + 1u) / float(pieces);
    total += StretchFactor(a, b, c, near_sq, cam, ray, pa, pb, g);
  }
  return c.xyz * (strength * total / float(pieces));
}

// --- the history's beam taken to the light's place (volumetrics_stable.h 4b: CompensateHeadlightHistory is the twin) ---------------------------------------------------------
float HeadlightLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The history's light `old_rgb` at the voxel taken to where the headlights are now: its headlight part (what is over `base`, this frame's light of everything but the headlights, up to
// the old term's own share) is scaled by luma(term_now) / luma(term_then). Writes the corrected light and returns the multiplier of the history's weight (1: kept; 0: the beam is new here and the
// history has nothing to scale). The terms carry the colours and the strength and not sigma nor the gain.
float CompensateHeadlightHistory(float3 old_rgb, float3 base, float3 term_now, float3 term_then, float sigma_now, float sigma_old, float gain, out float3 out_rgb) {
  const float luma_now = HeadlightLuma(term_now);
  const float luma_then = HeadlightLuma(term_then);
  float ratio = 1.0;  // neither lights the voxel
  if (!(luma_now > 0.0)) {
    ratio = luma_then > 0.0 ? 0.0 : 1.0;  // the light went out here: the old beam is taken out
  } else {
    ratio = luma_now / max(luma_then, 1e-20);
  }
  const float used = min(ratio, kHeadCompMaxRatio);
  const float3 bound = max(term_then * (kHeadCompOldBound * sigma_old * gain), 0.0);
  const float3 head_old = clamp(old_rgb - base, float3(0.0, 0.0, 0.0), bound);
  out_rgb = old_rgb + head_old * (used - 1.0);
  const float fall = 1.0 - saturate((kHeadCompMaxRatio - ratio) / (kHeadCompMaxRatio - kHeadCompFullRatio));
  const float beam = max(luma_now * sigma_now, luma_then * sigma_old) * gain;
  const float significance = saturate(beam / max(kHeadCompSignificance * HeadlightLuma(base), 1e-6));
  return 1.0 - significance * fall;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint4 grid = VolU4(kO_grid);
  if (id.x >= grid.x || id.y >= grid.y || id.z >= grid.z) {
    return;
  }
  const float4 depth = VolF4(kO_depth);       // near, far, ln(far / near), 1 / ln
  const float4 history = VolF4(kO_history);   // history far, its ln ratio, weight
  const float4 jitter = VolF4(kO_jitter);     // x, y, z the frame's offsets in 0..1, w the frame
  const float4 cam_pos = VolF4(kO_cam_pos);
  const float3 cam_right = VolF4(kO_cam_pos + 16u).xyz;
  const float3 cam_up = VolF4(kO_cam_pos + 32u).xyz;
  const float3 cam_fwd = VolF4(kO_cam_pos + 48u).xyz;
  const float tan_x = cam_pos.w;
  const float tan_y = VolF4(kO_cam_pos + 16u).w;

  // The voxel's nominal centre (the history's key) and its jittered sample (the frame's).
  const float2 cell = float2(id.xy) + 0.5;
  const float3 jit = frac(jitter.xyz + VolVoxelNoise(int(id.x), int(id.y), int(id.z), jitter.w, (g_push.slots.w & kInjectFlagBlueNoise) != 0u)) - 0.5;
  const float2 uv = (cell + jit.xy * 0.9) / float2(grid.xy);
  const float slice = float(id.z) + 0.5 + jit.z * 0.9;
  const float z = depth.x * exp(depth.z * slice / float(grid.z));
  const float zc = depth.x * exp(depth.z * (float(id.z) + 0.5) / float(grid.z));
  const float3 p = cam_pos.xyz + cam_right * ((2.0 * uv.x - 1.0) * tan_x * z) + cam_up * ((1.0 - 2.0 * uv.y) * tan_y * z) + cam_fwd * z;
  const float3 ray = normalize(p - cam_pos.xyz);

  // --- the medium ---
  const float4 d = VolF4(kO_world);           // sigma, ground_y, ground_h, ground_k
  const float4 nz = VolF4(kO_world + 16u);    // noise_k, noise_scale, wind x, wind z
  const float4 sun_dir = VolF4(kO_world + 32u);
  const float4 sun_col = VolF4(kO_world + 48u);
  const float4 haze = VolF4(kO_world + 64u);
  const float4 mixv = VolF4(kO_world + 80u);  // night, wet, g_lamp, g_head
  const float4 scale = VolF4(kO_world + 96u); // extinction_scale, lamp, head, sun strength
  float layer = 1.0;
  if (d.z > 0.0) {
    layer += d.w * exp(-max(p.y - d.y, 0.0) / d.z);
  }
  float noise = 1.0;
  if (nz.x > 0.0) {
    const float3 q = (p + float3(nz.z, 0.0, nz.w)) * nz.y;
    const float n = 0.65 * ValueNoise(q) + 0.35 * ValueNoise(q * 2.03 + 11.7);
    noise = 1.0 + nz.x * (2.0 * n - 1.0);
  }
  const float sigma = max(0.0, d.x * layer * noise);

  // --- the light that scatters toward the camera ---
  float3 scatter = haze.xyz * sigma;
  if (max(sun_col.x, max(sun_col.y, sun_col.z)) > 0.0) {
    const float vis = lerp(CascadeLit(p), 1.0, sun_dir.w);
    scatter += sun_col.xyz * (scale.w * vis * HenyeyGreenstein(dot(sun_dir.xyz, ray), sun_col.w)) * sigma;
  }
  const float3 scatter_haze = scatter;  // the medium's own and the sun's, before any lamp or headlight
  const uint4 counts = VolU4(kO_counts);
  float3 lights = 0.0;
  float3 head_lights = 0.0;  // the headlights' share of `lights`
  // the grid's view depth bounds of this voxel's slice
  const float z0 = depth.x * exp(depth.z * float(id.z) / float(grid.z));
  const float z1 = depth.x * exp(depth.z * float(id.z + 1u) / float(grid.z));
  if ((g_push.slots.w & kInjectFlagSliceLamps) != 0u) {
    // the lamps through the voxel's slice, along the ray through the voxel's CENTRE in x and y (the term is smooth over a voxel, so the jitter was noise only) and with no
    // jitter in depth: the slice's own bounds
    const float2 uv_c = cell / float2(grid.xy);
    const float3 dirvec = cam_right * ((2.0 * uv_c.x - 1.0) * tan_x) + cam_up * ((1.0 - 2.0 * uv_c.y) * tan_y) + cam_fwd;
    const float dir_len = length(dirvec);
    const float3 ray_l = dirvec / dir_len;
    [loop] for (uint i = 0u; i < counts.x; ++i) {
      lights += LampSlice(kO_lamps + i * 64u, cam_pos.xyz, ray_l, z0 * dir_len, z1 * dir_len, mixv.z, scale.y);
    }
  } else {
    [loop] for (uint i = 0u; i < counts.x; ++i) {
      lights += SpotLight(kO_lamps + i * 64u, p, ray, mixv.z, scale.y, false);
    }
  }
  const bool steady_heads = (g_push.slots.w & kInjectFlagSteadyHeads) != 0u;
  // the ray of the jittered sample, for the taps of a shadowed headlight along the slice (the same line as `ray`, at other depths)
  const float3 dirvec_j = (p - cam_pos.xyz) / z;
  const float tap_u = jit.z + 0.5;
  [loop] for (uint j = 0u; j < counts.y; ++j) {
    const uint offset = kO_headlights + j * 64u;
    if (steady_heads && int(g_vol.Load(offset + 48u)) >= 0 && (g_push.slots.z & 2u) != 0u) {
      // a shadowed headlight: the shadow map's taps in each part of the slice, averaged (an edge of a shadow inside the slice shares them by where it is)
      float3 taps = 0.0;
      [unroll] for (uint t = 0u; t < kHeadlightShadowTaps; ++t) {
        const float zt = z0 + (float(t) + tap_u) / float(kHeadlightShadowTaps) * (z1 - z0);
        taps += SpotLight(offset, cam_pos.xyz + dirvec_j * zt, ray, mixv.w, scale.z, true);
      }
      taps /= float(kHeadlightShadowTaps);
      lights += taps;
      head_lights += taps;
    } else {
      const float3 one = SpotLight(offset, p, ray, mixv.w, scale.z, true);
      lights += one;
      head_lights += one;
    }
  }
  scatter += lights * (sigma * kLightGain);
  // This voxel's light, a number held to what the volume keeps (nan_guard.hlsli): the volume is its own history (the next frame blends with it, and NaN times a weight of 0 is NaN), so a NaN or an infinity
  // out of a lamp's law must not get in (min(now, 60000) below leaves a NaN to the API on Vulkan).
  float4 now = NgHeld4(float4(scatter, sigma), 60000.0);

  // --- the history, trilinear in the previous camera's grid ---
  if ((g_push.slots.z & 1u) != 0u && history.z > 0.0) {
    float weight = history.z;
    const float3 pc = cam_pos.xyz + cam_right * ((2.0 * cell.x / float(grid.x) - 1.0) * tan_x * zc) +
                      cam_up * ((1.0 - 2.0 * cell.y / float(grid.y)) * tan_y * zc) + cam_fwd * zc;
    // the headlights' term (no shadow) at the voxel's centre: of this frame's lights and of the ones the history's volume was made with
    const uint prev_count = VolU4(kO_prev_counts).x;
    const bool compensate = steady_heads && (counts.y > 0u || prev_count > 0u);
    float3 term_now = 0.0;
    float3 term_then = 0.0;
    if (compensate) {
      const float3 ray_c = normalize(pc - cam_pos.xyz);
      [loop] for (uint k = 0u; k < counts.y; ++k) {
        term_now += SpotLightEx(kO_headlights + k * 64u, pc, ray_c, mixv.w, scale.z, true, false);
      }
      [loop] for (uint m = 0u; m < prev_count; ++m) {
        term_then += SpotLightEx(kO_prev_headlights + m * 64u, pc, ray_c, mixv.w, scale.z, true, false);
      }
    }
    const float4 pp = VolF4(kO_prev_pos);
    const float3 pr = VolF4(kO_prev_pos + 16u).xyz;
    const float3 pu = VolF4(kO_prev_pos + 32u).xyz;
    const float3 pf = VolF4(kO_prev_pos + 48u).xyz;
    const float ptan_y = VolF4(kO_prev_pos + 16u).w;
    const float3 rel = pc - pp.xyz;
    const float pz = dot(rel, pf);
    if (pz > depth.x) {
      const float2 puv = float2(0.5 * (dot(rel, pr) / (pz * pp.w) + 1.0), 0.5 * (1.0 - dot(rel, pu) / (pz * ptan_y)));
      const float s = float(grid.z) * log(pz / depth.x) / history.y - 0.5;
      if (all(puv > 0.0) && all(puv < 1.0) && s > -0.5 && s < float(grid.z) - 0.5) {
        const uint s0 = uint(clamp(floor(s), 0.0, float(grid.z) - 1.0));
        const uint s1 = min(s0 + 1u, grid.z - 1u);
        const float ft = clamp(s - float(s0), 0.0, 1.0);
        const Texture2D<float4> tex = g_textures[g_push.slots.x];
        const float4 a = tex.SampleLevel(g_samplers[kSamplerLinearClamp], AtlasUv(puv, s0, grid), 0);
        const float4 b = tex.SampleLevel(g_samplers[kSamplerLinearClamp], AtlasUv(puv, s1, grid), 0);
        float4 old = lerp(a, b, ft);
        // a history texel that is not a number (a NaN in the trilinear footprint) is no history: this frame's light stays (select, not a branch: nan_guard.hlsli)
        old = select(NgIsNumber4(old), old, now);
        if (compensate) {
          // the history's headlight part to where the lights are now; this frame's light of everything but the headlights is what the history's beam is told from
          const float3 base = scatter_haze + (lights - head_lights) * (sigma * kLightGain);
          float3 old_rgb;
          weight *= CompensateHeadlightHistory(old.rgb, base, term_now, term_then, sigma, old.w, kLightGain, old_rgb);
          old = float4(old_rgb, old.w);
        }
        now = lerp(now, old, weight);
      }
    }
  }
  g_out[AtlasTexel(id.xy, id.z, grid)] = min(now, 60000.0);
}
