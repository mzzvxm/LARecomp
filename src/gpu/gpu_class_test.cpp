// Check of the GPU class rule and the `auto` table (gpu_class.h). A program of its own: it needs no
// renderer, no ReXGlue and no GPU, and exits 0 when every check holds.
//
//   cmake --build <build dir> --target larecomp_gpu_class_test     (src/gpu/CMakeLists.txt)
//   <build dir>/src/gpu/larecomp_gpu_class_test
//
// The expected values are written out here again on purpose: the table of GFX-UPGRADES-DESIGN.md
// section 9 is the other side of the check, so a cell changed in gpu_class.h alone fails it, and
// changing a cell on purpose means changing it here too.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

#include "gpu_class.h"
#include "render_scale.h"
#include "upscale_defaults.h"
#include "upscale_modes.h"

using namespace mcla::gpu::gpu_class;

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

void CheckEq(const std::string& got, const std::string& want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got '%s', want '%s'\n", what.c_str(), got.c_str(), want.c_str());
  }
}

void CheckNum(double got, double want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failed;
    std::printf("FAILED: %s: got %g, want %g\n", what.c_str(), got, want);
  }
}

struct Row {
  Effect effect;
  const char* cvar;
  const char* low;
  const char* mid;
  const char* high;
  const char* label_low;  // the pause row's label for auto, per class
  const char* label_mid;
  const char* label_high;
};

// The table of GFX-UPGRADES-DESIGN.md section 9, in the order of the Effect enum.
const Row kTable[] = {
    {Effect::kShadowFilter, "mcla_gpu_shadow_filter", "game", "pcss", "pcss", "AUTO (GAME)",
     "AUTO (PCSS)", "AUTO (PCSS)"},
    {Effect::kShadowResolution, "mcla_gpu_shadow_resolution", "game", "high", "high", "AUTO (GAME)",
     "AUTO (HIGH)", "AUTO (HIGH)"},
    {Effect::kEnvResolution, "mcla_gpu_env_resolution", "game", "game", "high", "AUTO (GAME)",
     "AUTO (GAME)", "AUTO (HIGH)"},
    {Effect::kReflectionResolution, "mcla_gpu_reflection_resolution", "game", "high", "high",
     "AUTO (GAME)", "AUTO (HIGH)", "AUTO (HIGH)"},
    // The cells are the words of the settings, which are the words the player sees (nothing is called low: the contact shadows,
    // the bounce light and the ambient occlusion are off / high / ultra).
    {Effect::kContactShadows, "mcla_gpu_contact_shadows", "off", "high", "ultra", "AUTO (OFF)",
     "AUTO (HIGH)", "AUTO (ULTRA)"},
    {Effect::kSunShafts, "mcla_gpu_sun_shafts", "off", "default", "default", "AUTO (OFF)",
     "AUTO (DEFAULT)", "AUTO (DEFAULT)"},
    // Bounce light: high on the High class only (about 0.75 ms at 4K on an RTX 5080), off on Mid and Low.
    {Effect::kSsgi, "mcla_gpu_ssgi", "off", "off", "high", "AUTO (OFF)", "AUTO (OFF)",
     "AUTO (HIGH)"},
    {Effect::kTrafficShadows, "mcla_gpu_traffic_shadows", "off", "near", "near", "AUTO (OFF)",
     "AUTO (NEAR)", "AUTO (NEAR)"},
    // The car self-reflections have one level, `on` (the older low and high cost the same: perf/out/FXCOST-B6.txt, owner 2026-11-14).
    {Effect::kCarSelfRefl, "mcla_gpu_car_selfrefl", "off", "on", "on", "AUTO (OFF)", "AUTO (ON)",
     "AUTO (ON)"},
    // Ambient occlusion (AO-DESIGN.md section 5: ultra is about 3 ms at 1080p on a GTX 1050-class card,
    // high about 1 ms; nothing on shared memory).
    {Effect::kAmbientOcclusion, "mcla_gpu_ao", "off", "high", "ultra", "AUTO (OFF)", "AUTO (HIGH)",
     "AUTO (ULTRA)"},
    // The headlights have one level, `on`: the integrated class (Low) has none.
    {Effect::kHeadlights, "mcla_gpu_headlights", "off", "on", "on", "AUTO (OFF)", "AUTO (ON)", "AUTO (ON)"},
    // Light shadows (local_shadow.h; the headlights now, the street lamps later), the three level shape (owner 2026-11-20): medium on Mid (two maps of 512: one view for the player's car and one for the
    // nearest other car), ultra on High (six of 1024: a view for each lamp of the player's car and the two nearest others), nothing on shared memory; high (the player's lamps, 512) is for the player to pick.
    {Effect::kLightShadows, "mcla_gpu_light_shadows", "off", "medium", "ultra", "AUTO (OFF)",
     "AUTO (MEDIUM)", "AUTO (ULTRA)"},
    // Volumetric light (VOLUMETRICS-SURVEY.md section 4.1): off on Low, medium on Mid, high on High (ultra is a deliberate pick).
    {Effect::kVolumetrics, "mcla_gpu_volumetrics", "off", "medium", "high", "AUTO (OFF)", "AUTO (MEDIUM)",
     "AUTO (HIGH)"},
    // Ray traced sun shadows (CITY-RT M1, city_rt_rules.h): high on High only; a Mid card gets it by hand.
    {Effect::kRtShadows, "mcla_gpu_rt_shadows", "off", "off", "off", "AUTO (OFF)", "AUTO (OFF)", "AUTO (OFF)"},
    // Reflections on surfaces (ssr_rules.h; the row REFLECTIONS ON SURFACES, GB-J): off on every class until the owner has seen the pictures. The prepared cells (gpu_class.h) are
    // Low off, Mid high or off, High ultra or high, by the cost run's figures: switching them on is a change of this row and of CheckSsr.
    {Effect::kSsr, "mcla_gpu_ssr", "off", "off", "off", "AUTO (OFF)", "AUTO (OFF)", "AUTO (OFF)"},
    // The cars' neon underglow in the light pass (GB-NEON): one level, `on`, nothing on the integrated class.
    {Effect::kNeonLight, "mcla_gpu_neon_light", "off", "on", "on", "AUTO (OFF)", "AUTO (ON)", "AUTO (ON)"},
    // The side mirrors without ray tracing (MIRROR-VIEW.md, mirror_setting.h): off on the integrated class (the game's own mirrors), high (512 wide, every second frame) on Mid, ultra (768 wide, every frame) on High.
    {Effect::kSideMirrors, "mcla_gpu_side_mirrors", "off", "high", "ultra", "AUTO (OFF)", "AUTO (HIGH)", "AUTO (ULTRA)"},
};

// Every value a player's own choice can have, across the effects: none of them is touched.
const char* const kOwnChoices[] = {"off",  "game", "low",  "high", "ultra",
                                   "soft", "pcss", "on",   "",     "Auto", "medium",
                                   "AUTO", "mid",  "near", "all",
                                   "auto " /* a space: not auto */};

void CheckTable() {
  Check(sizeof(kTable) / sizeof(kTable[0]) == static_cast<std::size_t>(kEffectCount),
        "the test lists every effect");
  Check(sizeof(kEffects) / sizeof(kEffects[0]) == static_cast<std::size_t>(kEffectCount),
        "kEffects has an entry for every effect");

  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  for (int i = 0; i < kEffectCount; ++i) {
    const Row& row = kTable[i];
    const std::string name = row.cvar;
    Check(static_cast<int>(row.effect) == i, name + ": the test is in the order of the enum");
    CheckEq(std::string(CvarName(row.effect)), name, name + ": cvar name");

    const char* const want[] = {row.low, row.mid, row.high};
    const char* const label[] = {row.label_low, row.label_mid, row.label_high};
    for (int k = 0; k < 3; ++k) {
      const GpuClass c = classes[k];
      const std::string cell = name + " on " + std::string(ClassName(c));
      CheckEq(std::string(Cell(row.effect, c)), want[k], cell + ": cell");
      CheckEq(ResolveFor(c, row.effect, "auto"), want[k], cell + ": Resolve(auto)");
      CheckEq(AutoLabelFor(c, row.effect), label[k], cell + ": AutoLabel");
      for (const char* own : kOwnChoices) {
        CheckEq(ResolveFor(c, row.effect, own), own, cell + ": Resolve('" + own + "') is unchanged");
      }
    }
  }
}

// Ambient occlusion's class-only `auto` (mcla_gpu_ao): off on shared memory, high on a small discrete card, ultra on
// the rest; explicit values pass through for the AO resolver.
void CheckAmbientOcclusion() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "high", "ultra"};
  const char* const own[] = {"off", "high", "ultra", "rt"};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "high" ? 1 : 2; };
  int last = -1;
  for (int k = 0; k < 3; ++k) {
    const GpuClass c = classes[k];
    const std::string at = "ambient occlusion on " + std::string(ClassName(c));
    CheckEq(ResolveFor(c, Effect::kAmbientOcclusion, "auto"), want[k], at + ": auto");
    for (const char* v : own) {
      CheckEq(ResolveFor(c, Effect::kAmbientOcclusion, v), v, at + ": the saved '" + v + "' is kept");
    }
    // The classes only go up, and the look it gives (the base of the contact shadows' apply pass) is never
    // below the contact shadows' own cell.
    const int now = rank(Cell(Effect::kAmbientOcclusion, c));
    Check(now >= last, at + ": not below the class under it");
    last = now;
    Check(now >= rank(Cell(Effect::kContactShadows, c)), at + ": not below the contact shadows' cell");
  }
}

