// Check of the FULLSCREEN RESOLUTION list's rules (display_sizes.h). A program of its own: it needs no
// Windows display, no ReXGlue and no GPU, and exits 0 when every check holds. The fixtures are what the
// driver says of real displays (the lists are written out, not read from the PC the test runs on), and
// of the wrong answers it gives: Windows lists by the mode the display is in, so a look made while the
// game has the display switched to 1280x720 lacks the sizes above it, the display's own among them
// (the owner's 4K display and Mz's 1080p one, 2026-11-13: no 3840x2160 and no 1920x1080 in FULLSCREEN).
//
//   cmake --build <build dir> --target larecomp_display_sizes_test     (CMakeLists.txt)
//   <build dir>/larecomp_display_sizes_test
//
// or, with no build directory at all:  clang++ -std=gnu++23 -I src/mc_engine display_sizes_test.cpp

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "display_sizes.h"

using mcla::display::Size;
using namespace mcla::display::sizes;

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

constexpr Size kMin = {1280, 720};

bool Has(const std::vector<Size>& list, Size size) {
  for (const Size& s : list) {
    if (s == size) return true;
  }
  return false;
}

std::string TextOf(const std::vector<Size>& list) {
  std::string text;
  for (const Size& s : list) text += (text.empty() ? "" : " ") + Text(s);
  return text;
}

// Every list: from the game's own screen up, ascending by area then width, no size twice.
void CheckShape(const std::vector<Size>& list, const std::string& what) {
  bool ok = true;
  for (size_t i = 0; i < list.size(); ++i) {
    if (!AtLeast(list[i], kMin)) ok = false;
    if (i > 0 && !SizeBefore(list[i - 1], list[i])) ok = false;
  }
  Check(ok, what + ": from 1280x720 up, ascending, no size twice (" + TextOf(list) + ")");
}

// ── Fixtures ───────────────────────────────────────────────────────────────────────────────────

// Every size at every rate, 32 bit, progressive.
std::vector<RawMode> Modes(std::initializer_list<Size> sizes, std::initializer_list<uint32_t> rates) {
  std::vector<RawMode> out;
  for (const Size& s : sizes) {
    for (const uint32_t hz : rates) out.push_back({s, hz, 32, false});
  }
  return out;
}

void Append(std::vector<RawMode>* to, const std::vector<RawMode>& more) {
  to->insert(to->end(), more.begin(), more.end());
}

// The list a 1920x1080 monitor on a PC graphics card gets (the shape of Mz's: the GPU scaling modes of
// other aspect ratios are listed too), with what the game must drop: modes below 1280x720, 16 bit modes
// and interlaced ones.
std::vector<RawMode> List1080p() {
  std::vector<RawMode> modes =
      Modes({{640, 480}, {800, 600}, {1024, 768}, {1152, 864}}, {60});  // below the game's screen
  Append(&modes, Modes({{1280, 720}, {1920, 1080}}, {50, 59, 60}));
  Append(&modes, Modes({{1280, 768}, {1280, 800}, {1280, 960}, {1280, 1024}, {1360, 768},
                        {1366, 768}, {1400, 1050}, {1440, 900}, {1440, 1080}, {1600, 900},
                        {1600, 1024}, {1600, 1200}, {1680, 1050}, {1768, 992}},
                       {60}));
  modes.push_back({{1920, 1080}, 30, 32, true});   // 1080i
  modes.push_back({{1920, 1080}, 25, 32, true});
  modes.push_back({{1280, 1440}, 30, 32, true});   // a size listed only interlaced
  modes.push_back({{1920, 1200}, 60, 16, false});  // a size listed only at 16 bit
  modes.push_back({{1280, 720}, 60, 16, false});
  return modes;
}

// The same display asked while it is in 1280x720: the driver lists by the mode it is in, and the sizes
// above 1600x900 (the display's own among them) are gone; the 4:3 one stays, as in Mz's report.
std::vector<RawMode> List1080pFrom720() {
  std::vector<RawMode> modes = Modes({{1280, 720}}, {50, 59, 60});
  Append(&modes, Modes({{1280, 768}, {1280, 800}, {1360, 768}, {1366, 768}, {1440, 1080}, {1600, 900}},
                       {60}));
  return modes;
}

Look OwnLook1080p() {
  Look look;
  look.modes = List1080p();
  look.desktop = {1920, 1080};
  look.desktop_hz = 60;
  look.desktop_is_registry = true;
  look.current = {1920, 1080};
  look.current_hz = 60;
  look.preferred = {1920, 1080};
  return look;
}

