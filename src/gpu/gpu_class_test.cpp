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
    {Effect::kShadowFilter, "mcla_gpu_shadow_filter", "game", "soft", "soft", "AUTO (GAME)",
     "AUTO (SOFT)", "AUTO (SOFT)"},
    {Effect::kShadowResolution, "mcla_gpu_shadow_resolution", "game", "high", "high", "AUTO (GAME)",
     "AUTO (HIGH)", "AUTO (HIGH)"},
    {Effect::kEnvResolution, "mcla_gpu_env_resolution", "game", "game", "high", "AUTO (GAME)",
     "AUTO (GAME)", "AUTO (HIGH)"},
    {Effect::kReflectionResolution, "mcla_gpu_reflection_resolution", "game", "high", "high",
     "AUTO (GAME)", "AUTO (HIGH)", "AUTO (HIGH)"},
    // The cells are STORED words; the labels are what the player sees (ShownWord: nothing is called low, so the stored low is
    // HIGH and the stored high is ULTRA for the contact shadows, the bounce light and the ambient occlusion).
    {Effect::kContactShadows, "mcla_gpu_contact_shadows", "off", "low", "high", "AUTO (OFF)",
     "AUTO (HIGH)", "AUTO (ULTRA)"},
    {Effect::kSunShafts, "mcla_gpu_sun_shafts", "off", "default", "default", "AUTO (OFF)",
     "AUTO (DEFAULT)", "AUTO (DEFAULT)"},
    // Bounce light: low on the High class only (about 0.75 ms at 4K on an RTX 5080), off on Mid and Low.
    // (the stored low is shown as HIGH: gpu_class.h ShownWord)
    {Effect::kSsgi, "mcla_gpu_ssgi", "off", "off", "low", "AUTO (OFF)", "AUTO (OFF)",
     "AUTO (HIGH)"},
    {Effect::kTrafficShadows, "mcla_gpu_traffic_shadows", "off", "near", "near", "AUTO (OFF)",
     "AUTO (NEAR)", "AUTO (NEAR)"},
    // The car self-reflections have one level, `on` (low and high cost the same: perf/out/FXCOST-B6.txt, owner 2026-11-14).
    {Effect::kCarSelfRefl, "mcla_gpu_car_selfrefl", "off", "on", "on", "AUTO (OFF)", "AUTO (ON)",
     "AUTO (ON)"},
    // Ambient occlusion (AO-DESIGN.md section 5: high is about 3 ms at 1080p on a GTX 1050-class card,
    // low about 1 ms; nothing on shared memory).
    {Effect::kAmbientOcclusion, "mcla_gpu_ao", "off", "low", "high", "AUTO (OFF)", "AUTO (HIGH)",
     "AUTO (ULTRA)"},
};

// Every value a player's own choice can have, across the effects: none of them is touched.
const char* const kOwnChoices[] = {"off",  "game", "low",  "high", "ultra",
                                   "soft", "pcss", "on",   "",     "Auto",
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

// Ambient occlusion's `auto` (mcla_gpu_ao): off on shared memory, low on a small discrete card, high on
// the rest; a value an older larecomp.toml holds (off, low, high) is the player's and is kept in every class.
void CheckAmbientOcclusion() {
  const GpuClass classes[] = {GpuClass::kLow, GpuClass::kMid, GpuClass::kHigh};
  const char* const want[] = {"off", "low", "high"};
  const char* const own[] = {"off", "low", "high"};
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "low" ? 1 : 2; };
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

void CheckLabels() {
  CheckEq(AutoLabelOf("high"), "AUTO (HIGH)", "AutoLabelOf(high)");
  CheckEq(AutoLabelOf("Soft"), "AUTO (SOFT)", "AutoLabelOf(Soft)");
  CheckEq(Upper("a1-z"), "A1-Z", "Upper keeps what is not a letter");
}

// What a player sees for a stored value (ShownWord): nothing is called low. The ambient occlusion, the contact shadows and the
// bounce light keep the stored words off / low / high and show them as OFF / HIGH / ULTRA; the car self-reflections' one level is
// ON (the old low and high too); every other setting's words are shown as they are stored.
void CheckShownWords() {
  for (const Effect effect : {Effect::kAmbientOcclusion, Effect::kContactShadows, Effect::kSsgi}) {
    const std::string name(CvarName(effect));
    CheckEq(std::string(ShownWord(effect, "off")), "off", name + ": off is shown as off");
    CheckEq(std::string(ShownWord(effect, "low")), "high", name + ": the stored low is shown as high");
    CheckEq(std::string(ShownWord(effect, "high")), "ultra", name + ": the stored high is shown as ultra");
    CheckEq(std::string(ShownWord(effect, "auto")), "auto", name + ": auto is shown as auto");
  }
  for (const char* stored : {"on", "low", "high"}) {
    CheckEq(std::string(ShownWord(Effect::kCarSelfRefl, stored)), "on", std::string("car self-reflections: ") + stored + " is shown as on");
  }
  CheckEq(std::string(ShownWord(Effect::kCarSelfRefl, "off")), "off", "car self-reflections: off is shown as off");
  // The other settings keep their words (the side views' high is HIGH already, the shafts, the filter and the traffic shadows have none of these).
  for (const Effect effect : {Effect::kShadowFilter, Effect::kShadowResolution, Effect::kEnvResolution, Effect::kReflectionResolution,
                              Effect::kSunShafts, Effect::kTrafficShadows}) {
    for (const char* stored : {"off", "game", "low", "high", "ultra", "soft", "default", "near"}) {
      CheckEq(std::string(ShownWord(effect, stored)), stored, std::string(CvarName(effect)) + ": '" + stored + "' is shown as stored");
    }
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

  // Shared memory (what the render scale cap leaves alone): software, UMA or integrated; a discrete card
  // is not, whatever its size.
  Check(!SharesMemory(Discrete(16303)) && !SharesMemory(Discrete(512)) && !SharesMemory(Discrete(0)),
        "a discrete card does not share memory");
  Check(SharesMemory(igpu) && SharesMemory(deck) && SharesMemory(big_apu) && SharesMemory(vk_igpu) &&
            SharesMemory(soft),
        "an iGPU, the Deck, an APU with a large carve-out, a Vulkan integrated GPU and a software adapter share memory");

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

}  // namespace

int main() {
  CheckTable();
  CheckSunShaftSteps();
  CheckAmbientOcclusion();
  CheckLabels();
  CheckShownWords();
  CheckNames();
  CheckClassify();
  CheckCityLod();
  if (g_failed != 0) {
    std::printf("gpu_class_test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  std::printf("gpu_class_test: %d checks passed\n", g_checks);
  return 0;
}
