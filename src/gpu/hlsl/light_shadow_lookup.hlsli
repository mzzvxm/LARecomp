// MCLA GPU renderer -- the shadow lookup in HLSL: THE TWIN of src/gpu/light_shadow_lookup.h (perf/LIGHT-UNIFY.md 4.2 "one lookup source", package LU-5).
//
// For a pass that reads a light's shadow from the atlas by the light's ShadowRef (GpuLight packed1: shadow kind, first view, view count; shadow_weight): the deferred light pass GB-H1/H2 and
// any later reader. It is NOT included by the lit shaders' headlight variant or by the volume's inject, which keep their own copies of the lookup (a text change moves the variant pack; the
// volume's shader is compiled): shadow_atlas_test.cpp holds the volume's copy to the same C++ reference this file is held to, and light_shadow_filter_test.cpp / headlight_test.cpp hold the
// variant's helper (HeadlightShadow) to light_shadow_filter.h, which is the reference's surface mode: so that when a run has shown the pictures equal they can include this file in place of
// the copy and nothing changes (CHECK: the main session decides when).
//
// THE SEAMS. The file DECLARES these five (below, after LslView) and the including shader DEFINES them, anywhere after the include (the file reads the atlas and the view table through them, so
// that it does not depend on how the pass binds them):
//     float LslTexel(uint slot, int ix, int iy);   one texel of the atlas's depth plane, as a float; the index clamped into the atlas (Load with a clamp, or a clamp sampler)
//     uint  LslAtlasWidth(uint slot);              the atlas's size in texels
//     uint  LslAtlasHeight(uint slot);
//     LslView LslLoadView(uint entry);             entry `entry` of the frame's view table (GpuShadowBuffer: a 32 byte header, then the 96 byte rows of LocalShadowView)
//     uint  LslLayerOf(uint entry);                its layer: LslLayerBits(header.layers, entry)
// src/gpu/light_shadow_lookup_test (shadow_atlas_test.cpp) compiles THIS FILE as C++ through a shim of the HLSL types, so the text keeps to a subset both languages read: scalars and float3 with
// the operators + - and * by a scalar, .x .y .z and .w, no swizzles of more than one component, dot / cross / length / abs / sqrt / saturate / floor / min / max / lerp only, no file-scope
// constants of the static kind (every number is a #define).
//
// THE MODES. LSL_MODE_SURFACE is the lit shaders' lookup (HL-SHADOW-QUALITY, 2026-11-21: the 4 x 4 footprint of texels read at the receiver's own plane, from the surface's unit normal; a
// kernel of 1 to 3 texels by contact hardening; a fade over the last 5 percent of the tile; no screen position and no noise in it), LSL_MODE_AIR the volume's (bilinear inside the tile, a depth
// in (0, 1)). A deferred pass on surfaces takes LSL_MODE_SURFACE and gives it the surface's normal: the GEOMETRIC one (the depth plane's quad, local_light_ps.hlsl LpShadowNormal), because a
// normal-mapped normal would put the plane's slope wrong at the grazing angles the headlights see the road at. The footprint is sixteen LslTexel reads where the lit shaders take four
// gathers (the pass binds no sampler): the same texels, the same numbers.
//
// THE TEXT must not use the names of the game's registers (r0..r31, c0..c255, p0, ps, a0, aL): every name here starts with Lsl or LSL_.

#ifndef LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI
#define LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI

#define LSL_MODE_SURFACE 0
#define LSL_MODE_AIR 1
// the most entries a light may name: the headlights' table (8). The deferred light pass raises it to its combined entry space (the street lamps' table after the headlights': local_light_ps.hlsl).
#ifndef LSL_MAX_VIEWS
#define LSL_MAX_VIEWS 8
#endif

