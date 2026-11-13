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
    {Effect::kContactShadows, "mcla_gpu_contact_shadows", "off", "low", "high", "AUTO (OFF)",
     "AUTO (LOW)", "AUTO (HIGH)"},
    {Effect::kSunShafts, "mcla_gpu_sun_shafts", "off", "low", "high", "AUTO (OFF)", "AUTO (LOW)",
     "AUTO (HIGH)"},
    {Effect::kSsgi, "mcla_gpu_ssgi", "off", "off", "low", "AUTO (OFF)", "AUTO (OFF)",
     "AUTO (LOW)"},
};

// Every value a player's own choice can have, across the effects: none of them is touched.
const char* const kOwnChoices[] = {"off",  "game", "low",  "high", "ultra",
                                   "soft", "pcss", "on",   "",     "Auto",
                                   "AUTO", "mid",  "auto " /* a space: not auto */};

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

void CheckLabels() {
  CheckEq(AutoLabelOf("high"), "AUTO (HIGH)", "AutoLabelOf(high)");
  CheckEq(AutoLabelOf("Soft"), "AUTO (SOFT)", "AutoLabelOf(Soft)");
  CheckEq(Upper("a1-z"), "A1-Z", "Upper keeps what is not a letter");
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

}  // namespace

int main() {
  CheckTable();
  CheckLabels();
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