Look SwitchedLook1080p() {
  Look look;
  look.modes = List1080pFrom720();
  look.desktop = {1920, 1080};  // the registry settings: the switch does not change them
  look.desktop_hz = 60;
  look.desktop_is_registry = true;
  look.current = {1280, 720};
  look.current_hz = 60;
  look.preferred = {1920, 1080};
  return look;
}

std::vector<RawMode> List4k() {
  std::vector<RawMode> modes = Modes({{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}}, {60, 120, 144});
  Append(&modes, Modes({{1280, 800}, {1366, 768}, {1440, 900}, {1680, 1050}, {1920, 1200}}, {60}));
  Append(&modes, Modes({{3200, 1800}, {3840, 2160}}, {30, 60, 120}));
  Append(&modes, Modes({{800, 600}, {1024, 768}}, {60}));
  return modes;
}

Look OwnLook4k() {
  Look look;
  look.modes = List4k();
  look.desktop = {3840, 2160};
  look.desktop_hz = 120;
  look.desktop_is_registry = true;
  look.current = {3840, 2160};
  look.current_hz = 120;
  look.preferred = {3840, 2160};
  return look;
}

// ── Checks ─────────────────────────────────────────────────────────────────────────────────────

void Check1080pMonitor() {
  Book book;
  book.Add(OwnLook1080p(), false, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "1080p");
  Check(Has(list, {1920, 1080}), "1080p: the monitor's own 1920x1080 is on the list");
  for (const Size s : {Size{1280, 720}, Size{1366, 768}, Size{1360, 768}, Size{1600, 900},
                       Size{1440, 1080}, Size{1280, 1024}, Size{1680, 1050}, Size{1768, 992}}) {
    Check(Has(list, s), "1080p: the driver's " + Text(s) + " is on the list (every aspect ratio)");
  }
  Check(!Has(list, {1024, 768}) && !Has(list, {640, 480}), "1080p: nothing below the game's screen");
  Check(!Has(list, {1280, 1440}), "1080p: a size listed only interlaced is not offered");
  Check(!Has(list, {1920, 1200}), "1080p: a size listed only at 16 bit is not offered");
  Check(book.Added().empty(), "1080p: the driver listed everything that is always in the list");
  CheckEq(Text(book.desktop()), "1920x1080", "1080p: the display's own mode");
  CheckEq(Text(book.preferred()), "1920x1080", "1080p: the monitor's own size");
  CheckEq(std::to_string(list.size()), "16", "1080p: sizes offered");
  CheckEq(TextOf(list),
          "1280x720 1280x768 1280x800 1360x768 1366x768 1280x960 1440x900 1280x1024 1600x900 1400x1050"
          " 1440x1080 1600x1024 1768x992 1680x1050 1600x1200 1920x1080",
          "1080p: the whole list, in order");
}

void CheckSwitchedLookAfterTheOwn() {
  // The display is switched to 1280x720 and the menu looks at the driver: what the first look found stays.
  Book book;
  book.Add(OwnLook1080p(), false, kMin);
  const std::vector<Size> before = book.Sizes();
  book.Add(SwitchedLook1080p(), true, kMin);
  const std::vector<Size> after = book.Sizes();
  CheckShape(after, "1080p switched");
  Check(Has(after, {1920, 1080}), "1080p switched: 1920x1080 stays (a look in 1280x720 lacks it)");
  Check(after == before, "1080p switched: the list does not change (" + TextOf(after) + ")");
  CheckEq(Text(book.desktop()), "1920x1080", "1080p switched: the display's own mode");
  // Even a look the switch flag did not cover (the restore failed, the flag was cleared early).
  book.Add(SwitchedLook1080p(), false, kMin);
  Check(book.Sizes() == before, "1080p, a wrong look with no flag: the list does not change");
}

