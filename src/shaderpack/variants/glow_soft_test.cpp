// Check of the glow variant's text transform and of the rule it writes (glow_soft.h). A program of its own: it
// needs no renderer, no ReXGlue, no compiler and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_glow_soft_test     (src/shaderpack/CMakeLists.txt)
//   <build dir>/src/shaderpack/larecomp_glow_soft_test [<folder of translated HLSL> [<folder for the results>]]
//
// Without arguments it runs on small texts made here (the lines of the two shaders that the anchors look at, and
// some other lines that must stay as they are). Then it reads the helper's disc table back out of the transformed
// text and runs the same rule in C++ (a model of GlowVisibleShare, written separately from the HLSL) on a depth
// picture with an edge: the share is 1 and 0 away from the edge, a ramp of a few pixels across it, never a
// step, and the same on average at 1x, 3x and 5x (the disc is in guest pixels). With a folder of translated HLSL (the
// builder's own output, `<group>/<entry>.hlsl` files, for example shaderpack/w1/hlsl of the development PC) it also
// runs on the two real shaders, and a second folder receives the two texts, for a compile with dxc
// (-T ps_6_0 -E main -HV 2021 -spirv -fvk-use-dx-layout, and without -spirv for DXIL).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "shaderpack/variants/glow_soft.h"

namespace sp = larecomp::shaderpack;

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

