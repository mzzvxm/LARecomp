#pragma once
// ===========================================================================
// MCLA GPU renderer -- the side mirrors without ray tracing: the SETTING, its pause row and what it comes to (MIRROR-VIEW.md package MV6)
// ===========================================================================
// One row on GRAPHICS > REFLECTIONS, right before RT SIDE MIRRORS: SIDE MIRRORS, AUTO / OFF / HIGH / ULTRA (mcla_gpu_side_mirrors, live, saved). This header is every decision about it that needs no
// device, so that mirror_setting_test.cpp (and the pause menu, which includes it) can check them: what a word comes to on a card, how the row meets RT SIDE MIRRORS, what the debug switches below it
// do, and the row's words. mirror_setting.cpp holds the setting's LiveText and the one function the renderer and the guest's hooks call (Current).
//
// ONE MIRRORS STORY (the owner's plan, MIRROR-VIEW.md section 13). There are two rows, the pair every ray traced effect has (BOUNCE LIGHT and RT BOUNCE LIGHT, SHADOWS and RT SHADOWS): SIDE MIRRORS is
// the effect on every card, the shared rear view the game's own mirror phase draws into a small picture and both side mirrors' glass read; RT SIDE MIRRORS is the same effect traced, on cards that trace
// rays. Where the traced mirrors run they REPLACE the plain ones: the plain row says " (REPLACED)" after its label (as BOUNCE LIGHT does) and nothing of it runs, so the HUD mirror is the game's own
// there. The row stays settable (the word applies again the moment RT SIDE MIRRORS goes off). A card that cannot trace, or traces slowly (AUTO of RT SIDE MIRRORS is off on the weak tier), runs this.
//
// THE LEVELS (two: HIGH, ULTRA; nothing is called low). They are the shared view's size and rate: HIGH 512 pixels wide, drawn every second frame (7 pixels a degree; mirror_view_rules.h Class::kMid), ULTRA
// 768 wide, every frame (10 a degree; Class::kHigh). The third size, 384 wide, is the integrated class's and AUTO turns the integrated class off, so a hand pick there gets HIGH: a view under 512 is too soft
// (5 pixels a degree against a glass that is 190 pixels wide at 1080p) and, the cost being the phase's DRAWS (a second walk of the city), not its pixels, it would save under 0.1 ms [E]. The sizes do not
// follow the render scale (the view is a picture of its own, like the cascades).
//
// WHAT THE WORDS COME TO (Resolve). `auto` is gpu_class.h's table cell of mcla_gpu_side_mirrors (Effect::kSideMirrors): OFF on the Low class (the game's own mirrors: the stop rule of MIRROR-VIEW.md 3.4
// keeps the weakest cards out), HIGH on Mid, ULTRA on High. off / high / ultra are the player's own and never overridden, on any class (a hand pick on the Low class is the player's cost, as every other
// graphics row). A word that is none of the four (an older larecomp.toml) is `auto`. The traced mirrors win: where car_mirror_setting.h Decide says RT SIDE MIRRORS is wanted, the row comes to off.
//
// THE DEBUG SWITCHES UNDERNEATH (they stay, the row maps onto them): mcla_gpu_mirror_view, 0 (the default) = the row decides; 1 = on at the class's size, 2, 3, 4 = on at the Low, Mid or High size: any of
// 1 to 4 FORCES the view whatever the row and the traced mirrors say (the A/B arms of chain-mirror-view.ps1), at the old cadence (every frame) and with the old finder rule (below). To switch the plain
// view off for an A/B, the row says OFF. mcla_gpu_mirror_glass = true runs the glass finder in addition (the arm "glass"). The glass overlay of package MV4 has its own debug switch
// mcla_gpu_mirror_overlay: see Facts::debug_overlay and Result::overlay (the row's view turns the overlay on by itself; under a forced view the switch is its only one).
//
// WHERE THE FINDER RUNS BY DEFAULT (MV-JOIN left it to MV6): exactly while the row's view runs (a mode from the row, not forced): never where the traced mirrors replace the row, never on the Low class
// under AUTO. It costs CarFrame collecting every frame (MIRROR-VIEW.md 9.4 CHECK 1), which is why it is not on without a reader. The need rule (the glass on the picture, wide enough, the 20 frame hold)
// still decides, per camera, whether the guest runs a phase for it.
//
// THE PAGE WIDTH (car_mirror_setting.h): "SIDE MIRRORS (REPLACED)" is 23 characters, the page's own widest label, and AUTO's value at most 12 ("AUTO (ULTRA)"): 35 of the 41. Every state is held to
// the budget by a static_assert below.
//
// The decisions are header only and include nothing of Plume or ReXGlue; the two functions at the end (Current, LogIfChanged) are mirror_setting.cpp's. THREADING: Resolve and the rest are pure; Current is callable from
// any thread (the guest's hooks call it from theirs).
//
// CHECK: nothing here was run in the game. The cells and the cadence of HIGH are first guesses of the design (the cost chain corrects them); the traced mirrors' own run-time answer (the pass is ready and
// found a pair) is not asked: where the row says RT SIDE MIRRORS is wanted and the pass then finds no pair, neither runs and the game's own glass shows.
// ===========================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "car_mirror_setting.h"
#include "gpu_class.h"
#include "mirror_view_rules.h"
#include "rt_tier.h"

