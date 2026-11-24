// Check of the side mirrors' setting without ray tracing, its pause row and how it meets RT SIDE MIRRORS and the debug switches (mirror_setting.h, gpu_class.h kSideMirrors): MIRROR-VIEW.md package MV6.
// A program of its own: it needs no renderer, no game files and no GPU, and exits 0 when every check holds.
//
//   build: cmake --build <dir> --target larecomp_mirror_setting_test (src/gpu/CMakeLists.txt), or by hand
//     clang++ -std=c++23 -O1 -I src -I src/gpu src/gpu/mirror_setting_test.cpp
//   run: larecomp_mirror_setting_test [-v] [--table] [<src/gpu dir>]   (without the path the sources are found from this file's own place; -v prints every check; --table prints the cells and levels)
//
// It checks, against a model written out here (not computed from the code under test): what each word of mcla_gpu_side_mirrors comes to on every class, handheld flag and ray tracing tier, next to every word
// of mcla_gpu_car_mirror_rt and every value of the two debug switches (mode, cadence, finder, overlay, the (REPLACED) state, the reason); that the traced mirrors win exactly where the pass's own Wanted says,
// and not otherwise; the class table's cells and AUTO's labels (nothing is called low); the levels' size and rate (the shared view's plan); the row's words, label and page width; and then the TEXT of the wiring:
// the row in pause_menu.cpp (its place, its words, its dynamics, never grey), the setting in the three settings files (live, not debug-only, the four words), the class table's entry, the readers of the
// debug switches (all go through mirror_setting.h now), the request's cadence, the finder's rule, the MV4 hook and the CMake lines.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "car_mirror_rt_rules.h"
#include "car_mirror_setting.h"
#include "gpu_class.h"
#include "mirror_setting.h"
#include "mirror_view_rules.h"
#include "rt_tier.h"

namespace ms = mcla::gpu::mirror_setting;
namespace cs = mcla::gpu::car_mirror_setting;
namespace cm = mcla::gpu::car_mirror;
namespace gc = mcla::gpu::gpu_class;
namespace mv = mcla::gpu::mirror_view;

namespace {

int g_checks = 0;
int g_failed = 0;
bool g_verbose = false;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED: %s\n", what.c_str());
  } else if (g_verbose) {
    std::printf("ok: %s\n", what.c_str());
  }
}

constexpr gc::GpuClass kClasses[] = {gc::GpuClass::kLow, gc::GpuClass::kMid, gc::GpuClass::kHigh};
constexpr gc::RtTier kTiers[] = {gc::RtTier::kNone, gc::RtTier::kWeak, gc::RtTier::kStrong};
const char* const kWords[] = {"auto", "off", "high", "ultra", "", "low", "AUTO", "on", "bogus", "auto ", "medium"};
const char* const kRtWords[] = {"auto", "off", "on", "", "high", "bogus"};
constexpr int kDebugViews[] = {-1, 0, 1, 2, 3, 4, 5};

std::string Name(gc::GpuClass c, bool handheld, gc::RtTier t, const std::string& word, const std::string& rt) {
  return std::string("class ") + std::string(gc::ClassName(c)) + (handheld ? " (handheld)" : "") + ", tier " + std::string(gc::RtTierName(t)) + ", word '" + word + "', rt word '" + rt + "'";
}

// ---------------------------------------------------------------------------
// the model: the owner's plan, written out
// ---------------------------------------------------------------------------

// AUTO's cell: off on the Low class (a handheld has no cell of its own for this effect), high on Mid, ultra on High.
std::string ModelCell(gc::GpuClass c) { return c == gc::GpuClass::kLow ? "off" : c == gc::GpuClass::kMid ? "high" : "ultra"; }

// The traced mirrors' own answer: the rung (the tier, no more than weak on Mid, none on Low), and strong only by AUTO; `on` runs on any rung but none; a word that is none of the three is auto.
bool ModelTraced(gc::GpuClass c, gc::RtTier t, const std::string& word) {
  gc::RtTier rung = t;
  if (c == gc::GpuClass::kLow) rung = gc::RtTier::kNone;
  if (c == gc::GpuClass::kMid && t == gc::RtTier::kStrong) rung = gc::RtTier::kWeak;
  if (rung == gc::RtTier::kNone) return false;
  if (word == "off") return false;
  if (word == "on") return true;
  return rung == gc::RtTier::kStrong;
}

int ModelLevelMode(gc::GpuClass c, const std::string& word) {
  const std::string level = word == "off" || word == "high" || word == "ultra" ? word : ModelCell(c);
  return level == "high" ? 3 : level == "ultra" ? 4 : 0;
}