// Light shadows' `auto` (mcla_gpu_light_shadows, local_shadow.h): nothing on shared memory, one view for a car (medium) on Mid, a view for each lamp of three cars
// (ultra) on High; high (the player's lamps) is never a cell. A value the player holds (off, medium, high, ultra) is kept in every class, the classes only go up, and nothing is called low.
void CheckLightShadows() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "medium", "ultra"};
  const char* const own[] = {"off", "medium", "high", "ultra"};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : 3; };
  int last = -1;
  for (int k = 0; k < 3; ++k) {
    const GpuClass c = classes[k];
    const std::string at = "light shadows on " + std::string(ClassName(c));
    CheckEq(ResolveFor(c, Effect::kLightShadows, "auto"), want[k], at + ": auto");
    for (const char* v : own) {
      CheckEq(ResolveFor(c, Effect::kLightShadows, v), v, at + ": the saved '" + v + "' is kept");
    }
    const int now = rank(Cell(Effect::kLightShadows, c));
    Check(now >= last, at + ": not below the class under it");
    Check(Cell(Effect::kLightShadows, c) != "high" && Cell(Effect::kLightShadows, c) != "low", at + ": auto never picks high (the player's own pick) or a level called low");
    last = now;
  }
  CheckEq(AutoLabelFor(GpuClass::kMid, Effect::kLightShadows), "AUTO (MEDIUM)", "the Mid class's row says AUTO (MEDIUM)");
  CheckEq(AutoLabelFor(GpuClass::kHigh, Effect::kLightShadows), "AUTO (ULTRA)", "the High class's row says AUTO (ULTRA)");
  CheckEq(std::string(CvarName(Effect::kLightShadows)), "mcla_gpu_light_shadows", "the cvar's name");
}

// Volumetric light's `auto` (mcla_gpu_volumetrics): off on shared memory, medium on a small discrete card, high on the rest. Ultra is a
// deliberate pick, never a cell; a value the player holds (off, medium, high, ultra) is kept in every class; nothing is called low.
void CheckVolumetrics() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "medium", "high"};
  const char* const own[] = {"off", "medium", "high", "ultra"};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : 3; };
  int last = -1;
  for (int k = 0; k < 3; ++k) {
    const GpuClass c = classes[k];
    const std::string at = "volumetric light on " + std::string(ClassName(c));
    CheckEq(ResolveFor(c, Effect::kVolumetrics, "auto"), want[k], at + ": auto");
    for (const char* v : own) {
      CheckEq(ResolveFor(c, Effect::kVolumetrics, v), v, at + ": the saved '" + v + "' is kept");
    }
    const int now = rank(Cell(Effect::kVolumetrics, c));
    Check(now >= last, at + ": not below the class under it");
    Check(now < 3, at + ": auto never picks ultra");
    last = now;
  }
  CheckEq(AutoLabelFor(GpuClass::kMid, Effect::kVolumetrics), "AUTO (MEDIUM)", "the Mid class's row says AUTO (MEDIUM)");
  Check(AutoLabelFor(GpuClass::kHigh, Effect::kVolumetrics).size() <= 13, "the longest value of the row fits the page");
}

// Reflections on surfaces' `auto` (mcla_gpu_ssr, GB-J): OFF ON EVERY CLASS until the owner approves the pictures, so the effect is off by default as before the row existed; a value the player
// holds (off, high, ultra) is kept in every class; the classes never go down; nothing is called low. When the owner approves, the cells become the prepared ones (Low off, Mid high or off,
// High ultra or high: gpu_class.h) and the "all off" check below is what changes.
void CheckSsr() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "high" ? 1 : 2; };
  int last = -1;
  for (const GpuClass c : classes) {
    const std::string at = "reflections on surfaces on " + std::string(ClassName(c));
    CheckEq(ResolveFor(c, Effect::kSsr, "auto"), "off", at + ": auto is off until the pictures are approved");
    for (const char* v : {"off", "high", "ultra"}) {
      CheckEq(ResolveFor(c, Effect::kSsr, v), v, at + ": the saved '" + v + "' is kept");
    }
    CheckEq(AutoLabelFor(c, Effect::kSsr), "AUTO (OFF)", at + ": the row says AUTO (OFF)");
    const int now = rank(Cell(Effect::kSsr, c));
    Check(now >= last, at + ": not below the class under it");
    last = now;
    const std::string cell(Cell(Effect::kSsr, c));
    Check(cell == "off" || cell == "high" || cell == "ultra", at + ": the cell '" + cell + "' is off, high or ultra");
    Check(AutoLabelFor(c, Effect::kSsr).size() <= 12, at + ": the longest value of the row fits the page (AUTO (ULTRA) is 12)");
  }
  CheckEq(std::string(CvarName(Effect::kSsr)), "mcla_gpu_ssr", "the cvar's name");
}

void CheckLabels() {
  CheckEq(AutoLabelOf("high"), "AUTO (HIGH)", "AutoLabelOf(high)");
  CheckEq(AutoLabelOf("Soft"), "AUTO (SOFT)", "AutoLabelOf(Soft)");
  CheckEq(Upper("a1-z"), "A1-Z", "Upper keeps what is not a letter");
}

// The stored word is the shown word: nothing in the graphics settings is called low (owner, 2026-11-14). The ambient occlusion, the
// contact shadows, the bounce light and the screen-space reflections are off / high / ultra (the light shadows have three levels: the loop below), the car self-reflections off / on; every cell of the table is one of the
// words its setting allows, and no cell and no label of an `auto` says low.
void CheckWords() {
  for (const Effect effect : {Effect::kAmbientOcclusion, Effect::kContactShadows, Effect::kSsgi, Effect::kSsr}) {
    const std::string name(CvarName(effect));
    for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
      const std::string cell(Cell(effect, c));
      Check(cell == "off" || cell == "high" || cell == "ultra", name + " on " + std::string(ClassName(c)) + ": the cell '" + cell + "' is off, high or ultra");
    }
  }
  // The light shadows have three levels (the three level shape): their cells are words of off / medium / high / ultra.
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    const std::string cell(Cell(Effect::kLightShadows, c));
    Check(cell == "off" || cell == "medium" || cell == "high" || cell == "ultra",
          "mcla_gpu_light_shadows on " + std::string(ClassName(c)) + ": the cell '" + cell + "' is off, medium, high or ultra");
  }
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    const std::string cell(Cell(Effect::kCarSelfRefl, c));
    Check(cell == "off" || cell == "on", "car self-reflections on " + std::string(ClassName(c)) + ": the cell '" + cell + "' is off or on");
    const std::string headlights(Cell(Effect::kHeadlights, c));
    Check(headlights == "off" || headlights == "on", "headlights on " + std::string(ClassName(c)) + ": the cell '" + headlights + "' is off or on");
    const std::string neon(Cell(Effect::kNeonLight, c));
    Check(neon == "off" || neon == "on", "neon light on " + std::string(ClassName(c)) + ": the cell '" + neon + "' is off or on");
  }
  // The neon light (GB-NEON): ON by default where the light pass runs (Mid and High), off on the integrated class, and a player's own off or on is kept.
  CheckEq(std::string(Cell(Effect::kNeonLight, GpuClass::kLow)), "off", "neon light: off on Low");
  CheckEq(std::string(Cell(Effect::kNeonLight, GpuClass::kMid)), "on", "neon light: on on Mid");
  CheckEq(std::string(Cell(Effect::kNeonLight, GpuClass::kHigh)), "on", "neon light: on on High");
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    CheckEq(ResolveFor(c, Effect::kNeonLight, "on"), "on", "neon light: the saved on is kept");
    CheckEq(ResolveFor(c, Effect::kNeonLight, "off"), "off", "neon light: the saved off is kept");
  }
  CheckEq(AutoLabelFor(GpuClass::kLow, Effect::kNeonLight), "AUTO (OFF)", "neon light: the integrated class's row says AUTO (OFF)");
  CheckEq(AutoLabelFor(GpuClass::kHigh, Effect::kNeonLight), "AUTO (ON)", "neon light: the High class's row says AUTO (ON)");
  // The headlights: on for Mid and High, off for Low (the integrated GPU), and a player's own off or on is kept in every class.
  CheckEq(std::string(Cell(Effect::kHeadlights, GpuClass::kLow)), "off", "headlights: off on Low");
  CheckEq(std::string(Cell(Effect::kHeadlights, GpuClass::kMid)), "on", "headlights: on on Mid");
  CheckEq(std::string(Cell(Effect::kHeadlights, GpuClass::kHigh)), "on", "headlights: on on High");
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    CheckEq(ResolveFor(c, Effect::kHeadlights, "on"), "on", "headlights: the saved on is kept");
    CheckEq(ResolveFor(c, Effect::kHeadlights, "off"), "off", "headlights: the saved off is kept");
  }
  // No label of an `auto` says low, in any class.
  for (int i = 0; i < kEffectCount; ++i) {
    for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
      const std::string label = AutoLabelFor(c, static_cast<Effect>(i));
      Check(label.find("LOW") == std::string::npos, std::string("the auto label of ") + std::string(CvarName(static_cast<Effect>(i))) + " on " + std::string(ClassName(c)) + " has no LOW: " + label);
    }
  }
}

