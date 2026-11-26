// MCLA GPU renderer -- screen-space reflections (ssr.h, GB-I1): the Hi-Z ray march. Pure: it reads the Hi-Z through SsrHizAt, which the includer defines, so that src/gpu/ssr_test.cpp can compile
// this text as C++ and trace through analytic scenes (it is checked against a brute force march of the full resolution depth and against the planes' own geometry).
//
// THE SPACE. A ray starts at the receiver (view space, z forward) and runs along R. It is projected into the Hi-Z's level 0 grid (texels, y down) with the stored depth's own coordinate d
// (ndc, 0 near .. 1 far: 1 - the guest's stored value), which is the one that stays AFFINE along a straight ray in screen space (it is a function of 1 / z, and 1 / z is affine in screen
// space), so that the ray is a straight line (px, py, d) and a cell of the Hi-Z can be crossed exactly. The Hi-Z holds the nearest surface of the texels of a cell (min of the linear
// depth, screen_frame.h kHizMin); its linear depth is turned into d once per cell read.
//
// THE WALK (Stachowiak's, "Stochastic Screen-Space Reflections", 2015, the hierarchical form of Uludag's 2014): at a level, the ray enters a cell at tau. If its depth is at or behind the
// cell's nearest surface it may hit something in the cell: at level 0 it hits (when it is not too far behind, the thickness), above it the walk goes down a level and looks again. If it is in
// front, it either crosses the nearest surface's depth inside the cell (it hits at level 0, else the walk goes down at that point) or leaves the cell first (it moves to the cell's edge and
// the walk goes UP a level: empty space is crossed in big steps). The start skips the first texels so that the ray does not meet its own surface.
//
// WHAT COMES BACK. A hit: the ray's texel position and the surface's linear depth there. A miss: whether the ray's end was on the picture (it did not leave it), where, so that the caller can
// take the sky's colour there (a ray that reaches the vanishing point of its direction sees the sky of that direction, exactly).

#ifndef MCLA_GPU_SSR_TRACE_HLSLI
#define MCLA_GPU_SSR_TRACE_HLSLI

#include "ssr_common.hlsli"

// The includer defines: the Hi-Z's min linear depth at cell (cx, cy) of `level` (0 is the traced size). kSsrSky or more for the sky.
// float SsrHizAt(uint level, int cx, int cy);

static const float kSsrThickAbs = 0.3f;         // a surface is at least this thick, world units
static const float kSsrStartTexels = 1.5f;      // the ray's first texels are skipped (its own surface)
static const float kSsrStepEps = 0.02f;         // the nudge across a cell's edge, in texels of level 0

struct SsrTraceParams {
  float width;          // the Hi-Z level 0's size in texels
  float height;
  uint width_i;
  uint height_i;
  float tan_x;
  float tan_y;
  float lin_x;          // the projection's depth rows (ScreenCamera::lin)
  float lin_y;
  float lin_z;
  float lin_w;
  float near_plane;
  float max_distance;   // the longest ray, world units
  float thick_rel;      // a surface's thickness as a share of its depth
  uint levels;          // the Hi-Z's levels that can be read (1 up to 6)
  uint max_iter;
};

struct SsrHit {
  bool hit;
  bool inside_end;      // a miss whose ray did not leave the picture: (px, py) is where it ended
  float px;             // the hit's or the end's position in texels of level 0
  float py;
  float surface_z;      // the hit surface's linear depth
  float depth_error_fraction;  // how deeply the ray entered a cell behind its surface, relative to hit thickness
  uint iterations;
};

// The stored depth coordinate of a linear depth (0 near .. 1 far, beyond 1 past the far plane) and back: the projection's depth rows (ao_common.hlsli AoLinearDepth is the inverse).
float SsrDOfZ(SsrTraceParams p, float z) {
  const float den = p.lin_y * z - p.lin_w;
  return (p.lin_x * z - p.lin_z) / (abs(den) > 1.0e-12f ? den : 1.0e-12f);
}
float SsrZOfD(SsrTraceParams p, float d) {
  const float den = p.lin_x - d * p.lin_y;
  return (p.lin_z - d * p.lin_w) / (abs(den) > 1.0e-12f ? den : 1.0e-12f);
}