void CheckWrongLookFirst() {
  // The first look the game ever gets is a wrong one (a display left in 1280x720 by something else): the
  // display's own size and the monitor's own are still on the list.
  Book book;
  book.Add(SwitchedLook1080p(), true, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "1080p wrong look first");
  Check(Has(list, {1920, 1080}), "1080p, the first look in 1280x720: 1920x1080 is on the list anyway");
  CheckEq(TextOf(book.Added()), "1920x1080", "1080p, the first look in 1280x720: what was put in");
  CheckEq(Text(book.desktop()), "1920x1080", "1080p, the first look in 1280x720: the display's own mode");
  // The same with no switch flag.
  Book unflagged;
  unflagged.Add(SwitchedLook1080p(), false, kMin);
  Check(Has(unflagged.Sizes(), {1920, 1080}), "1080p, the same look with no flag: 1920x1080 is on the list");
  // The registry settings are not there (an old Windows, a driver that has none) and the display is in
  // another mode than its own: the mode it is in is not taken for the display's own, the monitor's own
  // size is still on the list.
  Look no_registry = SwitchedLook1080p();
  no_registry.desktop = no_registry.current;
  no_registry.desktop_hz = no_registry.current_hz;
  no_registry.desktop_is_registry = false;
  Book blind;
  blind.Add(no_registry, true, kMin);
  Check(blind.desktop().empty(), "no registry mode while switched: the display's own mode is not guessed");
  Check(Has(blind.Sizes(), {1920, 1080}),
        "no registry mode while switched: the monitor's own 1920x1080 is on the list");
  // And no registry mode at all on a display in its own mode: the mode it is in is the display's own.
  Look no_registry_own = OwnLook1080p();
  no_registry_own.desktop_is_registry = false;
  Book plain;
  plain.Add(no_registry_own, false, kMin);
  CheckEq(Text(plain.desktop()), "1920x1080", "no registry mode, display in its own: its current mode");
}

void Check4kMonitor() {
  Book book;
  book.Add(OwnLook4k(), false, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "4K");
  Check(Has(list, {3840, 2160}), "4K: the monitor's own 3840x2160 is on the list");
  Check(Has(list, {3200, 1800}) && Has(list, {2560, 1440}) && Has(list, {1920, 1080}),
        "4K: the sizes between");
  CheckEq(Text(list.back()), "3840x2160", "4K: the last (largest) is the monitor's own");
  CheckEq(std::to_string(list.size()), "11", "4K: sizes offered");

  // The LA11 / FS-RES case: the display switched to 1280x720, the driver's list lacks 3840x2160.
  Look low;
  low.modes = Modes({{1280, 720}, {1600, 900}, {1280, 800}, {1366, 768}}, {60});
  low.desktop = {3840, 2160};
  low.desktop_hz = 120;
  low.desktop_is_registry = true;
  low.current = {1280, 720};
  low.current_hz = 60;
  low.preferred = {3840, 2160};
  const std::vector<Size> before = list;
  book.Add(low, true, kMin);
  Check(book.Sizes() == before, "4K switched to 1280x720: the list is what it was");
  Check(Has(book.Sizes(), {3840, 2160}), "4K switched to 1280x720: 3840x2160 is still on it");
  // The same look as the first one the game gets.
  Book first;
  first.Add(low, true, kMin);
  Check(Has(first.Sizes(), {3840, 2160}), "4K, the first look in 1280x720: 3840x2160 is on the list");
  CheckEq(Text(first.desktop()), "3840x2160", "4K, the first look in 1280x720: the display's own mode");
}

void Check4kAt150PercentDpi() {
  // A process that sees logical pixels (2560x1440 for a 3840x2160 display at 150%): the mode list, the
  // registry mode and the current mode are all in logical pixels. The monitor's own size (the display
  // configuration API) is physical, and is on the list.
  Look look;
  look.modes = Modes({{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}}, {60});
  look.desktop = {2560, 1440};
  look.desktop_hz = 60;
  look.desktop_is_registry = true;
  look.current = {2560, 1440};
  look.current_hz = 60;
  look.preferred = {3840, 2160};
  Book book;
  book.Add(look, false, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "4K at 150%");
  Check(Has(list, {3840, 2160}), "4K at 150%: the physical 3840x2160 is on the list");
  Check(Has(list, {2560, 1440}), "4K at 150%: so is what the process was told");
  CheckEq(Text(book.preferred()), "3840x2160", "4K at 150%: the monitor's own size");
  CheckEq(TextOf(book.Added()), "3840x2160", "4K at 150%: 3840x2160 came from the display configuration");
  // A later look from the same process, now in physical pixels, adds what it lists and drops nothing.
  book.Add(OwnLook4k(), false, kMin);
  Check(Has(book.Sizes(), {3200, 1800}) && Has(book.Sizes(), {3840, 2160}) && Has(book.Sizes(), {1600, 900}),
        "4K at 150%, then a physical look: both are in");
  CheckEq(Text(book.desktop()), "3840x2160", "4K at 150%, then a physical look: the display's own mode");
}