void CheckNames() {
  CheckEq(std::string(ClassName(GpuClass::kLow)), "low", "ClassName(low)");
  CheckEq(std::string(ClassName(GpuClass::kMid)), "mid", "ClassName(mid)");
  CheckEq(std::string(ClassName(GpuClass::kHigh)), "high", "ClassName(high)");
  GpuClass c = GpuClass::kHigh;
  Check(ParseClass("low", &c) && c == GpuClass::kLow, "ParseClass(low)");
  Check(ParseClass("mid", &c) && c == GpuClass::kMid, "ParseClass(mid)");
  Check(ParseClass("high", &c) && c == GpuClass::kHigh, "ParseClass(high)");
  c = GpuClass::kMid;
  Check(!ParseClass("auto", &c) && c == GpuClass::kMid, "ParseClass(auto) is no class and leaves the value");
  Check(!ParseClass("", &c), "ParseClass('') is no class");
  Check(!ParseClass("High", &c), "ParseClass is exact");
}

AdapterFacts Discrete(uint64_t mib) {
  AdapterFacts f;
  f.dedicated_bytes = mib << 20;
  return f;
}

void CheckClassify() {
  // The adapters of the test PC and the ones the table is written for.
  Check(Classify(Discrete(16303)).cls == GpuClass::kHigh, "RTX 5080 (16 GB): high");
  Check(Classify(Discrete(8192)).cls == GpuClass::kHigh, "an 8 GB card: high");
  Check(Classify(Discrete(6144)).cls == GpuClass::kHigh, "a 6 GB card (6144 MiB): high");
  Check(Classify(Discrete(5950)).cls == GpuClass::kHigh, "a 6 GB card that reports 5950 MiB: high");
  Check(Classify(Discrete(5632)).cls == GpuClass::kHigh, "exactly the limit: high");
  Check(Classify(Discrete(5631)).cls == GpuClass::kMid, "one MiB under the limit: mid");
  Check(Classify(Discrete(4096)).cls == GpuClass::kMid, "a 4 GB card: mid");
  Check(Classify(Discrete(2048)).cls == GpuClass::kMid, "a 2 GB card: mid");
  Check(Classify(Discrete(512)).cls == GpuClass::kMid, "a 512 MB discrete card: mid");

  AdapterFacts igpu;  // the 9800X3D's 2-CU RDNA 2: UMA, 512 MiB carve-out
  igpu.uma = true;
  igpu.dedicated_bytes = 512ull << 20;
  Check(Classify(igpu).cls == GpuClass::kLow, "the 2-CU iGPU: low");

  AdapterFacts deck;  // Steam Deck on Vulkan: integrated and UMA, 1 GiB carve-out
  deck.uma = true;
  deck.integrated = true;
  deck.dedicated_bytes = 1024ull << 20;
  Check(Classify(deck).cls == GpuClass::kLow, "Steam Deck: low");

  AdapterFacts big_apu;  // a handheld APU with an 8 GiB carve-out is still shared memory
  big_apu.uma = true;
  big_apu.dedicated_bytes = 8192ull << 20;
  Check(Classify(big_apu).cls == GpuClass::kLow, "UMA with a large carve-out: low");

  AdapterFacts vk_igpu;  // Vulkan integrated without host-visible local memory (Plume's uma stays false)
  vk_igpu.integrated = true;
  vk_igpu.dedicated_bytes = 8192ull << 20;
  Check(Classify(vk_igpu).cls == GpuClass::kLow, "integrated type without the uma flag: low");

  AdapterFacts soft;  // WARP, llvmpipe, SwiftShader
  soft.software = true;
  soft.dedicated_bytes = 16384ull << 20;
  Check(Classify(soft).cls == GpuClass::kLow, "software adapter: low");

  Check(Classify(Discrete(0)).cls == GpuClass::kLow, "no dedicated memory reported: low");

  // The reason is one phrase the log line carries.
  Check(!Classify(Discrete(16303)).why.empty(), "a reason for high");
  Check(Classify(Discrete(16303)).why.find("16303 MiB") != std::string::npos, "the reason names the memory");
  Check(Classify(igpu).why.find("UMA") != std::string::npos, "the reason names UMA");
}

// The city LOD scale (lod_city_scale, 0 = auto): 2 on a Mid or High class once the class is known, the
// game's own 1 on Low and while the class is not known (before the device opens; always with the Xenia
// renderer); the player's own value, whatever it is, in every state.
void CheckCityLod() {
  struct State {
    bool known;
    GpuClass cls;
    double want_auto;
    const char* label;
    const char* what;
  };
  const State states[] = {
      {false, GpuClass::kLow, 1.0, "AUTO (1X)", "class not known (Low default)"},
      {false, GpuClass::kMid, 1.0, "AUTO (1X)", "class not known (Mid)"},
      {false, GpuClass::kHigh, 1.0, "AUTO (1X)", "class not known (High default, as Current() is before Initialize)"},
      {true, GpuClass::kLow, 1.0, "AUTO (1X)", "Low"},
      {true, GpuClass::kMid, 2.0, "AUTO (2X)", "Mid"},
      {true, GpuClass::kHigh, 2.0, "AUTO (2X)", "High"},
  };
  // What the cvar can hold: the steps (0.5, 1, 1.5, 2), an old config's values (0.1, 5, 10), a value set by
  // hand.
  const double kOwn[] = {0.1, 0.5, 1.0, 1.5, 2.0, 2.5, 5.0, 10.0};
  const char* const kOwnText[] = {"0.1X", "0.5X", "1X", "1.5X", "2X", "2.5X", "5X", "10X"};
  // Zero is auto; so is anything below it and a value that is no number.
  const double kAuto[] = {0.0, -0.0, -1.0, -10.0, std::numeric_limits<double>::quiet_NaN()};

  for (const State& s : states) {
    const std::string at = std::string("city LOD, ") + s.what;
    CheckNum(CityLodAutoFor(s.known, s.cls), s.want_auto, at + ": auto");
    CheckEq(CityLodLabelFor(s.known, s.cls, 0.0), s.label, at + ": auto's label");
    for (const double v : kAuto) {
      CheckNum(CityLodScaleFor(s.known, s.cls, v), s.want_auto, at + ": scale of " + std::to_string(v));
      CheckEq(CityLodLabelFor(s.known, s.cls, v), s.label, at + ": label of " + std::to_string(v));
    }
    for (std::size_t i = 0; i < sizeof(kOwn) / sizeof(kOwn[0]); ++i) {
      CheckNum(CityLodScaleFor(s.known, s.cls, kOwn[i]), kOwn[i],
               at + ": the player's " + kOwnText[i] + " is kept");
      CheckEq(CityLodLabelFor(s.known, s.cls, kOwn[i]), kOwnText[i], at + ": label of " + kOwnText[i]);
    }
  }

  // The values the two constants and the text helper stand for.
  CheckNum(kCityLodAutoGame, 1.0, "the game's own city LOD scale is 1");
  CheckNum(kCityLodAutoFull, 2.0, "the strong class's city LOD scale is 2");
  CheckEq(CityLodScaleText(2.0), "2X", "CityLodScaleText(2)");
  CheckEq(CityLodScaleText(1.5), "1.5X", "CityLodScaleText(1.5)");
  CheckEq(CityLodScaleText(0.5), "0.5X", "CityLodScaleText(0.5)");
}