// the plane lookup's numbers (light_shadow_filter.h: the same names with k for LSL_)
#define LSL_PCF_MIN_RADIUS 0.5f   // texels: the kernel's half width at a contact
#define LSL_PCF_MAX_RADIUS 1.5f   // ... and the most the 4 x 4 footprint holds
#define LSL_LAMP_RADIUS 0.07f     // m: the size of a headlamp seen from the receiver
#define LSL_MAX_SLOPE 60.0f       // the steepest receiver believed (depth per metre across)
#define LSL_SLOPE_BIAS 0.15f      // the share of the receiver's depth change over one texel added to the bias
#define LSL_BORDER_FADE 0.05f     // the share of the tile, from its border in, over which the shadow fades to light
#define LSL_DEGENERATE 1.0e-3f    // the sine of the angle between the light's ray and the plane below which the plane is not believed

#define LSL_LAYER_SINGLE 0
#define LSL_LAYER_STATIC 1
#define LSL_LAYER_LIVE 2

#define LSL_SHADOW_NONE 0
#define LSL_SHADOW_ATLAS 1
#define LSL_SHADOW_RT_MASK 2

// the table's layout (light_shadow_lookup.h GpuShadowBuffer)
#define LSL_HEADER_BYTES 32
#define LSL_VIEW_BYTES 96

// One row of the view table: LocalShadowView (local_shadow_types.h), 96 bytes. m0..m3 = world_to_shadow's rows (out = M * (p, 1), then / w: atlas uv in xy, depth in z); rect = u0 v0 u1 v1
// of the view's tile; bias = the constant depth bias; texel = one texel as a share of the tile; the last two are the rig's light index and the valid flag.
struct LslView {
  float4 m0;
  float4 m1;
  float4 m2;
  float4 m3;
  float4 rect;
  float bias;
  float texel;
  uint light_index;
  uint valid;
};

// The seams (the including shader defines them).
float LslTexel(uint slot, int ix, int iy);
uint LslAtlasWidth(uint slot);
uint LslAtlasHeight(uint slot);
LslView LslLoadView(uint entry);
uint LslLayerOf(uint entry);

// The two bits of entry `entry` in the header's `layers`.
uint LslLayerBits(uint layers, uint entry) {
  return (layers >> (entry * 2u)) & 3u;
}

int LslClampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// The footprint's texel k of a Gather quad (x = (0, 1), y = (1, 1), z = (1, 0), w = (0, 0) of the quad: its column and its row from the quad's lowest).
int LslQuadCol(int k) {
  return (k == 1 || k == 2) ? 1 : 0;
}
int LslQuadRow(int k) {
  return k < 2 ? 1 : 0;
}

