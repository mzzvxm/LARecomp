// Check of the FUSED Hi-Z (screen_extract.h "the fused Hi-Z", hlsl/screen_hiz.hlsli, hlsl/ao_mip.hlsli, SS-3): while the reflections want the Hi-Z, the depth chain's own passes write its first
// levels beside their images in the same draw (two render targets), and the images must be the separate passes' to the bit. A program of its own: it needs no renderer, no ReXGlue and no GPU, and
// exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_screen_hiz_fuse_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_screen_hiz_fuse_test [<src/gpu dir>] [--print]
//
// WHAT IT PROVES.
//   1. THE TWIN. The two .hlsli files the fused shaders call (hlsl/screen_hiz.hlsli, hlsl/ao_mip.hlsli) are compiled here AS C++ through a shim of the HLSL types, and a whole frame's depth chain and Hi-Z are
//      made two ways on random depth buffers: the OLD WAY (the AO's depth pass, then its mip pass twice, with the statements of the old ao_mip_ps.hlsl copied below verbatim; then the Hi-Z's own pass,
//      level 0 from the depth chain's level 0 with the marks off, every level above it the min of 2x2 of the level below with the odd edge read twice, as hlsl/screen_hiz_ps.hlsl does it) and the
//      FUSED WAY (what each fused draw computes for its two targets, in the order of the draws, then the Hi-Z's own pass for the levels above them). Every texel of every level of both chains, the
//      depth's three levels (the R16_FLOAT levels rounded to half as the target stores them) and the Hi-Z's, must be the same bits. The buffers: sizes from 1x1 to 480x270 (odd ones, tiny ones), the depth
//      target's values REVERSED-Z (1 near, 0 far) through the AO's linearisation (the far plane, the sky, a value one unit below 1, the smallest values, planes at a grazing slope) with the vehicle mark
//      and the half size block's source bits in the low bits, and raw random finite floats with random low bits.
//   2. THE RULES. FusedHizLevels (how many of the Hi-Z's levels the depth passes write) over every size from 1 to 4200 on both axes and the AO's own level sizes (ao_size.h); the push blocks of the
//      fused draws against the plain passes' (they differ in slots.w and nothing else); the kinds' numbering; and the pass list and bandwidth model, printed for 1080p and 4K (--print): what a frame
//      with the Hi-Z records with the separate passes and with the fused ones.
//   3. THE SOURCES (given the src/gpu folder): the fused shaders are the old shaders compiled with a define (one function writes the first target for both, so the depth chain's images cannot differ),
//      the CMake and host_shaders.h registration, the recording's order and fallbacks in screen_extract.cpp, Publish's use of what the depth step made, the setting.
//   dxc DXIL / SPIR-V compilation and spirv-val of the three fused shaders and the two plain ones are the chain's unit stage (chain-ss-2.ps1).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ao_size.h"
#include "screen_extract.h"
#include "screen_frame.h"
#include "screen_rules.h"