// The numeric `auto` settings (kScales: the city LOD, the traffic LOD, the traffic range, the pedestrian and parked car densities):
// the cell of each class written out again (owner, 2026-11-20: as high as each class affords; the old defaults were one value for every
// card), `auto` while the class is not known is the Low cell, the player's own value is kept in every state (the traffic range is held to
// 100 to 600), and what is `auto`: 0 and below for the settings with no 0, below 0 for the two densities (0 is "none" there).
void CheckScales() {
  struct Cell {
    Scale scale;
    const char* cvar;
    double low, mid, high;
    bool zero_is_auto;
    char unit;
    const char* label_low;
    const char* label_mid;
    const char* label_high;
  };
  const Cell cells[] = {
      {Scale::kCityLod, "lod_city_scale", 1.0, 2.0, 2.0, true, 'X', "AUTO (1X)", "AUTO (2X)", "AUTO (2X)"},
      {Scale::kTrafficLod, "lod_traffic_scale", 1.5, 2.0, 3.0, true, 'X', "AUTO (1.5X)", "AUTO (2X)", "AUTO (3X)"},
      {Scale::kTrafficRange, "traffic_unspawn_dist", 250.0, 300.0, 400.0, true, 'M', "AUTO (250M)", "AUTO (300M)", "AUTO (400M)"},
      {Scale::kPedDensity, "ped_density_scale", 0.5, 0.75, 1.0, false, 'X', "AUTO (0.5X)", "AUTO (0.75X)", "AUTO (1X)"},
      {Scale::kParkedDensity, "parked_car_scale", 0.5, 0.75, 1.0, false, 'X', "AUTO (0.5X)", "AUTO (0.75X)", "AUTO (1X)"},
  };
  CheckNum(kScaleCount, static_cast<double>(sizeof(cells) / sizeof(cells[0])), "every numeric auto setting is in the check");

  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const class_names[] = {"low", "mid", "high"};
  const double nan = std::numeric_limits<double>::quiet_NaN();

  for (const Cell& c : cells) {
    const std::string name = c.cvar;
    CheckEq(std::string(ScaleCvar(c.scale)), c.cvar, name + ": the cvar's name");
    const double want[3] = {c.low, c.mid, c.high};
    const char* const labels[3] = {c.label_low, c.label_mid, c.label_high};

    // The cells; a class never gets less than a weaker one.
    for (int i = 0; i < 3; ++i) {
      CheckNum(ScaleCell(c.scale, classes[i]), want[i], name + ": the cell of " + class_names[i]);
      CheckNum(ScaleAutoFor(c.scale, true, classes[i]), want[i], name + ": auto on " + class_names[i]);
      // Until the class is known, auto is the Low cell, whatever class Current() says (it says High before Initialize).
      CheckNum(ScaleAutoFor(c.scale, false, classes[i]), c.low, name + ": auto before the class is known (" + class_names[i] + ")");
      CheckEq(ScaleLabelFor(c.scale, true, classes[i], c.zero_is_auto ? 0.0 : -1.0), labels[i], name + ": the AUTO label on " + class_names[i]);
    }
    Check(c.low <= c.mid && c.mid <= c.high, name + ": a stronger class never gets less");

    // What is `auto`: the sentinel, anything below it, a value that is no number.
    const double autos[] = {-1.0, -0.5, -10.0, nan};
    for (const double v : autos) {
      CheckNum(ScaleFor(c.scale, true, GpuClass::kHigh, v), c.high, name + ": " + std::to_string(v) + " is auto");
      Check(IsAutoValue(c.scale, v), name + ": IsAutoValue(" + std::to_string(v) + ")");
    }
    Check(IsAutoValue(c.scale, 0.0) == c.zero_is_auto, name + ": whether 0 is auto");
    if (c.zero_is_auto) {
      CheckNum(ScaleFor(c.scale, true, GpuClass::kMid, 0.0), c.mid, name + ": 0 is auto");
    } else {
      // 0 is "none" for a density: kept, on every class, and its label is its own.
      for (int i = 0; i < 3; ++i) {
        CheckNum(ScaleFor(c.scale, true, classes[i], 0.0), 0.0, name + ": 0 is kept on " + class_names[i]);
      }
      CheckEq(ScaleLabelFor(c.scale, true, GpuClass::kHigh, 0.0), "0X", name + ": the label of none");
    }

    // The player's own values are kept in every state (the steps of the row and a few by hand).
    const double own_x[] = {0.1, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0};
    const double own_m[] = {100.0, 150.0, 200.0, 250.0, 300.0, 400.0, 500.0, 600.0};
    const bool metres = c.unit == 'M';
    const std::size_t count = metres ? sizeof(own_m) / sizeof(own_m[0]) : sizeof(own_x) / sizeof(own_x[0]);
    for (std::size_t k = 0; k < count; ++k) {
      const double v = metres ? own_m[k] : own_x[k];
      for (int i = 0; i < 3; ++i) {
        CheckNum(ScaleFor(c.scale, true, classes[i], v), v, name + ": the player's " + std::to_string(v) + " on " + class_names[i]);
        CheckNum(ScaleFor(c.scale, false, classes[i], v), v, name + ": the player's " + std::to_string(v) + " before the class is known");
      }
    }
    CheckEq(ScaleText(c.scale, metres ? 250.0 : 2.0), metres ? "250M" : "2X", name + ": ScaleText");
    CheckEq(ScaleText(c.scale, metres ? 400.0 : 0.5), metres ? "400M" : "0.5X", name + ": ScaleText 2");
    CheckEq(ScaleLabelFor(c.scale, true, GpuClass::kHigh, metres ? 250.0 : 2.0), metres ? "250M" : "2X",
            name + ": the label of the player's value has no AUTO");
  }

  // The traffic range set by hand is held to the menu's range (the setting accepts 0 to 600 so that 0 can be auto).
  CheckNum(ScaleFor(Scale::kTrafficRange, true, GpuClass::kHigh, 50.0), 100.0, "traffic range: 50 is held to 100");
  CheckNum(ScaleFor(Scale::kTrafficRange, true, GpuClass::kHigh, 0.5), 100.0, "traffic range: 0.5 is held to 100");
  CheckNum(ScaleFor(Scale::kTrafficRange, true, GpuClass::kLow, 700.0), 600.0, "traffic range: 700 is held to 600");
  CheckEq(ScaleLabelFor(Scale::kTrafficRange, true, GpuClass::kHigh, 50.0), "100M", "traffic range: the label of a held value");

  // The Low cells of the three settings that were one value for every card are those values, so that a build with no class (the Xenia
  // renderer) and a Low card keep what they had: 250 m, 0.5 and 0.5. The city LOD's Low cell is the game's own 1.
  CheckNum(ScaleCell(Scale::kTrafficRange, GpuClass::kLow), 250.0, "the old traffic range is the Low cell");
  CheckNum(ScaleCell(Scale::kPedDensity, GpuClass::kLow), 0.5, "the old pedestrian density is the Low cell");
  CheckNum(ScaleCell(Scale::kParkedDensity, GpuClass::kLow), 0.5, "the old parked car density is the Low cell");
  CheckNum(ScaleCell(Scale::kCityLod, GpuClass::kLow), 1.0, "the game's own city LOD is the Low cell");

  // The ceilings: where more adds nothing, so that the best class is never given a number that only costs. The city is loaded to 600 m and
  // seen in detail to 300 m x scale x up to 1.5; traffic at 1x keeps its middle model to about 155 m and the game draws a car to 500 m
  // (a scale above 500 / 155 only moves a step past the cull); the game's own traffic range is 400 m; the game's own density is 1.
  Check(300.0 * ScaleCell(Scale::kCityLod, GpuClass::kHigh) * 1.5 >= 600.0, "the High city LOD already shows every loaded sector (600 m)");
  Check(300.0 * ScaleCell(Scale::kCityLod, GpuClass::kHigh) <= 600.0, "the High city LOD is not above what the 600 m radius can use");
  Check(155.0 * ScaleCell(Scale::kTrafficLod, GpuClass::kHigh) <= 500.0, "the High traffic LOD ends the middle model before the 500 m cull");
  Check(40.0 * ScaleCell(Scale::kTrafficLod, GpuClass::kHigh) >= 100.0, "the High traffic LOD keeps the most detailed model past 100 m");
  Check(ScaleCell(Scale::kTrafficRange, GpuClass::kHigh) <= 400.0, "the High traffic range is the game's own at most");
  Check(ScaleCell(Scale::kPedDensity, GpuClass::kHigh) <= 1.0, "the High pedestrian density is the game's own at most");
  Check(ScaleCell(Scale::kParkedDensity, GpuClass::kHigh) <= 1.0, "the High parked car density is the game's own at most");
  Check(ScaleCell(Scale::kTrafficRange, GpuClass::kLow) >= kTrafficRangeMin && ScaleCell(Scale::kTrafficRange, GpuClass::kHigh) <= kTrafficRangeMax,
        "the traffic range cells are inside the range the menu offers");
}

// The sun shafts' ray steps are chosen by the class and are no setting: Mid 32, High 64, Low (a strength turned on by
// hand there) the cheaper 32.
void CheckSunShaftSteps() {
  CheckNum(kSunShaftStepsMid, 32, "kSunShaftStepsMid");
  CheckNum(kSunShaftStepsHigh, 64, "kSunShaftStepsHigh");
  CheckNum(SunShaftSteps(GpuClass::kLow), 32, "sun shaft steps on Low");
  CheckNum(SunShaftSteps(GpuClass::kMid), 32, "sun shaft steps on Mid");
  CheckNum(SunShaftSteps(GpuClass::kHigh), 64, "sun shaft steps on High");
}

// --- the speed table (kSpeedTable): GPU names that cap the class --------------------------------------

AdapterFacts Named(uint32_t vendor, const char* name, uint64_t mib) {
  AdapterFacts f;
  f.vendor_id = vendor;
  f.name = name;
  f.dedicated_bytes = mib << 20;
  return f;
}

int g_row_hits[64] = {};
int g_integrated_row_hits[64] = {};

// One adapter as the log line names it: its class and the rule that chose it. `phrase` (when not empty) must be
// in the reason, so the log line says what it should.
void CheckAdapter(const AdapterFacts& facts, GpuClass want, Rule rule, const char* phrase = "") {
  const std::string at = std::string("'") + std::string(facts.name) + "' (" + std::to_string(facts.dedicated_bytes >> 20) +
                         " MiB)";
  const Verdict v = Classify(facts);
  CheckEq(std::string(ClassName(v.cls)), std::string(ClassName(want)), at + ": class");
  Check(v.rule == rule, at + ": the rule that chose it (" + v.why + ")");
  Check(!v.why.empty(), at + ": a reason");
  const char* prefix = rule == Rule::kName ? "by name: " : rule == Rule::kMemory ? "by memory: " : "by adapter type: ";
  Check(v.why.rfind(prefix, 0) == 0, at + ": the reason starts with '" + prefix + "': " + v.why);
  if (phrase[0] != '\0') {
    Check(v.why.find(phrase) != std::string::npos, at + ": the reason says '" + phrase + "': " + v.why);
  }
  const SpeedMatch m = MatchSpeedTable(facts.vendor_id, facts.name);
  if (m.row >= 0 && m.row < 64) {
    ++g_row_hits[m.row];
  }
}

void CheckNormalize() {
  CheckEq(NormalizeName("NVIDIA GeForce GTX 1060 6GB"), "nvidia geforce gtx 1060 6gb", "normalise: plain name");
  CheckEq(NormalizeName("Intel(R) Arc(TM) A380 Graphics"), "intel arc a380 graphics", "normalise: (R) and (TM)");
  CheckEq(NormalizeName("Intel(r) ARC(tm) A380 Graphics"), "intel arc a380 graphics", "normalise: (r) and (tm) in any case");
  CheckEq(NormalizeName("Intel\xC2\xAE Arc\xE2\x84\xA2 A380 Graphics"), "intel arc a380 graphics",
          "normalise: the UTF-8 signs for registered and trade mark");
  CheckEq(NormalizeName("AMD Radeon RX 580 Series (RADV POLARIS10)"), "amd radeon rx 580 series radv polaris10",
          "normalise: brackets");
  CheckEq(NormalizeName("  GeForce GTX 1060 with Max-Q Design "), "geforce gtx 1060 with max q design",
          "normalise: dashes and edge spaces");
  CheckEq(NormalizeName(""), "", "normalise: empty");
  CheckEq(NormalizeName("AMD Radeon(TM) Graphics"), "amd radeon graphics", "normalise: AMD's iGPU name");
}