// The texel position of a view space point and the other way round.
float SsrPixelX(SsrTraceParams p, float x, float z) {
  return (0.5f + 0.5f * x / (p.tan_x * z)) * p.width;
}
float SsrPixelY(SsrTraceParams p, float y, float z) {
  return (0.5f - 0.5f * y / (p.tan_y * z)) * p.height;
}

uint SsrLevelSize(uint extent, uint level) {
  return (extent + (1u << level) - 1u) >> level;
}

// Walks the ray from the origin (ox, oy, oz) along the unit direction (rx, ry, rz). The origin must be on the picture and in front of the camera.
SsrHit SsrTrace(SsrTraceParams p, float ox, float oy, float oz, float rx, float ry, float rz) {
  SsrHit h;
  h.hit = false;
  h.inside_end = false;
  h.px = 0.0f;
  h.py = 0.0f;
  h.surface_z = 0.0f;
  h.depth_error_fraction = 0.0f;
  h.iterations = 0u;
  if (!(oz > 0.0f)) {
    return h;
  }
  // The ray's length: the longest, shortened so that it ends in front of the near plane when it points back at the camera.
  const float z_clip = max(p.near_plane, 0.05f) * 1.02f;
  float len = p.max_distance;
  if (rz < 0.0f) {
    len = min(len, (z_clip - oz) / rz);
  }
  if (!(len > 1.0e-3f)) {
    return h;
  }
  const float ex = ox + rx * len;
  const float ey = oy + ry * len;
  const float ez = oz + rz * len;
  const float px0 = SsrPixelX(p, ox, oz);
  const float py0 = SsrPixelY(p, oy, oz);
  const float d0 = SsrDOfZ(p, oz);
  const float px1 = SsrPixelX(p, ex, ez);
  const float py1 = SsrPixelY(p, ey, ez);
  const float d1 = SsrDOfZ(p, ez);
  const float dx = px1 - px0;
  const float dy = py1 - py0;
  const float dd = d1 - d0;
  // The part of the segment that is on the picture.
  float tau_end = 1.0f;
  if (dx > 1.0e-9f) {
    tau_end = min(tau_end, (p.width - px0) / dx);
  } else if (dx < -1.0e-9f) {
    tau_end = min(tau_end, (0.0f - px0) / dx);
  }
  if (dy > 1.0e-9f) {
    tau_end = min(tau_end, (p.height - py0) / dy);
  } else if (dy < -1.0e-9f) {
    tau_end = min(tau_end, (0.0f - py0) / dy);
  }
  const bool leaves = tau_end < 1.0f;
  if (px0 < 0.0f || py0 < 0.0f || px0 >= p.width || py0 >= p.height) {
    return h;
  }
  const float major = max(abs(dx), abs(dy));
  if (!(major > 1.0e-6f)) {
    return h;
  }
  const float eps_tau = kSsrStepEps / major;
  float tau = kSsrStartTexels / major;
  const uint max_level = min(p.levels, 6u) - 1u;
  uint level = min(1u, max_level);
  const float sky_half = kSsrSky * 0.5f;
  uint i = 0u;
  SSR_LOOP for (; i < p.max_iter; ++i) {
    if (tau >= tau_end) {
      break;
    }
    const float px = px0 + dx * tau;
    const float py = py0 + dy * tau;
    const float d = d0 + dd * tau;
    const float scale = float(1u << level);
    const int lw = int(SsrLevelSize(p.width_i, level));
    const int lh = int(SsrLevelSize(p.height_i, level));
    const int cx = clamp(int(floor(px / scale)), 0, lw - 1);
    const int cy = clamp(int(floor(py / scale)), 0, lh - 1);
    const float zs = SsrHizAt(level, cx, cy);
    const float ds = zs >= sky_half ? 2.0f : SsrDOfZ(p, zs);
    // where the ray leaves this cell
    float tx = 1.0e30f;
    if (dx > 1.0e-9f) {
      tx = (float(cx + 1) * scale - px0) / dx;
    } else if (dx < -1.0e-9f) {
      tx = (float(cx) * scale - px0) / dx;
    }
    float ty = 1.0e30f;
    if (dy > 1.0e-9f) {
      ty = (float(cy + 1) * scale - py0) / dy;
    } else if (dy < -1.0e-9f) {
      ty = (float(cy) * scale - py0) / dy;
    }
    const float t_exit = min(min(tx, ty), tau_end);
    if (d >= ds) {
      // at or behind the nearest surface of the cell
      if (level == 0u) {
        const float zr = SsrZOfD(p, d);
        const float thick = max(kSsrThickAbs, p.thick_rel * zs);
        if (zr - zs <= thick) {
          h.hit = true;
          h.px = px;
          h.py = py;
          h.surface_z = zs;
          h.depth_error_fraction = max(zr - zs, 0.0f) / thick;
          break;
        }
        tau = t_exit + eps_tau;  // behind a thin thing: on past it
      } else {
        level -= 1u;
      }
    } else {
      bool crosses = false;
      float t_cross = 0.0f;
      if (dd > 1.0e-12f) {
        t_cross = (ds - d0) / dd;
        crosses = t_cross < t_exit;
      }
      if (crosses) {
        tau = max(t_cross, tau);
        if (level == 0u) {
          h.hit = true;
          h.px = px0 + dx * tau;
          h.py = py0 + dy * tau;
          h.surface_z = zs;
          break;
        }
        level -= 1u;
      } else {
        tau = t_exit + eps_tau;
        level = min(level + 1u, max_level);
      }
    }
  }
  h.iterations = i;
  if (!h.hit) {
    h.inside_end = !leaves && tau >= tau_end && i < p.max_iter;
    h.px = px0 + dx * min(tau_end, 1.0f);
    h.py = py0 + dy * min(tau_end, 1.0f);
  }
  return h;
}