struct Model {
  int mode;
  unsigned every;
  bool finder, overlay, replaced;
  ms::Why why;
};

Model ModelOf(gc::GpuClass c, gc::RtTier t, const std::string& word, const std::string& rt, int debug_view, bool glass, bool overlay) {
  Model m{};
  const int level = ModelLevelMode(c, word);
  const bool traced = ModelTraced(c, t, rt);
  m.replaced = level != 0 && traced;
  if (debug_view >= 1 && debug_view <= 4) {
    m.mode = debug_view;
    m.every = 1;
    m.finder = glass;
    m.overlay = overlay;
    m.why = ms::Why::kForced;
    return m;
  }
  m.mode = traced ? 0 : level;
  m.every = m.mode == 3 ? 2 : 1;
  m.finder = glass || m.mode != 0;
  m.overlay = overlay || m.mode != 0;
  m.why = m.mode != 0 ? ms::Why::kRow : m.replaced ? ms::Why::kReplaced : ms::Why::kOff;
  return m;
}

void ResolveChecks() {
  long combos = 0;
  long bad = 0;
  for (const gc::GpuClass c : kClasses) {
    for (const bool handheld : {false, true}) {
      for (const gc::RtTier t : kTiers) {
        for (const char* word : kWords) {
          for (const char* rt : kRtWords) {
            for (const int dv : kDebugViews) {
              for (const bool glass : {false, true}) {
                for (const bool overlay : {false, true}) {
                  ++combos;
                  ms::Facts f;
                  f.word = word;
                  f.rt_word = rt;
                  f.cls = c;
                  f.handheld = handheld;
                  f.tier = t;
                  f.debug_view = dv;
                  f.debug_glass = glass;
                  f.debug_overlay = overlay;
                  const ms::Result r = ms::Resolve(f);
                  const Model m = ModelOf(c, t, word, rt, dv, glass, overlay);
                  const bool same = r.mode == m.mode && r.every == m.every && r.finder == m.finder && r.overlay == m.overlay && r.replaced == m.replaced && r.why == m.why;
                  if (!same) {
                    ++bad;
                    if (bad <= 10) {
                      std::printf("  %s, debug %d glass %d overlay %d: got mode %d every %u finder %d overlay %d replaced %d why %d; model mode %d every %u finder %d overlay %d replaced %d why %d\n",
                                  Name(c, handheld, t, word, rt).c_str(), dv, glass, overlay, r.mode, r.every, r.finder, r.overlay, r.replaced, int(r.why), m.mode, m.every, m.finder, m.overlay,
                                  m.replaced, int(m.why));
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  Check(bad == 0, "Resolve is the model's for every class, handheld flag, tier, word, traced word and debug value (" + std::to_string(combos) + " combinations, " + std::to_string(bad) + " differ)");
  Check(combos == 3L * 2 * 3 * 11 * 6 * 7 * 2 * 2, "every combination was looked at");
}

// The named cases of the plan, so a reader sees them without the model.
void NamedCases() {
  const auto go = [](gc::GpuClass c, gc::RtTier t, const char* word, const char* rt, int debug = 0, bool glass = false) {
    ms::Facts f;
    f.word = word;
    f.rt_word = rt;
    f.cls = c;
    f.tier = t;
    f.debug_view = debug;
    f.debug_glass = glass;
    return ms::Resolve(f);
  };
  // AUTO per class on a card that cannot trace: the weakest is off, Mid is high (512 wide, every second frame), High is ultra (768, every frame)
  Check(go(gc::GpuClass::kLow, gc::RtTier::kNone, "auto", "auto").mode == 0 && go(gc::GpuClass::kMid, gc::RtTier::kNone, "auto", "auto").mode == 3 && go(gc::GpuClass::kHigh, gc::RtTier::kNone, "auto", "auto").mode == 4,
        "AUTO: off on Low, high (mode 3) on Mid, ultra (mode 4) on High, where the card cannot trace");
  Check(go(gc::GpuClass::kMid, gc::RtTier::kNone, "auto", "auto").every == 2 && go(gc::GpuClass::kHigh, gc::RtTier::kNone, "auto", "auto").every == 1, "HIGH redraws every second frame, ULTRA every frame");
  // the player's own words are never overridden by the class, on every class
  for (const gc::GpuClass c : kClasses) {
    Check(go(c, gc::RtTier::kNone, "high", "auto").mode == 3 && go(c, gc::RtTier::kNone, "ultra", "auto").mode == 4 && go(c, gc::RtTier::kNone, "off", "auto").mode == 0,
          std::string("a hand pick is kept on the ") + std::string(gc::ClassName(c)) + " class");
  }
  // a weak ray tracing card on the High class: the traced mirrors' AUTO is off, so this row runs (the RTX 3060 case)
  Check(go(gc::GpuClass::kHigh, gc::RtTier::kWeak, "auto", "auto").mode == 4 && !go(gc::GpuClass::kHigh, gc::RtTier::kWeak, "auto", "auto").replaced, "a weak ray tracing card on High: the traced mirrors are off, the plain ones run");
  // a strong card: the traced mirrors (AUTO is on) replace the row, and the row says so
  const ms::Result strong = go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", "auto");
  Check(strong.mode == 0 && strong.replaced && strong.why == ms::Why::kReplaced && !strong.finder && !strong.overlay && strong.every == 1, "a strong card: RT SIDE MIRRORS replaces the row, nothing of the plain view runs");
  // ... and the plain row applies again the moment the traced mirrors go off
  Check(go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", "off").mode == 4 && !go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", "off").replaced, "RT SIDE MIRRORS off on a strong card: the plain row runs again");
  // a row that is off has nothing to replace: no (REPLACED)
  Check(!go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", "auto").replaced && go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "off", "auto").why == ms::Why::kOff, "an off row is not replaced by anything");
  // a forced switch beats both (the A/B arms of the cost chain: the 5080 has strong ray tracing)
  const ms::Result forced = go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", "auto", 1);
  Check(forced.mode == 1 && forced.every == 1 && !forced.finder && !forced.overlay && forced.why == ms::Why::kForced && forced.replaced, "debug 1 forces the view on a strong card, every frame, with no finder of its own");
  Check(go(gc::GpuClass::kHigh, gc::RtTier::kStrong, "auto", "auto", 1, true).finder, "... and mcla_gpu_mirror_glass adds the finder (the arm 'glass')");
  Check(go(gc::GpuClass::kLow, gc::RtTier::kNone, "off", "off", 2).mode == 2 && go(gc::GpuClass::kLow, gc::RtTier::kNone, "off", "off", 3).mode == 3 && go(gc::GpuClass::kLow, gc::RtTier::kNone, "off", "off", 4).mode == 4,
        "debug 2, 3, 4 force the Low, Mid, High size whatever the row says");
  // the finder by default: only with the row's view
  Check(go(gc::GpuClass::kMid, gc::RtTier::kNone, "auto", "auto").finder && !go(gc::GpuClass::kLow, gc::RtTier::kNone, "auto", "auto").finder && !strong.finder, "the finder runs with the row's view: on Mid, not on Low under AUTO, not where the traced mirrors replace it");
  Check(go(gc::GpuClass::kLow, gc::RtTier::kNone, "auto", "auto", 0, true).finder, "the debug switch runs the finder whatever the row says");
  // the glass overlay (MV4)
  ms::Facts f;
  f.word = "auto";
  f.cls = gc::GpuClass::kMid;
  Check(ms::Resolve(f).overlay && !ms::Resolve(ms::Facts{}).overlay, "the overlay hook follows the row's view (and its own debug switch)");
}

void RtMeetsChecks() {
  // The traced mirrors win exactly where the pass's own rule says they are wanted, for every class, tier and word: the row's hand-off is the renderer's Wanted, not a copy of it.
  for (const gc::GpuClass c : kClasses) {
    for (const gc::RtTier t : kTiers) {
      for (const char* rt : kRtWords) {
        const bool host = cm::Wanted(cm::SettingOf(rt), true, gc::RtClass(c, t));
        Check(ms::TracedWanted(c, t, rt) == host, Name(c, false, t, "", rt) + ": the traced mirrors are wanted as the renderer's Wanted says");
        ms::Facts f;
        f.word = "ultra";
        f.rt_word = rt;
        f.cls = c;
        f.tier = t;
        const ms::Result r = ms::Resolve(f);
        Check((r.mode == 0) == host, Name(c, false, t, "ultra", rt) + ": the plain row runs exactly where the traced mirrors do not");
        Check(r.replaced == host, Name(c, false, t, "ultra", rt) + ": and says (REPLACED) exactly where they do");
      }
    }
  }
}

// ---------------------------------------------------------------------------
// the class table, the levels
// ---------------------------------------------------------------------------

void TableChecks() {
  Check(gc::CvarName(gc::Effect::kSideMirrors) == "mcla_gpu_side_mirrors", "the table's cvar is mcla_gpu_side_mirrors");
  for (const gc::GpuClass c : kClasses) {
    const std::string cell(gc::Cell(gc::Effect::kSideMirrors, c));
    Check(cell == ModelCell(c), std::string("the cell on the ") + std::string(gc::ClassName(c)) + " class is " + ModelCell(c) + " (is '" + cell + "')");
    Check(gc::ResolveFor(c, gc::Effect::kSideMirrors, "auto") == ModelCell(c), std::string("Resolve(auto) on ") + std::string(gc::ClassName(c)));
    for (const char* own : {"off", "high", "ultra"}) {
      Check(gc::ResolveFor(c, gc::Effect::kSideMirrors, own) == own, std::string("the saved '") + own + "' is kept on " + std::string(gc::ClassName(c)));
    }
    for (const bool handheld : {false, true}) {
      const std::string label = ms::AutoLabel(c, handheld);
      Check(label == "AUTO (" + std::string(ModelCell(c) == "off" ? "OFF" : ModelCell(c) == "high" ? "HIGH" : "ULTRA") + ")", std::string("AUTO's label on ") + std::string(gc::ClassName(c)) + ": " + label);
      Check(label.find("LOW") == std::string::npos && label.size() <= ms::kWidestValue, "AUTO's label says no low and fits the page's value width: " + label);
      Check(ms::CellWord(c, handheld) == (ModelCell(c) == "off" ? ms::Word::kOff : ModelCell(c) == "high" ? ms::Word::kHigh : ms::Word::kUltra), "CellWord is the cell as a word");
    }
  }
  // the classes only go up
  const auto rank = [](std::string_view v) { return v == "off" ? 0 : v == "high" ? 1 : 2; };
  Check(rank(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kLow)) <= rank(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kMid)) &&
            rank(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kMid)) <= rank(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kHigh)),
        "the classes only go up");
  Check(gc::kEffectCount == 17 && int(gc::Effect::kSideMirrors) == 16, "the effect is the seventeenth, last in the table");
  // the cells are words of the row, none called low
  for (const gc::GpuClass c : kClasses) {
    const std::string cell(gc::Cell(gc::Effect::kSideMirrors, c));
    Check(cell == "off" || cell == "high" || cell == "ultra", "the cell '" + cell + "' is off, high or ultra");
  }
}

void LevelChecks() {
  // HIGH is the Mid size (512 wide, every second frame), ULTRA the High size (768, every frame): the plan of mirror_view_rules.h, one table.
  Check(ms::kModeHigh == 3 && ms::kModeUltra == 4 && ms::kModeOff == 0, "the modes the row gives are 0, 3 and 4 (the debug switch's own numbers)");
  Check(ms::SizeOfMode(ms::kModeHigh) == mv::Class::kMid && ms::SizeOfMode(ms::kModeUltra) == mv::Class::kHigh, "HIGH is the Mid size, ULTRA the High size");
  Check(mv::MaxWidth(ms::SizeOfMode(ms::kModeHigh)) == 512 && mv::MaxWidth(ms::SizeOfMode(ms::kModeUltra)) == 768, "512 and 768 pixels wide");
  Check(ms::CadenceOfMode(ms::kModeHigh) == 2 && ms::CadenceOfMode(ms::kModeUltra) == 1 && ms::CadenceOfMode(ms::kModeOff) == 1, "every second frame, every frame, and 1 for off");
  Check(ms::CadenceOfMode(ms::kModeHigh) == mv::Cadence(mv::Class::kMid) && ms::CadenceOfMode(ms::kModeUltra) == mv::Cadence(mv::Class::kHigh), "the cadence is the plan's");
  // the view's pixels per degree (why two levels): 7 and 10, the third size (5) is the Low class's, which AUTO turns off
  Check(mv::PixelsPerDegree(mv::Class::kMid) == 7.0 && mv::PixelsPerDegree(mv::Class::kHigh) == 10.0 && mv::PixelsPerDegree(mv::Class::kLow) == 5.0 && mv::MaxWidth(mv::Class::kLow) == 384, "the three sizes of the plan");
  // the row's words
  Check(ms::WordOf("auto") == ms::Word::kAuto && ms::WordOf("off") == ms::Word::kOff && ms::WordOf("high") == ms::Word::kHigh && ms::WordOf("ultra") == ms::Word::kUltra,
        "the four words");
  for (const char* w : {"", "low", "medium", "on", "AUTO", "Ultra", "auto "}) {
    Check(ms::WordOf(w) == ms::Word::kAuto, std::string("a word that is none of the four ('") + w + "') is auto (an older larecomp.toml)");
  }
  // the mode of an explicit word
  Check(ms::ModeOfWord(ms::Word::kOff) == 0 && ms::ModeOfWord(ms::Word::kHigh) == 3 && ms::ModeOfWord(ms::Word::kUltra) == 4 && ms::ModeOfWord(ms::Word::kAuto) == 0, "the mode of a word");
}

void RowChecks() {
  Check(std::string(ms::RowLabel()) == "SIDE MIRRORS" && std::string(ms::kFullLabel) + ": " == "SIDE MIRRORS: ", "the row's whole label, the menu's prefix");
  Check(std::string(ms::RowSuffix(false)).empty() && std::string(ms::RowSuffix(true)) == " (REPLACED)", "no suffix, or ' (REPLACED)'");
  Check(ms::LabelChars(false) == 12 && ms::LabelChars(true) == 23, "12 characters, 23 with the suffix");
  Check(ms::LabelChars(true) <= cs::kWidestOtherLabel && ms::LabelChars(true) + ms::kWidestValue <= cs::kPageChars, "the widest state fits the REFLECTIONS page (41 characters) and is no wider than its widest label (23)");
  Check(ms::kWidestValue == 12 && ms::kWidestValue <= cs::kWidestValue, "the widest value, 'AUTO (ULTRA)', is under the page's widest (17)");
  // every other label of the page in every state (the page's table is row_dependency_test.cpp's; this keeps the new row from widening it)
  for (const bool replaced : {false, true}) {
    Check(ms::LabelChars(replaced) + std::string("AUTO (RAY TRACED)").size() <= 41, "the page stays within 41 with the widest value of the page");
  }
  Check(ms::LauncherAutoLine() == "Auto is Off on integrated graphics, High on a mid range graphics card and Ultra on a fast one. Where RT SIDE MIRRORS runs, it takes the place of this row.",
        "the launcher's line: " + ms::LauncherAutoLine());
  Check(ms::WhyName(ms::Why::kForced) == "forced by mcla_gpu_mirror_view" && ms::WhyName(ms::Why::kReplaced) == "replaced by the traced mirrors" && ms::WhyName(ms::Why::kRow) == "the row" && ms::WhyName(ms::Why::kOff) == "off",
        "the reasons' names (for the log)");
}

// ---------------------------------------------------------------------------
// the text of the wiring
// ---------------------------------------------------------------------------

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("cannot open %s\n", path.c_str());
    return "";
  }
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}
bool Has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
size_t At(const std::string& text, const std::string& what, size_t from = 0) { return text.find(what, from); }
int Count(const std::string& text, const std::string& what) {
  int n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
  return n;
}
bool Before(const std::string& text, const std::string& a, const std::string& b) {
  const size_t x = At(text, a), y = At(text, b);
  return x != std::string::npos && y != std::string::npos && x < y;
}
std::string Between(const std::string& text, const std::string& open, const std::string& close) {
  const size_t at = At(text, open);
  if (at == std::string::npos) return "";
  const size_t end = At(text, close, at);
  return end == std::string::npos ? "" : text.substr(at, end - at);
}

// The text of one definition in a settings header: from the name's opening to the blank line that ends its statement (a help text has ");" in it, so the end is the blank line).
std::string DefinitionOf(const std::string& header, const std::string& open) {
  const size_t at = At(header, open);
  if (at == std::string::npos) return "";
  const size_t end = header.find("\n\n", at);
  return end == std::string::npos ? "" : header.substr(at, end - at);
}

void MenuChecks(const std::string& dir) {
  const std::string menu = ReadFile(dir + "/../mc_engine/menus/pause_menu.cpp");
  const size_t items = At(menu, "const ItemDef kReflectionItems[] = {");
  const size_t items_end = items == std::string::npos ? std::string::npos : At(menu, "\n};", items);
  const std::string page = items == std::string::npos || items_end == std::string::npos ? "" : menu.substr(items, items_end - items);
  Check(Has(page, "NativeOnly(Str(\"PM_RxGpuSideMirrors\", \"mcla_gpu_side_mirrors\",") && Has(page, "\"SIDE MIRRORS: \", kNativeSideMirrorsVals, kNativeSideMirrorsNames, 4)),"),
        "the row is a native-only cycle of mcla_gpu_side_mirrors with four values, on the REFLECTIONS page");
  Check(Before(page, "\"PM_RxGpuRtCityRefl\"", "\"PM_RxGpuSideMirrors\"") && Before(page, "\"PM_RxGpuSideMirrors\"", "\"PM_RxGpuRtMirror\"") && Before(page, "\"PM_RxGpuRtMirror\"", "\"PM_RxGpuSsr\""),
        "the row is after RT CITY REFLECTIONS and right before RT SIDE MIRRORS, which comes before REFLECTIONS ON SURFACES (the base row first, as BOUNCE LIGHT is before RT BOUNCE LIGHT)");
  Check(Has(menu, "kNativeSideMirrorsVals[]  = {\"auto\", \"off\", \"high\", \"ultra\"};") && Has(menu, "kNativeSideMirrorsNames[] = {\"AUTO\", \"OFF\", \"HIGH\", \"ULTRA\"};"),
        "the row's words are AUTO / OFF / HIGH / ULTRA (two levels; nothing is called low)");
  // The row's AUTO label and its suffix are data now (src/settings/ui_rules.toml; ui_parity_test.cpp holds them against gpu_class.h and mirror_setting.h)
  const std::string rules = ReadFile(dir + "/../settings/ui_rules.toml");
  const size_t begin = At(rules, "[rows.PM_RxGpuSideMirrors]");
  const size_t next = At(rules, "[rows.PM_RxGpuRtMirror]", begin);
  const std::string row = begin == std::string::npos ? "" : rules.substr(begin, next - begin);
  Check(Has(row, "live.side_mirrors_replaced") && Has(row, "suffix = \" (REPLACED)\"") && Has(row, "enabled = true") && !Has(row, "enabled = false") && Has(row, "auto_label = true"),
        "the rules give AUTO's label (the class table's) and the suffix, and never grey the row (it still sets what runs where the traced mirrors do not)");
  Check(Has(menu, "case \"live.side_mirrors_replaced\"") || Has(menu, "path == \"live.side_mirrors_replaced\""), "the menu answers the rules' fact from the setting's own answer");
  Check(Has(menu, "gpu::mirror_setting::Current().replaced"), "the fact is the setting's own answer (mirror_setting.h Current().replaced)");
  Check(Has(menu, "#include \"gpu/mirror_setting.h\""), "the menu includes the setting's rules");
  Check(Has(menu, "SIDE MIRRORS, RT SIDE MIRRORS, REFLECTIONS ON SURFACES (kReflectionItems)") && Has(menu, "REFLECTIONS (kReflectionItems, 7 rows)"), "the page's comments name the seventh row");
  Check(Has(menu, "\"Road, car, city, mirrors and surfaces.\""), "the tab's line names the mirrors already (RT SIDE MIRRORS)");
  Check(Has(menu, "RT SIDE MIRRORS (the real view in the car's side mirrors") || Has(menu, "RT SIDE MIRRORS (the same traced"), "the page's own comment says what the pair is");
}

void SettingsChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/../settings/generated/settings_gpu.h");
  const std::string manifest = ReadFile(dir + "/../settings/generated/manifest.json");
  const std::string meta = ReadFile(dir + "/../settings/generated/settings_meta.inc");
  // (Written in pieces: tools/check_settings_exposure.py takes a literal definition macro anywhere in src for a setting definition.)
  const std::string define = std::string("REX_SET") + "TING(";
  const std::string def = DefinitionOf(header, define + "std::string, mcla_gpu_side_mirrors, \"auto\"");
  Check(!def.empty() && Has(def, "rex::Allowed(\"auto\", \"off\", \"high\", \"ultra\")"), "mcla_gpu_side_mirrors is a string setting, auto by default, with the four words");
  Check(!def.empty() && !Has(def, "kDebugOnly") && !Has(def, "kRestart") && !Has(def, "kRequiresRestart") && Has(def, "Live."), "it is not debug-only (it has its row) and it is live (no restart trait)");
  Check(!def.empty() && Has(def, "SIDE MIRRORS") && Has(def, "RT SIDE MIRRORS") && Has(def, "GRAPHICS, REFLECTIONS"), "its help names the row and the traced mirrors that replace it");
  Check(Has(header, "inline auto& mcla_gpu_side_mirrors = g_gpu.mcla_gpu_side_mirrors;") && Has(meta, "{\"mcla_gpu_side_mirrors\", \"MCLA/GPU\"},"), "the alias and the meta list have it");
  const size_t mat = At(manifest, "\"mcla_gpu_side_mirrors\": {");
  const size_t mend = mat == std::string::npos ? 0 : manifest.find("\"comments\"", mat);
  const std::string entry = mat == std::string::npos ? "" : manifest.substr(mat, mend - mat);
  Check(!entry.empty() && Has(entry, "\"type\": \"std::string\"") && Has(entry, "\"default\": \"\\\"auto\\\"\"") && Has(entry, "rex::Allowed(\\\"auto\\\", \\\"off\\\", \\\"high\\\", \\\"ultra\\\")") && !Has(entry, "kDebugOnly") &&
            Has(entry, "SIDE MIRRORS"),
        "the manifest entry is the same setting: live, not debug-only, with the row named in its help");
  // the debug switches underneath stay, debug-only, with their numbers
  const std::string view = DefinitionOf(header, define + "i32, mcla_gpu_mirror_view, 0,");
  Check(!view.empty() && Has(view, "rex::kDebugOnly") && Has(view, "rex::Range{0, 4}") && Has(view, "SIDE MIRRORS row decides") && Has(view, "forces the shared rear view on"), "mcla_gpu_mirror_view is still the debug force, 0 to 4, and says that 0 leaves it to the row");
  const std::string glass = DefinitionOf(header, define + "bool, mcla_gpu_mirror_glass, false,");
  Check(!glass.empty() && Has(glass, "rex::kDebugOnly") && Has(glass, "SIDE MIRRORS"), "mcla_gpu_mirror_glass is still a debug switch, and says the row runs the finder by itself");
  // the exposure list needs no exception: the row classifies the setting
  const std::string toml = ReadFile(dir + "/../../tools/settings_exposure.toml");
  Check(!toml.empty() && !Has(toml, "mcla_gpu_side_mirrors"), "tools/settings_exposure.toml has no exception for it (the row classifies it)");
}

void ClassTableChecks(const std::string& dir) {
  const std::string header = ReadFile(dir + "/gpu_class.h");
  Check(Has(header, "kSideMirrors,           // mcla_gpu_side_mirrors") && Has(header, "inline constexpr int kEffectCount = 17;") && Has(header, "{\"mcla_gpu_side_mirrors\", \"off\", \"high\", \"ultra\"},"),
        "gpu_class.h has the effect, last, with its cells");
  const std::string test = ReadFile(dir + "/gpu_class_test.cpp");
  Check(Has(test, "{Effect::kSideMirrors, \"mcla_gpu_side_mirrors\", \"off\", \"high\", \"ultra\", \"AUTO (OFF)\", \"AUTO (HIGH)\", \"AUTO (ULTRA)\"},"), "the class test pins the cells and the labels");
  for (const char* other : {"/neon_light_test.cpp", "/ssr_test.cpp"}) {
    const std::string t = ReadFile(dir + other);
    Check(Has(t, "inline constexpr int kEffectCount = 17;") && !Has(t, "inline constexpr int kEffectCount = 16;"), std::string(other + 1) + " pins the table's new size");
  }
}

void ReaderChecks(const std::string& dir) {
  const std::string cpp = ReadFile(dir + "/mirror_setting.cpp");
  Check(Has(cpp, "LiveText text(\"mcla_gpu_side_mirrors\");") && Has(cpp, "LiveText text(\"mcla_gpu_car_mirror_rt\");") && Count(cpp, "gc::Known() ? gc::Current() : gc::GpuClass::kLow") == 2 && Has(cpp, "f.cls = gc::Known() ? gc::Current() : gc::GpuClass::kLow;") && Has(cpp, "f.handheld = gc::Handheld();") &&
            Has(cpp, "f.tier = gc::CurrentRtTier();") && Has(cpp, "larecomp::settings::mcla_gpu_mirror_view.Get()") && Has(cpp, "larecomp::settings::mcla_gpu_mirror_glass.Get()") && Has(cpp, "return Resolve(f);"),
        "the run-time reader reads both words live, the class (the weakest while unknown), the handheld flag, the tier and the two debug switches, and is the pure Resolve");
  Check(Has(cpp, "int MirrorViewMode() { return mirror_setting::Current().mode; }"), "gpu.h's MirrorViewMode is the answer's mode");
  Check(!Has(cpp, "HOOK(MV4)") && Has(cpp, "f.debug_overlay = larecomp::settings::mcla_gpu_mirror_overlay.Get();"), "the overlay's debug switch is read live (MV4 is merged: the hook is wired)");
  const std::string ov = ReadFile(dir + "/mirror_overlay.cpp");
  Check(!Has(ov, "mcla_gpu_mirror_overlay.Get()") && Count(ov, "mirror_setting::Current().overlay") == 2 && Has(ov, "#include \"mirror_setting.h\""),
        "the overlay's two reads (the precache list at the start, once a frame) are the answer's: the row's view or the debug switch");
  const std::string gpu_h = ReadFile(dir + "/gpu.h");
  Check(Has(gpu_h, "int MirrorViewMode();"), "gpu.h declares what the guest's hooks and the host read");
  // every reader of the old switch goes through the answer now
  const std::string view = ReadFile(dir + "/mirror_view.cpp");
  Check(!Has(view, "mcla_gpu_mirror_view.Get()") && Has(view, "const int32_t mode = mirror_setting::Current().mode;") && Has(view, "mirror_setting::Current()") && Has(view, "void NoteMirrorPhase(bool begin, bool hud_stamp)") && Has(view, "request.every = setting.every;") &&
            Has(view, "mirror_setting::LogIfChanged(setting);"),
        "the host's reads (NoteMirrorPhase, BeginFrame) are the answer's, with the level's cadence in the request, and the answer is logged when it changes");
  const std::string rv = ReadFile(dir + "/../mc_engine/hooks/rear_view.cpp");
  Check(!Has(rv, "mcla_gpu_mirror_view.Get()") && Has(rv, "mcla::gpu::MirrorViewMode() != 0 && mcla::gpu::Enabled()"), "the guest's hooks read the same answer");
  const std::string glass = ReadFile(dir + "/mirror_glass.cpp");
  Check(!Has(glass, "mcla_gpu_mirror_glass.Get()") && Has(glass, "const bool wanted = mirror_setting::Current().finder;"), "the finder runs by the answer's rule");
  const std::string rules = ReadFile(dir + "/mirror_view_rules.h");
  Check(Has(rules, "r.every = r.redirect ? std::clamp<uint32_t>(f.every, 1, kMaxEvery) : 1;") && Has(rules, "uint32_t every = 1;      // the cadence of the SIDE MIRRORS level"), "RequestFor carries the cadence, only with the redirect");
  // nothing else reads the debug switches
  int others = 0;
  for (const char* f : {"/scene.cpp", "/renderer.cpp", "/mirror_glass.h", "/mirror_view.h"}) {
    const std::string t = ReadFile(dir + f);
    if (Has(t, "mcla_gpu_mirror_view.Get()") || Has(t, "mcla_gpu_mirror_glass.Get()")) ++others;
  }
  Check(others == 0, "no other file reads the debug switches directly");
}

void BuildChecks(const std::string& dir) {
  const std::string cmake = ReadFile(dir + "/CMakeLists.txt");
  Check(Has(cmake, "add_executable(larecomp_mirror_setting_test EXCLUDE_FROM_ALL mirror_setting_test.cpp)") && Has(cmake, "target_include_directories(larecomp_mirror_setting_test PRIVATE \"${CMAKE_CURRENT_SOURCE_DIR}/..\")"),
        "the test has its CMake target");
  Check(Has(cmake, "    mirror_setting.cpp\n"), "the library builds the reader");
}

std::string DirOfThisFile() {
  std::string path = __FILE__;
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

void PrintTable() {
  std::printf("class   cell    label         mode every size(width x pixels per degree)\n");
  for (const gc::GpuClass c : kClasses) {
    ms::Facts f;
    f.word = "auto";
    f.cls = c;
    const ms::Result r = ms::Resolve(f);
    std::printf("%-7s %-7s %-13s %4d %5u %s\n", std::string(gc::ClassName(c)).c_str(), std::string(gc::Cell(gc::Effect::kSideMirrors, c)).c_str(), ms::AutoLabel(c, false).c_str(), r.mode, r.every,
                r.mode == 0 ? "-" : (std::to_string(mv::MaxWidth(ms::SizeOfMode(r.mode))) + " x " + std::to_string(int(mv::PixelsPerDegree(ms::SizeOfMode(r.mode))))).c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = DirOfThisFile();
  bool table = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v") {
      g_verbose = true;
    } else if (a == "--table") {
      table = true;
    } else {
      dir = a;
    }
  }
  if (table) {
    PrintTable();
    return 0;
  }
  ResolveChecks();
  NamedCases();
  RtMeetsChecks();
  TableChecks();
  LevelChecks();
  RowChecks();
  MenuChecks(dir);
  SettingsChecks(dir);
  ClassTableChecks(dir);
  ReaderChecks(dir);
  BuildChecks(dir);
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