// ---------------------------------------------------------------------------
// the .hlsli as C++: a shim of what the two files use, in a namespace of its own so that the standard library's min ... are not in the way
// ---------------------------------------------------------------------------
namespace hlsl_shim {
typedef unsigned int uint;
struct float4 {
  float x = 0, y = 0, z = 0, w = 0;
  float4() = default;
  float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float saturate(float a) { return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
inline float4 saturate(float4 a) { return float4(saturate(a.x), saturate(a.y), saturate(a.z), saturate(a.w)); }
inline float4 operator-(float s, float4 v) { return float4(s - v.x, s - v.y, s - v.z, s - v.w); }
inline float4 operator/(float4 v, float s) { return float4(v.x / s, v.y / s, v.z / s, v.w / s); }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline uint asuint(float f) {
  uint u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
inline float asfloat(uint u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
static const float kAoSky = 60000.0f;  // ao_common.hlsli: what the depth chain holds for the sky; the includer of ao_mip.hlsli defines it
#include "hlsl/ao_mip.hlsli"
#include "hlsl/screen_hiz.hlsli"
}  // namespace hlsl_shim

namespace ex = mcla::gpu::screen::extract;
namespace sc = mcla::gpu::screen;
using mcla::gpu::AoPush;
using hlsl_shim::float4;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_print = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Slurp(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream text;
  text << in.rdbuf();
  *out = text.str();
  return true;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
size_t At(const std::string& text, const std::string& needle, size_t from = 0) { return text.find(needle, from); }
size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint32_t Next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return uint32_t(s >> 32);
  }
  float Unit() { return float(Next() >> 8) / float(1u << 24); }
  float Range(float lo, float hi) { return lo + (hi - lo) * Unit(); }
  bool Chance(float p) { return Unit() < p; }
};

uint32_t BitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}
float FloatOf(uint32_t u) {
  float f;
  std::memcpy(&f, &u, sizeof(f));
  return f;
}
// What an R16_FLOAT render target keeps of a float: the mantissa rounded to 10 bits to nearest even (a carry into the exponent is right), the halves below 2^-14 multiples of 2^-24. The values the
// depth chain's means hold are at most kAoSky = 60000, inside the half range (65504); the twin only needs both ways to store the same.
float ToHalf(float f) {
  const float a = std::fabs(f);
  if (a < 6.103515625e-05f) {
    return std::copysign(std::nearbyint(a * 16777216.0f) / 16777216.0f, f);
  }
  uint32_t u = BitsOf(a);
  u = (u + 0xFFFu + ((u >> 13) & 1u)) & ~0x1FFFu;
  float r = FloatOf(u);
  if (r > 65504.0f) r = INFINITY;
  return std::copysign(r, f);
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------
struct Image {
  uint32_t w = 0, h = 0;
  std::vector<float> v;
  Image() = default;
  Image(uint32_t w_, uint32_t h_) : w(w_), h(h_), v(size_t(w_) * h_, 0.0f) {}
  float& At(uint32_t x, uint32_t y) { return v[size_t(y) * w + x]; }
  float At(uint32_t x, uint32_t y) const { return v[size_t(y) * w + x]; }
  // A load as the passes do it: the coordinate clamped inside the image (the odd edge is read twice).
  float Load(int32_t x, int32_t y) const { return At(uint32_t(std::min<int32_t>(x, int32_t(w) - 1)), uint32_t(std::min<int32_t>(y, int32_t(h) - 1))); }
};
bool SameBits(const Image& a, const Image& b) {
  if (a.w != b.w || a.h != b.h) return false;
  for (size_t i = 0; i < a.v.size(); ++i) {
    if (BitsOf(a.v[i]) != BitsOf(b.v[i])) return false;
  }
  return true;
}
uint32_t Half(uint32_t n) { return (n + 1) / 2; }

// ---------------------------------------------------------------------------
// the AO's own statements the buffers are made with (hlsl/ao_common.hlsli, ao_gbuffer_normal.hlsli): the linearisation of the reversed-Z depth target and the two bit fields of the low bits
// ---------------------------------------------------------------------------
struct Lens {
  float x, y, z, w;  // push.lin: t = (z - ndc * w) / (x - ndc * y), ndc = 1 - saturate(stored)
};
float AoLinearDepth(float stored, const Lens& lin) {
  const float ndc = 1.0f - std::min(std::max(stored, 0.0f), 1.0f);
  const float num = lin.z - ndc * lin.w;
  const float den = lin.x - ndc * lin.y;
  const float t = num / den;
  return (stored > 0.0f && den != 0.0f && t > 0.0f && t < 60000.0f) ? t : 60000.0f;
}
float AoSetVehicleBit(float depth, bool vehicle) { return FloatOf((BitsOf(depth) & ~1u) | (vehicle ? 1u : 0u)); }
float AoGbSetSource(float depth, uint32_t index) { return FloatOf((BitsOf(depth) & ~6u) | ((index & 3u) << 1u)); }

// A perspective projection: near n, far f, the linear depth of a stored (reversed-Z: 1 at the near plane, 0 at the far) value. t = n f / (f - ndc (f - n)).
Lens PerspectiveLens(float n, float f) { return Lens{f, f - n, n * f, 0.0f}; }

// The depth chain's level 0 for a w x h depth target's pixels: the values the depth pass writes (the half size block's choice is not touched by the fusion; what the Hi-Z sees is these values).
// `style` 0: a scene (a ground plane at a grazing angle, a wall, boxes, a sky), 1: values at the edges of the reversed-Z range, 2: uniformly random stored values.
Image MakeLevel0(uint32_t w, uint32_t h, int style, Rng& rng, bool source_bits) {
  Image img(w, h);
  const Lens lens = PerspectiveLens(rng.Range(0.1f, 1.0f), rng.Range(2000.0f, 60000.0f));
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      float stored;
      if (style == 0) {
        const float horizon = 0.45f * float(h);
        if (float(y) < horizon) {
          stored = 0.0f;  // the sky
        } else {
          const float k = (float(y) - horizon + 0.5f) / (float(h) - horizon + 0.5f);
          stored = 1.0f - 0.9999f * (1.0f - k);  // the ground: the nearer the lower, a long grazing slope
          if (x * 3 > w && x * 3 < w * 2 && y * 4 > h * 2) stored = 0.95f + 0.04f * rng.Unit();  // a box near the camera
          if (rng.Chance(0.01f)) stored = rng.Chance(0.5f) ? 1.0f : 0.0f;
        }
      } else if (style == 1) {
        switch (rng.Next() % 8) {
          case 0: stored = 0.0f; break;                     // the far plane (and the sky: AoLinearDepth says 60000)
          case 1: stored = 1.0f; break;                     // the near plane
          case 2: stored = FloatOf(BitsOf(1.0f) - 1u); break;  // one unit below 1
          case 3: stored = FloatOf(1u); break;              // the smallest positive (a denormal)
          case 4: stored = 1e-30f; break;
          case 5: stored = rng.Range(0.0f, 1e-6f); break;
          case 6: stored = 1.0f - rng.Range(0.0f, 1e-6f); break;
          default: stored = rng.Unit(); break;
        }
      } else {
        stored = rng.Unit();
      }
      float t = AoLinearDepth(stored, lens);
      t = AoSetVehicleBit(t, rng.Chance(0.1f));
      if (source_bits) t = AoGbSetSource(t, rng.Next() & 3u);
      img.At(x, y) = t;
    }
  }
  return img;
}

// Raw random finite floats with random low bits (and the sky), the harshest input for the strip and the min.
Image MakeRawLevel0(uint32_t w, uint32_t h, Rng& rng) {
  Image img(w, h);
  for (float& f : img.v) {
    // non-negative (a distance), every mantissa bit random, the exponent from 2^-63 to 2^33
    const uint32_t exponent = 64u + (rng.Next() % 160u);
    f = rng.Chance(0.05f) ? 60000.0f : FloatOf((exponent << 23) | (rng.Next() & 0x7FFFFFu));
  }
  return img;
}

// ---------------------------------------------------------------------------
// the OLD WAY: the AO's coarser level passes (ao_mip_ps.hlsl as it was before SS-3, the statements copied verbatim) and the Hi-Z's own passes (hlsl/screen_hiz_ps.hlsl)
// ---------------------------------------------------------------------------
// The body of ao_mip_ps.hlsl's main as it stood at 244ddd76 (the line of `range` is the pass's own; `lens.z * lens.w` is the push block's):
float OldMipTexel(const Image& below, int32_t px, int32_t py, float lens_zw) {
  using namespace hlsl_shim;
  float4 d;
  d.x = below.Load(px * 2 + 0, py * 2 + 0);
  d.y = below.Load(px * 2 + 1, py * 2 + 0);
  d.z = below.Load(px * 2 + 0, py * 2 + 1);
  d.w = below.Load(px * 2 + 1, py * 2 + 1);
  const float far_d = max(max(d.x, d.y), max(d.z, d.w));
  const float range = max(0.75f * lens_zw, 0.01f);
  const float4 w = saturate(1.0f - (far_d - d) / range);
  const float sum = dot(w, d);
  const float weights = w.x + w.y + w.z + w.w;
  return min(sum / max(weights, 1e-6f), kAoSky);
}

// The Hi-Z's own pass for one texel (screen_hiz_ps.hlsl main): mode 0 the depth chain's level 0 with the marks off, mode 1 the min of the four texels of the level below.
float OldHizTexel(const Image& source, bool level0, int32_t px, int32_t py) {
  if (level0) return hlsl_shim::ScreenHizStrip(source.Load(px, py));
  float4 d;
  d.x = source.Load(px * 2 + 0, py * 2 + 0);
  d.y = source.Load(px * 2 + 1, py * 2 + 0);
  d.z = source.Load(px * 2 + 0, py * 2 + 1);
  d.w = source.Load(px * 2 + 1, py * 2 + 1);
  return hlsl_shim::ScreenHizMin4(d);
}

struct Chain {
  Image depth[3];                       // the depth chain: level 0 (R32_FLOAT, marks on), 1 and 2 (R16_FLOAT)
  std::vector<Image> hiz;               // the Hi-Z: HizLevelCount levels (R32_FLOAT)
};

Chain OldChain(const Image& level0, float lens_zw, bool levels) {
  Chain c;
  c.depth[0] = level0;
  if (levels) {
    for (uint32_t l = 1; l <= 2; ++l) {
      const Image& below = c.depth[l - 1];
      c.depth[l] = Image(Half(below.w), Half(below.h));
      for (uint32_t y = 0; y < c.depth[l].h; ++y) {
        for (uint32_t x = 0; x < c.depth[l].w; ++x) c.depth[l].At(x, y) = ToHalf(OldMipTexel(below, int32_t(x), int32_t(y), lens_zw));
      }
    }
  }
  const uint32_t n = sc::HizLevelCount(level0.w, level0.h);
  for (uint32_t l = 0; l < n; ++l) {
    Image img(sc::HizLevelSize(level0.w, l), sc::HizLevelSize(level0.h, l));
    const Image& source = l == 0 ? c.depth[0] : c.hiz[l - 1];
    for (uint32_t y = 0; y < img.h; ++y) {
      for (uint32_t x = 0; x < img.w; ++x) img.At(x, y) = OldHizTexel(source, l == 0, int32_t(x), int32_t(y));
    }
    c.hiz.push_back(std::move(img));
  }
  return c;
}

// ---------------------------------------------------------------------------
// the FUSED WAY: what each fused draw computes for its two targets (hlsl/ao_depth_ps.hlsl with AO_HIZ0, hlsl/ao_mip_ps.hlsl with AO_HIZ), then the Hi-Z's own pass for the levels above them
// ---------------------------------------------------------------------------
Chain FusedChain(const Image& level0, float lens_zw, bool levels, uint32_t fused) {
  using namespace hlsl_shim;
  Chain c;
  const uint32_t n = sc::HizLevelCount(level0.w, level0.h);
  for (uint32_t l = 0; l < n; ++l) c.hiz.push_back(Image(sc::HizLevelSize(level0.w, l), sc::HizLevelSize(level0.h, l)));
  // draw 1: the depth pass: SV_Target0 the texel as it is, SV_Target1 its marks off
  c.depth[0] = level0;
  for (uint32_t y = 0; y < level0.h; ++y) {
    for (uint32_t x = 0; x < level0.w; ++x) c.hiz[0].At(x, y) = ScreenHizStrip(level0.At(x, y));
  }
  if (levels) {
    for (uint32_t l = 1; l <= 2; ++l) {
      const Image& below = c.depth[l - 1];
      c.depth[l] = Image(Half(below.w), Half(below.h));
      for (uint32_t y = 0; y < c.depth[l].h; ++y) {
        for (uint32_t x = 0; x < c.depth[l].w; ++x) {
          const int32_t px = int32_t(x), py = int32_t(y);
          float4 d;
          d.x = below.Load(px * 2 + 0, py * 2 + 0);
          d.y = below.Load(px * 2 + 1, py * 2 + 0);
          d.z = below.Load(px * 2 + 0, py * 2 + 1);
          d.w = below.Load(px * 2 + 1, py * 2 + 1);
          const float range = max(0.75f * lens_zw, 0.01f);
          const float mean = AoMipMean4(d, range);
          c.depth[l].At(x, y) = ToHalf(mean);
          if (l < fused) {
            // the Hi-Z's level of the same size: slots.w == slots.z (level 1) takes the four texels the mean took, else (level 2) four texels of the Hi-Z's level 1
            float4 h = d;
            if (l == 2) {
              const Image& hb = c.hiz[1];
              h.x = hb.Load(px * 2 + 0, py * 2 + 0);
              h.y = hb.Load(px * 2 + 1, py * 2 + 0);
              h.z = hb.Load(px * 2 + 0, py * 2 + 1);
              h.w = hb.Load(px * 2 + 1, py * 2 + 1);
            }
            c.hiz[l].At(x, y) = ScreenHizMin4Stripped(h);
          }
        }
      }
    }
  }
  // the Hi-Z's own pass for the levels the depth passes did not write
  for (uint32_t l = fused; l < n; ++l) {
    const Image& source = c.hiz[l - 1];
    for (uint32_t y = 0; y < c.hiz[l].h; ++y) {
      for (uint32_t x = 0; x < c.hiz[l].w; ++x) c.hiz[l].At(x, y) = OldHizTexel(source, false, int32_t(x), int32_t(y));
    }
  }
  return c;
}

ex::Frame FrameFor(uint32_t w, uint32_t h, bool levels) {
  ex::Frame f;
  f.traced[0] = w;
  f.traced[1] = h;
  f.level[0][0] = Half(w);
  f.level[0][1] = Half(h);
  f.level[1][0] = (w + 3) / 4;
  f.level[1][1] = (h + 3) / 4;
  f.render[0] = w * 2;
  f.render[1] = h * 2;
  f.levels = levels;
  return f;
}

// One buffer through both ways: every level of both chains the same bits.
void TwinOne(const Image& level0, float lens_zw, bool levels, const std::string& what, int* compared_levels) {
  const ex::Frame frame = FrameFor(level0.w, level0.h, levels);
  const uint32_t n = sc::HizLevelCount(level0.w, level0.h);
  const uint32_t fused = ex::FusedHizLevels(frame, n);
  const Chain old_chain = OldChain(level0, lens_zw, levels);
  const Chain new_chain = FusedChain(level0, lens_zw, levels, fused);
  bool depth_same = true;
  for (uint32_t l = 0; l < (levels ? 3u : 1u); ++l) depth_same = depth_same && SameBits(old_chain.depth[l], new_chain.depth[l]);
  bool hiz_same = old_chain.hiz.size() == new_chain.hiz.size();
  for (size_t l = 0; hiz_same && l < old_chain.hiz.size(); ++l) hiz_same = SameBits(old_chain.hiz[l], new_chain.hiz[l]);
  Check(depth_same, "the depth chain's images are the old passes' bits: " + what);
  Check(hiz_same, "the Hi-Z's levels are the separate passes' bits: " + what);
  *compared_levels += int(old_chain.hiz.size());
}

void TwinChecks() {
  Rng rng;
  const uint32_t sizes[][2] = {{1, 1},  {2, 1},  {1, 2},   {2, 2},   {3, 3},   {4, 4},   {5, 7},    {8, 8},    {16, 9},   {17, 33},  {32, 32},  {33, 17},
                               {64, 36}, {65, 37}, {100, 57}, {129, 65}, {160, 90}, {257, 129}, {480, 270}, {481, 271}};
  int compared = 0;
  int buffers = 0;
  for (const auto& size : sizes) {
    for (int style = 0; style < 3; ++style) {
      for (int rep = 0; rep < 2; ++rep) {
        const bool levels = ((rep + style) & 1) == 0 || size[0] * size[1] < 100;
        const bool source_bits = rep == 1;
        const Image level0 = MakeLevel0(size[0], size[1], style, rng, source_bits);
        const float lens_zw = rng.Range(0.1f, 3.0f) * rng.Range(0.1f, 3.0f);  // push.lens.z * push.lens.w
        char what[160];
        std::snprintf(what, sizeof(what), "%ux%u, style %d, %s, source bits %d", size[0], size[1], style, levels ? "coarser levels" : "no coarser levels", int(source_bits));
        TwinOne(level0, lens_zw, levels, what, &compared);
        ++buffers;
      }
    }
    // raw finite floats with random low bits, with the coarser levels and without
    for (int levels = 0; levels < 2; ++levels) {
      char what[160];
      std::snprintf(what, sizeof(what), "%ux%u, raw random floats, %s", size[0], size[1], levels ? "coarser levels" : "no coarser levels");
      TwinOne(MakeRawLevel0(size[0], size[1], rng), rng.Range(0.1f, 3.0f), levels != 0, what, &compared);
      ++buffers;
    }
  }
  if (g_print) std::printf("twin: %d buffers, %d Hi-Z levels compared bit for bit with the separate passes\n", buffers, compared);

  // The mean cannot stand for the Hi-Z's level 2: its four texels are the Hi-Z's level 1, not the depth chain's (a mean over an edge hides the surface a ray hits). On a buffer with an edge the two differ.
  {
    Rng edge_rng;
    Image level0(16, 16);
    for (uint32_t y = 0; y < 16; ++y) {
      for (uint32_t x = 0; x < 16; ++x) level0.At(x, y) = x < 8 ? 3.0f : 400.0f + float(y);
    }
    const Chain c = OldChain(level0, 1.0f, true);
    int differing = 0;
    for (uint32_t y = 0; y < c.hiz[2].h; ++y) {
      for (uint32_t x = 0; x < c.hiz[2].w; ++x) differing += BitsOf(c.hiz[2].At(x, y)) != BitsOf(c.depth[2].At(x, y)) ? 1 : 0;
    }
    Check(differing > 0, "the depth chain's level 2 (a mean) is not the Hi-Z's level 2 (a min): the fused level 2 draw reads the Hi-Z's level 1");
    (void)edge_rng;
  }

  // The strip: ScreenHizMin4Stripped of four marked texels is the min of the four stripped; the min of marked texels, stripped, is the same (the strip is monotone).
  {
    Rng r;
    bool order_same = true;
    bool idempotent = true;
    for (int i = 0; i < 100000; ++i) {
      float v[4];
      for (float& x : v) {
        do {
          x = FloatOf(r.Next() & 0x7FFFFFFFu);
        } while (!std::isfinite(x));
      }
      const float a = hlsl_shim::ScreenHizMin4Stripped(float4(v[0], v[1], v[2], v[3]));
      const float b = hlsl_shim::ScreenHizStrip(hlsl_shim::ScreenHizMin4(float4(v[0], v[1], v[2], v[3])));
      order_same = order_same && BitsOf(a) == BitsOf(b);
      // level 2 takes texels that are stripped already: the strip changes nothing
      const float s[4] = {hlsl_shim::ScreenHizStrip(v[0]), hlsl_shim::ScreenHizStrip(v[1]), hlsl_shim::ScreenHizStrip(v[2]), hlsl_shim::ScreenHizStrip(v[3])};
      idempotent = idempotent && BitsOf(hlsl_shim::ScreenHizMin4Stripped(float4(s[0], s[1], s[2], s[3]))) == BitsOf(hlsl_shim::ScreenHizMin4(float4(s[0], s[1], s[2], s[3])));
    }
    Check(order_same, "the min of the texels with their marks off is the min's marks off (the strip is monotone)");
    Check(idempotent, "the strip leaves a Hi-Z texel (marks already off) as it is: level 2's loads need no second kind of pass");
  }
  // The shim holds the interface's constants (the twin of the twin).
  Check(hlsl_shim::kScreenDepthMarkMask == sc::kDepthMarkMask, "the .hlsli's mark mask is the interface's");
  // The same function the C++ twin of screen_rules.h has.
  {
    Rng r;
    bool same = true;
    for (int i = 0; i < 20000; ++i) {
      const float f = FloatOf(r.Next() & 0x7FFFFFFFu);
      if (!std::isfinite(f)) continue;
      same = same && BitsOf(hlsl_shim::ScreenHizStrip(f)) == BitsOf(sc::StripMarks(f));
    }
    Check(same, "ScreenHizStrip is screen_rules.h's StripMarks");
  }
}

// ---------------------------------------------------------------------------
// the rules
// ---------------------------------------------------------------------------
void RuleChecks() {
  // The Hi-Z's level sizes are the depth chain's coarser levels' (one draw has one target size): every size from 1 to 4200 on both axes, and the sizes ao_size.h makes.
  bool level_sizes = true;
  for (uint32_t n = 1; n <= 4200; ++n) {
    level_sizes = level_sizes && sc::HizLevelSize(n, 1) == Half(n) && sc::HizLevelSize(n, 2) == (n + 3) / 4 && sc::HizLevelSize(n, 1) == mcla::gpu::ao_size::Half(n);
  }
  Check(level_sizes, "the Hi-Z's levels 1 and 2 have the size of the depth chain's coarser levels at every extent from 1 to 4200");
  {
    bool ao_sizes = true;
    const uint32_t renders[][2] = {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2880}, {1366, 768}, {1601, 901}, {3441, 1440}};
    for (const auto& r : renders) {
      for (int low = 0; low < 2; ++low) {
        mcla::gpu::ao_size::Choice choice;
        choice.low = low != 0;
        const uint32_t render[2] = {r[0], r[1]};
        const mcla::gpu::ao_size::Sizes sz = mcla::gpu::ao_size::SizesOf(render, choice);
        ao_sizes = ao_sizes && sc::HizLevelSize(sz.ao[0], 1) == sz.level0[0] && sc::HizLevelSize(sz.ao[1], 1) == sz.level0[1] && sc::HizLevelSize(sz.ao[0], 2) == sz.level1[0] &&
                   sc::HizLevelSize(sz.ao[1], 2) == sz.level1[1];
      }
    }
    Check(ao_sizes, "ao_size.h's coarser levels have the Hi-Z's level 1 and 2 sizes at every size the renderer runs at");
  }
  // FusedHizLevels: level 0 always, 1 and 2 with the coarser depth levels while the Hi-Z has them; never more than the Hi-Z's own count
  bool rule = true;
  bool never_more = true;
  for (uint32_t w : {1u, 2u, 3u, 4u, 5u, 31u, 32u, 33u, 640u, 641u, 1920u, 3840u}) {
    for (uint32_t h : {1u, 2u, 3u, 4u, 7u, 64u, 360u, 1080u, 2160u}) {
      for (int levels = 0; levels < 2; ++levels) {
        const ex::Frame f = FrameFor(w, h, levels != 0);
        const uint32_t n = sc::HizLevelCount(w, h);
        const uint32_t fused = ex::FusedHizLevels(f, n);
        const uint32_t want = levels ? std::min(n, ex::kHizFusedMaxLevels) : 1u;
        rule = rule && fused == want;
        never_more = never_more && fused <= n && fused <= ex::kHizFusedMaxLevels && fused >= 1;
      }
    }
  }
  Check(rule, "FusedHizLevels: level 0 alone without the coarser depth levels, levels 0 to 2 with them (fewer when the Hi-Z has fewer)");
  Check(never_more, "FusedHizLevels is never above the Hi-Z's levels or three, and at least the depth pass's own");
  {
    ex::Frame f = FrameFor(1920, 1080, true);
    Check(ex::FusedHizLevels(f, 0) == 0, "no Hi-Z, nothing fused");
    f.traced[0] = 0;
    Check(ex::FusedHizLevels(f, 7) == 0, "an empty frame fuses nothing");
    // a frame whose level sizes were not the Hi-Z's would fuse less, not wrongly
    ex::Frame odd = FrameFor(1920, 1080, true);
    odd.level[0][0] += 1;
    Check(ex::FusedHizLevels(odd, 7) == 1, "a coarser level of another size than the Hi-Z's level 1 cannot share a draw with it");
    ex::Frame odd2 = FrameFor(1920, 1080, true);
    odd2.level[1][1] += 1;
    Check(ex::FusedHizLevels(odd2, 7) == 2, "a level 2 of another size than the Hi-Z's level 2 is left to the Hi-Z's own pass");
  }

