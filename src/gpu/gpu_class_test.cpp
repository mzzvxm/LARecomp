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
    // Light shadows (local_shadow.h; the headlights now, the street lamps later): high on Mid (four maps of 512: a map for each lamp of the player's car and of the nearest other car), ultra on High
    // (six of 1024: the next nearest car's too), nothing on shared memory.
    {Effect::kLightShadows, "mcla_gpu_light_shadows", "off", "high", "ultra", "AUTO (OFF)",
     "AUTO (HIGH)", "AUTO (ULTRA)"},
    // Volumetric light (VOLUMETRICS-SURVEY.md section 4.1): off on Low, medium on Mid, high on High (ultra is a deliberate pick).
    {Effect::kVolumetrics, "mcla_gpu_volumetrics", "off", "medium", "high", "AUTO (OFF)", "AUTO (MEDIUM)",
     "AUTO (HIGH)"},
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

// Ambient occlusion's `auto` (mcla_gpu_ao): off on shared memory, high on a small discrete card, ultra on
// the rest; a value the player holds (off, high, ultra) is kept in every class.
void CheckAmbientOcclusion() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "high", "ultra"};
  const char* const own[] = {"off", "high", "ultra"};
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

// Light shadows' `auto` (mcla_gpu_light_shadows, local_shadow.h): nothing on shared memory, the small map (high) on Mid, the big one (ultra)
// on High; a value the player holds is kept in every class, and the classes only go up.
void CheckLightShadows() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "high", "ultra"};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "high" ? 1 : 2; };
  int last = -1;
  for (int k = 0; k < 3; ++k) {
    const GpuClass c = classes[k];
    const std::string at = "light shadows on " + std::string(ClassName(c));
    CheckEq(ResolveFor(c, Effect::kLightShadows, "auto"), want[k], at + ": auto");
    for (const char* v : {"off", "high", "ultra"}) {
      CheckEq(ResolveFor(c, Effect::kLightShadows, v), v, at + ": the saved '" + v + "' is kept");
    }
    const int now = rank(Cell(Effect::kLightShadows, c));
    Check(now >= last, at + ": not below the class under it");
    last = now;
  }
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

void CheckLabels() {
  CheckEq(AutoLabelOf("high"), "AUTO (HIGH)", "AutoLabelOf(high)");
  CheckEq(AutoLabelOf("Soft"), "AUTO (SOFT)", "AutoLabelOf(Soft)");
  CheckEq(Upper("a1-z"), "A1-Z", "Upper keeps what is not a letter");
}

// The stored word is the shown word: nothing in the graphics settings is called low (owner, 2026-11-14). The ambient occlusion, the
// contact shadows and the bounce light are off / high / ultra, the car self-reflections off / on; every cell of the table is one of the
// words its setting allows, and no cell and no label of an `auto` says low.
void CheckWords() {
  for (const Effect effect : {Effect::kAmbientOcclusion, Effect::kContactShadows, Effect::kSsgi, Effect::kLightShadows}) {
    const std::string name(CvarName(effect));
    for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
      const std::string cell(Cell(effect, c));
      Check(cell == "off" || cell == "high" || cell == "ultra", name + " on " + std::string(ClassName(c)) + ": the cell '" + cell + "' is off, high or ultra");
    }
  }
  for (const GpuClass c : {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh}) {
    const std::string cell(Cell(Effect::kCarSelfRefl, c));
    Check(cell == "off" || cell == "on", "car self-reflections on " + std::string(ClassName(c)) + ": the cell '" + cell + "' is off or on");
    const std::string headlights(Cell(Effect::kHeadlights, c));
    Check(headlights == "off" || headlights == "on", "headlights on " + std::string(ClassName(c)) + ": the cell '" + headlights + "' is off or on");
  }
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
  CheckAdapter(Named(nv, "NVIDIA GeForce GTX 1650", 4096), kM, kByMem);
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
  CheckAdapter(Named(amd, "AMD Radeon RX 6400", 4096), kM, kByMem);
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

}  // namespace

int main() {
  CheckTable();
  CheckSunShaftSteps();
  CheckAmbientOcclusion();
  CheckLightShadows();
  CheckVolumetrics();
  CheckLabels();
  CheckWords();
  CheckNames();
  CheckClassify();
  CheckNormalize();
  CheckSpeedRows();
  CheckSeriesNumber();
  CheckSpeedTable();
  CheckCityLod();
  if (g_failed != 0) {
    std::printf("gpu_class_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("gpu_class_test: %d checks passed\n", g_checks);
  return 0;
}
