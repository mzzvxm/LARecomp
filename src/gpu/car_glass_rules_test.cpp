// Check of the player's car windows reflecting the city (car_glass_rules.h, CITY-RT M2 R3b): the Fresnel, the primary ray's reach, the colour a pane adds, how a hit becomes a colour, the dispatch's
// rectangle, the decision of a frame, the apply pass's arithmetic (its twin of hlsl/car_glass_apply_ps.hlsl: taps kept inside the rectangle, the mean over the panes, the fade-in of the coverage) and,
// given the src folder, that the shaders, the host, the CMake files and the settings hold the header's numbers and are wired as the design says.
// A program of its own: it needs no renderer, no ReXGlue and no game files, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_car_glass_rules_test, then run src/gpu/larecomp_car_glass_rules_test [<src dir>]
//   (the src folder is the one that holds gpu and shaderpack; by default none: the text checks are skipped)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "car_glass_rules.h"

namespace cg = mcla::gpu::car_glass;
namespace opp = mcla::gpu::car_rt_opp_refl;
namespace ci = mcla::gpu::car_rt_instances;
namespace cr = mcla::gpu::city_refl;
namespace rr = mcla::gpu::car_rt_rules;

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

bool Near(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance * std::max(1.0, std::fabs(b)); }

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// The number a `static const <type> <name> = <value>;` line of an HLSL text holds (u or f suffix and a trailing .0 allowed); NaN when the line is not there.
double HlslNumber(const std::string& text, const std::string& name) {
  const std::regex re("(?:uint|float|int)\\s+" + name + "\\s*=\\s*(-?[0-9.]+)[uf]?\\s*;");
  std::smatch m;
  if (!std::regex_search(text, m, re)) {
    return std::nan("");
  }
  return std::stod(m[1].str());
}