void CheckUltrawide() {
  Look look;
  look.modes = Modes({{1280, 720}, {1920, 1080}, {2560, 1080}, {2560, 1440}}, {60});
  Append(&look.modes, Modes({{3440, 1440}}, {60, 100, 144}));
  Append(&look.modes, Modes({{3840, 1600}}, {60}));
  look.desktop = {3440, 1440};
  look.desktop_hz = 144;
  look.desktop_is_registry = true;
  look.current = {3440, 1440};
  look.current_hz = 144;
  look.preferred = {3440, 1440};
  Book book;
  book.Add(look, false, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "ultrawide");
  CheckEq(TextOf(list), "1280x720 1920x1080 2560x1080 2560x1440 3440x1440 3840x1600",
          "ultrawide: 21:9 and 16:9 sizes together, by area");
  CheckEq(std::to_string(book.RefreshFor({3440, 1440})), "144", "ultrawide: the display's own rate");
  CheckEq(std::to_string(book.RefreshFor({2560, 1440})), "60", "ultrawide: the only rate of a size");

  // The driver's list lacks the ultrawide size (asked in 1920x1080): put in.
  Look low = look;
  low.modes = Modes({{1280, 720}, {1920, 1080}}, {60});
  low.current = {1920, 1080};
  low.current_hz = 60;
  Book first;
  first.Add(low, true, kMin);
  Check(Has(first.Sizes(), {3440, 1440}), "ultrawide, a look from 1920x1080 first: 3440x1440 is on the list");
  CheckEq(std::to_string(first.RefreshFor({3440, 1440})), "144",
          "ultrawide: a size the driver did not list is switched at the display's own rate");
}

void CheckCurrentModeMissingFromTheEnumeration() {
  // The driver lists 1920x1200 for no refresh rate at all (an enumeration that stops before it), and the
  // display is in it: the display's own mode and the current one are on the list.
  Look look;
  look.modes = Modes({{1280, 720}, {1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1080}}, {60});
  look.desktop = {1920, 1200};
  look.desktop_hz = 60;
  look.desktop_is_registry = true;
  look.current = {1920, 1200};
  look.current_hz = 60;
  look.preferred = {1920, 1200};
  Book book;
  book.Add(look, false, kMin);
  const std::vector<Size> list = book.Sizes();
  CheckShape(list, "current missing");
  Check(Has(list, {1920, 1200}), "current missing: the current mode is on the list");
  CheckEq(TextOf(book.Added()), "1920x1200", "current missing: it is what was put in");
  CheckEq(std::to_string(book.RefreshFor({1920, 1200})), "60", "current missing: its rate is the display's own");
  CheckEq(Text(list.back()), "1920x1200", "current missing: it sorts last");
  const std::string text = book.Describe();
  Check(text.find("own mode 1920x1200 at 60 Hz") != std::string::npos, "current missing: the log line says the mode: " + text);
  Check(text.find("put in though the driver did not list them: 1920x1200") != std::string::npos,
        "current missing: the log line says what was put in: " + text);

  // Not the display's own mode, but the one it is in (a mode some other program or the player set last
  // second): on the list too.
  Look odd = look;
  odd.current = {1600, 1200};
  odd.current_hz = 75;
  Book other;
  other.Add(odd, false, kMin);
  Check(Has(other.Sizes(), {1600, 1200}) && Has(other.Sizes(), {1920, 1200}),
        "current missing: the mode the display is in and its own are both on the list");
  CheckEq(std::to_string(other.RefreshFor({1600, 1200})), "75", "current missing: the current mode's rate");
}

void CheckPortraitAndSmallDisplays() {
  // A display turned to portrait: its mode is 1440x2560, the monitor's own size comes landscape.
  Look portrait;
  portrait.modes = Modes({{1440, 2560}, {1280, 720}}, {60});
  portrait.desktop = {1440, 2560};
  portrait.desktop_hz = 60;
  portrait.desktop_is_registry = true;
  portrait.current = {1440, 2560};
  portrait.current_hz = 60;
  portrait.preferred = {2560, 1440};
  Book book;
  book.Add(portrait, false, kMin);
  CheckEq(Text(book.preferred()), "1440x2560", "portrait: the monitor's own size is turned with the display");
  CheckEq(TextOf(book.Sizes()), "1280x720 1440x2560", "portrait: the list");

  // A display too small for the game's own screen: nothing is offered (the menu has its own steps).
  Look small;
  small.modes = Modes({{800, 600}, {1024, 768}}, {60});
  small.desktop = {1024, 768};
  small.desktop_hz = 60;
  small.desktop_is_registry = true;
  small.current = {1024, 768};
  small.current_hz = 60;
  small.preferred = {1024, 768};
  Book tiny;
  tiny.Add(small, false, kMin);
  Check(tiny.Sizes().empty() && tiny.empty(), "a 1024x768 display: nothing from 1280x720 up to offer");

  // No look at all.
  Book none;
  Check(none.Sizes().empty() && none.desktop().empty() && none.preferred().empty(), "no look: an empty book");
}

