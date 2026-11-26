// Check of the occlusion's rule for a car's paint (ao_paint.h). A program of its own: it needs no renderer, no ReXGlue and no GPU, and
// exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_ao_paint_test          (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_ao_paint_test [<src/gpu dir>] [--print]
//
// With the src/gpu folder as its argument it also checks that the shader's and ao.cpp's constants are the header's (they repeat them) and
// that the flags the code rides in overlap none of the apply's. With --print it lists the table the header quotes.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "ao_paint.h"
#include "setting_source.h"
#include "ssgi_light.h"

using namespace mcla::gpu;
using namespace mcla::gpu::ao_paint;

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
std::string Num(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[240];
  std::snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}
bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// The looks of ao.cpp (kLooks): the power of the visibility (the third number) and the direct share (the fourth).
struct Look {
  const char* name;
  float power;
  float direct;
};
constexpr Look kLooks[] = {{"light", 3.2f, 0.25f}, {"default", 3.6f, 0.375f}, {"strong", 4.0f, 0.5f}, {"max", 4.5f, 0.75f}};

// ---------------------------------------------------------------------------
// the code
// ---------------------------------------------------------------------------
void CheckCode() {
  Check(kCodeBits == 4 && kCodeShift == 17 && kCodeMask == (0xFu << 17), "the code is four bits at bit 17");
  for (uint32_t code = 0; code < 16; ++code) {
    const float power = PowerOf(code);
    Check(power > 0.0f && power <= 1.0f, Num("the power of code %.0f is in (0, 1]", double(code)));
    Check(CodeFor(double(power)) == code, Num("the code %.0f round trips through its power", double(code)));
  }
  Check(PowerOf(15) == 1.0f && !Relaxes(15), "code 15 is the power 1, the world's darkening: off");
  Check(PowerOf(0) == 0.0625f && Relaxes(0), "code 0 is the smallest power, 1/16");
  Check(PowerOf(kDefaultCode) == kDefaultPower && kDefaultPower == 0.3125f, "the default code is 4 and its power 0.3125 (5/16)");
  Check(CodeFor(double(kDefaultPower)) == kDefaultCode, "the setting's default gives the default code");
  Check(CodeFor(0.3) == 4 && CodeFor(0.34) == 4 && CodeFor(0.35) == 5, "the setting is rounded to the nearest sixteenth");
  Check(CodeFor(0.0) == 0 && CodeFor(-3.0) == 0 && CodeFor(1.0) == 15 && CodeFor(7.0) == 15, "the setting is kept between 1/16 and 1");
  Check(CodeFor(std::numeric_limits<double>::quiet_NaN()) == kDefaultCode && CodeFor(std::numeric_limits<double>::infinity()) == kDefaultCode,
        "a setting that is not a number is the default");
  // The flags the code rides in must overlap none of the apply's: view 0-7, the bounce light's limit 8-15, low 16, the contact image at the
  // picture's size 21, the apply's terms 22-24, the contact filter 25, the cap 26-31 (ao.cpp, ssgi_light.h).
  const uint32_t others = 0xFFu | ssgi_light::kLimitMask | (1u << 16) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | ssgi_light::kCapMask;
  Check((kCodeMask & others) == 0u, "the code's bits overlap none of the apply's flags");
  // They are the place of the denoise's radius (17-19) and bit 20, which no pass but the temporal one reads, and the apply is not one.
  Check((kCodeMask & (0x7u << 17)) == (0x7u << 17) && (kCodeMask & (1u << 20)) != 0u, "the code takes the denoise's radius bits and bit 20");
}