cg::Texel Pane(float r, float g, float b) {
  cg::Texel t;
  t.rgb[0] = r;
  t.rgb[1] = g;
  t.rgb[2] = b;
  t.alpha = 1.0f;
  return t;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string src = argc > 1 ? argv[1] : "";

  // ---- the parts and the numbers -------------------------------------------------------------------------------------------------------------
  Check(cg::kPrimaryParts == 0x02 && cg::kPrimaryParts == mcla::gpu::car_rt_refl::kPartGlass, "the primary ray finds the player's glass alone (instance mask 0x02)");
  Check(cg::kF0 == 0.04f && cg::kPower == 5.0f && cg::kMaxFresnel == 0.6f && cg::kStrength == 1.0f, "the glass: F0 0.04, power 5, at most 0.6, strength 1");
  Check(cg::kDepthSlack == 0.02f && cg::kDepthSlackRel == 0.01f && cg::kFarDepth == 10000.0f && cg::kCapUnits == 64.0f && cg::kApplyCoverageFrom == 0.5f && cg::kRectMargin == 4 && cg::kGroup == 8,
        "the depth slack 2 cm + 1%, far 10 km, the cap 64, the coverage's start 0.5, margin 4 texels, groups of 8");

  // ---- the Fresnel ---------------------------------------------------------------------------------------------------------------------------
  Check(Near(cg::Fresnel(1.0f), 0.04), "facing the pane: 4%");
  Check(Near(cg::Fresnel(0.5f), 0.04 + 0.96 * std::pow(0.5, 5.0)), "N.V 0.5: Schlick's 7%");
  Check(cg::Fresnel(0.3f) > 0.19f && cg::Fresnel(0.3f) < 0.21f, "N.V 0.3: about 20%");
  Check(cg::Fresnel(0.0f) == cg::kMaxFresnel && cg::Fresnel(-1.0f) == cg::kMaxFresnel && cg::Fresnel(2.0f) == cg::kF0, "a grazing view is capped at 0.6, a nonsense N.V is clamped");
  {
    bool monotonic = true;
    float last = 0.0f;
    for (int i = 100; i >= 0; --i) {
      const float f = cg::Fresnel(float(i) / 100.0f);
      monotonic &= f >= last && f >= cg::kF0 && f <= cg::kMaxFresnel;
      last = f;
    }
    Check(monotonic, "the Fresnel only rises as the view grazes, between F0 and the cap");
  }

  // ---- the primary ray's reach -----------------------------------------------------------------------------------------------------------------
  Check(Near(cg::PrimaryReach(10.0f), 10.12) && Near(cg::PrimaryReach(0.0f), 0.02) && cg::PrimaryReach(1.0e9f) == cg::kFarDepth, "the ray is cast as far as the picture's depth plus 2 cm and 1%, at most the far limit");

  // ---- the colour a pane adds --------------------------------------------------------------------------------------------------------------------
  {
    const float colour[3] = {2.0f, 1.0f, -3.0f};
    float out[3];
    cg::AddedColour(colour, 1.0f, 1.0f, 1.0f, 16.0f, out);
    Check(Near(out[0], 2.0 * 0.04) && Near(out[1], 0.04) && out[2] == 0.0f, "facing the pane, a full hit adds 4% of its colour; a negative colour adds nothing");
    cg::AddedColour(colour, 0.2f, 0.5f, 2.0f, 16.0f, out);
    Check(Near(out[0], 2.0 * cg::Fresnel(0.2f) * 0.5 * 2.0), "the Fresnel, the weight and the strength multiply");
    const float lamp[3] = {1.0e6f, 0.0f, 0.0f};
    cg::AddedColour(lamp, 0.0f, 1.0f, 1.0f, 16.0f, out);
    Check(out[0] == cg::kCapUnits * 16.0f, "a lamp is held to the paint's cap in the history's units (64 x 2^4)");
    cg::AddedColour(colour, 0.5f, 0.0f, 1.0f, 16.0f, out);
    Check(out[0] == 0.0f && out[1] == 0.0f, "a hit of weight 0 adds nothing");
  }

  // ---- a hit becomes a colour ----------------------------------------------------------------------------------------------------------------------
  {
    const float seen[3] = {1.0f, 0.5f, 0.25f};
    const float hidden[3] = {0.2f, 0.2f, 0.2f};
    float colour[3];
    float weight = 0.0f;
    cg::HitColour(false, 0.8f, 1.0f, seen, hidden, 0.85f, colour, &weight);
    Check(Near(weight, 0.8) && colour[0] == 1.0f && colour[1] == 0.5f, "the city (visible 1) or a part of the car the history shows: its colour, the fade's weight");
    cg::HitColour(false, 0.8f, 0.25f, seen, hidden, 0.85f, colour, &weight);
    Check(Near(weight, 0.2), "a part of the player's own car the history shows a quarter of: a quarter of the weight");
    cg::HitColour(false, 0.8f, 0.0f, seen, hidden, 0.85f, colour, &weight);
    Check(weight == 0.0f, "a part the history does not show: no weight (a pane has no own surface in shade)");
    cg::HitColour(true, 1.0f, 0.0f, seen, hidden, 0.85f, colour, &weight);
    Check(Near(weight, 0.85) && Near(colour[0], 0.2), "another car the history does not show: its stand-in colour at the hidden weight");
    cg::HitColour(true, 1.0f, 1.0f, seen, hidden, 0.85f, colour, &weight);
    Check(Near(weight, 1.0) && Near(colour[0], 1.0), "another car the history shows in full: its colour, whole weight");
    cg::HitColour(true, 1.0f, 0.5f, seen, hidden, 0.85f, colour, &weight);
    Check(Near(colour[0], 0.6) && Near(weight, 0.85 + 0.15 * 0.5), "another car half shown: the blend of the two");
  }

  // ---- the rectangle ---------------------------------------------------------------------------------------------------------------------------------
  {
    ci::ViewBox box;
    const float lo[3] = {-1.0f, -1.0f, 6.0f}, hi[3] = {1.0f, 0.5f, 10.0f};
    box.Add(lo);
    box.Add(hi);
    const double tan_x = 0.9, tan_y = 0.5;
    const opp::TexelRect r = cg::RectFor(box, tan_x, tan_y, 1920.0, 1080.0, 1.0, 1920, 1080);
    const opp::TexelRect bare = opp::BoxRect(box, tan_x, tan_y, 1920.0, 1080.0, 1.0, 1920, 1080, 0);
    Check(!r.Empty() && r.x0 == std::max(0, bare.x0 - cg::kRectMargin) && r.x1 == std::min(1920, bare.x1 + cg::kRectMargin) && r.y0 == std::max(0, bare.y0 - cg::kRectMargin) &&
              r.y1 == std::min(1080, bare.y1 + cg::kRectMargin),
          "the rectangle is the box's, kRectMargin texels larger, clamped to the image");
    const opp::TexelRect half = cg::RectFor(box, tan_x, tan_y, 3840.0, 2160.0, 2.0, 1920, 1080);
    Check(!half.Empty() && Near(double(half.Width()), double(r.Width()), 0.05) && half.x1 <= 1920 && half.y1 <= 1080, "at 4K with the half size image the rectangle covers about the same texels");
    ci::ViewBox behind;
    const float blo[3] = {-1.0f, -1.0f, -10.0f}, bhi[3] = {1.0f, 1.0f, -6.0f};
    behind.Add(blo);
    behind.Add(bhi);
    Check(cg::RectFor(behind, tan_x, tan_y, 1920.0, 1080.0, 1.0, 1920, 1080).Empty(), "a box behind the camera: nothing to trace");
    ci::ViewBox across;
    const float alo[3] = {-1.0f, -1.0f, -2.0f}, ahi[3] = {1.0f, 1.0f, 2.0f};
    across.Add(alo);
    across.Add(ahi);
    const opp::TexelRect whole = cg::RectFor(across, tan_x, tan_y, 1920.0, 1080.0, 1.0, 1920, 1080);
    Check(whole.x0 == 0 && whole.y0 == 0 && whole.x1 == 1920 && whole.y1 == 1080, "a box across the camera's plane has no finite rectangle: the whole image");
    Check(cg::RectFor(ci::ViewBox{}, tan_x, tan_y, 1920.0, 1080.0, 1.0, 1920, 1080).Empty(), "an invalid box: empty");
  }

  // ---- the decision --------------------------------------------------------------------------------------------------------------------------------
  {
    cg::Facts f;
    f.wanted = f.city_on = f.screen = f.box = f.ready = true;
    Check(cg::Decide(f) == cg::Why::kUsed, "every fact in: the pass runs");
    cg::Facts g = f;
    g.wanted = false;
    g.city_on = false;
    Check(cg::Decide(g) == cg::Why::kSettingOff, "not wanted: the setting (it is the first reason)");
    g = f;
    g.city_on = false;
    Check(cg::Decide(g) == cg::Why::kNoCity, "no city rows");
    g = f;
    g.screen = false;
    Check(cg::Decide(g) == cg::Why::kNoScreen, "no lin_depth: nothing tells a pane from what is in front of it");
    g = f;
    g.box = false;
    Check(cg::Decide(g) == cg::Why::kNoBox, "no box on the picture");
    g = f;
    g.glass_instance = false;
    Check(cg::Decide(g) == cg::Why::kNoMask, "no glass instance in the top level");
    g = f;
    g.ready = false;
    Check(cg::Decide(g) == cg::Why::kNotReady, "pipelines not ready");
    bool named = true;
    for (uint32_t w = 0; w <= uint32_t(cg::Why::kNoTarget); ++w) {
      named &= std::string(cg::WhyName(cg::Why(w))) != "?";
    }
    Check(named, "every reason has a name");
  }

  // ---- the facts the host makes of an ordinary frame (CITY-M2-PICS: Trace left `box` out, so the pass never ran) -------------------------------------
  {
    cg::HostInputs in;
    in.wanted = in.city_set = in.city_on = in.screen = in.view_valid = in.view_set = true;
    in.rect_empty = false;
    Check(cg::Decide(cg::HostFacts(in)) == cg::Why::kUsed, "an ordinary frame (the setting on, the city's rows, lin_depth, the player's box on the picture, a valid view): the pass runs");
    cg::HostInputs g = in;
    g.rect_empty = true;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoBox, "an empty rectangle: no box");
    g = in;
    g.view_valid = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoBox, "no valid view: no box");
    g = in;
    g.view_set = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoBox, "a view without its descriptor set: no box");
    g = in;
    g.city_set = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoCity, "no city set: the city is not in the frame");
    g = in;
    g.city_on = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoCity, "the city's rows off: the city is not in the frame");
    g = in;
    g.screen = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kNoScreen, "no lin_depth");
    g = in;
    g.wanted = false;
    Check(cg::Decide(cg::HostFacts(g)) == cg::Why::kSettingOff, "not wanted");
    // the default inputs (nothing known) must NOT run: a field left out of the host's list shows here as a pass that cannot run, not one that always does
    Check(cg::Decide(cg::HostFacts(cg::HostInputs{})) != cg::Why::kUsed, "nothing known: the pass does not run");
  }

  // ---- the apply pass's arithmetic -----------------------------------------------------------------------------------------------------------------
  Check(sizeof(cg::ApplyPush) == 48, "the apply's push block is 48 bytes (the host passes' range is 64)");
  {
    // the pixel's place: the pixel that is texel i's own (i x divisor + floor(divisor / 2)) is exactly on texel i
    bool exact = true;
    for (const int divisor : {1, 2}) {
      for (int i = 0; i < 10; ++i) {
        int base;
        float frac;
        cg::TapPlace(i * divisor + divisor / 2, divisor, &base, &frac);
        exact &= base == i && frac == 0.0f;
      }
    }
    Check(exact, "texel i stands for the pixel i x divisor + floor(divisor / 2): the pixel is exactly on it (VisibleAt's mapping)");
    int base;
    float frac;
    cg::TapPlace(4, 2, &base, &frac);
    Check(base == 1 && Near(frac, 0.5), "at the half size the pixel between two texels is half on each");
  }
  {
    // divisor 1: pixel exact. A 3x3 pane inside an 8x8 rectangle; the texels outside the pane hold nothing.
    const int rect[4] = {0, 0, 8, 8};
    const auto at = [](int x, int y) { return (x >= 2 && x <= 4 && y >= 2 && y <= 4) ? Pane(0.4f, 0.2f, 0.1f) : cg::Texel{}; };
    float out[3];
    cg::ApplyPixel(3, 3, 1, rect, at, out);
    Check(Near(out[0], 0.4) && Near(out[1], 0.2) && Near(out[2], 0.1), "divisor 1: a pixel on a pane adds the pane's colour, whole");
    cg::ApplyPixel(5, 3, 1, rect, at, out);
    Check(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "divisor 1: the pixel beside the pane adds nothing: no halo");
    cg::ApplyPixel(2, 2, 1, rect, at, out);
    Check(Near(out[0], 0.4), "divisor 1: the pane's corner pixel is a whole pane");
  }
  {
    // divisor 2: a pane block 4x4 texels with a black pane texel (no reflection) at (5, 5): the mean is over the panes' texels, not diluted by the texels beside the pane.
    const int rect[4] = {0, 0, 16, 16};
    const auto at = [](int x, int y) {
      if (x >= 4 && x <= 7 && y >= 4 && y <= 7) {
        return (x == 5 && y == 5) ? Pane(0.0f, 0.0f, 0.0f) : Pane(0.8f, 0.8f, 0.8f);
      }
      return cg::Texel{};
    };
    float out[3];
    cg::ApplyPixel(11, 11, 2, rect, at, out);  // exactly on the black pane texel (5, 5)
    Check(out[0] == 0.0f, "divisor 2: a pixel on a pane texel that reflects nothing adds nothing");
    cg::ApplyPixel(12, 11, 2, rect, at, out);  // half on (5, 5) black, half on (6, 5)
    Check(Near(out[0], 0.4), "divisor 2: the mean over the panes' texels (black and 0.8: 0.4)");
    cg::ApplyPixel(14, 14, 2, rect, at, out);  // texels (6, 6) (7, 6) (6, 7) (7, 7): all pane of one colour
    Check(Near(out[0], 0.8), "divisor 2: a pixel whose four texels are all pane of one colour: that colour");
    cg::ApplyPixel(16, 14, 2, rect, at, out);  // texels 7 (pane) and 8 (none) at half: coverage 0.5, where the fade-in starts
    Check(out[0] == 0.0f, "divisor 2: a pixel half on the pane's edge adds nothing (coverage 0.5 is where the fade-in starts)");
    cg::ApplyPixel(2, 2, 2, rect, at, out);
    Check(out[0] == 0.0f, "divisor 2: no pane under the pixel: nothing");
    // three quarters of the four taps are pane: half the fade-in
    const auto corner = [](int x, int y) { return (x <= 8 && y <= 8 && !(x == 8 && y == 8)) ? Pane(0.8f, 0.8f, 0.8f) : cg::Texel{}; };
    cg::ApplyPixel(16, 16, 2, rect, corner, out);  // taps (7, 7) (8, 7) (7, 8) (8, 8): the last is none
    Check(Near(out[0], 0.8 * 0.5), "divisor 2: three quarters of the pane under the pixel: half the colour (the fade-in), the colour of the panes' mean");
  }
  {
    // taps are kept inside the rectangle and the texels outside it are never read (they hold the frames before's)
    const int rect[4] = {4, 4, 12, 12};
    bool outside_read = false;
    const auto at = [&](int x, int y) {
      if (x < 4 || y < 4 || x > 11 || y > 11) {
        outside_read = true;
        return Pane(9.0f, 9.0f, 9.0f);  // a stale texel
      }
      return Pane(0.3f, 0.3f, 0.3f);
    };
    float out[3];
    for (int py = 0; py < 30; ++py) {
      for (int px = 0; px < 30; ++px) {
        for (const int divisor : {1, 2}) {
          cg::ApplyPixel(px, py, divisor, rect, at, out);
          if (out[0] > 0.31f) {
            outside_read = true;
          }
        }
      }
    }
    Check(!outside_read, "no tap reads a texel outside the rectangle, at either divisor, for every pixel near and far from it");
    cg::ApplyPixel(2, 2, 1, rect, at, out);
    Check(out[0] == 0.0f && out[1] == 0.0f, "a pixel outside the rectangle adds nothing even where its clamped taps are pane");
    cg::ApplyPixel(8, 8, 1, rect, at, out);
    Check(Near(out[0], 0.3), "a pixel inside the rectangle on a pane adds its colour");
  }

  // ---- the memory and the log ------------------------------------------------------------------------------------------------------------------------
  Check(cg::HeldBytes(1920, 1080) == uint64_t(1920) * 1080 * 8, "the glass image is 8 bytes a texel (RGBA16F)");
  {
    cg::Window w;
    w.first_frame = 10;
    w.passes = 300;
    w.texels = 300ull * 200000ull;
    w.not_run[uint32_t(cg::Why::kNoScreen)] = 7;
    const std::string text = cg::WindowText(w, 400);
    Check(Has(text, "[car glass] frames 10..400: 300 passes, the rectangle 200000 texels on average;") && Has(text, "no lin_depth 7,"), "the window's log line: " + text);
  }

  // ---- the files -----------------------------------------------------------------------------------------------------------------------------------------
  if (src.empty()) {
    std::printf("no src folder: the text checks are skipped\n");
  } else {
    const std::string glass_hlsli = ReadFile(src + "/gpu/hlsl/car_rt_refl_glass.hlsli");
    const std::string apply_ps = ReadFile(src + "/gpu/hlsl/car_glass_apply_ps.hlsl");
    const std::string refl = ReadFile(src + "/gpu/hlsl/car_rt_refl_cs.hlsl");
    const std::string host = ReadFile(src + "/gpu/car_rt_glass_refl.cpp");
    const std::string host_h = ReadFile(src + "/gpu/car_rt_glass_refl.h");
    const std::string player = ReadFile(src + "/gpu/car_rt_refl.cpp");
    const std::string player_h = ReadFile(src + "/gpu/car_rt_refl.h");
    const std::string selfrefl = ReadFile(src + "/gpu/car_selfrefl.cpp");
    const std::string city_host = ReadFile(src + "/gpu/city_refl.cpp");
    const std::string pipelines = ReadFile(src + "/gpu/pipelines.cpp");
    const std::string cmake = ReadFile(src + "/gpu/CMakeLists.txt");
    const std::string shaders_h = ReadFile(src + "/gpu/host_shaders.h");
    const std::string settings = ReadFile(src + "/settings/generated/settings_gpu.h");
    const std::string manifest = ReadFile(src + "/settings/generated/manifest.json");
    const std::string meta = ReadFile(src + "/settings/generated/settings_meta.inc");
    if (glass_hlsli.empty() || apply_ps.empty() || refl.empty() || host.empty() || host_h.empty() || player.empty() || selfrefl.empty() || city_host.empty() || cmake.empty()) {
      std::printf("the sources were not found under %s: the text checks are skipped\n", src.c_str());
    } else {
      // the shader's numbers are the header's
      Check(Near(HlslNumber(glass_hlsli, "kGlassF0"), double(cg::kF0), 1e-6) && Near(HlslNumber(glass_hlsli, "kGlassPower"), double(cg::kPower), 1e-6) &&
                Near(HlslNumber(glass_hlsli, "kGlassMaxFresnel"), double(cg::kMaxFresnel), 1e-6) && Near(HlslNumber(glass_hlsli, "kGlassDepthSlack"), double(cg::kDepthSlack), 1e-6) &&
                Near(HlslNumber(glass_hlsli, "kGlassDepthSlackRel"), double(cg::kDepthSlackRel), 1e-6) && Near(HlslNumber(glass_hlsli, "kGlassFarDepth"), double(cg::kFarDepth), 1e-6) &&
                Near(HlslNumber(glass_hlsli, "kGlassCapUnits"), double(cg::kCapUnits), 1e-6),
            "hlsl/car_rt_refl_glass.hlsli: the numbers are the rules' (F0, power, cap, slack, far, 64)");
      Check(Has(glass_hlsli, "return min(kGlassF0 + (1.0 - kGlassF0) * pow(saturate(1.0 - nv), kGlassPower), kGlassMaxFresnel);"), "hlsl/car_rt_refl_glass.hlsli: the Fresnel is Schlick's, capped");
      Check(Has(glass_hlsli, "primary.TMax = min(depth * (1.0 + kGlassDepthSlackRel) + kGlassDepthSlack, kGlassFarDepth);") && Has(glass_hlsli, "TraceRayInline(gCarRtTlas, RAY_FLAG_NONE, g_push.slots.z, primary)"),
            "hlsl/car_rt_refl_glass.hlsli: the primary ray is cast as far as PrimaryReach says, with the push's mask (the host gives it the glass bit)");
      Check(Has(glass_hlsli, "colour = hidden + (seen - hidden) * visible;") && Has(glass_hlsli, "weight = fade * (g_push.tune.y + (1.0 - g_push.tune.y) * visible);") && Has(glass_hlsli, "float weight = fade * visible;"),
            "hlsl/car_rt_refl_glass.hlsli: a hit becomes a colour as HitColour does");
      Check(Has(glass_hlsli, "min(NgHeld3(colour, kMaxColour) * (GlassFresnel(nv) * weight * strength), float3(cap, cap, cap))") && Has(glass_hlsli, "const float cap = kGlassCapUnits * g_push.up.w;") &&
                Has(glass_hlsli, "return float4(add, 1.0);"),
            "hlsl/car_rt_refl_glass.hlsli: the term is AddedColour (cap in the history's units), the texel's alpha 1");
      Check(Has(glass_hlsli, "if (nv < kMinNv)") && Has(glass_hlsli, "(kCityFlagOn | kCityFlagScreen)") && Has(glass_hlsli, "TraceRay(history, origin, mirror, g_push.lens.w, 0.0,"),
            "hlsl/car_rt_refl_glass.hlsli: the city's rows must be on with the picture's depth, a grazing view adds nothing, one mirror ray (cone 0) through TraceRay");
      Check(Has(glass_hlsli, "const uint2 texel = id.xy + g_push.glass.xy;") && Has(glass_hlsli, "if (any(texel >= size))"), "hlsl/car_rt_refl_glass.hlsli: the dispatch covers the rectangle from the push's glass.xy");
      Check(Has(apply_ps, "kGlassApplyCoverageFrom = 0.5;") && Near(HlslNumber(apply_ps, "kGlassApplyCoverageFrom"), double(cg::kApplyCoverageFrom), 1e-6) && Has(apply_ps, "if (coverage < kGlassApplyCoverageFrom)") &&
                Has(apply_ps, "colour * saturate((coverage - kGlassApplyCoverageFrom) / (1.0 - kGlassApplyCoverageFrom))") && Has(apply_ps, "const int2 texel = clamp(base + offset, lo, hi);") &&
                Has(apply_ps, "if (any(own < lo) || any(own > hi))") && Has(apply_ps, "const float2 f = (float2(p) - floor(divisor * 0.5)) / divisor;"),
            "hlsl/car_glass_apply_ps.hlsl: the arithmetic is ApplyPixel's (the place, the taps kept in the rectangle, the coverage's fade-in)");
      Check(Has(apply_ps, "uint4 image;") && Has(apply_ps, "uint4 rect;") && Has(apply_ps, "float4 sizes;") && Has(apply_ps, "register(b0, space4)"), "hlsl/car_glass_apply_ps.hlsl: the push block is ApplyPush (48 bytes, space 4)");
      // the compute file: the glass variant has its own entry point and push member
      Check(Has(refl, "#ifdef CAR_RT_REFL_GLASS") && Has(refl, "uint4 glass;") && Has(refl, "#include \"car_rt_refl_glass.hlsli\"") && Has(refl, "#define main CarRtReflPlayerMain  // the glass variant"),
            "hlsl/car_rt_refl_cs.hlsl: the glass variant (CAR_RT_REFL_GLASS) has its push member and its entry point, the player's main renamed away");
      // the host
      Check(Has(host, "HostPrecache::Key(HostPrecache::kCarRtRefl, 3)") && Has(host, "HostPrecache::Key(HostPrecache::kCarRtRefl, 4 + index)"), "car_rt_glass_refl.cpp: the keys beside the trace's (0), the denoise's (1) and the opponents' (2): 3 and 4 to 7");
      Check(Has(host_h, "static_assert(sizeof(Push) == 112);") && Has(host_h, "uint32_t glass[4];") && Has(host, "push.slots[2] = glass::kPrimaryParts;") && Has(host, "push.slots[3] &= ~car_rt_hit::census::kStatsFlag;"),
            "car_rt_glass_refl: the push block is the opponents' size (112), the primary ray's parts are the glass, the paint's counters are not touched");
      Check(Has(host, "blend.srcBlend = plume::RenderBlend::ONE;") && Has(host, "blend.dstBlend = plume::RenderBlend::ONE;") && Has(host, "blend.renderTargetWriteMask = kWriteRgb;") &&
                Has(host, "passes_->CreatePipeline(passes_->layout(), *apply_shader_, kApplyFormat, blend, 1u << index, ApplyName(index))"),
            "car_rt_glass_refl.cpp: the apply adds (ONE, ONE) on the colour channels, one pipeline for each sample count");
      Check(Has(host, "glass::Decide(glass::HostFacts(inputs))") && Has(host, "inputs.rect_empty = rect.Empty();") && Has(host, "inputs.view_valid = view.valid;") && !Has(host, "glass::Facts facts;"),
            "car_rt_glass_refl.cpp: Trace asks Decide() with HostFacts (every fact of the frame, the box included), not a Facts it fills by hand (CITY-M2-PICS: the pass never ran)");
      Check(Has(host, "glass::RectFor(view.player_box,") && Has(host, "car_rt_opp_refl::GroupsX(rect, kGroup)") && Has(host, "pass_timing_->Mark(list, \"car glass refl\")") && Has(host, "pass_timing_->Mark(list, \"car glass apply\")"),
            "car_rt_glass_refl.cpp: the dispatch covers the player's box, the passes are timed");
      Check(Has(player, "glass_.Trace(windows);") && Has(player, "windows.wanted = city_.glass_wanted();") && Has(player, "glass_.Initialize(device, passes, heap, frames, pass_timing, *scene, output_builder_, city_.Builder(),") &&
                Has(player, "glass_.NewFrame();") && Has(player, "glass_.ReleaseImage();") && Has(player_h, "bool ApplyGlass(") && Has(player_h, "uint32_t colour_samples = 1;"),
            "car_rt_refl: the glass trace is recorded after the opponents' dispatch, with the city's wish, and the image goes back with the others");
      Check(Has(player, "opp_.Record(opponents);\n  }\n  // The player's windows") && player.find("glass_.Trace(windows);") < player.find("city_.StatsEnd(list, info.frame);"), "car_rt_refl: the glass trace is in the same stats frame, before the city's counters are copied");
      Check(Has(selfrefl, "rt_->ApplyGlass(list, *edram_, *colour, render_w, render_h);") && Has(selfrefl, "info.colour_samples = depth.host_samples;") && selfrefl.find("rt_->OnHistory(list, info);") < selfrefl.find("rt_->ApplyGlass("),
            "car_selfrefl.cpp: the additive pass follows the history and the trace, with the main colour target");
      Check(Has(city_host, "glass_wanted_ = cr::Serves(cell_.level, cr::kClassGlass) && larecomp::settings::mcla_gpu_city_refl_glass.Get();") &&
                Has(city_host, "g_chrome_wanted.store(cr::Serves(cell_.level, cr::kClassChrome) && larecomp::settings::mcla_gpu_city_refl_chrome.Get(), std::memory_order_relaxed);"),
            "city_refl.cpp: the glass and the chrome are wanted by the level's served classes and their own debug switches");
      Check(Has(pipelines, "if (has(sp::kFeatureCarChromeCity) && CityReflChromeWanted())") && Has(pipelines, "want(sp::kFeatureCarChromeCity);"), "pipelines.cpp: the chrome's city stage is wanted with the chrome's and while the city is in the reflections");
      // the build and the settings
      Check(Has(cmake, "larecomp_gpu_host_shader_rt(car_rt_refl_glass_cs cs_6_5 SOURCE car_rt_refl_cs DEFINES CAR_RT_REFL_GLASS=1)") && Has(cmake, "larecomp_gpu_host_shader(car_glass_apply_ps ps_6_0)") && Has(cmake, "car_rt_glass_refl.cpp"),
            "CMakeLists.txt: the glass variant, the apply's pixel shader and the host file are built");
      Check(Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(car_rt_refl_glass_cs)") && Has(shaders_h, "MCLA_GPU_HOST_SHADER_BLOB(car_glass_apply_ps)") && Has(shaders_h, "#include \"car_rt_refl_glass_cs.hlsl.dxil.h\"") &&
                Has(shaders_h, "#include \"car_glass_apply_ps.hlsl.spirv.h\""),
            "host_shaders.h: both blobs, for both backends");
      for (const char* name : {"mcla_gpu_city_refl_chrome", "mcla_gpu_city_refl_glass"}) {
        Check(Has(settings, std::string("REX_SETTING(bool, ") + name + ", true,") && Has(settings, std::string("inline auto& ") + name + " = g_gpu." + name + ";") && Has(manifest, std::string("\"") + name + "\": {") &&
                  Has(meta, std::string("{\"") + name + "\", \"MCLA/GPU\"},"),
              std::string(name) + ": a live bool, true by default, in the settings, the manifest and the meta list");
      }
      Check(!Has(host, "StatAdd") && !Has(glass_hlsli, "StatAdd"), "the glass pass has no counters of its own");
    }
  }

  // ---- the city's served classes ----------------------------------------------------------------------------------------------------------------------
  Check(cr::Serves(cr::Level::kHigh, cr::kClassGlass) && cr::Serves(cr::Level::kUltra, cr::kClassGlass) && !cr::Serves(cr::Level::kOff, cr::kClassGlass), "the glass is served at high and ultra, not at off");
  Check(rr::kMaskGlass == cg::kPrimaryParts, "the instance mask the primary ray takes is the scene's glass bit");

  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