namespace mcla::gpu::mirror_setting {

namespace gc = gpu_class;
namespace cs = car_mirror_setting;
namespace mv = mirror_view;

// ---------------------------------------------------------------------------
// the words
// ---------------------------------------------------------------------------

enum class Word : uint8_t { kAuto, kOff, kHigh, kUltra };

// The stored word as a choice; a word that is none of the four is `auto` (an older larecomp.toml).
constexpr Word WordOf(std::string_view word) {
  if (word == "off") return Word::kOff;
  if (word == "high") return Word::kHigh;
  if (word == "ultra") return Word::kUltra;
  return Word::kAuto;
}

// What mcla_gpu_mirror_view meant, and still means to the code that reads it: 0 off, 1 the class's size, 2 to 4 the Low, Mid, High size. The row gives 0, 3 (HIGH: the Mid size, every 2nd frame) or 4 (ULTRA).
inline constexpr int kModeOff = 0;
inline constexpr int kModeHigh = 3;
inline constexpr int kModeUltra = 4;

// The view's size class of a mode from the row (mirror_view_rules.h): the Mid size is 512 wide and drawn every 2nd frame, the High size 768 and every frame.
constexpr mv::Class SizeOfMode(int mode) { return mode == kModeUltra ? mv::Class::kHigh : mv::Class::kMid; }
constexpr uint32_t CadenceOfMode(int mode) { return mode == kModeHigh ? mv::Cadence(mv::Class::kMid) : mode == kModeUltra ? mv::Cadence(mv::Class::kHigh) : 1u; }

constexpr int ModeOfWord(Word word) { return word == Word::kHigh ? kModeHigh : word == Word::kUltra ? kModeUltra : kModeOff; }

// The words of the row's cell for a class (the table's text), as a word; a cell that is no word of the row is off.
inline Word CellWord(gc::GpuClass cls, bool handheld) {
  const Word cell = WordOf(gc::CellFor(gc::Effect::kSideMirrors, cls, handheld));
  return cell == Word::kAuto ? Word::kOff : cell;
}

// ---------------------------------------------------------------------------
// what a setting's word comes to, with the traced mirrors and the debug switches
// ---------------------------------------------------------------------------

// Everything Resolve reads, so the test can walk every combination.
struct Facts {
  std::string_view word;        // mcla_gpu_side_mirrors
  std::string_view rt_word;     // mcla_gpu_car_mirror_rt
  gc::GpuClass cls = gc::GpuClass::kLow;
  bool handheld = false;
  gc::RtTier tier = gc::RtTier::kNone;
  int debug_view = 0;           // mcla_gpu_mirror_view
  bool debug_glass = false;     // mcla_gpu_mirror_glass
  bool debug_overlay = false;   // mcla_gpu_mirror_overlay (MV4's debug switch for the glass overlay)
};

enum class Why : uint8_t {
  kOff,       // the row's word, or AUTO on this class, says off
  kRow,       // the row's view runs (HIGH or ULTRA, by hand or by AUTO)
  kReplaced,  // the row would run, but the traced mirrors are on and replace it
  kForced,    // mcla_gpu_mirror_view is 1 to 4: the debug force, whatever the row says
};

constexpr std::string_view WhyName(Why why) {
  switch (why) {
    case Why::kOff: return "off";
    case Why::kRow: return "the row";
    case Why::kReplaced: return "replaced by the traced mirrors";
    default: return "forced by mcla_gpu_mirror_view";
  }
}

struct Result {
  int mode = kModeOff;        // the value the readers of mcla_gpu_mirror_view take: 0 off, 1 to 4 on (see above)
  uint32_t every = 1;         // the cadence asked of the guest (Request::every): the row's level, 1 when forced
  bool finder = false;        // the glass finder (MV3) runs
  bool overlay = false;       // the glass overlay (MV4) is wanted: by the row's view (not forced, not replaced), or by its own debug switch (mirror_overlay.cpp reads this)
  bool replaced = false;      // the row's level is not off and the traced mirrors are wanted: the row says (REPLACED)
  Why why = Why::kOff;
};

// The traced side mirrors are wanted on this card with this word: the answer car_mirror_setting.h gives for its row (and, held by its test, the renderer's own Wanted).
inline bool TracedWanted(gc::GpuClass cls, gc::RtTier tier, std::string_view rt_word) { return cs::Decide(cls, tier, rt_word).wanted; }

inline Result Resolve(const Facts& f) {
  Result r;
  const Word word = WordOf(f.word);
  const Word level = word == Word::kAuto ? CellWord(f.cls, f.handheld) : word;
  const int row_mode = ModeOfWord(level);
  const bool traced = TracedWanted(f.cls, f.tier, f.rt_word);
  r.replaced = row_mode != kModeOff && traced;
  if (f.debug_view >= 1 && f.debug_view <= 4) {
    // the debug force: as the switch was built (MIRROR-VIEW.md 12.1): the view at that size every frame, the finder and the overlay only by their own switches
    r.mode = f.debug_view;
    r.every = 1;
    r.finder = f.debug_glass;
    r.overlay = f.debug_overlay;
    r.why = Why::kForced;
    return r;
  }
  r.mode = traced ? kModeOff : row_mode;
  r.every = CadenceOfMode(r.mode);
  r.finder = f.debug_glass || r.mode != kModeOff;
  r.overlay = f.debug_overlay || r.mode != kModeOff;
  r.why = r.mode != kModeOff ? Why::kRow : r.replaced ? Why::kReplaced : Why::kOff;
  return r;
}

// ---------------------------------------------------------------------------
// the row: its words, its label and what it says
// ---------------------------------------------------------------------------

inline constexpr char kFullLabel[] = "SIDE MIRRORS";
inline constexpr char kReplacedSuffix[] = " (REPLACED)";

constexpr const char* RowLabel() { return kFullLabel; }
// " (REPLACED)" while the traced mirrors replace the row, else nothing. The row is never grey: it still sets what runs where the traced mirrors do not (the word applies again when they go off).
constexpr const char* RowSuffix(bool replaced) { return replaced ? kReplacedSuffix : ""; }

// "AUTO (HIGH)": the row's label for `auto`, what the class table's cell says for this card (the traced mirrors' replacement is the label's suffix, not part of AUTO).
inline std::string AutoLabel(gc::GpuClass cls, bool handheld) { return gc::AutoLabelFor(cls, handheld, gc::Effect::kSideMirrors); }

constexpr size_t LabelChars(bool replaced) { return std::string_view(kFullLabel).size() + std::string_view(RowSuffix(replaced)).size(); }
// The widest value of the row: AUTO's label of the strongest cell.
inline constexpr size_t kWidestValue = std::string_view("AUTO (ULTRA)").size();
static_assert(LabelChars(true) + kWidestValue <= cs::kPageChars && LabelChars(false) + kWidestValue <= cs::kPageChars, "both states of the SIDE MIRRORS row fit the REFLECTIONS page (41 characters of label and value)");
static_assert(LabelChars(true) <= cs::kWidestOtherLabel && kWidestValue <= cs::kWidestValue, "the row's reason is no wider than the page's own widest label, so the page is as wide as it was");

// What the launcher says under its row (plain words, the launcher cannot tell the class before the game has run): the cells' own words, capitalised.
inline std::string LauncherAutoLine() {
  const auto cap = [](std::string_view cell) {
    std::string s(cell);
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
  };
  return "Auto is " + cap(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kLow)) + " on integrated graphics, " + cap(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kMid)) +
         " on a mid range graphics card and " + cap(gc::Cell(gc::Effect::kSideMirrors, gc::GpuClass::kHigh)) + " on a fast one. Where RT SIDE MIRRORS runs, it takes the place of this row.";
}

// ---------------------------------------------------------------------------
// this process's answer (mirror_setting.cpp)
// ---------------------------------------------------------------------------

// Resolve for this process: the two settings' words (LiveText), the class (Known() ? Current() : Low, and Handheld()), the ray tracing tier and the two debug switches. Any thread, cheap (the guest's hooks
// ask it on every frame they run).
Result Current();
// One log line, "[mirror setting] side mirrors: ...", when `now` differs from the last one logged (the first call logs): the line the cost chain and a tester's log read. Render thread, once a frame.
void LogIfChanged(const Result& now);

}  // namespace mcla::gpu::mirror_setting