// ---------------------------------------------------------------------------
// the rule
// ---------------------------------------------------------------------------
void CheckRule() {
  // The world is not touched, bit for bit, whatever the code: no vehicle in the neighbourhood, and the road beside a car, whose
  // neighbourhood is at most half vehicle (the contact line that grounds the car is there).
  bool exact = true;
  for (uint32_t code = 0; code < 16; ++code) {
    for (float ao = 0.0f; ao <= 1.0001f; ao += 0.01f) {
      for (float share : {-1.0f, 0.0f, 0.125f, 0.25f, 0.4f, 0.5f}) {
        exact = exact && PaintVisibility(ao, share, code) == ao;
      }
    }
  }
  Check(exact, "a pixel with at most half of its neighbourhood vehicle takes the stored visibility as it is, for every code");
  Check(PaintVisibility(0.2f, 0.52f, kDefaultCode) > 0.2f, "a pixel with over half of its neighbourhood vehicle starts to take the paint's rule");
  // Off is off on a car as well.
  exact = true;
  for (float ao = 0.0f; ao <= 1.0001f; ao += 0.01f) {
    exact = exact && PaintVisibility(ao, 1.0f, kCodeOff) == ao && PaintVisibility(ao, 0.5f, kCodeOff) == ao;
  }
  Check(exact, "code 15 leaves a vehicle's pixel as it was");
  // On a car: never darker than the world's, never over 1, in step with the stored value (a darker stored value never reads lighter), and
  // the open sky (1) and a fully occluded value (0) stay where they are.
  for (uint32_t code = 0; code < kCodeOff; ++code) {
    bool lighter = true, bounded = true, monotone = true;
    float last = -1.0f;
    for (float ao = 0.0f; ao <= 1.0001f; ao += 0.005f) {
      const float seen = PaintVisibility(ao, 1.0f, code);
      lighter = lighter && seen >= ao - 1e-6f;
      bounded = bounded && seen <= 1.0f + 1e-6f && seen >= 0.0f;
      monotone = monotone && seen >= last - 1e-6f;
      last = seen;
    }
    Check(lighter && bounded && monotone, Num("code %.0f: the paint's visibility is at least the stored one, within 0 to 1, and rises with it", double(code)));
    Check(PaintVisibility(1.0f, 1.0f, code) == 1.0f && PaintVisibility(0.0f, 1.0f, code) == 0.0f, Num("code %.0f: 1 stays 1 and 0 stays 0", double(code)));
  }
  // A smaller power keeps more colour.
  for (float ao : {0.1f, 0.3f, 0.6f}) {
    bool ordered = true;
    for (uint32_t code = 1; code < 16; ++code) {
      ordered = ordered && PaintVisibility(ao, 1.0f, code - 1) >= PaintVisibility(ao, 1.0f, code) - 1e-6f;
    }
    Check(ordered, Num("a smaller power keeps more of the colour (stored visibility %.2f)", double(ao)));
  }
  // The silhouette: a straight blend between the world's value and the car's by the neighbourhood's vehicle share.
  {
    const float world = 0.3f, car = PaintVisibility(0.3f, 1.0f, kDefaultCode);
    bool blend = true;
    for (float share = 0.0f; share <= 1.0001f; share += 0.0625f) {
      const float on_car = std::clamp((share - kVehicleFrom) * 2.0f, 0.0f, 1.0f);
      blend = blend && Near(PaintVisibility(world, share, kDefaultCode), world + (car - world) * on_car, 1e-6);
    }
    Check(blend && car > world, "the silhouette blends the world's value and the car's from half a neighbourhood of vehicle to all of it");
    Check(PaintVisibility(0.3f, 7.0f, kDefaultCode) == car, "a vehicle share over 1 is 1");
  }
  // The deepest crease: the stored floor takes out about half of the colour at a full share, not all.
  {
    const float floor_seen = PaintVisibility(kStoredFloor, 1.0f, kDefaultCode);
    Check(Near(floor_seen, std::pow(0.1, 0.3125), 1e-6) && floor_seen > 0.48f && floor_seen < 0.5f, "the stored floor 0.1 reads as 0.49 on paint");
  }
}