size_t Count(std::string_view text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// ---------------------------------------------------------------------------
// Fixtures: the parts of the two translated shaders the transform looks at.
// ---------------------------------------------------------------------------

struct Options {
  bool with_main = true;
  bool changed_compare = false;  // the round glow's compare is >= (not the game's)
  bool no_guard = false;         // the round glow without its z > 0 guard
  bool changed_limit = false;    // the star's limit is another constant
  bool second_tap = false;       // a second copy of the depth tap
  bool no_colour = false;        // the colour write is another one
  bool crlf = false;
};

std::string Dress(std::string body, const Options& o) {
  std::string t;
  t += "#define g_InvResolutionScale vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 592)\n";
  t += "float4 tfetch2DLod(uint a, uint b, float2 c, float2 d, float e);\n\n";
  if (o.with_main) {
    t += "#ifndef __spirv__\n[shader(\"pixel\")]\n#endif\nvoid main(\n\tin float4 iPos : SV_Position,\n"
         "\tout float4 oC0 : SV_Target0\n)\n{\n";
  } else {
    t += "void notmain()\n{\n";
  }
  t += body;
  t += "\treturn;\n}\n";
  if (o.crlf) {
    std::string out;
    for (const char c : t) {
      if (c == '\n') out += '\r';
      out += c;
    }
    return out;
  }
  return t;
}

std::string TrailFixture(const Options& o = Options()) {
  std::string b;
  b += "\tfloat4 c254 = asfloat(uint4(0x40000000, 0xBD4CCCCD, 0x0, 0x0));\n";
  b += "\tfloat4 c255 = asfloat(uint4(0x0, 0x3BDA740E, 0xBF800000, 0x3F800000));\n\n";
  b += "\tps = clamp(rcp(r1.w), FLT_MIN, FLT_MAX);\n\tr0.w = ps;\n\tr3.xy = r0.ww * r1.yx;\n";
  const std::string tap =
      "\tr3.x = tfetch2D(DepthMapSampler_Texture2DDescriptorIndex, DepthMapSampler_SamplerDescriptorIndex, r3.yx, "
      "float2(0, 0)).x;\n";
  b += tap;
  if (o.second_tap) b += tap;
  b += "\tr0.w = saturate(-r1.z * c255.y + c255.w);\n\tps = c255.w - r0.z;\n\tr1.x = ps;\n";
  b += o.changed_compare ? "\tr1.x = r3.x >= r1.x;\n" : "\tr1.x = r3.x > r1.x;\n";
  if (o.no_guard) {
    b += "\tr1.y = 1.0;\n";
  } else {
    b += "\tps = r0.z > 0.0;\n\tr1.y = ps;\n";
  }
  b += "\tr1.x = r1.y * r1.x;\n\tp0 = r1.x == 0.0;\n";
  b += "\tif (p0)\n\t{\n\t\tr0.xy = r0.yx * c254.xx + c255.zz;\n\t}\n";
  b += "\tif (p0)\n\t{\n\t\tr2.w = r2.w * r0.w;\n\t}\n";
  if (o.no_colour) {
    b += "\tif (p0)\n\t{\n\t\toC0.xyz = saturate(r2.wwww * r2.xyzz);\n\t\toC0.w = 1.0;\n\t}\n";
  } else {
    b += "\tif (p0)\n\t{\n\t\toC0.xyz = saturate(r2.wwww * r2.xyzz + c254.yyyy);\n\t\toC0.w = 1.0;\n\t}\n";
  }
  b += "\tif (!p0)\n\t{\n\t\tps = -abs(r2.x) > 0.0;\n\t\toC0.x = ps;\n\t\toC0.y = 0.0;\n\t\toC0.z = 0.0;\n"
       "\t\toC0.w = 0.0;\n\t}\n";
  return Dress(b, o);
}

std::string StarFixture(const Options& o = Options()) {
  std::string b;
  b += "\tfloat4 c255 = asfloat(uint4(0x3F800000, 0x0, 0x0, 0x0));\n\n";
  b += "\tps = clamp(rcp(r1.w), FLT_MIN, FLT_MAX);\n\tr0.w = ps;\n\tr1.xy = r0.ww * r1.xy;\n";
  const std::string tap =
      "\tr0.w = tfetch2D(DepthMapSampler_Texture2DDescriptorIndex, DepthMapSampler_SamplerDescriptorIndex, r1.xy, "
      "float2(0, 0)).x;\n";
  b += tap;
  if (o.second_tap) b += tap;
  b += o.changed_limit ? "\tps = c255.y - r0.z;\n" : "\tps = c255.x - r0.z;\n";
  b += "\tr1.x = ps;\n\tr0.w = r1.x > r0.w;\n\tp0 = r0.w != 0.0;\n";
  b += "\tif (p0)\n\t{\n\t\tps = r2.w * r2.w;\n\t\tr1.w = ps;\n\t}\n";
  if (o.no_colour) {
    b += "\tif (p0)\n\t{\n\t\toC0.xyz = saturate(r1.zyx);\n\t\tps = saturate(r0.w * ps);\n\t\toC0.w = ps;\n\t}\n";
  } else {
    b += "\tif (p0)\n\t{\n\t\toC0.xyz = saturate(r1.zyx * r2.xyz);\n\t\tps = saturate(r0.w * ps);\n\t\toC0.w = ps;\n\t}\n";
  }
  return Dress(b, o);
}

// ---------------------------------------------------------------------------
// The rule, as a model written here
// ---------------------------------------------------------------------------

struct Vec2 {
  double x = 0;
  double y = 0;
};

// The 24 offsets of the helper's table, read back out of the transformed text.
std::vector<Vec2> ReadTable(const std::string& hlsl) {
  std::vector<Vec2> table;
  const size_t begin = hlsl.find("kGlowDisc[24] = {");
  const size_t end = hlsl.find("};", begin);
  if (begin == std::string::npos || end == std::string::npos) return table;
  size_t at = begin;
  while (true) {
    at = hlsl.find("float2(", at);
    if (at == std::string::npos || at > end) break;
    at += 7;
    char* tail = nullptr;
    const double x = std::strtod(hlsl.c_str() + at, &tail);
    const double y = std::strtod(tail + 1, &tail);  // ", "
    table.push_back({x, y});
  }
  return table;
}

// A depth picture of one scene as the game would resolve it at the scale S: an edge at x = edge (in pixels of the
// 1280x720 screen, vertical, the scene nearer than the glow to the right of it), in texels S times finer than the
// guest's. The sampler is a point sampler: the texel the position falls in, and the texel's own centre decides.
struct Picture {
  double edge;
  int scale;
  bool Occluded(double guest_x, double /*guest_y*/) const {
    const double texel = std::floor(guest_x * scale);
    const double centre = (texel + 0.5) / scale;
    return centre > edge;
  }
};

struct Share {
  double vis = 0;     // what the sprite is multiplied by
  double share = 0;   // the share of the taps that pass
  int taps_used = 0;  // 8 when the first stage agreed, else 24
};

// GlowVisibleShare of glow_soft.cpp, again: the outer 8 taps (16..23), the inner 16 only when they disagree.
Share Model(const std::vector<Vec2>& table, const Picture& picture, double centre_x, double centre_y) {
  const auto passes = [&](int i) {
    return picture.Occluded(centre_x + table[i].x * sp::kGlowSoftRadius, centre_y + table[i].y * sp::kGlowSoftRadius)
               ? 0
               : 1;
  };
  int passed = 0;
  const int inner = sp::kGlowSoftTaps - sp::kGlowSoftFirstTaps;
  for (int i = inner; i < sp::kGlowSoftTaps; ++i) passed += passes(i);
  Share out;
  out.taps_used = sp::kGlowSoftFirstTaps;
  if (passed == 0 || passed == sp::kGlowSoftFirstTaps) {
    out.share = passed == 0 ? 0.0 : 1.0;
    out.vis = out.share;
    return out;
  }
  for (int i = 0; i < inner; ++i) passed += passes(i);
  out.taps_used = sp::kGlowSoftTaps;
  out.share = double(passed) / sp::kGlowSoftTaps;
  const double v = std::min(1.0, out.share / sp::kGlowSoftFullShare);
  out.vis = v * v * (3.0 - 2.0 * v);
  return out;
}

void CheckRule(const std::string& transformed) {
  const std::vector<Vec2> table = ReadTable(transformed);
  Check(table.size() == size_t(sp::kGlowSoftTaps), "the helper's table has 24 offsets (read " +
                                                       std::to_string(table.size()) + ")");
  if (table.size() != size_t(sp::kGlowSoftTaps)) return;

  // The table is a Vogel disc: radius sqrt((i + 0.5) / 24), a golden angle turn per index.
  const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
  bool vogel = true;
  bool inside = true;
  for (int i = 0; i < sp::kGlowSoftTaps; ++i) {
    const double r = std::sqrt((i + 0.5) / sp::kGlowSoftTaps);
    const double want_x = r * std::cos(i * golden);
    const double want_y = r * std::sin(i * golden);
    vogel &= std::fabs(table[i].x - want_x) < 2e-6 && std::fabs(table[i].y - want_y) < 2e-6;
    inside &= std::hypot(table[i].x, table[i].y) < 1.0;
  }
  Check(vogel, "the table is the Vogel disc of 24 taps to 6 digits");
  Check(inside, "every tap lies inside the unit disc");
  Check(Contains(transformed, "* 2.5"), "the helper's disc radius is the 2.5 of glow_soft.h");
  Check(sp::kGlowSoftRadius == 2.5 && sp::kGlowSoftFullShare == 0.5, "the constants of glow_soft.h are 2.5 and 0.5");
  Check(Contains(transformed, "(2.0 / 24.0)") && Contains(transformed, "visible * visible * (3.0 - 2.0 * visible)"),
        "the helper's visibility is the smoothstep of passed * 2 / 24 (full at half)");
  Check(Contains(transformed, "passed > 7.5") && Contains(transformed, "passed < 0.5"),
        "the helper stops after 8 taps when all 8 agree");
  Check(Contains(transformed, "for (int i = 16; i < 24; ++i)") && Contains(transformed, "for (int j = 0; j < 16; ++j)"),
        "the helper takes taps 16..23 first and 0..15 after");
  Check(!Contains(transformed, "floor("), "no tap is moved to a guest pixel centre");

  // The first stage's 8 taps (16..23) are the rim, in all directions: radii from 0.8 up, no gap of more than 60
  // degrees between neighbours.
  std::vector<double> angles;
  double lowest = 1.0;
  for (int i = sp::kGlowSoftTaps - sp::kGlowSoftFirstTaps; i < sp::kGlowSoftTaps; ++i) {
    lowest = std::min(lowest, std::hypot(table[i].x, table[i].y));
    angles.push_back(std::atan2(table[i].y, table[i].x) * 180.0 / 3.14159265358979323846);
  }
  std::sort(angles.begin(), angles.end());
  double widest = 360.0 + angles.front() - angles.back();
  for (size_t i = 1; i < angles.size(); ++i) widest = std::max(widest, angles[i] - angles[i - 1]);
  Check(lowest > 0.8, "the first stage's taps are on the rim (radius above 0.8: " + std::to_string(lowest) + ")");
  Check(widest <= 60.0, "no gap of more than 60 degrees between the rim taps (" + std::to_string(widest) + ")");
  Check(sp::kGlowSoftFirstTaps == 8 && sp::kGlowSoftTaps == 24, "8 of 24 taps in the first stage");

  // A sweep of the glow's centre across a vertical edge, in steps of a twentieth of a pixel, at 1x, 3x and 5x.
  // The game's own single tap is a step: 1 then 0. The share is 1 well before the edge, 0 well after, and its
  // largest step between neighbours is a fraction of that.
  double means[3] = {};
  double steps[3] = {};
  int index = 0;
  for (const int scale : {1, 3, 5}) {
    double previous = -1.0;
    double largest_step = 0.0;
    double ramp_begin = 1e9;
    double ramp_end = -1e9;
    bool monotone = true;
    double sum = 0.0;
    int n = 0;
    const Picture picture = {400.0, scale};
    for (double x = 390.0; x <= 410.0; x += 0.05) {
      const Share s = Model(table, picture, x, 300.5);
      if (previous >= 0.0) {
        largest_step = std::max(largest_step, std::fabs(s.vis - previous));
        monotone &= s.vis <= previous + 1e-12;
      }
      previous = s.vis;
      sum += s.vis;
      ++n;
      if (s.vis > 0.0 && s.vis < 1.0) {
        ramp_begin = std::min(ramp_begin, x);
        ramp_end = std::max(ramp_end, x);
      }
    }
    const Share before = Model(table, picture, 390.0, 300.5);
    const Share after = Model(table, picture, 410.0, 300.5);
    const std::string s = " at " + std::to_string(scale) + "x";
    Check(before.vis == 1.0 && before.taps_used == 8, "far in front of the edge the sprite is in full from 8 taps" + s);
    Check(after.vis == 0.0 && after.taps_used == 8, "far behind the edge the sprite is left out from 8 taps" + s);
    Check(monotone, "the visibility never rises while the glow moves behind the edge" + s);
    Check(largest_step <= 0.30, "no step of the visibility above 0.30 per twentieth of a pixel" + s + " (largest " +
                                    std::to_string(largest_step) + ")");
    Check(ramp_end - ramp_begin >= 1.5 && ramp_end - ramp_begin <= 5.0,
          "the ramp (from half of the disc hidden to all of it) is 1.5 to 5 pixels wide" + s + " (" +
              std::to_string(ramp_end - ramp_begin) + ")");
    std::printf("  edge sweep at %dx: ramp %.2f..%.2f px (%.2f wide), largest step %.3f, mean %.4f\n", scale,
                ramp_begin, ramp_end, ramp_end - ramp_begin, largest_step, sum / n);
    means[index] = sum / n;
    steps[index] = largest_step;
    ++index;
  }
  // The disc is in guest pixels: the same picture at every scale, on average, and a finer copy follows a moving
  // edge in steps no larger than the guest's.
  Check(std::fabs(means[0] - means[1]) < 0.03 && std::fabs(means[1] - means[2]) < 0.03,
        "the mean visibility over the sweep is the same at 1x, 3x and 5x to 0.03");
  Check(steps[1] <= steps[0] + 1e-9 && steps[2] <= steps[0] + 1e-9,
        "a finer depth copy follows a moving edge in steps that are no larger than the guest's");

  // The game's one tap, for the record: the same sweep is one step of 1.
  {
    const Picture picture = {400.0, 3};
    double largest = 0.0;
    double previous = -1.0;
    for (double x = 390.0; x <= 410.0; x += 0.05) {
      const double single = picture.Occluded(x, 300.5) ? 0.0 : 1.0;
      if (previous >= 0.0) largest = std::max(largest, std::fabs(single - previous));
      previous = single;
    }
    Check(largest == 1.0, "the game's single tap steps by 1 across the edge (the pop)");
  }

  // The sway of the K-NIGHT palm (LAMP3X.md 3.6): the glow 0.25 of a pixel inside the edge, the trunk moving by
  // 0.3 pixels either way: the sprite stays in a narrow band of brightness, it does not flip from in full to
  // nothing (the game's single tap does).
  int flips = 0;
  for (const double centre : {399.75, 400.25}) {
    double low = 2.0;
    double high = -1.0;
    double single_low = 2.0;
    double single_high = -1.0;
    for (double sway = -0.3; sway <= 0.3; sway += 0.01) {
      const Share s = Model(table, {400.0 + sway, 3}, centre, 300.5);
      low = std::min(low, s.vis);
      high = std::max(high, s.vis);
      const double single = Picture{400.0 + sway, 3}.Occluded(centre, 300.5) ? 0.0 : 1.0;
      single_low = std::min(single_low, single);
      single_high = std::max(single_high, single);
    }
    std::printf("  a trunk swaying 0.3 px, the glow's centre at %.2f, the edge at 400: visibility %.2f to %.2f "
                "(the game's tap %.0f to %.0f)\n",
                centre, low, high, single_low, single_high);
    Check(high - low <= 0.35, "a trunk swaying by 0.3 px changes the visibility by at most 0.35 (" +
                                  std::to_string(high - low) + ")");
    flips += single_high - single_low == 1.0 ? 1 : 0;
  }
  Check(flips >= 1, "the game's single tap flips from 1 to 0 under the same sway at one of the two places");
}

// ---------------------------------------------------------------------------
// The checks
// ---------------------------------------------------------------------------

void CheckFails(const sp::VariantShader& shader, const std::string& text, const std::string& what,
                std::string_view expected) {
  const sp::VariantTransformResult r = sp::TransformGlowSoftHlsl(shader, text);
  Check(!r.ok && r.hlsl.empty() && Contains(r.error, expected),
        what + ": not built, and the error names '" + std::string(expected) + "' (" + r.error + ")");
}

void CheckTransformed(const sp::VariantShader& shader, const std::string& original, bool trail,
                      std::string* transformed) {
  const std::string name = std::string(shader.name);
  const sp::VariantTransformResult r = sp::TransformGlowSoftHlsl(shader, original);
  Check(r.ok && r.error.empty(), name + ": the transform succeeds (" + r.error + ")");
  if (!r.ok) return;
  *transformed = r.hlsl;
  const std::string& t = r.hlsl;
  Check(Count(t, "GlowVisibleShare(") == 2, name + ": the helper is defined once and called once");
  Check(Count(t, "glowShare") == 3, name + ": the share is made, tested and multiplied in once each");
  Check(Count(t, "tfetch2D(DepthMapSampler") == 0, name + ": no depth tap of the game's is left");
  Check(Count(t, "tfetch2DLod(resourceDescriptorIndex") == 1, name + ": the helper's one tap site uses the explicit level");
  Check(Count(t, "-1.0e20 : 1.0e20;") == 1, name + ": the compare's register gets the share's answer");
  Check(Contains(t, trail ? "oC0.xyz = saturate(r2.wwww * r2.xyzz + c254.yyyy) * glowShare;"
                          : "oC0.xyz = saturate(r1.zyx * r2.xyz) * glowShare;"),
        name + ": the colour write is the game's times the share");
  Check(trail ? Contains(t, "r3.yx, c255.w - r0.z, r0.z > 0.0, false);")
              : Contains(t, "r1.xy, c255.x - r0.z, true, true);"),
        name + ": the call has the limit, the guard and the compare's strictness of its shader");
  Check(Contains(t, "oC0.w = " + std::string(trail ? "1.0;" : "ps;")), name + ": the alpha write is the game's");
  // The helper stands before main, and main is still there once.
  const size_t helper = t.find("float GlowVisibleShare(");
  const size_t main_at = t.find("void main(");
  Check(helper != std::string::npos && main_at != std::string::npos && helper < main_at && Count(t, "void main(") == 1,
        name + ": the helper stands before main");
  // Everything of the original is still there but the replaced lines: the length grew by the helper and the edits.
  Check(t.size() > original.size(), name + ": the text is longer than the original");
  // Both line ending conventions give one result.
  Options crlf;
  crlf.crlf = true;
  const sp::VariantTransformResult w = sp::TransformGlowSoftHlsl(shader, trail ? TrailFixture(crlf) : StarFixture(crlf));
  Check(w.ok && w.hlsl == r.hlsl, name + ": CRLF and LF texts give one result");
  // The input is not touched.
  const std::string copy = original;
  (void)sp::TransformGlowSoftHlsl(shader, original);
  Check(original == copy, name + ": the input is not changed");
}

void CheckFixtures() {
  std::string trail_text;
  std::string star_text;
  CheckTransformed(sp::kGlowSoftTrail, TrailFixture(), true, &trail_text);
  CheckTransformed(sp::kGlowSoftStar, StarFixture(), false, &star_text);
  Check(trail_text != star_text, "the two shaders give different texts");

  // The anchors: each edit that does not match leaves the variant out.
  Options o;
  o.with_main = false;
  CheckFails(sp::kGlowSoftTrail, TrailFixture(o), "round glow without main", "pixel shader main");
  CheckFails(sp::kGlowSoftStar, StarFixture(o), "star without main", "pixel shader main");
  o = Options();
  o.changed_compare = true;
  CheckFails(sp::kGlowSoftTrail, TrailFixture(o), "a compare of another kind", "compare with 1 - TexCoord0.z");
  o = Options();
  o.no_guard = true;
  CheckFails(sp::kGlowSoftTrail, TrailFixture(o), "no z > 0 guard", "guard");
  o = Options();
  o.changed_limit = true;
  CheckFails(sp::kGlowSoftStar, StarFixture(o), "another limit constant", "compare with 1 - c255.x");
  o = Options();
  o.second_tap = true;
  CheckFails(sp::kGlowSoftTrail, TrailFixture(o), "two depth taps in the round glow", "found 2 times");
  CheckFails(sp::kGlowSoftStar, StarFixture(o), "two depth taps in the star", "found 2 times");
  o = Options();
  o.no_colour = true;
  CheckFails(sp::kGlowSoftTrail, TrailFixture(o), "another colour write in the round glow", "colour write");
  CheckFails(sp::kGlowSoftStar, StarFixture(o), "another colour write in the star", "colour write");
  // A shader of another name, and the swapped fixtures (each text is only for its own shader).
  CheckFails({"xstar_glow__PSGlowSeedRT", 0}, TrailFixture(), "another glow shader", "not one of the glow sprites");
  CheckFails(sp::kGlowSoftStar, TrailFixture(), "the round glow's text given as the star", "the star's depth tap");
  CheckFails(sp::kGlowSoftTrail, StarFixture(), "the star's text given as the round glow", "round glow's depth tap");
  CheckFails(sp::kGlowSoftTrail, "", "an empty text", "found 0 times");

  // The variant's definition.
  const sp::VariantDef def = sp::MakeGlowSoftVariant();
  Check(def.feature == sp::kFeatureGlowSoft && def.feature == 4 && def.name == "glow_soft" && def.pixel_shaders.size() == 2 &&
            def.vertex_shaders.empty() && def.needs_rpf && def.transform,
        "the definition: feature 4 (the old id 6), name, two pixel shaders, no vertex shader, the RPF, a transform");
  Check(def.version == sp::kGlowSoftVariantVersion && def.version >= 1, "the definition carries the version");
  Check(def.pixel_shaders[0].name == "xstar_glow__PSTrailSeedRT" && def.pixel_shaders[0].key == 0xF6A55A0E0E26E0BBull &&
            def.pixel_shaders[1].name == "xstar_glow__PSSeedRT1" && def.pixel_shaders[1].key == 0x92CAA8C0D85D8EC2ull,
        "the identities are the census's");
  Check(sp::kFeatureGlowSoft < sp::kMaxFeatures && sp::kFeatureGlowSoft > sp::kFeatureBloomGauss &&
            sp::kFeatureGlowSoft != sp::kFeatureEnvMap && sp::kFeatureGlowSoft != sp::kFeatureShadowFilter,
        "feature 4 is in the word and is neither the env map's nor the shadow filter's");

  CheckRule(trail_text);
}

// ---------------------------------------------------------------------------
// The game's own files (optional)
// ---------------------------------------------------------------------------

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void CheckGameFiles(const std::filesystem::path& folder, const std::filesystem::path& results) {
  for (const sp::VariantShader& shader : {sp::kGlowSoftTrail, sp::kGlowSoftStar}) {
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
      if (entry.is_regular_file() && entry.path().filename().string() == std::string(shader.name) + ".hlsl") {
        found = entry.path();
      }
    }
    Check(!found.empty(), "game file " + std::string(shader.name) + ".hlsl is in the folder");
    if (found.empty()) continue;
    const sp::VariantTransformResult result = sp::TransformGlowSoftHlsl(shader, ReadFile(found));
    Check(result.ok, "game file " + found.string() + ": " + result.error);
    if (!result.ok) continue;
    std::printf("game file %s: edited, %zu bytes\n", std::string(shader.name).c_str(), result.hlsl.size());
    if (!results.empty()) {
      std::filesystem::create_directories(results);
      std::ofstream(results / (std::string(shader.name) + ".hlsl"), std::ios::binary) << result.hlsl;
    }
  }
  if (!results.empty()) std::printf("the two texts are in %s\n", results.string().c_str());
}

}  // namespace

int main(int argc, char** argv) {
  CheckFixtures();
  if (argc > 1) {
    CheckGameFiles(argv[1], argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path());
  }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
