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
//   lamps       the street lamps of the frame's list: colour x spot cone x 1 / (d^2 + 0.8^2), windowed to zero at the reach, Henyey-Greenstein(g_lamp).
//   headlights  the same for the cars' spot lights with Henyey-Greenstein(g_head), shadowed by their shadow view in the headlights' atlas.
//   haze        the game's own fog colour x sigma: a thickened fog has the game's colour and nothing else is invented.
//   history     the previous frame's volume at the voxel's world position (the previous camera's grid), trilinear, blended in by the weight
//               the host gives (0: never read). The voxel's sample is jittered inside the voxel by the frame's offsets so that the history
//               averages the noise.
// The shadow lookups are the stubs of the first version: a sun tap and a headlight tap; VOL-SHADERS refines them (calibration against the road's
// pool of light, CHECK V7, and the shadow taps, V9).
//
// Shader Model 6.0, 8 x 8 x 1 threads: x, y the voxel in a tile, the dispatch's z the slice.

#include "volumetrics_common.hlsli"

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
float InterleavedGradient(float2 p, float frame) {
  const float shift = 5.588238 * fmod(frame, 64.0);
  const float v = 0.06711056 * (p.x + shift) + 0.00583715 * (p.y + shift);
  return frac(52.9829189 * frac(v));
}

// --- the lights ---------------------------------------------------------------------------------------------------------------------

// The scene units of the lights' colours: a lamp's or a headlight's colour is the game's (its colour times its intensity, a few units), and its
// scattered light is that times this (the survey's section 5.1 estimate: a lamp's cone should show over a clear night's haze of 0.0012 per metre);
// calibrated against the road's pool of light in V7 (volumetrics_rules.h kLightGain is the twin).
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

float LampFalloff(float dist, float range) {
  if (!(dist < range)) {
    return 0.0;
  }
  const float r = dist / range;
  const float w = saturate(1.0 - r * r * r * r);
  return w * w / (dist * dist + 0.64);
}

// A depth texel of a bindless texture, clamped to its size.
float LoadDepth(uint slot, int2 texel, uint2 size) {
  const int2 t = clamp(texel, int2(0, 0), int2(size) - 1);
  return g_textures[slot].Load(int3(t, 0)).x;
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

// The fraction of a headlight's beam that reaches `p` (1 without a shadow view): the published matrix (column vectors, the atlas uv and the
// depth z / w in it), lit when stored >= depth - bias, a stored 1 meaning nothing drawn there (local_shadow_rules.h).
float HeadlightLit(float3 p, int view_index) {
  if (view_index < 0 || (g_push.slots.z & 2u) == 0u) {
    return 1.0;
  }
  const uint base = kO_shadow_views + uint(view_index) * 96u;
  const float4 m0 = VolF4(base);
  const float4 m1 = VolF4(base + 16u);
  const float4 m2 = VolF4(base + 32u);
  const float4 m3 = VolF4(base + 48u);
  const float4 info = VolF4(base + 80u);  // x depth bias, y texel size
  const float4 pp = float4(p, 1.0);
  const float4 o = float4(dot(m0, pp), dot(m1, pp), dot(m2, pp), dot(m3, pp));
  if (!(o.w > 0.0)) {
    return 1.0;
  }
  const float2 uv = o.xy / o.w;
  const float depth = o.z / o.w;
  if (!(depth > 0.0 && depth < 1.0)) {
    return 1.0;
  }
  const uint slot = VolU4(kO_counts).w;
  uint w, h;
  g_textures[slot].GetDimensions(w, h);
  const uint2 size = uint2(w, h);
  const float2 tc = uv * float2(size) - 0.5;
  const int2 i0 = int2(floor(tc));
  const float2 fr = tc - float2(i0);
  const float ref = depth - info.x;
  const float a = LoadDepth(slot, i0, size);
  const float b = LoadDepth(slot, i0 + int2(1, 0), size);
  const float c = LoadDepth(slot, i0 + int2(0, 1), size);
  const float d = LoadDepth(slot, i0 + int2(1, 1), size);
  const float s00 = (a >= 1.0 || a >= ref) ? 1.0 : 0.0;
  const float s10 = (b >= 1.0 || b >= ref) ? 1.0 : 0.0;
  const float s01 = (c >= 1.0 || c >= ref) ? 1.0 : 0.0;
  const float s11 = (d >= 1.0 || d >= ref) ? 1.0 : 0.0;
  return lerp(lerp(s00, s10, fr.x), lerp(s01, s11, fr.x), fr.y);
}

// One spot light (a lamp or a headlight) of the constants at `offset`: its scattered radiance at `p` seen along the unit ray `ray` (camera to p).
float3 SpotLight(uint offset, float3 p, float3 ray, float g, float strength, bool shadowed) {
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
  if (shadowed) {
    lit = HeadlightLit(p, asint(VolF4(offset + 48u).x));
  }
  return c.xyz * (strength * spot * lit * LampFalloff(dist, a.w) * HenyeyGreenstein(dot(travel, -ray), g));
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
  const float n0 = InterleavedGradient(float2(id.xy) + float2(37.0, 17.0) * float(id.z), jitter.w);
  const float3 jit = frac(jitter.xyz + float3(n0, frac(n0 * 7.31), frac(n0 * 3.77))) - 0.5;
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
  const uint4 counts = VolU4(kO_counts);
  float3 lights = 0.0;
  [loop] for (uint i = 0u; i < counts.x; ++i) {
    lights += SpotLight(kO_lamps + i * 64u, p, ray, mixv.z, scale.y, false);
  }
  [loop] for (uint j = 0u; j < counts.y; ++j) {
    lights += SpotLight(kO_headlights + j * 64u, p, ray, mixv.w, scale.z, true);
  }
  scatter += lights * (sigma * kLightGain);
  float4 now = float4(scatter, sigma);

  // --- the history, trilinear in the previous camera's grid ---
  if ((g_push.slots.z & 1u) != 0u && history.z > 0.0) {
    const float3 pc = cam_pos.xyz + cam_right * ((2.0 * cell.x / float(grid.x) - 1.0) * tan_x * zc) +
                      cam_up * ((1.0 - 2.0 * cell.y / float(grid.y)) * tan_y * zc) + cam_fwd * zc;
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
        const float4 old = lerp(a, b, ft);
        now = lerp(now, old, history.z);
      }
    }
  }
  g_out[AtlasTexel(id.xy, id.z, grid)] = min(now, 60000.0);
}