void CheckLooksOnlyAdd() {
  // Any order of looks, any of them wrong: the list never loses a size.
  const Look own = OwnLook1080p();
  const Look low = SwitchedLook1080p();
  Look empty_modes = own;
  empty_modes.modes.clear();
  Look nothing;  // the driver answered nothing at all
  const Look* order[] = {&low, &own, &empty_modes, &nothing, &low, &empty_modes, &own, &nothing};
  Book book;
  std::vector<Size> before;
  bool grew_only = true;
  for (const Look* look : order) {
    book.Add(*look, look == &low, kMin);
    const std::vector<Size> now = book.Sizes();
    for (const Size& s : before) {
      if (!Has(now, s)) grew_only = false;
    }
    before = now;
    CheckShape(now, "looks in any order");
  }
  Check(grew_only, "looks in any order: a size once on the list stays");
  Check(Has(before, {1920, 1080}), "looks in any order: 1920x1080 is on the list at the end");
}

void CheckDisplayChangedByThePlayer() {
  // The player sets Windows to 2560x1440 while the game runs (a look with no flag): the display's own mode
  // follows, the sizes the display listed before stay on the list (it can still be switched to them).
  Book book;
  book.Add(OwnLook4k(), false, kMin);
  Look now = OwnLook4k();
  now.modes = Modes({{1280, 720}, {1920, 1080}, {2560, 1440}}, {60});
  now.desktop = {2560, 1440};
  now.desktop_hz = 60;
  now.current = {2560, 1440};
  now.current_hz = 60;
  book.Add(now, false, kMin);
  CheckEq(Text(book.desktop()), "2560x1440", "the player changed Windows' mode: the display's own mode follows");
  Check(Has(book.Sizes(), {3840, 2160}), "the player changed Windows' mode: 3840x2160 stays (the monitor's own)");
  // A switched display does not move the display's own mode, whatever its look says.
  Look wrong = now;
  wrong.desktop = {1280, 720};
  wrong.current = {1280, 720};
  book.Add(wrong, true, kMin);
  CheckEq(Text(book.desktop()), "2560x1440", "a switched display's look does not move the display's own mode");
}

void CheckRefreshRates() {
  Look look = OwnLook4k();
  Book book;
  book.Add(look, false, kMin);
  CheckEq(std::to_string(book.RefreshFor({3840, 2160})), "120", "refresh: the display's own rate when the size has it");
  CheckEq(std::to_string(book.RefreshFor({2560, 1440})), "120", "refresh: again for a size that has it");
  CheckEq(std::to_string(book.RefreshFor({1920, 1200})), "60", "refresh: the only rate a size lists");
  CheckEq(std::to_string(book.RefreshFor({3200, 1800})), "120", "refresh: the display's own among 30, 60, 120");
  CheckEq(std::to_string(book.RefreshFor({1234, 567})), "0", "refresh: a size that is not on the list");

  // The display's own rate is not one a size has: the highest it lists.
  Look odd;
  odd.modes = Modes({{1280, 720}}, {50, 60, 75});
  odd.desktop = {1920, 1080};
  odd.desktop_hz = 144;
  odd.desktop_is_registry = true;
  odd.current = {1920, 1080};
  odd.current_hz = 144;
  Book other;
  other.Add(odd, false, kMin);
  CheckEq(std::to_string(other.RefreshFor({1280, 720})), "75", "refresh: the highest when the display's own is not listed");
  CheckEq(std::to_string(other.RefreshFor({1920, 1080})), "144", "refresh: a size put in has the display's own rate");

  // 0 and 1 are the driver's "default rate": not a rate to ask for.
  Look def = odd;
  def.desktop_hz = 1;
  def.current_hz = 1;
  Book unnamed;
  unnamed.Add(def, false, kMin);
  CheckEq(std::to_string(unnamed.RefreshFor({1920, 1080})), "0", "refresh: no rate to name for a size put in at the default");
  CheckEq(std::to_string(unnamed.RefreshFor({1280, 720})), "75", "refresh: a listed size has its highest rate");
}

}  // namespace

int main() {
  Check1080pMonitor();
  CheckSwitchedLookAfterTheOwn();
  CheckWrongLookFirst();
  Check4kMonitor();
  Check4kAt150PercentDpi();
  CheckUltrawide();
  CheckCurrentModeMissingFromTheEnumeration();
  CheckPortraitAndSmallDisplays();
  CheckLooksOnlyAdd();
  CheckDisplayChangedByThePlayer();
  CheckRefreshRates();
  std::printf("display_sizes_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