// The table's own shape: what a row may hold, so a typo in a row fails here.
void CheckSpeedRows() {
  Check(kSpeedRows >= 1 && kSpeedRows <= 64, "the speed table has rows");
  for (int i = 0; i < kSpeedRows; ++i) {
    const SpeedRow& r = kSpeedTable[i];
    const std::string at = "speed row " + std::to_string(i) + " (" + std::string(r.family) + ")";
    Check(!r.family.empty(), at + ": a family name for the log");
    Check(r.vendor == kVendorNvidia || r.vendor == kVendorAmd || r.vendor == kVendorIntel, at + ": a known vendor");
    Check(r.cap == GpuClass::kLow || r.cap == GpuClass::kMid, at + ": caps at low or mid (a cap at high does nothing)");
    Check(!(r.word.empty() && r.series.empty()), at + ": has a word or a series to match");
    Check(NormalizeName(r.word) == r.word && NormalizeName(r.series) == r.series,
          at + ": its word and series are written the way NormalizeName writes a name");
    if (!r.series.empty()) {
      Check(r.lo <= r.hi, at + ": lo <= hi");
    }
  }
  // The integrated exception's rows: AMD only (the exception is Strix Halo's), and Mid, never a lower cap.
  Check(kIntegratedMidRows >= 1 && kIntegratedMidRows <= 64, "the integrated exception table has rows");
  for (int i = 0; i < kIntegratedMidRows; ++i) {
    const SpeedRow& r = kIntegratedMidTable[i];
    const std::string at = "integrated row " + std::to_string(i) + " (" + std::string(r.family) + ")";
    Check(!r.family.empty(), at + ": a family name for the log");
    Check(r.vendor == kVendorAmd, at + ": AMD only");
    Check(r.cap == GpuClass::kMid, at + ": Mid (the exception never raises an integrated GPU higher, nor lowers it)");
    Check(!(r.word.empty() && r.series.empty()), at + ": has a word or a series to match");
    Check(NormalizeName(r.word) == r.word && NormalizeName(r.series) == r.series,
          at + ": its word and series are written the way NormalizeName writes a name");
    if (!r.series.empty()) {
      Check(r.lo <= r.hi, at + ": lo <= hi");
    }
  }
}

void CheckSeriesNumber() {
  unsigned n = 0;
  Check(speed_detail::SeriesNumber("nvidia geforce gtx 1060 6gb", "gtx", &n) && n == 1060, "series: 'gtx 1060'");
  Check(speed_detail::SeriesNumber("geforce gtx1060", "gtx", &n) && n == 1060, "series: 'gtx1060'");
  Check(speed_detail::SeriesNumber("geforce gtx 1060m", "gtx", &n) && n == 1060, "series: '1060m'");
  Check(speed_detail::SeriesNumber("radeon rx 5600xt", "rx", &n) && n == 5600, "series: 'rx 5600xt'");
  Check(speed_detail::SeriesNumber("geforce gt 1030", "gt", &n) && n == 1030, "series: 'gt 1030'");
  Check(!speed_detail::SeriesNumber("geforce gtx 1060", "gt", &n), "series: 'gt' is not 'gtx'");
  Check(!speed_detail::SeriesNumber("nvidia geforce rtx 5080", "gt", &n), "series: no 'gt' in an RTX");
  Check(!speed_detail::SeriesNumber("xgtx 1060", "gtx", &n), "series: the word starts a word");
  Check(!speed_detail::SeriesNumber("geforce gtx titan x", "gtx", &n), "series: a name word is not a number");
  Check(!speed_detail::SeriesNumber("geforce gtx", "gtx", &n), "series: nothing after it");
  Check(!speed_detail::SeriesNumber("geforce gtx 1234567", "gtx", &n), "series: more than five digits is no model");
  Check(speed_detail::SeriesNumber("intel arc a380 graphics", "a", &n) && n == 380, "series: 'a380'");
  Check(!speed_detail::SeriesNumber("amd radeon rx 580", "a", &n), "series: 'amd' is not 'a' + number");
  Check(speed_detail::HasWord("amd radeon rx vega 56", "vega"), "word: vega");
  Check(!speed_detail::HasWord("amd radeon rx vegas", "vega"), "word: whole words only");
  Check(speed_detail::HasWord("intel hd graphics 630", "hd graphics"), "word: two words");
}