// The surface's facing at a hit: the normal from the Hi-Z's level 0 neighbours (the one on each axis whose depth is nearer to the hit's, as hlsl/ao_common.hlsli AoNormalFromImage), the
// dot product of its view-space direction with the ray's. A surface facing the ray's origin has a negative value; a positive one is a back face the screen does not show properly.
float SsrHitFacing(SsrTraceParams p, int cx, int cy, float z, float rx, float ry, float rz) {
  const int xl = max(cx - 1, 0);
  const int xr = min(cx + 1, int(p.width_i) - 1);
  const int yt = max(cy - 1, 0);
  const int yb = min(cy + 1, int(p.height_i) - 1);
  const float zl = SsrHizAt(0u, xl, cy);
  const float zr = SsrHizAt(0u, xr, cy);
  const float zt = SsrHizAt(0u, cx, yt);
  const float zb = SsrHizAt(0u, cx, yb);
  const float3 pc = SsrViewPos(float(cx) + 0.5f, float(cy) + 0.5f, p.width, p.height, z, p.tan_x, p.tan_y);
  const float3 pl = SsrViewPos(float(xl) + 0.5f, float(cy) + 0.5f, p.width, p.height, zl, p.tan_x, p.tan_y);
  const float3 pr = SsrViewPos(float(xr) + 0.5f, float(cy) + 0.5f, p.width, p.height, zr, p.tan_x, p.tan_y);
  const float3 pt = SsrViewPos(float(cx) + 0.5f, float(yt) + 0.5f, p.width, p.height, zt, p.tan_x, p.tan_y);
  const float3 pb = SsrViewPos(float(cx) + 0.5f, float(yb) + 0.5f, p.width, p.height, zb, p.tan_x, p.tan_y);
  const float3 hh = abs(zl - z) < abs(zr - z) ? pc - pl : pr - pc;
  const float3 vv = abs(zt - z) < abs(zb - z) ? pt - pc : pc - pb;
  float3 n = cross(vv, hh);
  const float len = sqrt(dot(n, n));
  if (!(len > 1.0e-20f)) {
    return -1.0f;
  }
  n = n * (1.0f / len);
  // facing the camera
  if (dot(n, pc) > 0.0f) {
    n = n * -1.0f;
  }
  return dot(n, float3(rx, ry, rz));
}

#endif  // MCLA_GPU_SSR_TRACE_HLSLI
