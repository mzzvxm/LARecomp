// Check of the car dump prototype's rules (car_dump_rules.h): the matrices out of a vertex shader bank, the recipe of a draw from what its
// vertex shader reads (the microcode walk, on microcode assembled here: no game shader is in this file), the wheel proxies and the
// skinned vertices, the choice of the player's car, the vertex and index bytes as the guest holds them, the triangle list, the
// rasteriser and the statistics that compare it with the car history image. A program
// of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_dump_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_dump_rules_test

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "car_dump_rules.h"

namespace cd = mcla::gpu::car_dump;
namespace tp = mcla::gpu::temporal;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

bool Near(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

// ---------------------------------------------------------------------------
// a hand-made vertex shader bank
// ---------------------------------------------------------------------------

struct Bank {
  uint8_t bytes[cd::kRegisterCount * 16] = {};

  void Set(uint32_t reg, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    for (int c = 0; c < 4; ++c) {
      uint32_t bits;
      std::memcpy(&bits, &v[c], 4);
      uint8_t* p = bytes + size_t(reg) * 16 + size_t(c) * 4;
      p[0] = uint8_t(bits >> 24);
      p[1] = uint8_t(bits >> 16);
      p[2] = uint8_t(bits >> 8);
      p[3] = uint8_t(bits);
    }
  }
  void SetMatrix(uint32_t first, const tp::Mat4& m) {
    for (uint32_t r = 0; r < 4; ++r) {
      Set(first + r, float(m.m[r][0]), float(m.m[r][1]), float(m.m[r][2]), float(m.m[r][3]));
    }
  }
};

tp::Mat4 Perspective(double fov_y, double aspect, double n, double f) {
  tp::Mat4 p;
  const double h = 1.0 / std::tan(fov_y * 0.5);
  p.m[0][0] = h / aspect;
  p.m[1][1] = h;
  p.m[2][2] = f / (n - f);
  p.m[2][3] = -1.0;
  p.m[3][2] = n * f / (n - f);
  return p;
}

tp::Mat4 Translation(double x, double y, double z) {
  tp::Mat4 t = tp::Identity();
  t.m[3][0] = x;
  t.m[3][1] = y;
  t.m[3][2] = z;
  return t;
}

// ---------------------------------------------------------------------------

void TestBank() {
  // The register bytes are big-endian: 1.0f is 3F 80 00 00.
  Bank bank;
  bank.Set(5, 1.0f, -2.5f, 0.0f, 1e6f);
  Check(bank.bytes[5 * 16] == 0x3F && bank.bytes[5 * 16 + 1] == 0x80, "the test bank holds a big-endian float");
  Check(cd::BankFloat(bank.bytes, 5, 0) == 1.0f && cd::BankFloat(bank.bytes, 5, 1) == -2.5f &&
            cd::BankFloat(bank.bytes, 5, 2) == 0.0f && cd::BankFloat(bank.bytes, 5, 3) == 1e6f,
        "BankFloat reads the four components of a register");
  Check(cd::BankFloat(bank.bytes, 6, 0) == 0.0f, "an unset register reads zero");

  // gWorld: a quarter turn about y (+x to -z, +z to +x) and a translation; the palette's bone 2: a scale of 2 and an offset of 0.5
  // along x; bone 0 holds something else, so that a wrong bone shows.
  tp::Mat4 world = tp::Identity();
  world.m[0][0] = 0.0;
  world.m[0][2] = -1.0;
  world.m[2][0] = 1.0;
  world.m[2][2] = 0.0;
  world.m[3][0] = 10.0;
  world.m[3][1] = 2.0;
  world.m[3][2] = -5.0;
  bank.SetMatrix(cd::kWorldRegister, world);
  bank.Set(cd::kPaletteRegister + 0, 0, 0, 1, 9);  // bone 0, not this draw's
  bank.Set(cd::kPaletteRegister + 1, 1, 0, 0, 0);
  bank.Set(cd::kPaletteRegister + 2, 0, 1, 0, 0);
  bank.Set(cd::kPaletteRegister + 6, 2, 0, 0, 0.5f);  // bone 2
  bank.Set(cd::kPaletteRegister + 7, 0, 2, 0, 0);
  bank.Set(cd::kPaletteRegister + 8, 0, 0, 2, 0);
  bank.Set(cd::kBoneIndexRegister, 2.75f, 0.0f, 0.0f, 0.0f);  // trunc(2.75) = 2

  // The camera: at z = +20 looking down -z, a 60 degree lens.
  const tp::Mat4 view = Translation(0, 0, -20);
  const tp::Mat4 vp = tp::Mul(view, Perspective(1.0471975511965976, 1.0, 1.0, 1000.0));
  bank.SetMatrix(cd::kWvpRegister, tp::Mul(world, vp));

  const cd::CarMatrices m = cd::DecodeCarMatrices(bank.bytes);
  Check(m.finite, "the hand-made bank decodes to finite matrices");
  Check(m.bone_valid && m.bone == 2 && m.bone_raw == 2.75f, "the bone is trunc(c204.x) = 2");
  Check(Near(m.world.m[3][0], 10.0) && Near(m.world.m[0][2], -1.0), "gWorld is c0-c3 as rows");

  // The point (1, 2, 3): local = (2 * 1 + 0.5, 2 * 2, 2 * 3) = (2.5, 4, 6); world = 2.5 c0 + 4 c1 + 6 c2 + c3 =
  // (0, 0, -2.5) + (0, 4, 0) + (6, 0, 0) + (10, 2, -5) = (16, 6, -7.5).
  const double p[3] = {1.0, 2.0, 3.0};
  double w[4];
  cd::TransformPoint(m.model, p, w);
  Check(Near(w[0], 16.0) && Near(w[1], 6.0) && Near(w[2], -7.5) && Near(w[3], 1.0), "model to world is the palette's bone, then gWorld");
  // gWorld alone (the tyres): (1, 2, 3) -> 1 c0 + 2 c1 + 3 c2 + c3 = (0,0,-1) + (0,2,0) + (3,0,0) + (10,2,-5) = (13, 4, -6).
  cd::TransformPoint(m.world, p, w);
  Check(Near(w[0], 13.0) && Near(w[1], 4.0) && Near(w[2], -6.0), "gWorld alone moves the point as the tyres' shader does");
  // The wrong bone gives another point (the registers of bone 0 hold a swap and an offset of 9).
  const tp::Mat4 bone0 = cd::BoneMatrix(bank.bytes, 0);
  double l[4];
  cd::TransformPoint(bone0, p, l);
  Check(Near(l[0], 3.0 + 9.0) && Near(l[1], 1.0) && Near(l[2], 2.0), "bone 0's rows apply as a 3x4 on a column vector (the shader's dots)");

  // The clip position through the model matrix and the camera is the one the vertex shader gets through gWorldViewProj (c8-c11) from
  // the local position.
  const tp::Mat4 model_clip = tp::Mul(m.model, vp);
  const cd::Vec4 via_model = cd::Project(model_clip, p);
  double local[4];
  cd::TransformPoint(m.palette, p, local);
  double via_wvp[4];
  cd::TransformPoint(m.wvp, local, via_wvp);
  Check(Near(via_model.x, via_wvp[0], 1e-5) && Near(via_model.y, via_wvp[1], 1e-5) && Near(via_model.z, via_wvp[2], 1e-5) &&
            Near(via_model.w, via_wvp[3], 1e-5),
        "palette, gWorld and the camera agree with palette and gWorldViewProj");
  // w is the view distance: the world point (16, 6, -7.5) is 20 - (-7.5) = 27.5 in front of a camera at z = 20.
  Check(Near(via_model.w, 27.5, 1e-6), "clip w is the distance along the view axis");

  // gWorld times the camera equals gWorldViewProj; a draw whose bank differs says so.
  Check(cd::WvpError(m.world, vp, m.wvp) < 1e-6, "gWorld * VP equals gWorldViewProj of the same draw (float rounding only)");
  Check(cd::WvpError(m.world, vp, m.wvp) < cd::kWvpTolerance, "... inside the tolerance");
  Bank other = bank;
  other.Set(cd::kWvpRegister + 3, float(m.wvp.m[3][0]) + 5.0f, float(m.wvp.m[3][1]), float(m.wvp.m[3][2]), float(m.wvp.m[3][3]));
  const cd::CarMatrices bad = cd::DecodeCarMatrices(other.bytes);
  Check(cd::WvpError(bad.world, vp, bad.wvp) > cd::kWvpTolerance, "a gWorldViewProj that is another matrix is outside the tolerance");
  // The tyres' case: a world matrix that is not the draw's own matrix fails too (the camera's translation was dropped).
  Check(cd::WvpError(tp::Identity(), vp, m.wvp) > cd::kWvpTolerance, "an identity gWorld does not make the draw's gWorldViewProj");

  // A bone index outside the palette or no number gives no bone: the model matrix is gWorld alone.
  for (float raw : {64.0f, 1000.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    Bank b = bank;
    b.Set(cd::kBoneIndexRegister, raw, 0, 0, 0);
    const cd::CarMatrices r = cd::DecodeCarMatrices(b.bytes);
    Check(!r.bone_valid && tp::MaxAbsDiff(r.model, r.world) == 0.0, "an invalid bone index (" + std::to_string(raw) + ") leaves gWorld alone");
  }
  {
    Bank b = bank;
    b.Set(cd::kBoneIndexRegister, 63.9f, 0, 0, 0);
    b.Set(cd::kPaletteRegister + 189, 1, 0, 0, 7.0f);  // bone 63's first row: c253
    const cd::CarMatrices r = cd::DecodeCarMatrices(b.bytes);
    Check(r.bone_valid && r.bone == 63 && Near(r.palette.m[3][0], 7.0), "the last bone of the palette is bone 63 (c253-c255)");
  }
  {
    Bank b = bank;
    b.Set(cd::kPaletteRegister + 6, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0);
    Check(!cd::DecodeCarMatrices(b.bytes).finite, "a bank with a NaN in the bone's rows is not finite");
  }
}

void TestVertexBytes() {
  // Half floats.
  Check(cd::HalfToFloat(0x3C00) == 1.0f && cd::HalfToFloat(0xC000) == -2.0f && cd::HalfToFloat(0x3800) == 0.5f &&
            cd::HalfToFloat(0x0000) == 0.0f && cd::HalfToFloat(0xFBFF) == -65504.0f,
        "half floats: 1, -2, 0.5, 0, -65504");
  Check(std::isinf(cd::HalfToFloat(0x7C00)) && std::isinf(cd::HalfToFloat(0xFC00)) && std::isnan(cd::HalfToFloat(0x7E00)), "half infinities and NaN");
  Check(cd::HalfToFloat(0x0001) == std::ldexp(1.0f, -24), "the smallest half subnormal is 2^-24");
  Check(std::fabs(cd::HalfToFloat(0x3555) - 0.333251953125f) < 1e-9f, "a half with a mantissa: 0x3555");

  // The packed declaration types of a position.
  Check(cd::PositionFormatOf(0x002A23B9) == cd::PositionFormat::kFloat3 && cd::PositionBytes(cd::PositionFormat::kFloat3) == 12, "float3 decltype");
  Check(cd::PositionFormatOf(0x001A23A6) == cd::PositionFormat::kFloat4 && cd::PositionBytes(cd::PositionFormat::kFloat4) == 16, "float4 decltype");
  Check(cd::PositionFormatOf(0x001A2360) == cd::PositionFormat::kHalf4 && cd::PositionBytes(cd::PositionFormat::kHalf4) == 8, "half4 decltype");
  Check(cd::PositionFormatOf(0x001A2187) == cd::PositionFormat::kNone, "a packed normal is no position");

  // A float3 position as the guest holds it (big-endian): 1.0, 2.0, -3.5.
  const uint8_t f3[12] = {0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0, 0x60, 0x00, 0x00};
  float out[3];
  cd::DecodePosition(f3, cd::PositionFormat::kFloat3, out);
  Check(out[0] == 1.0f && out[1] == 2.0f && out[2] == -3.5f, "a big-endian float3 position");
  cd::DecodePosition(f3, cd::PositionFormat::kFloat2, out);
  Check(out[0] == 1.0f && out[1] == 2.0f && out[2] == 0.0f, "a float2 position has z = 0");
  const uint8_t h4[8] = {0x3C, 0x00, 0xC0, 0x00, 0x38, 0x00, 0x7C, 0x00};  // 1, -2, 0.5, inf (w, not read)
  cd::DecodePosition(h4, cd::PositionFormat::kHalf4, out);
  Check(out[0] == 1.0f && out[1] == -2.0f && out[2] == 0.5f, "a big-endian half4 position (x y z, w unread)");

  // Index bytes under the endian swaps: the guest's 16-bit indices 0x1234, 0xABCD stored big-endian.
  const uint8_t raw16[4] = {0x12, 0x34, 0xAB, 0xCD};
  std::vector<uint32_t> idx;
  cd::DecodeIndices(raw16, 2, false, cd::kEndian8in16, &idx);
  Check(idx.size() == 2 && idx[0] == 0x1234 && idx[1] == 0xABCD, "16-bit indices, 8in16: each byte pair swapped");
  cd::DecodeIndices(raw16, 2, false, cd::kEndian8in32, &idx);
  Check(idx[0] == 0xABCD && idx[1] == 0x1234, "16-bit indices, 8in32: the dword reversed (the pair swaps places)");
  cd::DecodeIndices(raw16, 2, false, cd::kEndian16in32, &idx);
  Check(idx[0] == 0xCDAB && idx[1] == 0x3412, "16-bit indices, 16in32: the halves swap places");
  cd::DecodeIndices(raw16, 2, false, cd::kEndianNone, &idx);
  Check(idx[0] == 0x3412 && idx[1] == 0xCDAB, "16-bit indices, no swap: as little-endian");
  const uint8_t raw32[8] = {0x00, 0x00, 0x01, 0x02, 0x00, 0x01, 0x00, 0x00};
  cd::DecodeIndices(raw32, 2, true, cd::kEndian8in32, &idx);
  Check(idx[0] == 0x102 && idx[1] == 0x10000, "32-bit indices, 8in32");
}

void TestTriangleSet() {
  const uint32_t list[] = {0, 1, 2, 3, 3, 4, 0, 1, 9, 1, 2, 4};
  cd::TriangleSet set;
  cd::CollectTriangles(list, 12, 0, 5, &set);
  Check(set.triangles() == 2 && set.degenerate == 1 && set.out_of_range == 1, "two kept, one degenerate, one out of range");
  Check(set.first_vertex == 0 && set.last_vertex == 4 && set.span() == 5, "the kept triangles' vertex span");
  Check(set.tri[0] == 0 && set.tri[1] == 1 && set.tri[2] == 2 && set.tri[3] == 1 && set.tri[4] == 2 && set.tri[5] == 4, "the kept triangles in order");

  cd::TriangleSet based;
  const uint32_t strip[] = {1, 2, 3};
  cd::CollectTriangles(strip, 3, -1, 5, &based);
  Check(based.triangles() == 1 && based.tri[0] == 0 && based.tri[1] == 1 && based.tri[2] == 2, "a negative base vertex (an indexed draw's) shifts the vertices");
  cd::TriangleSet below;
  const uint32_t low[] = {0, 1, 2};
  cd::CollectTriangles(low, 3, -1, 5, &below);
  Check(below.triangles() == 0 && below.out_of_range == 1, "a vertex below zero is out of range");
  cd::TriangleSet partial;
  cd::CollectTriangles(list, 5, 0, 5, &partial);
  Check(partial.triangles() == 1, "a list that is not a multiple of three keeps its whole triangles only");
  Check(cd::TriangleSet{}.span() == 0, "no triangles, no span");
}

// The distance along the view axis at which the pixel (px, py) (in an image of w x h pixels) meets the plane through the three
// world points, for the camera of TestRaster (the view translation (0, 0, -20), a 60 degree lens, aspect 1).
double PlaneDistance(const double a[3], const double b[3], const double c[3], double px, double py, double w, double h) {
  const double tan_half = std::tan(1.0471975511965976 * 0.5);
  // View space = world + (0, 0, -20); the pixel's ray is (ndc_x tan, ndc_y tan, -1) * t.
  const double ndc_x = px / w * 2.0 - 1.0;
  const double ndc_y = 1.0 - py / h * 2.0;
  const double dx = ndc_x * tan_half, dy = ndc_y * tan_half;
  double n[3];
  const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const double v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  n[0] = u[1] * v[2] - u[2] * v[1];
  n[1] = u[2] * v[0] - u[0] * v[2];
  n[2] = u[0] * v[1] - u[1] * v[0];
  // n . (t (dx, dy, -1) - (a + (0, 0, -20))) = 0
  const double az = a[2] - 20.0;
  const double d = n[0] * a[0] + n[1] * a[1] + n[2] * az;
  const double den = n[0] * dx + n[1] * dy - n[2];
  return d / den;  // t is the distance along the view axis (view z = -t)
}

void TestRaster() {
  const tp::Mat4 vp = tp::Mul(Translation(0, 0, -20), Perspective(1.0471975511965976, 1.0, 1.0, 1000.0));
  cd::Image im;
  im.width = 100;
  im.height = 100;
  im.sample = 0.5;

  // A triangle facing the camera at distance 20: every covered pixel has distance 20 and the area is about 150 pixels.
  {
    const double a[3] = {-2, -2, 0}, b[3] = {2, -2, 0}, c[3] = {0, 2, 0};
    cd::Vec4 clip[3] = {cd::Project(vp, a), cd::Project(vp, b), cd::Project(vp, c)};
    Check(Near(clip[0].w, 20.0) && Near(clip[1].w, 20.0), "w of a point 20 in front of the camera is 20");
    cd::DepthGrid grid;
    grid.Reset(100, 100);
    uint64_t count = 0;
    bool all_20 = true;
    cd::RasterizeClip(im, clip, [&](uint32_t x, uint32_t y, double d) {
      ++count;
      all_20 &= Near(d, 20.0, 1e-9);
      grid.Splat(x, y, d);
    });
    Check(count >= 135 && count <= 165, "a triangle of 150 pixels' area covers about 150 sample points (got " + std::to_string(count) + ")");
    Check(all_20, "a plane facing the camera is at one distance");
    // The pixel at the middle of the base-to-apex line is covered, the corner of the image is not.
    Check(grid.Covered(size_t(52) * 100 + 50), "a pixel inside is covered");
    Check(!grid.Covered(0) && !grid.Covered(size_t(99) * 100 + 99), "a corner pixel of the image is not covered");
    // Either winding.
    cd::Vec4 flipped[3] = {clip[0], clip[2], clip[1]};
    uint64_t count2 = 0;
    cd::RasterizeClip(im, flipped, [&](uint32_t, uint32_t, double) { ++count2; });
    Check(count2 == count, "the other winding covers the same pixels");
    Check(!cd::OffImage(clip, im) && !cd::BehindCamera(clip), "the triangle is on the image and in front of the camera");
  }

  // A tilted plane (z = 0.8 x): the distance changes across the triangle and follows the plane, not the screen (perspective
  // correct).
  {
    const double a[3] = {-4, -3, -3.2}, b[3] = {4, -3, 3.2}, c[3] = {0, 3, 0};
    cd::Vec4 clip[3] = {cd::Project(vp, a), cd::Project(vp, b), cd::Project(vp, c)};
    double worst = 0.0;
    double lo = 1e9, hi = 0.0;
    uint64_t count = 0;
    cd::RasterizeClip(im, clip, [&](uint32_t x, uint32_t y, double d) {
      ++count;
      const double expected = PlaneDistance(a, b, c, double(x) + 0.5, double(y) + 0.5, 100.0, 100.0);
      worst = std::max(worst, std::fabs(d - expected) / expected);
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    });
    Check(count > 300, "the tilted triangle covers a good many pixels (got " + std::to_string(count) + ")");
    Check(worst < 1e-9, "the rasterised distance is the plane's along the pixel's ray (perspective correct): worst " + std::to_string(worst));
    Check(lo < 18.5 && hi > 21.5, "the distance spans the tilted plane");
  }

  // The near plane: a vertex behind the camera is clipped, nothing nearer than the clip distance is sent, and a triangle wholly behind is
  // nothing.
  {
    cd::Vec4 in[3] = {{0, 0, 0, 5}, {1, 0, 0, 5}, {0, 1, 0, -5}};
    cd::Vec4 out[4];
    Check(cd::ClipNear(in, out) == 4, "one vertex behind: a quad");
    for (int i = 0; i < 4; ++i) {
      Check(out[i].w >= cd::kNearW * (1.0 - 1e-12), "the clipped polygon stays in front of the near plane");
    }
    cd::Vec4 two[3] = {{0, 0, 0, 5}, {1, 0, 0, -5}, {0, 1, 0, -5}};
    Check(cd::ClipNear(two, out) == 3, "two vertices behind: a triangle");
    cd::Vec4 none[3] = {{0, 0, 0, -5}, {1, 0, 0, -5}, {0, 1, 0, -5}};
    Check(cd::ClipNear(none, out) == 0 && cd::BehindCamera(none), "all behind: nothing");

    // A triangle that crosses the camera's plane: world z from 0 to 40 (the camera at 20).
    const double a[3] = {-1, -1, 0}, b[3] = {1, -1, 40}, c[3] = {0, 1, 0};
    cd::Vec4 clip[3] = {cd::Project(vp, a), cd::Project(vp, b), cd::Project(vp, c)};
    Check(clip[1].w < 0.0, "the far vertex is behind the camera");
    uint64_t count = 0;
    bool ok = true;
    cd::RasterizeClip(im, clip, [&](uint32_t, uint32_t, double d) {
      ++count;
      ok &= d >= cd::kNearW * (1.0 - 1e-9) && std::isfinite(d);
    });
    Check(count > 0 && ok, "a triangle through the camera's plane is clipped and rasterised with finite distances");
  }

  // Off the image: all three vertices outside; a triangle that is bigger than the image has none inside either (counted as off by the
  // vertex rule, as the brief says), but one with a vertex inside is not.
  {
    const double a[3] = {500, 0, 0}, b[3] = {510, 0, 0}, c[3] = {505, 10, 0};
    cd::Vec4 clip[3] = {cd::Project(vp, a), cd::Project(vp, b), cd::Project(vp, c)};
    Check(cd::OffImage(clip, im), "a triangle far to the side is off the image");
    uint64_t count = 0;
    cd::RasterizeClip(im, clip, [&](uint32_t, uint32_t, double) { ++count; });
    Check(count == 0, "... and rasterises nothing");
    const double d[3] = {0, 0, 0}, e[3] = {500, 0, 0}, f[3] = {0, 10, 0};
    cd::Vec4 half[3] = {cd::Project(vp, d), cd::Project(vp, e), cd::Project(vp, f)};
    Check(!cd::OffImage(half, im), "a triangle with a vertex on the image is not off it");
  }

  Check(cd::HistorySample(1) == 0.5 && cd::HistorySample(2) == 0.75, "the history image is read at the pixel centre (divisor 1) or the lower right of a 2x2 block (2)");

  // Sample point and image size: a triangle that covers the upper left half of a 16x16 image, its hypotenuse x + y = 16. The sample
  // points on it count (edges are inclusive).
  {
    cd::Image small;
    small.width = 16;
    small.height = 16;
    small.sample = 0.5;
    // ndc (-1, 1), (1, 1), (-1, -1) at w = 1: image (0, 0), (16, 0), (0, 16).
    cd::Vec4 tri[3] = {{-1, 1, 0, 1}, {1, 1, 0, 1}, {-1, -1, 0, 1}};
    uint64_t count = 0;
    cd::RasterizeClip(small, tri, [&](uint32_t, uint32_t, double) { ++count; });
    Check(count == 136, "the half-image triangle covers the 136 pixel centres on or inside its hypotenuse (got " + std::to_string(count) + ")");
    // At 0.75 a sample point is at x + y + 1.5 <= 16, so only x + y <= 14: 120 pixels.
    small.sample = 0.75;
    uint64_t moved = 0;
    cd::RasterizeClip(small, tri, [&](uint32_t, uint32_t, double) { ++moved; });
    Check(moved == 120, "a sample point 3/4 into the texel covers 120 pixels of it (got " + std::to_string(moved) + ")");
  }
}

void TestStatistics() {
  const tp::Mat4 vp = tp::Mul(Translation(0, 0, -20), Perspective(1.0471975511965976, 1.0, 1.0, 1000.0));
  cd::Image im;
  im.width = 100;
  im.height = 100;
  const double a[3] = {-4, -3, -3.2}, b[3] = {4, -3, 3.2}, c[3] = {0, 3, 0};
  cd::Vec4 clip[3] = {cd::Project(vp, a), cd::Project(vp, b), cd::Project(vp, c)};
  cd::DepthGrid grid;
  grid.Reset(100, 100);
  cd::RasterizeClip(im, clip, [&](uint32_t x, uint32_t y, double d) { grid.Splat(x, y, d); });
  size_t covered_pixels = 0;
  for (size_t i = 0; i < grid.distance.size(); ++i) {
    covered_pixels += grid.Covered(i);
  }
  Check(covered_pixels > 300, "the statistics' triangle covers pixels");

  // The nearest triangle wins a pixel.
  cd::DepthGrid two;
  two.Reset(4, 4);
  two.Splat(1, 1, 5.0);
  two.Splat(1, 1, 3.0);
  two.Splat(1, 1, 7.0);
  Check(two.distance[5] == 3.0f && !two.Covered(0), "a pixel keeps the nearest distance splatted on it");

  // An image that is the triangle's own distances: all covered, no error.
  std::vector<float> image(grid.distance.size(), 0.0f);
  for (size_t i = 0; i < image.size(); ++i) {
    image[i] = grid.Covered(i) ? grid.distance[i] : 0.0f;
  }
  cd::CheckStats s = cd::CompareToDistance(image, grid, 0.01);
  Check(s.vehicle == covered_pixels && s.covered == covered_pixels && s.coverage == 1.0 && s.raster_only == 0, "an image of the triangle's own distances is fully covered");
  Check(s.mean_rel == 0.0 && s.p95_rel == 0.0 && s.max_rel == 0.0 && s.within_1pct == s.covered, "... with no error");

  // Make the image 3% farther on every vehicle pixel (a constant relative error): mean = 3% (as 1 / 1.03 - 1 is the other way:
  // the triangle is nearer than the image by 2.91%), all outside 1% and 2%, all nearer.
  std::vector<float> far_image = image;
  for (float& v : far_image) {
    v *= 1.03f;
  }
  s = cd::CompareToDistance(far_image, grid, 0.01);
  Check(Near(s.mean_rel, 0.03 / 1.03, 1e-4) && Near(s.p95_rel, 0.03 / 1.03, 1e-4), "a constant 3% error reads 2.91% mean and 95th percentile");
  Check(s.within_1pct == 0 && s.within_2pct == 0 && s.triangle_nearer == s.covered && s.triangle_farther == 0, "... all outside 1% and 2%, all nearer");
  Check(s.mean_abs > 0.0 && s.p95_abs > 0.0, "the absolute error is in the image's units");

  // Pixels the triangle does not cover but the image calls vehicle: coverage falls.
  std::vector<float> more = image;
  more[0] = 9.0f;
  more[1] = 9.0f;
  s = cd::CompareToDistance(more, grid, 0.01);
  Check(s.vehicle == covered_pixels + 2 && s.covered == covered_pixels && Near(s.coverage, double(covered_pixels) / double(covered_pixels + 2)),
        "vehicle pixels with no triangle lower the coverage");
  // Pixels the triangle covers and the image does not: counted apart (hidden behind the world, or not the car).
  std::vector<float> fewer = image;
  size_t cleared = 0;
  for (size_t i = 0; i < fewer.size() && cleared < 10; ++i) {
    if (fewer[i] > 0.0f) {
      fewer[i] = 0.0f;
      ++cleared;
    }
  }
  s = cd::CompareToDistance(fewer, grid, 0.01);
  Check(s.raster_only == 10 && s.vehicle == covered_pixels - 10 && s.coverage == 1.0, "triangle pixels on a non-vehicle pixel are counted apart");
  // NaN and infinite image values are no vehicle.
  std::vector<float> junk = image;
  junk[0] = std::numeric_limits<float>::quiet_NaN();
  junk[1] = std::numeric_limits<float>::infinity();
  s = cd::CompareToDistance(junk, grid, 0.01);
  Check(s.vehicle == covered_pixels, "NaN and infinity in the image are no vehicle pixels");

  // The quantile.
  std::vector<float> v = {5, 1, 4, 2, 3};
  Check(cd::Quantile(&v, 0.0) == 1.0 && cd::Quantile(&v, 1.0) == 5.0 && cd::Quantile(&v, 0.5) == 3.0, "min, median and max");
  std::vector<float> hundred;
  for (int i = 0; i < 101; ++i) {
    hundred.push_back(float(i));
  }
  Check(cd::Quantile(&hundred, 0.95) == 95.0, "the 95th percentile of 0..100 is 95");
  std::vector<float> empty;
  Check(cd::Quantile(&empty, 0.5) == 0.0, "no values, no quantile");

  // The per-draw fit: the pixels a draw's triangles cover, against the image, with no nearest test.
  cd::FitCount fit;
  fit.Add(10.0, 10.0f, 0.01);   // match
  fit.Add(10.05, 10.0f, 0.01);  // match (0.5%)
  fit.Add(9.0, 10.0f, 0.01);    // in front
  fit.Add(11.0, 10.0f, 0.01);   // behind
  fit.Add(10.0, 0.0f, 0.01);    // no vehicle there
  Check(fit.samples == 4 && fit.match == 2 && fit.front == 1 && fit.behind == 1 && fit.off == 1, "the fit counts match, front, behind and off");
  Check(Near(fit.Share(fit.match), 0.5), "the share of a count is over the vehicle pixels");
  cd::FitCount total;
  total.Merge(fit);
  total.Merge(fit);
  Check(total.samples == 8 && total.off == 2, "fits merge");
  Check(cd::FitCount{}.Share(0) == 0.0, "a share of nothing is zero");
}


// ---------------------------------------------------------------------------
// microcode assembled by hand (the layout of car_dump_rules.h's ScanConstantReads)
// ---------------------------------------------------------------------------

struct AluSpec {
  uint32_t vector_op = 1;    // mul: two operands
  uint32_t scalar_op = 50;   // retain_prev: none
  uint32_t reg[3] = {0, 0, 0};          // src1, src2, src3 register numbers
  bool temp[3] = {true, true, true};     // true: a temporary register, false: a float constant
  bool rel0 = false, rel1 = false;       // const_0_rel_abs, const_1_rel_abs
  bool a0 = true;                        // const_address_register_relative: a0 (else the loop counter)
};

struct Microcode {
  std::vector<uint32_t> dwords;
  // `count` ALU instructions follow the one pair of control flow (address 1); sequence bit 2k set marks instruction k as a fetch.
  explicit Microcode(const std::vector<AluSpec>& alu, uint32_t fetch_mask = 0, bool exec = true) {
    const uint32_t count = uint32_t(alu.size());
    uint32_t sequence = 0;
    for (uint32_t k = 0; k < count; ++k) {
      if (fetch_mask & (1u << k)) {
        sequence |= 1u << (2 * k);
      }
    }
    // the first control flow instruction: exec (opcode 1) at address 1; the second: nop
    const uint32_t w0 = 1u | (count << 12) | (sequence << 16);
    const uint32_t w1 = exec ? (1u << 12) : 0u;
    dwords = {w0, (w1 & 0xFFFFu), 0u};
    for (const AluSpec& a : alu) {
      dwords.push_back(a.scalar_op << 26);
      dwords.push_back((a.a0 ? 1u << 29 : 0u) | (a.rel1 ? 1u << 30 : 0u) | (a.rel0 ? 1u << 31 : 0u));
      dwords.push_back(a.reg[2] | (a.reg[1] << 8) | (a.reg[0] << 16) | (a.vector_op << 24) | (a.temp[2] ? 1u << 29 : 0u) |
                       (a.temp[1] ? 1u << 30 : 0u) | (a.temp[0] ? 1u << 31 : 0u));
    }
  }
  std::vector<uint8_t> Bytes() const {
    std::vector<uint8_t> out;
    for (uint32_t d : dwords) {
      out.push_back(uint8_t(d >> 24));
      out.push_back(uint8_t(d >> 16));
      out.push_back(uint8_t(d >> 8));
      out.push_back(uint8_t(d));
    }
    return out;
  }
  cd::ConstantReads Scan() const {
    const std::vector<uint8_t> bytes = Bytes();
    return cd::ScanConstantReads(bytes.data(), uint32_t(bytes.size()));
  }
};

AluSpec ReadsConstant(uint32_t reg) {  // mul r, c#, r
  AluSpec a;
  a.reg[0] = reg;
  a.temp[0] = false;
  return a;
}

// mad r, r, c#[a0], r: the constant is the second operand, addressed through const_0_rel_abs (the first operand is a register)
AluSpec ReadsRelative(uint32_t base, bool via_a0 = true) {
  AluSpec a;
  a.vector_op = 11;
  a.reg[1] = base;
  a.temp[1] = false;
  a.rel0 = true;
  a.a0 = via_a0;
  return a;
}

void TestMicrocodeScan() {
  // A rigid part's shader: gWorld (c0, c3), gWorldViewProj (c8), the bone index c204, the palette's three rows through a0.
  {
    const Microcode m({ReadsConstant(0), ReadsConstant(3), ReadsConstant(8), ReadsConstant(204), ReadsRelative(64), ReadsRelative(65),
                       ReadsRelative(66)});
    const cd::ConstantReads r = m.Scan();
    Check(r.valid && r.instructions == 7, "the walk finds the exec block's seven ALU instructions");
    Check(r.Reads(0) && r.Reads(3) && r.Reads(8) && r.Reads(204) && !r.Reads(1) && !r.Reads(64) && !r.Reads(255),
          "the registers read at a fixed index are exactly those named (a relative base is not one)");
    Check(r.bone_index() && r.relative_a0 && r.palette_rows && !r.relative_loop, "bone index, a0 reads of the palette rows, no loop reads");
    const cd::RecipeChoice c = cd::ChooseRecipe(r, false, 0x1234);
    Check(c.recipe == cd::Recipe::kPalette && c.source == cd::RecipeSource::kMicrocode, "reads the rows and c204: palette then gWorld, by the microcode");
  }
  // A character's shader: the rows through a0, no c204: the bones come per vertex.
  {
    const Microcode m({ReadsConstant(0), ReadsRelative(64), ReadsRelative(65), ReadsRelative(66), ReadsConstant(8)});
    const cd::ConstantReads r = m.Scan();
    Check(r.valid && r.palette_rows && !r.bone_index(), "rows and no bone index");
    Check(cd::ChooseRecipe(r, false, 1).recipe == cd::Recipe::kSkinned, "rows without c204: blended bones");
    Check(cd::ChooseRecipe(r, true, 1).recipe == cd::Recipe::kSkinned && cd::ChooseRecipe(r, true, 1).source == cd::RecipeSource::kMicrocode,
          "... with BLENDINDICES in the declaration too");
  }
  // The tyres' and the wheel proxies' shaders: gWorld and gWorldViewProj, literals in the high registers, no a0.
  {
    const Microcode m({ReadsConstant(0), ReadsConstant(1), ReadsConstant(8), ReadsConstant(253), ReadsConstant(255)});
    const cd::ConstantReads r = m.Scan();
    Check(r.valid && !r.relative_a0 && !r.palette_rows && !r.bone_index(), "no palette reads");
    Check(r.Reads(253) && r.Reads(255), "the literals in the palette's last registers are read as fixed registers");
    Check(cd::ChooseRecipe(r, false, 1).recipe == cd::Recipe::kWorld, "reads no rows: gWorld alone (the tyres)");
    // The microcode decides even when the declaration or the table says otherwise.
    Check(cd::ChooseRecipe(r, true, cd::kKnownWorldOnlyVs[0]).recipe == cd::Recipe::kWorld, "... whatever the declaration says");
  }
  // Relative reads through the loop counter, or of other registers, are not the palette.
  {
    const Microcode loop({ReadsConstant(0), ReadsRelative(64, false)});
    const cd::ConstantReads r = loop.Scan();
    Check(r.relative_loop && !r.relative_a0 && !r.palette_rows, "a loop-counter read of c64 is no palette read");
    const Microcode lights({ReadsConstant(0), ReadsRelative(128), ReadsRelative(163)});
    const cd::ConstantReads l = lights.Scan();
    Check(l.relative_a0 && !l.palette_rows, "a0 reads of the lights' registers are no palette read");
    Check(cd::ChooseRecipe(l, false, 1).recipe == cd::Recipe::kWorld, "... so the shader is gWorld alone");
    const Microcode near_rows({ReadsRelative(63)});
    const Microcode last_row({ReadsRelative(67)});
    Check(!near_rows.Scan().palette_rows && !last_row.Scan().palette_rows, "bases 63 and 67 are outside the rows 64-66");
  }
  // Which operands count: a one-operand vector op ignores src2, a fetch is skipped, a scalar mulsc takes its constant from src3.
  {
    AluSpec frc;  // frc r, r: src2 holds a constant number that nothing reads
    frc.vector_op = 8;
    frc.reg[1] = 204;
    frc.temp[1] = false;
    Check(!Microcode({frc}).Scan().Reads(204), "a one-operand vector op does not read src2");
    AluSpec mul = ReadsConstant(77);
    Check(Microcode({mul}).Scan().Reads(77), "a two-operand vector op reads src1");
    AluSpec second;  // mul r, r, c#: the constant is src2
    second.reg[1] = 88;
    second.temp[1] = false;
    Check(Microcode({second}).Scan().Reads(88), "... and src2");
    Check(!Microcode({second}, 1u).Scan().Reads(88) && Microcode({second}, 1u).Scan().valid, "an instruction the sequence marks as a fetch is not walked");
    AluSpec mulsc;  // mulsc: src3 is a constant whatever its sel bit says (the bit is part of the temporary's number there)
    mulsc.scalar_op = 42;
    mulsc.reg[2] = 100;
    mulsc.temp[2] = true;
    mulsc.vector_op = 30;  // no vector operands
    Check(Microcode({mulsc}).Scan().Reads(100), "mulsc reads its constant from src3");
    AluSpec rcp;  // rcp r: one scalar operand src3, a constant when its sel bit says so
    rcp.scalar_op = 19;
    rcp.vector_op = 30;
    rcp.reg[2] = 7;
    rcp.temp[2] = false;
    Check(Microcode({rcp}).Scan().Reads(7), "a one-operand scalar op reads src3 when it is a constant");
    rcp.temp[2] = true;
    Check(!Microcode({rcp}).Scan().Reads(7), "... and not when it is a register");
    AluSpec none;  // retain_prev: no operand
    none.vector_op = 30;
    none.reg[2] = 9;
    none.temp[2] = false;
    Check(!Microcode({none}).Scan().Reads(9), "an opcode without operands reads nothing");
  }
  // Which addressing bit belongs to an operand: with src1 a constant, const_0_rel_abs is the first constant's, const_1_rel_abs the second's.
  {
    AluSpec a;
    a.vector_op = 1;
    a.reg[0] = 64;
    a.temp[0] = false;
    a.reg[1] = 205;
    a.temp[1] = false;
    a.rel0 = true;  // src1 relative, src2 not
    const cd::ConstantReads r = Microcode({a}).Scan();
    Check(r.palette_rows && r.Reads(205) && !r.Reads(64), "two constants: const_0_rel_abs addresses the first, const_1_rel_abs the second");
    a.rel0 = false;
    a.rel1 = true;  // src2 relative, src1 not
    const cd::ConstantReads q = Microcode({a}).Scan();
    Check(!q.palette_rows && q.Reads(64) && !q.Reads(205) && q.relative_a0, "... and the other way round");
  }
  // Garbage in, nothing out.
  {
    Check(!cd::ScanConstantReads(nullptr, 100).valid, "no microcode");
    const uint8_t tiny[8] = {};
    Check(!cd::ScanConstantReads(tiny, 8).valid, "microcode shorter than a control flow pair");
    const uint8_t zeros[48] = {};
    Check(!cd::ScanConstantReads(zeros, 48).valid, "control flow without an exec block");
    const cd::ConstantReads empty_block = Microcode({}).Scan();
    Check(empty_block.valid && empty_block.instructions == 0, "an exec block with no instruction is a program that reads nothing");
    // an exec block that names instructions beyond the microcode: skipped, not read
    Microcode cut({ReadsConstant(5), ReadsConstant(6)});
    std::vector<uint8_t> bytes = cut.Bytes();
    bytes.resize(bytes.size() - 12);  // the second instruction is gone
    const cd::ConstantReads r = cd::ScanConstantReads(bytes.data(), uint32_t(bytes.size()));
    Check(r.valid && r.instructions == 1 && r.Reads(5) && !r.Reads(6), "an instruction beyond the end of the microcode is not walked");
  }
}

void TestRecipeFallback() {
  const cd::ConstantReads none;  // microcode not read
  const cd::RecipeChoice blend = cd::ChooseRecipe(none, true, 0x77);
  Check(blend.recipe == cd::Recipe::kSkinned && blend.source == cd::RecipeSource::kDeclaration, "without microcode: BLENDINDICES says skinned");
  for (uint64_t known : cd::kKnownWorldOnlyVs) {
    const cd::RecipeChoice c = cd::ChooseRecipe(none, false, known);
    Check(c.recipe == cd::Recipe::kWorld && c.source == cd::RecipeSource::kTable, "without microcode: a known world-only shader is gWorld alone");
  }
  Check(cd::ChooseRecipe(none, false, 0xEDDAF3E3986AE099ull).recipe == cd::Recipe::kWorld, "the tyres' shader (xWheelRubber VS_ParallaxRubber2) is in the table");
  Check(cd::ChooseRecipe(none, false, 0x8B2189251306AF48ull).recipe == cd::Recipe::kWorld, "the wheel proxy's shader (xTestRimBlur VS_drawCheap) is in the table");
  const cd::RecipeChoice other = cd::ChooseRecipe(none, false, 0x23FB35FC659D4A67ull);
  Check(other.recipe == cd::Recipe::kPalette && other.source == cd::RecipeSource::kDefault, "without microcode and unknown: the car default, the palette");
  Check(std::string(cd::RecipeName(cd::Recipe::kWorld)) == "gWorld alone" && std::string(cd::RecipeSourceName(cd::RecipeSource::kTable)) == "table",
        "recipe names");

  // The model matrix of a recipe.
  Bank bank;
  tp::Mat4 world = Translation(10, 0, 0);
  bank.SetMatrix(cd::kWorldRegister, world);
  bank.Set(cd::kPaletteRegister + 3, 1, 0, 0, 5.0f);  // bone 1: x + 5
  bank.Set(cd::kPaletteRegister + 4, 0, 1, 0, 0);
  bank.Set(cd::kPaletteRegister + 5, 0, 0, 1, 0);
  bank.Set(cd::kBoneIndexRegister, 1.0f, 0, 0, 0);
  const cd::CarMatrices m = cd::DecodeCarMatrices(bank.bytes);
  const double p[3] = {1.0, 2.0, 3.0};
  double w[4];
  cd::TransformPoint(cd::ModelMatrix(m, cd::Recipe::kPalette), p, w);
  Check(Near(w[0], 16.0) && Near(w[1], 2.0) && Near(w[2], 3.0), "the palette recipe moves the point by the bone, then by gWorld");
  cd::TransformPoint(cd::ModelMatrix(m, cd::Recipe::kWorld), p, w);
  Check(Near(w[0], 11.0) && Near(w[1], 2.0) && Near(w[2], 3.0), "gWorld alone leaves the bone out (the tyres: the stale bone is not applied)");
  cd::TransformPoint(cd::ModelMatrix(m, cd::Recipe::kSkinned), p, w);
  Check(Near(w[0], 11.0), "a skinned draw's matrix is gWorld (its vertices are blended first)");
}

void TestParts() {
  const uint64_t body_vs = 0x23FB35FC659D4A67ull, body_ps = cd::kCarPaintPs;
  Check(cd::WheelProxyOf(body_vs, body_ps, 0xF) == cd::ProxyWhy::kNo, "a paint draw with a colour write is an ordinary draw");
  Check(cd::WheelProxyOf(cd::kWheelProxyVs, body_ps, 0xF) == cd::ProxyWhy::kShader, "the wheel proxy's vertex shader marks a proxy");
  Check(cd::WheelProxyOf(body_vs, cd::kWheelProxyPs, 0xF) == cd::ProxyWhy::kShader, "... and its pixel shader");
  Check(cd::WheelProxyOf(body_vs, body_ps, 0) == cd::ProxyWhy::kNoColour, "no colour write at all: a stencil-only marker");
  Check(cd::WheelProxyOf(body_vs, body_ps, 0x10) == cd::ProxyWhy::kNoColour, "only bits above RGBA are no colour write");
  Check(cd::WheelProxyOf(body_vs, body_ps, 0x1) == cd::ProxyWhy::kNo, "one colour channel written is a draw that shows");
  Check(cd::kWheelProxyVs == 0x8B2189251306AF48ull && cd::kWheelProxyPs == 0x7D4887A7CB75AF65ull && cd::kCarPaintPs == 0x439B193A3F675533ull,
        "the identities of the wheel proxy pair and of the paint");
  Check(cd::RoleOf(cd::ProxyWhy::kShader, cd::Recipe::kWorld) == cd::Role::kProxy, "a proxy is a proxy whatever its recipe");
  Check(cd::RoleOf(cd::ProxyWhy::kNoColour, cd::Recipe::kSkinned) == cd::Role::kProxy, "... even a skinned one");
  Check(cd::RoleOf(cd::ProxyWhy::kNo, cd::Recipe::kSkinned) == cd::Role::kSkinned, "a skinned draw is a skinned part");
  Check(cd::RoleOf(cd::ProxyWhy::kNo, cd::Recipe::kWorld) == cd::Role::kBody && cd::RoleOf(cd::ProxyWhy::kNo, cd::Recipe::kPalette) == cd::Role::kBody,
        "the tyres (gWorld alone) and the rigid parts are the body");
}

void TestSkin() {
  Check(cd::BlendFormatOf(0x00182886) == cd::BlendFormat::kUnorm8 && cd::BlendFormatOf(0x001A2286) == cd::BlendFormat::kUint8 &&
            cd::BlendFormatOf(0x001A23A6) == cd::BlendFormat::kFloat4 && cd::BlendFormatOf(0x001A2360) == cd::BlendFormat::kHalf4 &&
            cd::BlendFormatOf(0x001A2187) == cd::BlendFormat::kNone,
        "the blend element formats");
  Check(cd::BlendBytes(cd::BlendFormat::kUnorm8) == 4 && cd::BlendBytes(cd::BlendFormat::kFloat4) == 16 && cd::BlendBytes(cd::BlendFormat::kHalf4) == 8 &&
            cd::BlendBytes(cd::BlendFormat::kNone) == 0,
        "their sizes");

  // Four guest bytes g0 g1 g2 g3 as the host reads them (4-byte swap, then B8G8R8A8: x y z w = g1 g2 g3 g0; R8G8B8A8_UINT: g3 g2 g1 g0).
  const uint8_t bytes[4] = {0x00, 0x80, 0xFF, 0x40};
  float v[4];
  cd::DecodeBlend4(bytes, cd::BlendFormat::kUnorm8, v);
  Check(Near(v[0], 128.0 / 255.0, 1e-6) && v[1] == 1.0f && Near(v[2], 64.0 / 255.0, 1e-6) && v[3] == 0.0f, "unorm bytes: x y z w = g1 g2 g3 g0 over 255");
  cd::DecodeBlend4(bytes, cd::BlendFormat::kUint8, v);
  Check(v[0] == 64.0f && v[1] == 255.0f && v[2] == 128.0f && v[3] == 0.0f, "raw bytes: x y z w = g3 g2 g1 g0");
  const uint8_t floats[16] = {0x3F, 0x80, 0, 0, 0x40, 0, 0, 0, 0xC0, 0x40, 0, 0, 0, 0, 0, 0};  // 1, 2, -3, 0
  cd::DecodeBlend4(floats, cd::BlendFormat::kFloat4, v);
  Check(v[0] == 1.0f && v[1] == 2.0f && v[2] == -3.0f && v[3] == 0.0f, "four big-endian floats");
  const uint8_t halves[8] = {0x3C, 0x00, 0x40, 0x00, 0x00, 0x00, 0x38, 0x00};  // 1, 2, 0, 0.5
  cd::DecodeBlend4(halves, cd::BlendFormat::kHalf4, v);
  Check(v[0] == 1.0f && v[1] == 2.0f && v[2] == 0.0f && v[3] == 0.5f, "four big-endian half floats");

  // Bones: the unorm byte times 255, the raw byte, the float rounded; outside the palette (0-63) or no number: no bone.
  Check(cd::BoneOf(3.0f / 255.0f, cd::BlendFormat::kUnorm8) == 3 && cd::BoneOf(63.0f / 255.0f, cd::BlendFormat::kUnorm8) == 63, "unorm bones 3 and 63");
  Check(cd::BoneOf(64.0f / 255.0f, cd::BlendFormat::kUnorm8) == cd::kNoBone, "bone 64 is outside the palette");
  Check(cd::BoneOf(5.4f, cd::BlendFormat::kFloat4) == 5 && cd::BoneOf(5.6f, cd::BlendFormat::kFloat4) == 6, "float bones are rounded");
  Check(cd::BoneOf(-1.0f, cd::BlendFormat::kFloat4) == cd::kNoBone && cd::BoneOf(std::numeric_limits<float>::quiet_NaN(), cd::BlendFormat::kHalf4) == cd::kNoBone,
        "a negative or NaN bone is none");
  Check(Near(cd::WeightOf(255.0f, cd::BlendFormat::kUint8), 1.0) && cd::WeightOf(0.25f, cd::BlendFormat::kUnorm8) == 0.25f, "raw weights are over 255, the others as they are");

  // A bank with bone 2 = move by +1 in x, bone 5 = scale by 2.
  Bank bank;
  bank.Set(cd::kPaletteRegister + 6, 1, 0, 0, 1.0f);
  bank.Set(cd::kPaletteRegister + 7, 0, 1, 0, 0);
  bank.Set(cd::kPaletteRegister + 8, 0, 0, 1, 0);
  bank.Set(cd::kPaletteRegister + 15, 2, 0, 0, 0);
  bank.Set(cd::kPaletteRegister + 16, 0, 2, 0, 0);
  bank.Set(cd::kPaletteRegister + 17, 0, 0, 2, 0);
  auto palette = std::make_unique<cd::SkinPalette>();
  cd::LoadPalette(bank.bytes, palette.get());
  Check(palette->rows[2][0][3] == 1.0f && palette->rows[5][2][2] == 2.0f, "the palette holds bone b's rows at c64 + 3 b");

  const float p[3] = {1.0f, 2.0f, 3.0f};
  cd::SkinInfluence skin;
  skin.bone[0] = 2;
  skin.weight[0] = 0.25f;
  skin.bone[1] = 5;
  skin.weight[1] = 0.75f;
  float out[3];
  cd::SkinStats stats;
  cd::SkinBlend(*palette, skin, p, out, &stats);
  // bone 2: (2, 2, 3); bone 5: (2, 4, 6); 0.25 and 0.75: (2, 3.5, 5.25)
  Check(Near(out[0], 2.0, 1e-6) && Near(out[1], 3.5, 1e-6) && Near(out[2], 5.25, 1e-6), "the blend is the weighted sum of the bones' transforms");
  Check(stats.vertices == 1 && stats.bad_bone == 0 && stats.bad_weight == 0 && stats.worst_weight_sum_error < 1e-6, "a good vertex adds nothing to the bad counts");

  cd::SkinInfluence bad;
  bad.bone[0] = cd::kNoBone;
  bad.weight[0] = 0.5f;  // a bone that is not in the palette, with weight
  bad.bone[1] = 2;
  bad.weight[1] = 0.3f;
  bad.bone[2] = cd::kNoBone;
  bad.weight[2] = 0.0f;  // a bone that is not in the palette, with no weight: harmless
  cd::SkinBlend(*palette, bad, p, out, &stats);
  Check(stats.vertices == 2 && stats.bad_bone == 1, "a weighted influence of a bone outside the palette is counted, a weightless one is not");
  Check(stats.bad_weight == 1 && Near(stats.worst_weight_sum_error, 0.2, 1e-6), "weights that add up to 0.8 are counted (error 0.2)");
  Check(Near(out[0], 0.3 * 2.0, 1e-6), "the bad influence adds nothing to the position");

  // From the vertex bytes: unorm indices (x y z w = g1 g2 g3 g0) and weights.
  const uint8_t index_bytes[4] = {0, 2, 5, 0};     // g0 = 0, g1 = 2, g2 = 5, g3 = 0: x = 2, y = 5, z = 0, w = 0
  const uint8_t weight_bytes[4] = {0, 64, 191, 0};  // x = 64/255, y = 191/255, z = 0, w = 0 (adds up to 1)
  const cd::SkinInfluence read = cd::ReadInfluence(index_bytes, cd::BlendFormat::kUnorm8, weight_bytes, cd::BlendFormat::kUnorm8);
  Check(read.bone[0] == 2 && read.bone[1] == 5 && read.bone[2] == 0 && read.bone[3] == 0, "the influences' bones from unorm bytes");
  Check(Near(read.weight[0] + read.weight[1], 1.0, 1e-6), "... and their weights");
  cd::SkinStats s2;
  cd::SkinBlend(*palette, read, p, out, &s2);
  // bone 2: (2, 2, 3) weight 64/255; bone 5: (2, 4, 6) weight 191/255; bone 0 has zero weight in z and w
  Check(Near(out[0], 2.0, 1e-5) && s2.bad_weight == 0 && s2.bad_bone == 0, "a vertex read from bytes blends to the expected position");
  // The order of the components does not matter as long as indices and weights use the same one.
  const uint8_t index_swapped[4] = {5, 0, 2, 0};   // g0 g1 g2 g3 permuted with the weights below
  const uint8_t weight_swapped[4] = {191, 0, 64, 0};
  const cd::SkinInfluence read2 = cd::ReadInfluence(index_swapped, cd::BlendFormat::kUnorm8, weight_swapped, cd::BlendFormat::kUnorm8);
  float out2[3];
  cd::SkinBlend(*palette, read2, p, out2, nullptr);
  Check(Near(out2[0], out[0], 1e-5) && Near(out2[1], out[1], 1e-5) && Near(out2[2], out[2], 1e-5), "the blend does not depend on the component order shared by indices and weights");
}

void TestPlayerPick() {
  const double inf = std::numeric_limits<double>::infinity();
  cd::IdSummary ids[cd::kIdSlots];
  auto car = [&](uint32_t id, uint32_t body, uint32_t paint, double camera, double player) {
    ids[id].body_draws = body;
    ids[id].paint_draws = paint;
    ids[id].camera_distance = camera;
    ids[id].player_distance = player;
  };
  // The first run: ID 1 the player's R34 (210 draws, 14 with paint, 7 m), ID 2 another car (138 draws, 10 m), IDs 3-10 wheel proxies.
  car(1, 208, 14, 7.06, 0.3);
  car(2, 136, 10, 10.0, 12.0);
  for (uint32_t id = 3; id <= 10; ++id) {
    ids[id].proxy_draws = 1;
    ids[id].camera_distance = 10.0 + id;
  }
  ids[1].skinned_draws = 15;
  cd::Pick p = cd::ChoosePlayerId(ids, 0, true);
  Check(p.id == 1 && p.reason == cd::PickReason::kPlayerVehicle, "the ID nearest to the player's vehicle in the game's memory");
  p = cd::ChoosePlayerId(ids, 0, false);
  Check(p.id == 1 && p.reason == cd::PickReason::kNearestPaint, "without the player's vehicle: the nearest ID with paint draws");
  // The nearest paint car is not the player's: the vehicle rule wins.
  car(1, 208, 14, 20.0, 12.0);
  car(2, 136, 10, 6.0, 0.4);
  p = cd::ChoosePlayerId(ids, 0, true);
  Check(p.id == 2 && p.reason == cd::PickReason::kPlayerVehicle, "the player's vehicle decides even when another car is nearer the camera");
  p = cd::ChoosePlayerId(ids, 0, false);
  Check(p.id == 2 && p.reason == cd::PickReason::kNearestPaint, "... and the camera decides without it");
  // The vehicle is too far from every ID (a stale read, or another coordinate system): the paint rule.
  car(1, 208, 14, 7.0, 40.0);
  car(2, 136, 10, 10.0, 30.0);
  p = cd::ChoosePlayerId(ids, 0, true);
  Check(p.id == 1 && p.reason == cd::PickReason::kNearestPaint, "no ID within 5 m of the player's vehicle: the nearest paint car");
  car(1, 208, 14, 7.0, 4.9);
  Check(cd::ChoosePlayerId(ids, 0, true).reason == cd::PickReason::kPlayerVehicle, "4.9 m is within the match distance");
  car(1, 208, 14, 7.0, 5.1);
  Check(cd::ChoosePlayerId(ids, 0, true).reason == cd::PickReason::kNearestPaint, "5.1 m is not");
  // Wheel proxies are never chosen, whatever their distance.
  ids[7].proxy_draws = 1;
  ids[7].camera_distance = 0.5;
  ids[7].player_distance = 0.0;
  car(1, 208, 14, 7.0, 40.0);
  p = cd::ChoosePlayerId(ids, 0, true);
  Check(p.id == 1, "an ID with only a wheel proxy is no car, however near");
  // No camera: the most paint draws.
  car(1, 208, 14, inf, inf);
  car(2, 136, 10, inf, inf);
  p = cd::ChoosePlayerId(ids, 0, false);
  Check(p.id == 1 && p.reason == cd::PickReason::kMostPaint, "the camera is not known: the most paint draws");
  // No paint draw anywhere: the most body draws.
  car(1, 208, 0, 7.0, inf);
  car(2, 300, 0, 10.0, inf);
  p = cd::ChoosePlayerId(ids, 0, false);
  Check(p.id == 2 && p.reason == cd::PickReason::kMostDraws, "no paint draw in the frame: the most body draws");
  // A requested ID is honoured when it holds a car's draw, even a skinned one; not when it holds only a proxy.
  p = cd::ChoosePlayerId(ids, 1, true);
  Check(p.id == 1 && p.reason == cd::PickReason::kRequested, "mcla_gpu_car_dump_id = 1 picks ID 1");
  p = cd::ChoosePlayerId(ids, 7, true);
  Check(p.id != 7 && p.reason != cd::PickReason::kRequested, "a requested ID that has only a wheel proxy is refused");
  p = cd::ChoosePlayerId(ids, 23, true);
  Check(p.id != 23 && p.reason != cd::PickReason::kRequested, "a requested ID with no draw is refused");
  p = cd::ChoosePlayerId(ids, 99, true);
  Check(p.reason != cd::PickReason::kRequested, "a requested ID out of range is refused");
  ids[12].skinned_draws = 3;
  Check(cd::ChoosePlayerId(ids, 12, false).id == 12, "an ID with a skinned draw only can be asked for");
  // Nothing at all.
  cd::IdSummary empty[cd::kIdSlots];
  p = cd::ChoosePlayerId(empty, 0, true);
  Check(p.id == 0 && p.reason == cd::PickReason::kNone, "no car in the frame: no ID");
  Check(ids[1].Draws() == ids[1].body_draws + ids[1].skinned_draws + ids[1].proxy_draws, "the draws of an ID add up");
  Check(std::string(cd::PickReasonName(cd::PickReason::kPlayerVehicle)).find("player") != std::string::npos, "the reason names the rule");
}

}  // namespace

int main() {
  TestBank();
  TestMicrocodeScan();
  TestRecipeFallback();
  TestParts();
  TestSkin();
  TestPlayerPick();
  TestVertexBytes();
  TestTriangleSet();
  TestRaster();
  TestStatistics();
  std::printf("car_dump_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