void CheckSpeedTable() {
  const GpuClass kH = GpuClass::kHigh;
  const GpuClass kM = GpuClass::kMid;
  const GpuClass kL = GpuClass::kLow;
  const Rule kByName = Rule::kName;
  const Rule kByMem = Rule::kMemory;
  const uint32_t nv = kVendorNvidia;
  const uint32_t amd = kVendorAmd;
  const uint32_t intel = kVendorIntel;

  // --- fast cards: no row applies, the memory rule stands -------------------------------------------------
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 5080", 16303), kH, kByMem);  // the test PC's card
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 4090", 24564), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 4060", 8188), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 4050 Laptop GPU", 6141), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 3060", 12288), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 3060 Ti", 8192), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 2060", 6144), kH, kByMem);  // the low end of High
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 2060 SUPER", 8192), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 2070", 8192), kH, kByMem);
  CheckAdapter(Named(nv, "NVIDIA RTX A2000 12GB", 12288), kH, kByMem);  // a workstation name: "rtx a2000" is no model
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX TITAN X", 12288), kH, kByMem);  // no number: the table leaves it
  CheckAdapter(Named(nv, "NVIDIA TITAN Xp", 12288), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 5600 XT", 6144), kH, kByMem);  // the other low end of High
  CheckAdapter(Named(amd, "AMD Radeon RX 5700", 8192), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 5700 XT", 8192), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 6600", 8192), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 6650 XT", 8192), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 6700 XT", 12288), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 6800 XT", 16368), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 7600", 8176), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 7900 XTX", 24560), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 9070 XT", 16304), kH, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon VII", 16368), kH, kByMem);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A580 Graphics", 8192), kH, kByMem);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A750 Graphics", 8192), kH, kByMem);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A770 Graphics", 16384), kH, kByMem);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) B580 Graphics", 12288), kH, kByMem);

  // --- GeForce, Mid by name: the memory alone says High ---------------------------------------------------
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1060 6GB", 6144), kM, kByName, "by name: GTX 1060 -> mid");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1060 6GB", 6144), kM, kByName, "the memory alone says high");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1060 with Max-Q Design", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX1060", 6144), kM, kByName);          // no space
  CheckAdapter(Named(nv, "NVIDIA GEFORCE GTX 1060 6GB", 6144), kM, kByName);     // upper case
  CheckAdapter(Named(nv, "nvidia geforce gtx 1060 6gb", 6144), kM, kByName);     // lower case
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1070", 8192), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1070 Ti", 8192), kM, kByName);
  // The GTX 1080 and 1080 Ti are Mid too (the owner, 2026-11-14): Pascal does worse than its paper speed on the
  // compute-heavy effects. The cut-off is 1099, the end of the GTX 10 series.
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1080", 8192), kM, kByName, "by name: GTX 1080 -> mid");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1080 Ti", 11264), kM, kByName, "by name: GTX 1080 -> mid");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1080 with Max-Q Design", 8192), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1080 Ti", 11264), kM, kByName, "the memory alone says high");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 980 Ti", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1660 SUPER", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1660 Ti", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1660 Ti with Max-Q Design", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 3050", 8192), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 3050 6GB", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 2050", 6144), kM, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce MX570", 8192), kM, kByName);
  // Mid by memory already: the name changes nothing and the log says "by memory".
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1060 3GB", 3072), kM, kByMem, "by memory: ");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 970", 4096), kM, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1650", 4096), kL, kByName, "GTX 1650 -> low");
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX1650 with Max-Q Design", 4096), kL, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1050 Ti", 4096), kM, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 3050 Ti Laptop GPU", 4096), kM, kByMem);
  CheckAdapter(Named(nv, "NVIDIA GeForce MX450", 2048), kM, kByMem);
  // Low by name: slower than the Steam Deck's class, whatever the memory says.
  CheckAdapter(Named(nv, "NVIDIA GeForce GT 1030", 2048), kL, kByName, "by name: GT 1030 -> low");
  CheckAdapter(Named(nv, "NVIDIA GeForce GT 730", 2048), kL, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce MX150", 2048), kL, kByName);
  CheckAdapter(Named(nv, "NVIDIA GeForce MX350", 2048), kL, kByName);

  // --- Radeon ---------------------------------------------------------------------------------------------
  CheckAdapter(Named(amd, "AMD Radeon RX 580 Series", 8192), kM, kByName, "by name: RX 580 -> mid");
  CheckAdapter(Named(amd, "AMD Radeon RX 590 GME", 8192), kM, kByName);
  CheckAdapter(Named(amd, "Radeon RX 480 Graphics", 8192), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon (TM) RX 480 Graphics", 8192), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX 580 Series (RADV POLARIS10)", 8192), kM, kByName);  // Linux, Vulkan
  CheckAdapter(Named(amd, "AMD Radeon RX 570 Series", 4096), kM, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 560 Series", 4096), kM, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 550 Series", 4096), kL, kByName, "by name: RX 550 -> low");
  CheckAdapter(Named(amd, "AMD Radeon RX 540", 2048), kL, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX 5500 XT", 8192), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX 5500 XT", 4096), kM, kByMem);
  CheckAdapter(Named(amd, "AMD Radeon RX 6500 XT", 8192), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX 6400", 4096), kL, kByName, "RX 6400 -> low");
  CheckAdapter(Named(amd, "AMD Radeon RX 6400 (RADV NAVI24)", 4096), kL, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX Vega 56", 8176), kM, kByName);
  CheckAdapter(Named(amd, "Radeon RX Vega 64", 8176), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon RX Vega (RADV VEGA10)", 8176), kM, kByName);  // no number in the name
  CheckAdapter(Named(amd, "Radeon Vega Frontier Edition", 16368), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon R9 390 Series", 8192), kM, kByName);
  CheckAdapter(Named(amd, "AMD Radeon R9 Fury X", 4096), kM, kByMem);

  // --- Intel ----------------------------------------------------------------------------------------------
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A380 Graphics", 6144), kM, kByName, "by name: A380 -> mid");
  CheckAdapter(Named(intel, "Intel\xC2\xAE Arc\xE2\x84\xA2 A380 Graphics", 6144), kM, kByName);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A310 Graphics", 4096), kM, kByMem);
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) A370M Graphics", 4096), kM, kByMem);
  // Intel's integrated names alone (no UMA flag, no integrated type): Low, as a second check.
  CheckAdapter(Named(intel, "Intel(R) UHD Graphics 770", 8192), kL, kByName);
  CheckAdapter(Named(intel, "Intel(R) HD Graphics 630", 8192), kL, kByName);
  CheckAdapter(Named(intel, "Intel(R) Iris(R) Xe Graphics", 8192), kL, kByName);
  // The meteor lake style name has no model number and no row: the memory rule decides (the API's UMA flag makes
  // it Low in practice, below).
  CheckAdapter(Named(intel, "Intel(R) Arc(TM) Graphics", 8192), kH, kByMem);

  // --- integrated and software adapters: Low by type, before any name is read -------------------------------
  AdapterFacts igpu = Named(amd, "AMD Radeon(TM) Graphics", 512);  // the 9800X3D's 2-CU RDNA 2: UMA
  igpu.uma = true;
  CheckAdapter(igpu, kL, Rule::kAdapterType, "by adapter type: UMA");
  AdapterFacts vk_igpu = Named(amd, "AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)", 8192);
  vk_igpu.integrated = true;
  CheckAdapter(vk_igpu, kL, Rule::kAdapterType);
  AdapterFacts deck = Named(amd, "AMD Custom GPU 0405 (RADV VANGOGH)", 1024);
  deck.uma = true;
  deck.integrated = true;
  CheckAdapter(deck, kL, Rule::kAdapterType);
  AdapterFacts apu = Named(intel, "Intel(R) Arc(TM) Graphics", 8192);  // an iGPU with a big carve-out
  apu.uma = true;
  CheckAdapter(apu, kL, Rule::kAdapterType);
  AdapterFacts fast_name_on_igpu = Named(amd, "AMD Radeon RX 6800 XT", 16368);  // a name never raises a class
  fast_name_on_igpu.uma = true;
  CheckAdapter(fast_name_on_igpu, kL, Rule::kAdapterType);
  AdapterFacts soft = Named(0, "Microsoft Basic Render Driver", 16384);
  soft.software = true;
  CheckAdapter(soft, kL, Rule::kAdapterType, "by adapter type: a software");
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 5080", 0), kL, Rule::kAdapterType);  // no dedicated memory reported

  // --- the integrated exception (kIntegratedMidTable): AMD's Strix Halo GPUs are Mid, every other integrated GPU Low ---
  // The owner's 2-CU RDNA 2 and the other integrated AMD and Intel names stay Low by type.
  const auto uma_gpu = [](uint32_t vendor, const char* name, uint64_t mib) {
    AdapterFacts f = Named(vendor, name, mib);
    f.uma = true;
    return f;
  };
  const auto integrated_gpu = [](uint32_t vendor, const char* name, uint64_t mib) {
    AdapterFacts f = Named(vendor, name, mib);
    f.integrated = true;
    return f;
  };
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 8060S Graphics", 512), kM, kByName, "by name: 8060S -> mid");
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 8060S Graphics", 32768), kM, kByName);  // a large carve-out
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 8050S Graphics", 4096), kM, kByName, "by name: 8050S -> mid");
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 8040S Graphics", 4096), kM, kByName, "by name: 8040S -> mid");
  CheckAdapter(uma_gpu(amd, "AMD Radeon 8060S Graphics", 4096), kM, kByName);          // without the (TM)
  CheckAdapter(uma_gpu(amd, "AMD RADEON(TM) 8060S GRAPHICS", 4096), kM, kByName);      // upper case
  CheckAdapter(integrated_gpu(amd, "AMD Radeon Graphics (RADV GFX1151)", 8192), kM, kByName, "by name: GFX1151 -> mid");
  AdapterFacts strix_both = uma_gpu(amd, "AMD Radeon(TM) 8060S Graphics", 8192);  // Vulkan sets both flags
  strix_both.integrated = true;
  CheckAdapter(strix_both, kM, kByName);
  CheckEq(std::string(ClassName(Classify(strix_both).cls)), "mid", "a Strix Halo GPU is mid");
  // Every other integrated GPU is Low, by type: AMD's other APUs, Intel's, the Deck's, the owner's iGPU.
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 890M Graphics", 512), kL, Rule::kAdapterType, "by adapter type: UMA");
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 880M Graphics", 512), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 780M Graphics", 512), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 760M Graphics", 512), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) 680M", 512), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(amd, "AMD Radeon(TM) Graphics", 512), kL, Rule::kAdapterType);  // the owner's 2-CU iGPU
  CheckAdapter(integrated_gpu(amd, "AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)", 8192), kL, Rule::kAdapterType);
  CheckAdapter(integrated_gpu(amd, "AMD Radeon Graphics (RADV GFX1150)", 8192), kL, Rule::kAdapterType);  // Strix Point
  CheckAdapter(uma_gpu(intel, "Intel(R) Arc(TM) 140V GPU (16GB)", 16384), kL, Rule::kAdapterType);  // Lunar Lake
  CheckAdapter(uma_gpu(intel, "Intel(R) Arc(TM) Graphics", 8192), kL, Rule::kAdapterType);
  // The exception is the AMD vendor's only, applies to integrated adapters only and never to a software one.
  CheckAdapter(uma_gpu(intel, "AMD Radeon(TM) 8060S Graphics", 4096), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(nv, "AMD Radeon(TM) 8060S Graphics", 4096), kL, Rule::kAdapterType);
  CheckAdapter(uma_gpu(0, "AMD Radeon(TM) 8060S Graphics", 4096), kL, Rule::kAdapterType);  // no vendor id
  AdapterFacts strix_soft = uma_gpu(amd, "AMD Radeon(TM) 8060S Graphics", 4096);
  strix_soft.software = true;
  CheckAdapter(strix_soft, kL, Rule::kAdapterType, "by adapter type: a software");
  // A discrete card with such a name (the table is not read for it) keeps the memory rule.
  CheckAdapter(Named(amd, "AMD Radeon(TM) 8060S Graphics", 16384), kH, kByMem);
  // The rows of the exception table: AMD only, Mid only, and each one decides a name above.
  Check(MatchIntegratedMid(amd, "AMD Radeon(TM) 8060S Graphics").row >= 0, "8060S matches its row");
  Check(MatchIntegratedMid(amd, "AMD Radeon(TM) 890M Graphics").row < 0, "890M matches no row");
  Check(MatchIntegratedMid(amd, "").row < 0, "an empty name matches no integrated row");
  Check(MatchIntegratedMid(nv, "AMD Radeon(TM) 8060S Graphics").row < 0, "an integrated row applies to AMD only");
  Check(MatchIntegratedMid(amd, "AMD Radeon(TM) 8060S Graphics").row != MatchIntegratedMid(amd, "AMD Radeon(TM) 8050S Graphics").row,
        "8060S and 8050S are different rows");
  for (const char* name : {"AMD Radeon(TM) 8060S Graphics", "AMD Radeon(TM) 8050S Graphics",
                           "AMD Radeon(TM) 8040S Graphics", "AMD Radeon Graphics (RADV GFX1151)"}) {
    const SpeedMatch m = MatchIntegratedMid(amd, name);
    Check(m.row >= 0 && kIntegratedMidTable[m.row].cap == GpuClass::kMid, std::string("an integrated row for ") + name);
    if (m.row >= 0 && m.row < 64) {
      ++g_integrated_row_hits[m.row];
    }
  }

  // --- a name never raises a class, and an unknown GPU keeps the memory rule ----------------------------------
  CheckAdapter(Named(nv, "NVIDIA GeForce RTX 5080", 4096), kM, kByMem);  // a 4 GB report stays Mid
  CheckAdapter(Named(0x1234, "Some New GPU 9000", 6144), kH, kByMem);
  CheckAdapter(Named(0x1234, "Some New GPU 9000", 4096), kM, kByMem);
  CheckAdapter(Named(0x1234, "NVIDIA GeForce GTX 1060 6GB", 6144), kH, kByMem);  // a row applies to its own vendor only
  CheckAdapter(Named(amd, "NVIDIA GeForce GTX 1060 6GB", 6144), kH, kByMem);
  CheckAdapter(Named(nv, "AMD Radeon RX 580 Series", 8192), kH, kByMem);
  CheckAdapter(Named(0, "NVIDIA GeForce GTX 1060 6GB", 6144), kH, kByMem);      // no vendor id: no row
  CheckAdapter(Named(nv, "", 6144), kH, kByMem);                                // no name: no row
  AdapterFacts no_name;
  no_name.dedicated_bytes = 8192ull << 20;
  CheckAdapter(no_name, kH, kByMem);  // the facts of before the table: the memory rule alone

  // The lowest cap wins where rows overlap: RX 550 is in the Polaris row (mid) and the RX 540/550 row (low).
  const SpeedMatch rx550 = MatchSpeedTable(amd, "AMD Radeon RX 550 Series");
  Check(rx550.row >= 0 && kSpeedTable[rx550.row].cap == GpuClass::kLow, "overlapping rows: the lowest cap is taken");
  CheckEq(rx550.text, "RX 550", "the matched text, for the log");
  CheckEq(MatchSpeedTable(amd, "AMD Radeon RX Vega 56").text, "VEGA", "the matched text of a word row");
  Check(MatchSpeedTable(nv, "NVIDIA GeForce RTX 5080").row < 0, "the RTX 5080 matches no row");
  Check(MatchSpeedTable(nv, "").row < 0, "an empty name matches no row");

  // Every row of the table is the best match of at least one name above: a row nobody tests is a row nobody
  // has seen work.
  for (int i = 0; i < kSpeedRows; ++i) {
    Check(g_row_hits[i] > 0, "speed row " + std::to_string(i) + " (" + std::string(kSpeedTable[i].family) +
                                 ") has a test name that it decides");
  }
  for (int i = 0; i < kIntegratedMidRows; ++i) {
    Check(g_integrated_row_hits[i] > 0, "integrated row " + std::to_string(i) + " (" +
                                            std::string(kIntegratedMidTable[i].family) +
                                            ") has a test name that it decides");
  }
}

