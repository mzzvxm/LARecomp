#pragma once
// TEST SUPPORT (CITY-RT M2 package H4): the reference of a texture read by slot, written apart from hlsl/rt_hit_sample.hlsli RtHitSample: a bilinear read in two levels, wrap or clamp, the game's
// piecewise-linear gamma where asked, over the fake texture set of hlsl_cpp_shim.h. rt_hit_frame_test.cpp checks the shader's read against it, city_hit_shade_rules_test.cpp gives the twin's
// texture and environment reads from it.

#include <algorithm>
#include <cmath>

#include "hlsl_cpp_shim.h"

namespace rt_hit_ref {

float RefPwl(float gamma) {
  gamma = std::clamp(gamma, 0.0f, 1.0f);
  float scale, offset;
  if (gamma >= 96.0f / 255.0f) {
    if (gamma >= 192.0f / 255.0f) {
      scale = 8.0f / 1024.0f;
      offset = -1024.0f;
    } else {
      scale = 4.0f / 1024.0f;
      offset = -256.0f;
    }
  } else if (gamma >= 64.0f / 255.0f) {
    scale = 2.0f / 1024.0f;
    offset = -64.0f;
  } else {
    scale = 1.0f / 1024.0f;
    offset = 0.0f;
  }
  float lin = gamma * ((255.0f * 1024.0f) * scale) + offset;
  lin += std::trunc(lin * scale);
  return lin * (1.0f / 1023.0f);
}

struct Rgba {
  float c[4];
};
Rgba RefTexel(const hlsl_shim::FakeTexture& t, int x, int y, int level, bool wrap, bool gamma) {
  const int w = std::max(int(t.width) >> level, 1), h = std::max(int(t.height) >> level, 1);
  int qx, qy;
  if (wrap) {
    qx = ((x % w) + w) % w;
    qy = ((y % h) + h) % h;
  } else {
    qx = std::clamp(x, 0, w - 1);
    qy = std::clamp(y, 0, h - 1);
  }
  const hlsl_shim::float4 v = t.levels[size_t(level)][size_t(qy) * size_t(w) + size_t(qx)];
  Rgba out{{v.x, v.y, v.z, v.w}};
  if (gamma) {
    for (int c = 0; c < 3; ++c) out.c[c] = RefPwl(out.c[c]);
  }
  return out;
}
Rgba RefBilinear(const hlsl_shim::FakeTexture& t, float u, float v, int level, bool wrap, bool gamma) {
  const int w = std::max(int(t.width) >> level, 1), h = std::max(int(t.height) >> level, 1);
  const float px = u * float(w) - 0.5f, py = v * float(h) - 0.5f;
  const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
  const float fx = px - float(x0), fy = py - float(y0);
  const Rgba a = RefTexel(t, x0, y0, level, wrap, gamma), b = RefTexel(t, x0 + 1, y0, level, wrap, gamma), c = RefTexel(t, x0, y0 + 1, level, wrap, gamma), d = RefTexel(t, x0 + 1, y0 + 1, level, wrap, gamma);
  Rgba out;
  for (int k = 0; k < 4; ++k) out.c[k] = (a.c[k] + (b.c[k] - a.c[k]) * fx) + ((c.c[k] + (d.c[k] - c.c[k]) * fx) - (a.c[k] + (b.c[k] - a.c[k]) * fx)) * fy;
  return out;
}
Rgba RefSample(const hlsl_shim::FakeTexture& t, float u, float v, float lod, bool wrap, bool gamma) {
  const int levels = int(t.levels.size());
  const float l = std::clamp(lod, 0.0f, float(std::max(levels, 1) - 1));
  const int l0 = int(std::floor(l));
  const int l1 = std::min(l0 + 1, std::max(levels, 1) - 1);
  const float f = l - float(l0);
  Rgba out = RefBilinear(t, u, v, l0, wrap, gamma);
  if (l1 != l0 && f > 0.001f) {
    const Rgba other = RefBilinear(t, u, v, l1, wrap, gamma);
    for (int k = 0; k < 4; ++k) out.c[k] = out.c[k] + (other.c[k] - out.c[k]) * f;
  }
  return out;
}

}  // namespace rt_hit_ref