// THE SURFACE LOOKUP, one view at the world point (px, py, pz) of the surface with the unit normal (nx, ny, nz): 1 lit .. 0 in shadow (light_shadow_filter.h ViewLit, line for line).
// The receiver's depth at the centre of each of the 4 x 4 texels around it is its own plus the slope of its plane (from the normal and the view's matrix) times the distance in texels;
// each texel is lit when its stored depth is not nearer; the kernel is a box of 2 r texels whose r is the lamp's penumbra through the average blocker (0.5 at a contact, 1.5 at most),
// each texel counted by the part of it the box covers; the shadow fades out over the last LSL_BORDER_FADE of the tile.
float LslPlaneLit(LslView v, float px, float py, float pz, float nx, float ny, float nz, uint slot) {
  float3 p = float3(px, py, pz);
  float3 normal = float3(nx, ny, nz);
  float3 row0 = float3(v.m0.x, v.m0.y, v.m0.z);
  float3 row1 = float3(v.m1.x, v.m1.y, v.m1.z);
  float3 row2 = float3(v.m2.x, v.m2.y, v.m2.z);
  float3 row3 = float3(v.m3.x, v.m3.y, v.m3.z);
  float clip_x = dot(row0, p) + v.m0.w;
  float clip_y = dot(row1, p) + v.m1.w;
  float clip_z = dot(row2, p) + v.m2.w;
  float clip_w = dot(row3, p) + v.m3.w;
  if (clip_w <= 0.0f) {
    return 1.0f;
  }
  float inv_w = 1.0f / clip_w;
  float sx = clip_x * inv_w;
  float sy = clip_y * inv_w;
  float sz = clip_z * inv_w;
  if (sx < v.rect.x || sy < v.rect.y || sx > v.rect.z || sy > v.rect.w || sz < 0.0f || sz > 1.0f) {
    return 1.0f;
  }
  float tile_x = v.rect.z - v.rect.x;
  float tile_y = v.rect.w - v.rect.y;
  float tx = tile_x * v.texel;  // one texel of the tile in atlas uv, on each axis
  float ty = tile_y * v.texel;
  float border = min(min(sx - v.rect.x, v.rect.z - sx) / tile_x, min(sy - v.rect.y, v.rect.w - sy) / tile_y);
  float fade = saturate(border * (1.0f / LSL_BORDER_FADE));
  if (fade <= 0.0f) {
    return 1.0f;
  }

  // The receiver's plane: the Jacobians of (u, v, z) with respect to the world point (u and v in texels), then the depth's change over one texel along u and along v.
  float3 axis = row3;
  float3 ju = (row0 - axis * sx) * (inv_w / tx);
  float3 jv = (row1 - axis * sy) * (inv_w / ty);
  float3 jz = (row2 - axis * sz) * inv_w;
  float len_u = length(ju);
  float len_v = length(jv);
  float a_scale = dot(row2, axis) / dot(axis, axis);  // A = f / (f - n): z/w = A (1 - n / w)
  float limit_u = LSL_MAX_SLOPE * (a_scale - sz) * inv_w / len_u;
  float limit_v = LSL_MAX_SLOPE * (a_scale - sz) * inv_w / len_v;
  float3 vn = cross(jv, normal);
  float det = dot(ju, vn);
  float grad_x = 0.0f;
  float grad_y = 0.0f;
  if (abs(det) > LSL_DEGENERATE * len_u * len_v) {
    grad_x = min(max(dot(jz, vn) / det, -limit_u), limit_u);
    grad_y = min(max(dot(jz, cross(normal, ju)) / det, -limit_v), limit_v);
  }
  float bias = v.bias + LSL_SLOPE_BIAS * (abs(grad_x) + abs(grad_y));
  float zr = sz - bias;

  // The footprint: texels first .. first + 3 on each axis, each read once (the lit shaders read the same sixteen with four gathers).
  float cx = sx / tx;  // the receiver's place in texels of the atlas
  float cy = sy / ty;
  float first_x = floor(cx - 0.5f) - 1.0f;
  float first_y = floor(cy - 0.5f) - 1.0f;
  int ix = int(first_x);
  int iy = int(first_y);
  // The receiver's offset from each texel's centre, in texels: column a is at first + a + 0.5.
  float ox0 = first_x + 0.5f - cx;
  float ox1 = first_x + 1.5f - cx;
  float ox2 = first_x + 2.5f - cx;
  float ox3 = first_x + 3.5f - cx;
  float oy0 = first_y + 0.5f - cy;
  float oy1 = first_y + 1.5f - cy;
  float oy2 = first_y + 2.5f - cy;
  float oy3 = first_y + 3.5f - cy;
  float stored[16];  // the footprint, row by row (rows from the lowest), columns 0 to 3
  float lit[16];
  for (int r = 0; r < 4; ++r) {
    float oy = r == 0 ? oy0 : (r == 1 ? oy1 : (r == 2 ? oy2 : oy3));
    for (int c = 0; c < 4; ++c) {
      float ox = c == 0 ? ox0 : (c == 1 ? ox1 : (c == 2 ? ox2 : ox3));
      float texel = LslTexel(slot, ix + c, iy + r);
      float need = min(zr + grad_x * ox + grad_y * oy, 1.0f);
      stored[r * 4 + c] = texel;
      lit[r * 4 + c] = texel >= need ? 1.0f : 0.0f;
    }
  }
  // The blockers, summed in the order of the four Gathers' components (the reference's order, so that the two agree to the last bit).
  float blockers = 0.0f;
  float blocker_sum = 0.0f;
  for (int q = 0; q < 4; ++q) {
    for (int k = 0; k < 4; ++k) {
      int col = 2 * (q & 1) + LslQuadCol(k);
      int row = 2 * (q >> 1) + LslQuadRow(k);
      float blocks = 1.0f - lit[row * 4 + col];
      blockers += blocks;
      blocker_sum += stored[row * 4 + col] * blocks;
    }
  }
  if (blockers < 0.5f) {
    return 1.0f;
  }
  if (blockers > 15.5f) {
    return lerp(1.0f, 0.0f, fade);
  }
  // CONTACT HARDENING. The lamp's disc seen from the receiver through the blockers' plane has the radius R (w_r - w_b) / w_r there; a texel is w_b / (w_r |ju|) wide there, and
  // w_b / w_r = (A - z_r) / (A - z_b), so the kernel's half width in texels is R |ju| (z_r - z_b) / (A - z_r).
  float mean_blocker = blocker_sum / blockers;
  float radius = LSL_LAMP_RADIUS * 0.5f * (len_u + len_v) * (sz - mean_blocker) / max(a_scale - sz, 1.0e-6f);
  radius = min(max(radius, LSL_PCF_MIN_RADIUS), LSL_PCF_MAX_RADIUS);
  // The box [c - r, c + r] over the texel intervals [e, e + 1] of the footprint, each texel by the part of it the box covers (the weights along an axis sum to 2 r).
  float wx[4];
  float wy[4];
  for (int k = 0; k < 4; ++k) {
    float ex = first_x + float(k);
    float ey = first_y + float(k);
    wx[k] = saturate(min(ex + 1.0f, cx + radius) - max(ex, cx - radius));
    wy[k] = saturate(min(ey + 1.0f, cy + radius) - max(ey, cy - radius));
  }
  float share = 0.0f;
  for (int b = 0; b < 4; ++b) {
    share += wy[b] * (wx[0] * lit[b * 4] + wx[1] * lit[b * 4 + 1] + wx[2] * lit[b * 4 + 2] + wx[3] * lit[b * 4 + 3]);
  }
  share /= 4.0f * radius * radius;
  return lerp(1.0f, saturate(share), fade);
}