// ---- the Steam Deck: kLow with the handheld cells (gpu_class.h kHandheldCells; perf\COMPAT-AUDIT.md section 11.4, DECK1-NOTES.md R5) ----

int g_handheld_row_hits[8] = {};

// What a Deck's adapter looks like to Classify: shared memory, the integrated type and a 1 GiB carve-out, AMD's vendor id and the name RADV or AMD's driver gives.
AdapterFacts DeckFacts(const char* name) {
  AdapterFacts f = Named(kVendorAmd, name, 1024);
  f.uma = true;
  f.integrated = true;
  return f;
}

void CheckHandheldCells() {
  const GpuClass kL = GpuClass::kLow;
  // The cells written out again: the Low column of the table above, except these three, which are the Mid values the Deck's GPU can run (soft for the
  // sun shadow's filter, since the blocker search of Mid's pcss is not run on a Low class).
  struct Want {
    Effect effect;
    const char* value;
  };
  const Want kWant[] = {
      {Effect::kShadowFilter, "soft"},
      {Effect::kShadowResolution, "high"},
      {Effect::kReflectionResolution, "high"},
  };
  Check(kHandheldCellCount == static_cast<int>(sizeof(kWant) / sizeof(kWant[0])), "the handheld cells are the three of the test");
  Check(sizeof(kHandheldCells) / sizeof(kHandheldCells[0]) == static_cast<std::size_t>(kHandheldCellCount), "kHandheldCellCount counts the cells");
  for (int i = 0; i < kHandheldCellCount; ++i) {
    for (int k = i + 1; k < kHandheldCellCount; ++k) {
      Check(kHandheldCells[i].effect != kHandheldCells[k].effect, "no effect has two handheld cells");
    }
  }

  for (int i = 0; i < kEffectCount; ++i) {
    const Effect effect = static_cast<Effect>(i);
    const std::string name(CvarName(effect));
    std::string expected(Cell(effect, kL));
    bool listed = false;
    for (const Want& w : kWant) {
      if (w.effect == effect) {
        expected = w.value;
        listed = true;
      }
    }
    const std::string at = name + " on a handheld";
    CheckEq(std::string(CellFor(effect, kL, true)), expected, at + ": cell");
    CheckEq(ResolveFor(kL, true, effect, "auto"), expected, at + ": Resolve(auto)");
    CheckEq(AutoLabelFor(kL, true, effect), AutoLabelOf(expected), at + ": AutoLabel");
    // A handheld cell is not the Low cell (else it would not be a cell of its own), and a cell that is not listed is.
    CheckEq(CellFor(effect, kL, true) == Cell(effect, kL) ? "same" : "different", listed ? "different" : "same",
            at + ": differs from Low's cell exactly where a handheld cell is listed");
    // A player's own choice is never touched, listed or not.
    for (const char* own : kOwnChoices) {
      CheckEq(ResolveFor(kL, true, effect, own), own, at + ": Resolve('" + own + "') is unchanged");
    }
    // Without the flag it is the Low column; and the flag does nothing to Mid or High (it only means anything on Low).
    CheckEq(std::string(CellFor(effect, kL, false)), std::string(Cell(effect, kL)), name + " on Low without the flag: the Low cell");
    CheckEq(ResolveFor(kL, effect, "auto"), std::string(Cell(effect, kL)), name + ": the three argument ResolveFor is the Low cell");
    CheckEq(AutoLabelFor(kL, effect), AutoLabelFor(kL, false, effect), name + ": the two argument AutoLabelFor is the Low label");
    for (const GpuClass c : {GpuClass::kMid, GpuClass::kHigh}) {
      CheckEq(std::string(CellFor(effect, c, true)), std::string(Cell(effect, c)),
              name + " on " + std::string(ClassName(c)) + ": the handheld flag changes nothing");
      CheckEq(ResolveFor(c, true, effect, "auto"), std::string(Cell(effect, c)), name + " on " + std::string(ClassName(c)) + ": Resolve ignores the flag");
    }
    // No label of a handheld's `auto` says low.
    Check(AutoLabelFor(kL, true, effect).find("LOW") == std::string::npos, at + ": no LOW in the label");
  }

  // Where the Deck can carry what Mid does: the resolutions are Mid's cells exactly; the filter is Mid's pcss without the blocker search.
  CheckEq(std::string(CellFor(Effect::kShadowResolution, kL, true)), std::string(Cell(Effect::kShadowResolution, GpuClass::kMid)),
          "the Deck's shadow detail is Mid's");
  CheckEq(std::string(CellFor(Effect::kReflectionResolution, kL, true)), std::string(Cell(Effect::kReflectionResolution, GpuClass::kMid)),
          "the Deck's reflection detail is Mid's");
  CheckEq(std::string(Cell(Effect::kShadowFilter, GpuClass::kMid)), "pcss", "Mid's shadow filter is pcss, which the Deck cannot run (soft is its pcss)");

  // What stays off or the game's on the Deck: every effect that costs the GPU more than these three, and the light stack's consumers (the headlights, the
  // light shadows, the volumetric light, the reflections on surfaces, the ambient occlusion and the bounce light).
  for (const Effect effect : {Effect::kContactShadows, Effect::kSunShafts, Effect::kSsgi, Effect::kTrafficShadows, Effect::kCarSelfRefl,
                              Effect::kAmbientOcclusion, Effect::kHeadlights, Effect::kLightShadows, Effect::kVolumetrics, Effect::kSsr}) {
    CheckEq(std::string(CellFor(effect, kL, true)), "off", std::string(CvarName(effect)) + " stays off on a handheld");
  }
  CheckEq(std::string(CellFor(Effect::kEnvResolution, kL, true)), "game", "the environment detail stays the game's on a handheld");

  // The numbers that are the CPU's stay Low's: the Deck's four Zen 2 cores keep the game's city detail (the scales have no handheld cells).
  for (int i = 0; i < kScaleCount; ++i) {
    const Scale scale = static_cast<Scale>(i);
    CheckNum(ScaleFor(scale, true, kL, scale == Scale::kPedDensity || scale == Scale::kParkedDensity ? -1.0 : 0.0), ScaleCell(scale, kL),
             std::string(ScaleCvar(scale)) + ": a handheld keeps Low's number");
  }
}