  // The push blocks: the fused depth draw's is the plain pass's; the fused coarser draw's is the plain pass's with slots.w the Hi-Z's source
  Rng r;
  bool depth_push = true;
  bool mip_push = true;
  bool level1_shares = true;
  for (int i = 0; i < 2000; ++i) {
    AoPush base;
    uint32_t* raw = reinterpret_cast<uint32_t*>(&base);
    for (size_t k = 0; k < sizeof(AoPush) / 4; ++k) raw[k] = r.Next();
    const uint32_t w = 1 + r.Next() % 4000, h = 1 + r.Next() % 2200;
    const ex::Frame f = FrameFor(w, h, true);
    const uint32_t stencil = r.Next() % 30000;
    const bool bits = (r.Next() & 1) != 0;
    const AoPush a = ex::DepthPush(base, f, stencil, bits);
    const AoPush b = ex::DepthHizPush(base, f, stencil, bits);
    depth_push = depth_push && std::memcmp(&a, &b, sizeof(AoPush)) == 0;
    for (uint32_t level = 1; level <= 2; ++level) {
      const uint32_t source = r.Next() % 30000, hiz_source = r.Next() % 30000;
      const AoPush plain = ex::DepthMipPush(a, f, level, source);
      const AoPush fused = ex::DepthMipHizPush(a, f, level, source, hiz_source);
      AoPush expect = plain;
      expect.slots[3] = hiz_source;
      mip_push = mip_push && std::memcmp(&expect, &fused, sizeof(AoPush)) == 0;
      const AoPush one = ex::DepthMipHizPush(a, f, 1, source, source);
      level1_shares = level1_shares && one.slots[3] == one.slots[2];
    }
  }
  Check(depth_push, "the fused depth draw's push block is the plain depth pass's, byte for byte");
  Check(mip_push, "the fused coarser level's push block is the plain pass's with slots.w the Hi-Z's source slot, and nothing else");
  Check(level1_shares, "level 1's Hi-Z source is the mean's own source (slots.w == slots.z: the shader shares its four loads)");