// The volume's kernel: the bilinear weights of the 2 x 2 texels around the receiver's place, every tap clamped into the view's tile, a stored 1 lit whatever the bias.
float LslBilinear(uint slot, float su, float sv, float4 rect, float ref_depth) {
  float sx = float(LslAtlasWidth(slot));
  float sy = float(LslAtlasHeight(slot));
  int lox = int(floor(rect.x * sx + 0.5f));
  int loy = int(floor(rect.y * sy + 0.5f));
  int hix = max(int(floor(rect.z * sx + 0.5f)) - 1, lox);
  int hiy = max(int(floor(rect.w * sy + 0.5f)) - 1, loy);
  float tcx = su * sx - 0.5f;
  float tcy = sv * sy - 0.5f;
  int i0x = int(floor(tcx));
  int i0y = int(floor(tcy));
  float frx = tcx - float(i0x);
  float fry = tcy - float(i0y);
  float a = LslTexel(slot, LslClampInt(i0x, lox, hix), LslClampInt(i0y, loy, hiy));
  float b = LslTexel(slot, LslClampInt(i0x + 1, lox, hix), LslClampInt(i0y, loy, hiy));
  float c = LslTexel(slot, LslClampInt(i0x, lox, hix), LslClampInt(i0y + 1, loy, hiy));
  float d = LslTexel(slot, LslClampInt(i0x + 1, lox, hix), LslClampInt(i0y + 1, loy, hiy));
  float s00 = (a >= 1.0f || a >= ref_depth) ? 1.0f : 0.0f;
  float s10 = (b >= 1.0f || b >= ref_depth) ? 1.0f : 0.0f;
  float s01 = (c >= 1.0f || c >= ref_depth) ? 1.0f : 0.0f;
  float s11 = (d >= 1.0f || d >= ref_depth) ? 1.0f : 0.0f;
  return lerp(lerp(s00, s10, frx), lerp(s01, s11, frx), fry);
}