void CheckHandheldDetect() {
  const GpuClass kL = GpuClass::kLow;
  // The Deck: RADV's names (the older Mesa prints the device's marketing name, the newer one a generic one; both end in the code name) and AMD's own driver's.
  const char* const kDeckNames[] = {
      "AMD Custom GPU 0405 (RADV VANGOGH)",  // the LCD model, SteamOS stable
      "AMD Custom GPU 0932 (RADV VANGOGH)",  // the OLED model
      "AMD Radeon Graphics (RADV VANGOGH)",  // the generic name a newer Mesa prints
      "AMD Radeon(TM) Graphics (RADV VANGOGH)",
      "AMD RADV VANGOGH",
      "amd custom gpu 0405 (radv vangogh)",  // any case
      "AMD Custom GPU 0405",                 // the AMD driver's name, LCD
      "AMD Custom GPU 0932",                 // the OLED
      "AMD Custom GPU 0405 (AMD open-source driver)",
  };
  for (const char* name : kDeckNames) {
    const std::string at = std::string("'") + name + "'";
    // Shared memory only, by either flag: Vulkan sets both, D3D12 only the UMA one.
    AdapterFacts both = DeckFacts(name);
    AdapterFacts uma_only = both;
    uma_only.integrated = false;
    AdapterFacts integrated_only = both;
    integrated_only.uma = false;
    for (const AdapterFacts& f : {both, uma_only, integrated_only}) {
      const Verdict v = Classify(f);
      Check(v.handheld, at + ": found as a handheld");
      Check(v.cls == kL, at + ": a handheld is class low");
      Check(v.rule == Rule::kAdapterType, at + ": by adapter type, as every integrated GPU");
      Check(v.why.rfind("by adapter type: ", 0) == 0, at + ": the reason starts as an integrated GPU's: " + v.why);
      Check(v.why.find("Steam Deck") != std::string::npos, at + ": the reason names the Steam Deck: " + v.why);
      Check(v.why.find("kHandheldCells") != std::string::npos, at + ": the reason names where the cells are: " + v.why);
    }
    const SpeedMatch m = MatchHandheld(kVendorAmd, name);
    Check(m.row >= 0 && kHandheldTable[m.row].cap == kL, at + ": a handheld row");
    if (m.row >= 0 && m.row < 8) {
      ++g_handheld_row_hits[m.row];
    }
  }

  // Not a handheld: another integrated GPU, a Strix Halo (Mid, not a handheld), a discrete card, another vendor's chip, a software adapter, no name.
  const auto not_handheld = [&](const AdapterFacts& f, GpuClass want, const std::string& what) {
    const Verdict v = Classify(f);
    Check(!v.handheld, what + ": not a handheld");
    Check(v.cls == want, what + ": class " + std::string(ClassName(want)) + " as before");
  };
  not_handheld(DeckFacts("AMD Radeon(TM) Graphics"), kL, "the 2-CU iGPU of the desktop CPUs");
  not_handheld(DeckFacts("AMD Radeon(TM) 780M Graphics"), kL, "a 780M (a laptop's and the ROG Ally's, measured nowhere)");
  not_handheld(DeckFacts("AMD Radeon Graphics (RADV PHOENIX)"), kL, "a Phoenix on RADV");
  not_handheld(DeckFacts("AMD Radeon Graphics (RADV RAPHAEL_MENDOCINO)"), kL, "a Raphael on RADV");
  not_handheld(DeckFacts("AMD Radeon Graphics (RADV GFX1150)"), kL, "Strix Point");
  not_handheld(DeckFacts("AMD Custom GPU 0406"), kL, "another custom GPU");
  not_handheld(DeckFacts("AMD Custom GPU 04051"), kL, "a custom GPU whose number only starts like the Deck's");
  not_handheld(DeckFacts("AMD Custom GPU"), kL, "a custom GPU without a number");
  not_handheld(DeckFacts("AMD Radeon(TM) 8060S Graphics"), GpuClass::kMid, "a Strix Halo is Mid by name and no handheld");
  not_handheld(DeckFacts("AMD Radeon Graphics (RADV GFX1151)"), GpuClass::kMid, "a Strix Halo on RADV");
  {
    AdapterFacts f = DeckFacts("AMD Custom GPU 0405 (RADV VANGOGH)");
    f.vendor_id = kVendorIntel;  // a row applies to its own vendor only
    not_handheld(f, kL, "the Deck's name on another vendor");
    f.vendor_id = 0;
    not_handheld(f, kL, "the Deck's name without a vendor id");
    f.vendor_id = kVendorNvidia;
    not_handheld(f, kL, "the Deck's name on NVIDIA");
  }
  {
    AdapterFacts f = DeckFacts("AMD Custom GPU 0405 (RADV VANGOGH)");
    f.software = true;  // a software adapter is Low by type before any name is read
    not_handheld(f, kL, "a software adapter with the Deck's name");
  }
  {
    AdapterFacts f = Named(kVendorAmd, "AMD Custom GPU 0405 (RADV VANGOGH)", 8192);  // not shared memory: the table is not read for a discrete card
    not_handheld(f, GpuClass::kHigh, "the Deck's name on a discrete card with 8 GB");
    f.dedicated_bytes = 4096ull << 20;
    not_handheld(f, GpuClass::kMid, "the Deck's name on a discrete card with 4 GB");
  }
  not_handheld(DeckFacts(""), kL, "an adapter without a name");
  not_handheld(Named(kVendorAmd, "AMD Radeon RX 580 Series (RADV POLARIS10)", 8192), GpuClass::kMid, "an RX 580 (a name that is no Deck)");
  not_handheld(Named(kVendorNvidia, "NVIDIA GeForce RTX 5080", 16303), GpuClass::kHigh, "the test PC's card");

  // The Deck is still found by adapter type and the rest of the facts are as for any integrated GPU: the log line's phrase for the rule.
  CheckAdapter(DeckFacts("AMD Custom GPU 0405 (RADV VANGOGH)"), kL, Rule::kAdapterType, "by adapter type: UMA");
  CheckAdapter(DeckFacts("AMD Custom GPU 0932 (RADV VANGOGH)"), kL, Rule::kAdapterType, "Steam Deck");

  // The table's own shape.
  Check(kHandheldRows >= 1 && kHandheldRows <= 8, "the handheld table has rows");
  for (int i = 0; i < kHandheldRows; ++i) {
    const SpeedRow& r = kHandheldTable[i];
    const std::string at = "handheld row " + std::to_string(i) + " (" + std::string(r.family) + ")";
    Check(!r.family.empty(), at + ": a family name for the log");
    Check(r.vendor == kVendorAmd, at + ": AMD only (the Deck's chip)");
    Check(r.cap == GpuClass::kLow, at + ": the class stays low");
    Check(!(r.word.empty() && r.series.empty()), at + ": has a word or a series to match");
    Check(NormalizeName(r.word) == r.word && NormalizeName(r.series) == r.series, at + ": written the way NormalizeName writes a name");
    Check(g_handheld_row_hits[i] > 0, at + ": has a test name that it decides");
  }
}

void CheckUpscaleDefaults() {
  using namespace mcla::gpu;
  using upscale_defaults::Path;
  for (const GpuClass cls : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    Check(upscale_defaults::Select(cls, {}, true) ==
              (cls == GpuClass::kLow ? Path::kFsr31 : Path::kUnchanged),
          "only Low defaults to FSR 3.1");
    Check(upscale_defaults::Select(cls, {}, false) ==
              (cls == GpuClass::kLow ? Path::kFsr1 : Path::kUnchanged),
          "only Low defaults to FSR 1 when FSR 3.1 is unavailable at startup");
  }
  const upscale_defaults::DisplayChoices explicit_paths[] = {
      {.upscaler = true}, {.render_scale = true}, {.fsr1 = true}, {.fsr1_preset = true},
  };
  for (const auto& choice : explicit_paths) {
    for (const bool fsr31 : {false, true}) {
      Check(upscale_defaults::Select(GpuClass::kLow, choice, fsr31) == Path::kUnchanged,
            "an explicit display path takes precedence on Low");
    }
  }
  Upscaler upscaler;
  UpscaleMode mode;
  FsrPreset fsr1 = FsrPreset::kCustom;
  const bool parsed_upscaler = upscale_modes::ParseUpscaler(upscale_defaults::kFsr31, &upscaler);
  const bool parsed_mode = upscale_modes::ParseMode(upscale_defaults::kQuality, &mode);
  Check(parsed_upscaler && upscaler == Upscaler::kFsr31, "the default selector requests FSR 3.1");
  Check(parsed_mode && mode == UpscaleMode::kQuality, "the default mode requests Quality");
  Check(ParseFsrPreset(upscale_defaults::kQuality, &fsr1) && fsr1 == FsrPreset::kQuality,
        "the fallback uses FSR 1 Quality");
  Check(upscale_defaults::kFsr31Sharpness == "0.4", "FSR 3.1 uses the 0.4 RCAS step");
  Check(upscale_defaults::kFsr1Sharpness == "0.8", "FSR 1 uses 0.8 stops of RCAS reduction");

  struct Size {
    uint32_t output_width, output_height, scene_width, scene_height;
  };
  const Size sizes[] = {
      {1280, 720, 832, 468}, {1920, 1080, 1280, 720},
      {2560, 1440, 1728, 972}, {3840, 2160, 2560, 1440},
  };
  for (const auto& size : sizes) {
    const auto output = ChooseUpscalerOutputScale(0.0, false, FsrPreset::kCustom,
                                                 size.output_width, size.output_height);
    const auto scene = ChooseSceneScale(output.scale, upscale_modes::InfoOf(upscaler, mode).ratio);
    Check(scene.scale.Edge(kAssumedScreenWidth) == size.scene_width &&
              scene.scale.Edge(kAssumedScreenHeight) == size.scene_height,
          "FSR Quality scene dimensions at output height " + std::to_string(size.output_height));
    const auto fallback = ChooseRenderScale(0.0, true, fsr1, size.output_width, size.output_height);
    Check(fallback.scale == scene.scale, "FSR 1 Quality matches the scene size at output height " +
                                            std::to_string(size.output_height));
    const auto native = ChooseRenderScale(0.0, false, FsrPreset::kCustom, size.output_width,
                                          size.output_height);
    Check(native.scale.Edge(kAssumedScreenWidth) == size.output_width &&
              native.scale.Edge(kAssumedScreenHeight) == size.output_height,
          "the stock scale matches the output at height " + std::to_string(size.output_height));
  }
}

}  // namespace

int main() {
  CheckTable();
  CheckSunShaftSteps();
  CheckAmbientOcclusion();
  CheckLightShadows();
  CheckVolumetrics();
  CheckSsr();
  CheckLabels();
  CheckWords();
  CheckNames();
  CheckClassify();
  CheckNormalize();
  CheckSpeedRows();
  CheckSeriesNumber();
  CheckSpeedTable();
  CheckUpscaleDefaults();
  CheckHandheldCells();
  CheckHandheldDetect();
  CheckCityLod();
  CheckScales();
  if (g_failed != 0) {
    std::printf("gpu_class_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("gpu_class_test: %d checks passed\n", g_checks);
  return 0;
}