  // the kinds
  Check(uint32_t(ex::Kind::kCount) == ex::kFirstFusedKind + 3, "three fused kinds after the eight of the extraction");
  Check(!ex::IsFused(ex::Kind::kDepth) && !ex::IsFused(ex::Kind::kColourMip) && ex::IsFused(ex::Kind::kDepthHiz) && ex::IsFused(ex::Kind::kDepthMsHiz) && ex::IsFused(ex::Kind::kMipHiz) &&
            !ex::IsFused(ex::Kind::kCount),
        "IsFused names the three");
  Check((ex::kBaseKinds & ex::kFusedKinds) == 0 && (ex::kBaseKinds | ex::kFusedKinds) == ex::kAllKinds && ex::kFusedKinds == (ex::Bit(ex::Kind::kDepthHiz) | ex::Bit(ex::Kind::kDepthMsHiz) | ex::Bit(ex::Kind::kMipHiz)),
        "the base and the fused kinds partition all of them");
  Check(ex::PassOf(ex::Kind::kDepthHiz) == ex::kNoAoPass && ex::PassOf(ex::Kind::kDepthMsHiz) == ex::kNoAoPass && ex::PassOf(ex::Kind::kMipHiz) == ex::kNoAoPass,
        "the fused kinds are no passes of the AO's table");
  Check(ex::PassOf(ex::Kind::kDepth) == 0 && ex::PassOf(ex::Kind::kDepthMs) == 1 && ex::PassOf(ex::Kind::kMip) == 2 && ex::PassOf(ex::Kind::kNormal) == 27 && ex::PassOf(ex::Kind::kColourMip) == 13,
        "the extraction's own kinds keep their passes");
  Check(ex::FusedPassOf(ex::Kind::kDepthHiz) == 0 && ex::FusedPassOf(ex::Kind::kDepthMsHiz) == 1 && ex::FusedPassOf(ex::Kind::kMipHiz) == 2, "the fused kinds' shader numbers");
  // the kinds the AO holds itself off for (KindsFor) never include a fused one: the owner fuses when it can and the AO does not wait for it
  bool no_fused_in_ao = true;
  for (int mask = 0; mask < 64; ++mask) {
    const uint32_t kinds = ex::KindsFor((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0, (mask & 16) != 0, (mask & 32) != 0);
    no_fused_in_ao = no_fused_in_ao && (kinds & ex::kFusedKinds) == 0;
  }
  Check(no_fused_in_ao, "the effects are never held off for a fused pipeline (KindsFor names none)");
}

// ---------------------------------------------------------------------------
// the pass list and the bandwidth model
// ---------------------------------------------------------------------------
struct Totals {
  uint32_t passes = 0;
  uint64_t write = 0, read = 0;
};
Totals Sum(uint32_t w, uint32_t h, bool half, bool levels, bool hiz, bool fused, std::vector<std::string>* list) {
  ex::cost::Pass passes[16];
  const uint32_t n = ex::cost::DepthAndHizPasses(w, h, half, levels, hiz, fused, passes);
  Totals t;
  for (uint32_t i = 0; i < n; ++i) {
    ++t.passes;
    t.write += passes[i].write_bytes;
    t.read += passes[i].read_bytes;
    if (list) list->push_back(passes[i].what);
  }
  return t;
}
double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

void CostChecks() {
  struct Case {
    const char* name;
    uint32_t w, h;  // the depth chain's traced size (the AO's trace: half the picture's above 1080 lines, or at high)
    bool half, levels;
  };
  // 1080p ultra traces the picture's size; 4K ultra traces its half (1080 lines above 1080 lines), high traces half also at 1080p
  const Case cases[] = {
      {"1080p, traced at 1920x1080, occlusion + reflections", 1920, 1080, false, true},
      {"1080p, traced at 960x540 (high), occlusion + reflections", 960, 540, true, true},
      {"4K, traced at 1920x1080 (half of the picture), occlusion + reflections", 1920, 1080, true, true},
      {"4K, traced at 3840x2160 (a trace at the picture's size, 4K), occlusion + reflections", 3840, 2160, false, true},
      {"1080p, traced at 1920x1080, reflections alone (no coarser depth levels)", 1920, 1080, false, false},
      {"4K, traced at 1920x1080, reflections alone (no coarser depth levels)", 1920, 1080, true, false},
  };
  for (const Case& c : cases) {
    std::vector<std::string> sep_list, fused_list;
    const Totals sep = Sum(c.w, c.h, c.half, c.levels, true, false, &sep_list);
    const Totals fus = Sum(c.w, c.h, c.half, c.levels, true, true, &fused_list);
    const Totals none = Sum(c.w, c.h, c.half, c.levels, false, false, nullptr);
    if (g_print) {
      std::printf("%s\n  separate Hi-Z passes : %2u passes, writes %7.2f MiB, reads %7.2f MiB\n  fused Hi-Z           : %2u passes, writes %7.2f MiB, reads %7.2f MiB\n", c.name, sep.passes,
                  Mib(sep.write), Mib(sep.read), fus.passes, Mib(fus.write), Mib(fus.read));
      std::printf("  saved by the fusion  : %2d passes, writes %7.2f MiB, reads %7.2f MiB per frame (the depth chain alone: %u passes, %.2f MiB written, %.2f MiB read)\n", int(sep.passes) - int(fus.passes),
                  Mib(sep.write - fus.write), Mib(sep.read - fus.read), none.passes, Mib(none.write), Mib(none.read));
    }
    const uint32_t saved_passes = c.levels ? 3u : 1u;
    Check(sep.passes - fus.passes == saved_passes, std::string("the fusion records ") + (c.levels ? "three" : "one") + " pass(es) fewer: " + c.name);
    Check(fus.write == sep.write, std::string("the same images are written, so the same bytes: ") + c.name);
    Check(fus.read < sep.read, std::string("the fused passes read less: ") + c.name);
    // what level 0 and level 1 saved: the Hi-Z level 0 pass's read of the depth image, and (with the coarser levels) level 1's four reads per texel
    const uint64_t texels0 = ex::cost::Texels(c.w, c.h);
    const uint64_t texels1 = ex::cost::Texels(Half(c.w), Half(c.h));
    const uint64_t expect = texels0 * 4ull + (c.levels ? texels1 * 4ull * 4ull : 0ull);
    Check(sep.read - fus.read == expect, std::string("the saved reads are the Hi-Z's level 0 copy's and level 1's: ") + c.name);
    Check(fus.passes + saved_passes == sep.passes && sep.passes == none.passes + sc::HizLevelCount(c.w, c.h), std::string("the Hi-Z is its level count of passes on top of the depth chain's: ") + c.name);
    Check(none.passes == (c.levels ? 3u : 1u), std::string("the depth chain alone is its own passes: ") + c.name);
    // the Hi-Z off: nothing differs whichever the setting says
    Check(Sum(c.w, c.h, c.half, c.levels, false, true, nullptr).passes == none.passes && Sum(c.w, c.h, c.half, c.levels, false, true, nullptr).read == none.read,
          std::string("without the Hi-Z the setting changes nothing: ") + c.name);
  }
  // the order of the fused list: the depth chain's passes (carrying the Hi-Z's first levels) first, then the Hi-Z's own for the levels above
  std::vector<std::string> list;
  Sum(1920, 1080, false, true, true, true, &list);
  Check(list.size() == 3 + 4 && list[0] == "depth level 0 + Hi-Z level 0" && list[1] == "depth level 1 + Hi-Z level 1" && list[2] == "depth level 2 + Hi-Z level 2" && list[3] == "Hi-Z level",
        "the fused frame: three draws of two targets, then the Hi-Z's own passes for the four levels above");
}

// ---------------------------------------------------------------------------
// the sources
// ---------------------------------------------------------------------------
bool Before(const std::string& text, const std::string& earlier, const std::string& later) {
  const size_t a = At(text, earlier);
  const size_t b = a == std::string::npos ? a : At(text, later, a + earlier.size());
  return a != std::string::npos && b != std::string::npos;
}

void SourceChecks(const std::string& gpu) {
  std::string depth_ps, mip_ps, mip_h, hiz_h, hiz_ps, cmake, shaders_h, ext_cpp, ext_h, shared_cpp, shared_h, passes_h, passes_cpp, manifest, settings, meta, ao_cpp;
  const std::string settings_dir = gpu + "/../settings/generated";
  if (!Slurp(gpu + "/hlsl/ao_depth_ps.hlsl", &depth_ps) || !Slurp(gpu + "/hlsl/ao_mip_ps.hlsl", &mip_ps) || !Slurp(gpu + "/hlsl/ao_mip.hlsli", &mip_h) || !Slurp(gpu + "/hlsl/screen_hiz.hlsli", &hiz_h) ||
      !Slurp(gpu + "/hlsl/screen_hiz_ps.hlsl", &hiz_ps) || !Slurp(gpu + "/CMakeLists.txt", &cmake) || !Slurp(gpu + "/host_shaders.h", &shaders_h) || !Slurp(gpu + "/screen_extract.cpp", &ext_cpp) ||
      !Slurp(gpu + "/screen_extract.h", &ext_h) || !Slurp(gpu + "/screen_shared.cpp", &shared_cpp) || !Slurp(gpu + "/screen_shared.h", &shared_h) || !Slurp(gpu + "/host_passes.h", &passes_h) ||
      !Slurp(gpu + "/host_passes.cpp", &passes_cpp) || !Slurp(settings_dir + "/manifest.json", &manifest) || !Slurp(settings_dir + "/settings_gpu.h", &settings) ||
      !Slurp(settings_dir + "/settings_meta.inc", &meta) || !Slurp(gpu + "/ao.cpp", &ao_cpp)) {
    Check(false, "the sources could not be read (give the src/gpu folder)");
    return;
  }
  // ---- the fused shaders are the old shaders with a define ----
  Check(Has(depth_ps, "float AoDepthTexel(float2 position_xy) {") &&
            Before(depth_ps, "marked = g_push.slots2.w != 0u ? AoGbSetSource(marked, source_index) : marked;", "return asfloat(CabinMarkDepth(asuint(marked), g_push.slots.w, id));"),
        "ao_depth_ps.hlsl: the shared depth function packs the source index before returning the cockpit-tagged texel");
  Check(Has(depth_ps, "#if AO_HIZ0\n#include \"screen_hiz.hlsli\"\n#endif") && Has(depth_ps, "float4 main(float4 position : SV_Position) : SV_Target {\n  return float4(AoDepthTexel(position.xy), 0.0f, 0.0f, 0.0f);\n}"),
        "ao_depth_ps.hlsl: the plain pass is the function's value in the first target, as it was");
  // CHECK: Both Hi-Z paths strip bits 0–2. The cockpit tag in bit 3 reaches Hi-Z and its in-game effect is unverified.
  Check(Has(depth_ps, "const float stored = AoDepthTexel(position.xy);") && Has(depth_ps, "o.depth = float4(stored, 0.0f, 0.0f, 0.0f);") &&
            Has(depth_ps, "o.hiz = float4(ScreenHizStrip(stored), 0.0f, 0.0f, 0.0f);") && Has(depth_ps, "float4 depth : SV_Target0;") && Has(depth_ps, "float4 hiz : SV_Target1;"),
        "ao_depth_ps.hlsl AO_HIZ0: SV_Target0 is the shared depth, SV_Target1 applies the same strip as the separate Hi-Z pass");
  Check(Has(mip_h, "float AoMipMean4(float4 d, float range) {") && Has(mip_h, "const float far_d = max(max(d.x, d.y), max(d.z, d.w));") &&
            Has(mip_h, "const float4 w = saturate(1.0f - (far_d - d) / range);") && Has(mip_h, "return min(sum / max(weights, 1e-6f), kAoSky);"),
        "ao_mip.hlsli: the mean's statements are the old pass's");
  Check(Has(mip_ps, "#include \"ao_mip.hlsli\"") && Has(mip_ps, "const float mean = AoMipMean4(d, range);") && Has(mip_ps, "return float4(mean, 0.0f, 0.0f, 0.0f);\n#endif") &&
            Has(mip_ps, "o.hiz = float4(ScreenHizMin4Stripped(h), 0.0f, 0.0f, 0.0f);") && Has(mip_ps, "o.mean = float4(mean, 0.0f, 0.0f, 0.0f);") && Has(mip_ps, "if (g_push.slots.w != slot) {"),
        "ao_mip_ps.hlsl: one function for the mean in both passes; AO_HIZ writes it to SV_Target0 and the min of the Hi-Z's texels to SV_Target1");
  Check(Has(hiz_h, "float ScreenHizMin4Stripped(float4 d) {") && Has(hiz_h, "return ScreenHizMin4(float4(ScreenHizStrip(d.x), ScreenHizStrip(d.y), ScreenHizStrip(d.z), ScreenHizStrip(d.w)));"),
        "screen_hiz.hlsli: the fused level's min is the min of the texels with their marks off");
  Check(Has(hiz_ps, "depth = ScreenHizStrip(Load(slot, min(pixel, last)));") && Has(hiz_ps, "depth = ScreenHizMin4(d);"), "screen_hiz_ps.hlsl, the separate pass (the A/B and the levels above), is as it was");
  // ---- registration ----
  Check(Has(cmake, "larecomp_gpu_host_shader(ao_depth_hiz_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=0 AO_HIZ0=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(ao_depth_hiz_ms_ps ps_6_0 SOURCE ao_depth_ps DEFINES AO_DEPTH_MS=1 AO_HIZ0=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(ao_mip_hiz_ps ps_6_0 SOURCE ao_mip_ps DEFINES AO_HIZ=1)") &&
            Has(cmake, "larecomp_gpu_host_shader(ao_depth_ps ps_6_0 DEFINES AO_DEPTH_MS=0)") && Has(cmake, "larecomp_gpu_host_shader(ao_mip_ps ps_6_0)"),
        "CMake: the three fused shaders are the old ones compiled with a define, the plain ones stay");
  Check(Has(cmake, "add_executable(larecomp_screen_hiz_fuse_test EXCLUDE_FROM_ALL screen_hiz_fuse_test.cpp)"), "this test is in the build");
  Check(Count(shaders_h, "ao_depth_hiz_ps.hlsl.") == 2 && Count(shaders_h, "ao_depth_hiz_ms_ps.hlsl.") == 2 && Count(shaders_h, "ao_mip_hiz_ps.hlsl.") == 2 &&
            Has(shaders_h, "case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_hiz_ps);") && Has(shaders_h, "case 1: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_hiz_ms_ps);") &&
            Has(shaders_h, "default: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_hiz_ps);") && Has(shaders_h, "ScreenFusedDepthPixelShader(uint32_t pass)"),
        "host_shaders.h: the fused blobs for both back ends and their accessor (0 plain depth, 1 multisampled depth, 2 the coarser level)");
  Check(Has(shaders_h, "case 0: return MCLA_GPU_HOST_SHADER_BLOB(ao_depth_ps);") && Has(shaders_h, "case 2: return MCLA_GPU_HOST_SHADER_BLOB(ao_mip_ps);"), "the AO's own table is as it was");
  // ---- the host pieces ----
  Check(Has(passes_h, "CreateMrtPipeline(") && Has(passes_h, "CreateMrtFramebuffer(") && Has(passes_h, "uint64_t serial = 0;") && Has(passes_cpp, "image.serial = ++image_serial_;") &&
            Has(passes_cpp, "desc.renderTargetCount = count;"),
        "HostPasses: a pipeline and a framebuffer of several targets, and a serial for every texture it makes");
  Check(Has(ext_cpp, "const plume::RenderFormat formats[2] = {FormatOf(kind), plume::RenderFormat::R32_FLOAT};") && Has(ext_cpp, "passes_->CreateMrtPipeline(*wide_layout_, *chain_shaders_[k], formats, 2, kKindNames[k]);"),
        "the fused pipelines have two targets: the depth chain's format and R32_FLOAT");
  Check(Has(ext_cpp, "chain_failed_ |= screen::extract::Bit(Kind(i));") && Has(ext_cpp, "ScreenFusedDepthPixelShader(screen::extract::FusedPassOf(Kind(i)))"),
        "a fused shader that cannot be made only keeps the owner from fusing (the extraction goes on)");
  Check(Has(ext_cpp, "const uint32_t first_use[2]") || Has(ext_cpp, "const bool first_use[2] = {first.layout == plume::RenderTextureLayout::UNKNOWN, second.layout == plume::RenderTextureLayout::UNKNOWN};"),
        "DrawWideMrt clears a new target of the two before drawing into it");
  Check(Has(ext_cpp, "for (const HostImage& image : hiz_) {\n      if (image.serial == serial) return true;") && Has(ext_cpp, "passes_->RetireFramebuffer(std::move(mrt_[i].framebuffer));"),
        "the two-target framebuffers are kept by the images' serials and retired when an image goes");
  // ---- the recording ----
  Check(Before(ext_cpp, "hiz_made_ = RecordDepthFused(list, frame, level0, mip, stencil_slot, source_bits, multisampled);", "DrawWide(list, depth_[0], depth, level0);"),
        "RecordDepth tries the fused recording first and records the plain passes when it recorded nothing");
  Check(Has(ext_cpp, "if (hiz_made_ == 0) {\n    DrawWide(list, depth_[0], depth, level0);"), "the plain passes are the fallback of the fused recording (the setting off, a pipeline not made yet, an image or a framebuffer that could not be made)");
  Check(Before(ext_cpp, "if (!hiz_fused_ || (plan_.make & screen::ProductBit(screen::kHizMin)) == 0) {", "const uint32_t fused = screen::extract::FusedHizLevels(frame, hiz_levels);") &&
            Before(ext_cpp, "if (!ChainReady(screen::extract::Bit(depth_kind)", "plume::RenderPipeline* tail = HizPipeline();") &&
            Before(ext_cpp, "DrawWideMrt(list, depth_[0], hiz_[0], depth_hiz,", "DrawWideMrt(list, depth_[1], hiz_[1], mip_hiz, mip1)") &&
            Before(ext_cpp, "DrawWideMrt(list, depth_[1], hiz_[1], mip_hiz, mip1)", "DrawWideMrt(list, depth_[2], hiz_[2], mip_hiz, mip2)") && Before(ext_cpp, "DrawWideMrt(list, depth_[2], hiz_[2], mip_hiz, mip2)", "RecordHizPasses(list, nullptr, made, hiz_levels, tail);"),
        "the fused draws run in the order of the chain (level 0, 1, 2), then the Hi-Z's own passes for the levels above, and only while the Hi-Z is wanted and the pipelines are made");
  Check(Has(ext_cpp, "DepthMipHizPush(level0, frame, 1, depth_[0].slot, depth_[0].slot)") && Has(ext_cpp, "DepthMipHizPush(level0, frame, 2, depth_[1].slot, hiz_[1].slot)"),
        "the fused coarser draws read the depth chain's level 0 for both (level 1) and the Hi-Z's level 1 for the Hi-Z (level 2)");
  Check(Has(ext_cpp, "pass_timing_->Mark(list, \"screen hiz\");\n    RecordHizPasses(list, nullptr, made, hiz_levels, tail);"), "the Hi-Z's own passes keep their label");
  Check(Has(shared_cpp, "if (hiz_made_ != 0 && hiz_made_ <= screen::kHizMaxLevels && hiz_images_ == hiz_made_) {") && Has(shared_cpp, "in.hiz[level] = hiz_[level].slot;") &&
            Has(shared_cpp, "in.hiz_levels = RecordHiz(list, *depth0, in.hiz);") && Has(shared_cpp, "hiz_made_ = 0;"),
        "Publish only publishes the Hi-Z the depth step made this frame and records the separate passes for any other frame (the old path stays)");
  Check(Has(shared_cpp, "hiz_fused_ = larecomp::settings::mcla_gpu_screen_hiz_fused.Get();"), "the setting is read at the frame's start (live)");
  Check(Has(shared_cpp, "bool ScreenShared::EnsureHizImages(") && Has(shared_cpp, "if (!EnsureHizImages(depth0.width, depth0.height, &levels)) {"), "the images are made one way for both recordings");
  Check(Has(shared_cpp, "void ScreenShared::ReleaseHiz() {\n  PurgeMrt();") && Has(ext_cpp, "void ScreenShared::ReleaseDepth() {\n  if (!passes_) {\n    return;\n  }\n  PurgeMrt();") && Has(ext_cpp, "mrt_.clear();  // the GPU is idle"),
        "the framebuffers go with the images (the Hi-Z's idle release, the depth chain's release) and at shutdown");
  Check(Has(ao_cpp, "screen_->RecordDepth(list, chain, base, summary.stencil_slot, normals_.depth_source_bits, ms);") && !Has(ao_cpp, "RecordDepthFused"),
        "the AO still calls one function for the depth chain and knows nothing of the fusion");
  // ---- the setting ----
  Check(Has(manifest, "\"mcla_gpu_screen_hiz_fused\": {\n  \"type\": \"bool\",\n  \"default\": \"true\",") && Has(settings, "(bool, mcla_gpu_screen_hiz_fused, true,") &&
            Has(settings, "inline auto& mcla_gpu_screen_hiz_fused = g_gpu.mcla_gpu_screen_hiz_fused;") && Has(meta, "{\"mcla_gpu_screen_hiz_fused\", \"MCLA/GPU\"},"),
        "the A/B switch is a bool, on by default, defined where the others are");
  {
    const size_t at = At(settings, "(bool, mcla_gpu_screen_hiz_fused, true,");
    const size_t end = at == std::string::npos ? at : At(settings, ");", at);
    Check(at != std::string::npos && end != std::string::npos && Has(settings.substr(at, end - at), "rex::kDebugOnly") && !Has(settings.substr(at, end - at), "kRestart"),
          "the switch is debug only and live (no player row, no restart)");
  }
  Check(Has(shared_h, "bool hiz_fused() const { return hiz_fused_; }"), "the owner says whether the Hi-Z is fused");
  Check(Has(ext_h, "inline constexpr uint32_t kHizFusedMaxLevels = 3;") && Has(ext_h, "kDepthHiz,") && Has(ext_h, "kDepthMsHiz,") && Has(ext_h, "kMipHiz,"), "screen_extract.h names the fused kinds and the three levels");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--print") {
      g_print = true;
    } else {
      gpu = arg;
    }
  }
  TwinChecks();
  RuleChecks();
  CostChecks();
  if (!gpu.empty()) {
    SourceChecks(gpu);
  } else {
    std::printf("(no src/gpu folder given: the source checks were left out)\n");
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