// ---------------------------------------------------------------------------
// the owner's case: a black glossy panel, partly occluded
// ---------------------------------------------------------------------------
void CheckPanel() {
  const float share = 0.8f;
  const float raw[] = {0.95f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.3f};
  if (g_print) {
    std::printf("the colour a panel whose colour is all reflection keeps (the default look: power 3.6, share %.1f)\n", double(share));
    std::printf("  open share of the sky   stored   before   after (code %u)   after at a share of 1: before, after\n", kDefaultCode);
    for (float r : raw) {
      std::printf("  %5.2f                   %5.3f    %5.3f    %5.3f             %5.3f  %5.3f\n", double(r), double(StoredVisibility(r, 3.6f)),
                  double(PanelKept(r, 3.6f, share, kCodeOff)), double(PanelKept(r, 3.6f, share, kDefaultCode)), double(PanelKept(r, 3.6f, 1.0f, kCodeOff)),
                  double(PanelKept(r, 3.6f, 1.0f, kDefaultCode)));
    }
    std::printf("the four looks, a panel that sees 70%% of its sky, share %.1f: kept before / after\n", double(share));
    for (const Look& l : kLooks) {
      const float s = Share(0.25f, 1.0f, 0.15f, l.direct);  // a low sun on a boot lid: N.L 0.15
      std::printf("  %-8s power %.1f  stored %.3f  kept %.3f / %.3f   (share at La 0.25, Ls 1, N.L 0.15: %.3f -> %.3f / %.3f)\n", l.name, double(l.power),
                  double(StoredVisibility(0.7f, l.power)), double(PanelKept(0.7f, l.power, share, kCodeOff)), double(PanelKept(0.7f, l.power, share, kDefaultCode)),
                  double(s), double(PanelKept(0.7f, l.power, s, kCodeOff)), double(PanelKept(0.7f, l.power, s, kDefaultCode)));
    }
  }
  // The bug: a part that sees 70% of its sky lost over half of its colour, one that sees 80% nearly half.
  Check(PanelKept(0.7f, 3.6f, share, kCodeOff) < 0.45f && PanelKept(0.8f, 3.6f, share, kCodeOff) < 0.6f,
        "before: a panel that sees 70% of its sky kept under 45% of its colour, one that sees 80% under 60%");
  // The change: a partly open part keeps most of its reflection.
  Check(PanelKept(0.9f, 3.6f, share, kDefaultCode) > 0.9f, "now: a panel that sees 90% of its sky keeps over 90% of its colour");
  Check(PanelKept(0.8f, 3.6f, share, kDefaultCode) > 0.8f, "now: a panel that sees 80% of its sky keeps over 80% of its colour");
  Check(PanelKept(0.7f, 3.6f, share, kDefaultCode) > 0.7f && PanelKept(0.7f, 3.6f, share, kDefaultCode) < 0.8f,
        "now: a panel that sees 70% of its sky (the lid under a wing) keeps 70 to 80% of its colour, a dip of about a quarter");
  Check(PanelKept(0.6f, 3.6f, share, kDefaultCode) > 0.65f, "now: a panel that sees 60% of its sky (an upright between wing and lid) keeps over 65%");
  // A crease that is closed (the floor) still takes a lot: at a full share about half.
  Check(PanelKept(0.2f, 3.6f, 1.0f, kDefaultCode) < 0.51f && PanelKept(0.2f, 3.6f, 1.0f, kDefaultCode) > 0.48f, "now: a closed crease (the floor) at a full share keeps about half of the colour");
  // The default power takes the look's power out again: 3.6 * 0.3125 = 1.125, near the visibility itself.
  {
    const float product = 3.6f * kDefaultPower;
    Check(Near(product, 1.125, 1e-5), "the default look's power times the paint power is 1.125");
    // The painted value of a visibility of v is v ^ 1.125 for the default look (where the floor does not bite): within 0.03 of v for the
    // open parts (v from 0.6 to 1), which is what the specular occlusion of a clear coat comes to.
    bool near_v = true;
    for (float v = 0.6f; v <= 1.0001f; v += 0.05f) {
      near_v = near_v && Near(PaintVisibility(StoredVisibility(v, 3.6f), 1.0f, kDefaultCode), v, 0.06);
    }
    Check(near_v, "for the default look the paint's visibility is the raw one, within 0.06, from 60% open to 100%");
  }
  // The looks keep their order on paint (light darkens it least, max most), at every share, every visibility.
  {
    bool ordered = true;
    for (float r = 0.65f; r < 0.99f; r += 0.05f) {  // (under 0.6 the max look's stored floor makes the looks equal)
      float last = 2.0f;
      for (const Look& l : kLooks) {
        const float kept = PanelKept(r, l.power, 0.9f, kDefaultCode);
        ordered = ordered && kept < last;
        last = kept;
      }
    }
    Check(ordered, "on paint the four looks stay in order: light keeps the most colour, max the least");
  }
  // At a share of 0 nothing is taken, whatever the paint does (a pixel the sun lights fully is not darkened by the sky's share).
  Check(PanelKept(0.5f, 3.6f, 0.0f, kDefaultCode) == 1.0f, "a share of 0 takes nothing");
  // The painted factor is never darker than the world's would be and never darker than the floor's share of it: a paint pixel keeps at
  // least the floor's value at the share its colour is multiplied by.
  {
    bool ok = true;
    for (float r = 0.0f; r <= 1.0001f; r += 0.05f) {
      for (float s = 0.0f; s <= 1.0001f; s += 0.25f) {
        ok = ok && PanelKept(r, 3.6f, s, kDefaultCode) >= PanelKept(r, 3.6f, s, kCodeOff) - 1e-6f &&
             PanelKept(r, 3.6f, s, kDefaultCode) >= 1.0f - s * (1.0f - std::pow(0.1f, 0.3125f)) - 1e-6f;
      }
    }
    Check(ok, "the paint keeps at least what the world's factor would, and at least the floor's 0.49 at a full share");
  }
  // The world's factor is the shader's old line: lerp(1, ao, strength * share * fade), for every visibility, share and fade.
  {
    bool same = true;
    for (float ao = 0.0f; ao <= 1.0001f; ao += 0.1f) {
      for (float s = 0.0f; s <= 1.0001f; s += 0.25f) {
        for (float fade : {0.0f, 0.5f, 1.0f}) {
          const float old_factor = 1.0f + (ao - 1.0f) * std::clamp(1.0f * s * fade, 0.0f, 1.0f);
          same = same && OcclusionFactor(PaintVisibility(ao, 0.0f, kDefaultCode), 1.0f, s, fade) == old_factor;
        }
      }
    }
    Check(same, "the world's factor is the old line, bit for bit");
  }
  // The share's rule, as the shader has it: all ambient where there is no light, towards 1 with the direct number.
  Check(Share(0.3f, 1.0f, 0.0f, 0.0f) == 1.0f && Share(0.0f, 0.0f, 0.0f, 0.0f) == 1.0f, "the share is 1 with no sun on the surface or no light");
  Check(Near(Share(0.25f, 1.0f, 1.0f, 0.0f), 0.2, 1e-6) && Near(Share(0.25f, 1.0f, 1.0f, 0.5f), 0.6, 1e-6), "the share is La / (La + Ls N.L) moved to 1 by the direct number");
}