// One view at the world point (px, py, pz) of the surface with the unit normal (nx, ny, nz): 1 lit .. 0 in shadow. A view that is not valid, a point behind it or outside its rectangle or depth
// range is lit. The air mode does not read the normal.
float LslViewLit(LslView v, float px, float py, float pz, float nx, float ny, float nz, uint slot, uint mode) {
  if (v.valid == 0u) {
    return 1.0f;
  }
  if (mode == LSL_MODE_SURFACE) {
    return LslPlaneLit(v, px, py, pz, nx, ny, nz, slot);
  }
  float cx = v.m0.x * px + v.m0.y * py + v.m0.z * pz + v.m0.w;
  float cy = v.m1.x * px + v.m1.y * py + v.m1.z * pz + v.m1.w;
  float cz = v.m2.x * px + v.m2.y * py + v.m2.z * pz + v.m2.w;
  float cw = v.m3.x * px + v.m3.y * py + v.m3.z * pz + v.m3.w;
  if (!(cw > 0.0f)) {
    return 1.0f;
  }
  float su = cx / cw;
  float sv = cy / cw;
  float sz = cz / cw;
  bool depth_out = !(sz > 0.0f && sz < 1.0f);
  if (depth_out || su < v.rect.x || sv < v.rect.y || su > v.rect.z || sv > v.rect.w) {
    return 1.0f;
  }
  return LslBilinear(slot, su, sv, v.rect, sz - v.bias);
}

// A light's MEAN visibility at the world point (px, py, pz) of the surface with the unit normal (nx, ny, nz), from its entries (GpuLight: shadow kind, first view, view count): the one combine
// rule without the weight (light_laws.h CombineShadow). The valid views are averaged and a LSL_LAYER_STATIC entry followed by its LSL_LAYER_LIVE twin is one view whose visibility is their
// product. A light with no atlas views is lit (1).
float LslLightMean(uint shadow_kind, uint first_view, uint view_count, float px, float py, float pz, float nx, float ny, float nz, uint slot, uint mode) {
  if (shadow_kind != LSL_SHADOW_ATLAS || view_count == 0u) {
    return 1.0f;
  }
  float sum = 0.0f;
  uint n = 0u;
  uint end = min(first_view + view_count, uint(LSL_MAX_VIEWS));
  for (uint e = first_view; e < end; ++e) {
    LslView v = LslLoadView(e);
    float vis = LslViewLit(v, px, py, pz, nx, ny, nz, slot, mode);
    if (LslLayerOf(e) == LSL_LAYER_STATIC && e + 1u < end && LslLayerOf(e + 1u) == LSL_LAYER_LIVE) {
      vis *= LslViewLit(LslLoadView(e + 1u), px, py, pz, nx, ny, nz, slot, mode);
      ++e;
    }
    if (v.valid != 0u) {
      sum += vis;
      ++n;
    }
  }
  if (n == 0u) {
    return 1.0f;
  }
  return sum / float(n);
}

// The mean blended by the light's weight: lit = lerp(1, mean, weight), exact at weight 1.
float LslBlend(float mean, float weight) {
  return weight >= 1.0f ? mean : 1.0f + (mean - 1.0f) * weight;
}

// A light's visibility at the world point (px, py, pz) of the surface with the unit normal (nx, ny, nz), from its ShadowRef (GpuLight: shadow kind, first view, view count; shadow_weight): the one
// combine rule (light_laws.h CombineShadow). The valid views are averaged, a LSL_LAYER_STATIC entry followed by its LSL_LAYER_LIVE twin is one view whose visibility is their product, and the
// share is blended by the weight. A light with no atlas views is lit.
float LslLightLit(uint shadow_kind, uint first_view, uint view_count, float weight, float px, float py, float pz, float nx, float ny, float nz, uint slot, uint mode) {
  if (shadow_kind != LSL_SHADOW_ATLAS || view_count == 0u) {
    return 1.0f;
  }
  return LslBlend(LslLightMean(shadow_kind, first_view, view_count, px, py, pz, nx, ny, nz, slot, mode), weight);
}

#endif  // LARECOMP_LIGHT_SHADOW_LOOKUP_HLSLI
