// Check of the car dump prototype's rules (car_dump_rules.h): the matrices out of a vertex shader bank, the vertex and index bytes as
// the guest holds them, the triangle list, the rasteriser and the statistics that compare it with the car history image. A program
// of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_dump_rules_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_car_dump_rules_test

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

}  // namespace

int main() {
  TestBank();
  TestVertexBytes();
  TestTriangleSet();
  TestRaster();
  TestStatistics();
  std::printf("car_dump_rules_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