// ---------------------------------------------------------------------------
// the shader and ao.cpp
// ---------------------------------------------------------------------------
std::string Slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return "";
  }
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void CheckSources(const std::string& gpu) {
  const std::string apply = Slurp(gpu + "/hlsl/ao_apply_ps.hlsl");
  const std::string cpp = Slurp(gpu + "/ao.cpp");
  const std::string cmake = Slurp(gpu + "/CMakeLists.txt");
  Check(!apply.empty() && !cpp.empty() && !cmake.empty(), "the apply shader, ao.cpp and CMakeLists.txt are readable from " + gpu);
  if (apply.empty() || cpp.empty() || cmake.empty()) {
    return;
  }
  auto has = [](const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; };
  // The code: where it is, what it counts in.
  Check(has(apply, Num("(g_push.slots2.z >> %.0f) & 0xFu", double(kCodeShift))), "ao_apply_ps.hlsl: the code is the four bits at the header's shift");
  Check(has(apply, "float(AoPaintCode() + 1u) * (1.0f / 16.0f)") && kCodeUnit == 1.0f / 16.0f, "ao_apply_ps.hlsl: the power is (code + 1) sixteenths");
  Check(has(apply, Num("AoPaintCode() != %.0fu", double(kCodeOff))), "ao_apply_ps.hlsl: code 15 is off");
  // The rule: the stored visibility raised to the power, blended by the vehicle share, before the multiply.
  Check(has(apply, "lerp(ao, pow(ao, AoPaintPower()), on_car)") && has(apply, Num("saturate((vehicle - %.1ff) * 2.0f)", double(kVehicleFrom))),
        "ao_apply_ps.hlsl: the paint's visibility is ao raised to the power, blended from over half a neighbourhood of vehicle to all of it");
  Check(has(apply, "PaintVisibility(ao, ao_vehicle)") && has(apply, "lerp(1.0f, ao_seen, saturate(g_push.shape.z * share * fade))"),
        "ao_apply_ps.hlsl: the factor is lerp(1, the paint's visibility, strength * share * fade)");
  Check(has(apply, "if (on_car <= 0.0f || !AoPaintOn())"), "ao_apply_ps.hlsl: the world (at most half of the neighbourhood vehicle) returns the visibility as it is");
  // The paint's vehicle share follows the image that supplied its occlusion.
  Check(has(apply, "VisibilityAt(g_push.slots.z, pixel, tc, ao_vehicle, thin, optimized && AoRotatingGrid())") &&
            has(apply, "ao = VisibilityAt(AoVisibilitySlot(), pixel, tc, ao_vehicle);") &&
            has(apply, "ao = gi_image.x;\n    ao_vehicle = gi_vehicle;"),
        "ao_apply_ps.hlsl: paint takes the vehicle share from the AO upsample, or from the shared image in the legacy bounce variant");
  // The setting (a REX_SETTING row of src/settings/generated; the REXCVAR_DEFINE of ao.cpp before the conversion): its default and that it is
  // debug-only, live and from 1/16 to 1. Then ao.cpp: the clearing of the radius bits, the code into the flags.
  const setting_source::Info paint = setting_source::Find(gpu, cpp, "mcla_gpu_ao_paint");
  Check(setting_source::DefaultIs(paint, double(kDefaultPower)), "mcla_gpu_ao_paint: the setting's default is kDefaultPower");
  Check(paint.found && paint.debug_only && paint.live && paint.has_range && paint.range_lo == 0.0625 && paint.range_hi == 1.0,
        "mcla_gpu_ao_paint is debug-only, live and from 1/16 to 1");
  Check(has(cpp, "ao_paint::CodeFor(larecomp::settings::mcla_gpu_ao_paint.Get())"), "ao.cpp: the setting's code is the header's");
  Check(has(cpp, "(push.slots2[2] & ~ao_paint::kCodeMask) | (look_.paint_code << ao_paint::kCodeShift)"), "ao.cpp: the apply's flags carry the code after the radius bits are cleared");
  Check(has(cpp, "static_assert((ao_paint::kCodeMask &"), "ao.cpp: the code's bits are asserted not to overlap the apply's flags");
  Check(has(cmake, "larecomp_ao_paint_test") && has(cmake, "ao_paint_test.cpp"), "CMakeLists.txt: the test is a target");
}

}  // namespace

int main(int argc, char** argv) {
  std::string gpu_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--print") == 0) {
      g_print = true;
    } else {
      gpu_dir = argv[i];
    }
  }
  CheckCode();
  CheckRule();
  CheckPanel();
  if (!gpu_dir.empty()) {
    CheckSources(gpu_dir);
  }
  std::printf("ao_paint_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
