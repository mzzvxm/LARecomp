// Check of the FULLSCREEN RESOLUTION list's rules (display_sizes.h). A program of its own: it needs no
// Windows display, no ReXGlue and no GPU, and exits 0 when every check holds. The fixtures are what the
// driver says of real displays (the lists are written out, not read from the PC the test runs on), and
// of the wrong answers it gives: Windows lists by the mode the display is in, so a look made while the
// game has the display switched to 1280x720 lacks the sizes above it, the display's own among them
// (the owner's 4K display and Mz's 1080p one, 2026-11-13: no 3840x2160 and no 1920x1080 in FULLSCREEN).
// The second half checks which size FULLSCREEN is asked to switch to and the sequences around it (Mz,
// 2026-11-16: DISPLAY MODE to FULLSCREEN put the 1080p display in 1280x720), with a fake display that lists
// by the mode it is in, and NVIDIA DSR / AMD VSR style lists with sizes above the monitor's own.
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


// ── The choice FULLSCREEN makes, and the sequences around it (fix/fullscreen-native-res-2) ─────

// Mz, 2026-11-16, a 1080p monitor: BORDERLESS showed "RESOLUTION (1920X1080)"; DISPLAY MODE to FULLSCREEN
// then showed 1280X720 (and the picture was 1280x720): the display was switched to the size of
// `resolution`, which he had only ever used for the window. FULLSCREEN has a setting of its own now
// (fullscreen_resolution, empty by default: the display's own size), and these are the rules it runs on.

void CheckNearest() {
  const std::vector<Size> list = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
  CheckEq(Text(Nearest(list, {1920, 1080})), "1920x1080", "nearest: a listed size is itself");
  CheckEq(Text(Nearest(list, {1900, 1070})), "1920x1080", "nearest: an unlisted size goes to the closest");
  CheckEq(Text(Nearest(list, {5000, 3000})), "2560x1440", "nearest: above the list, the largest");
  CheckEq(Text(Nearest(list, {100, 100})), "1280x720", "nearest: below the list, the smallest");
  CheckEq(Text(Nearest(list, {})), "1280x720", "nearest: no size asked, the first");
  CheckEq(Text(Nearest({}, {1234, 567})), "1234x567", "nearest: no list, the size asked");
}

void CheckChooseExclusive() {
  const std::vector<Size> list = Book().Sizes();  // empty
  Book book;
  book.Add(OwnLook1080p(), false, kMin);
  const std::vector<Size> sizes = book.Sizes();
  const Size own = book.desktop();
  // Nothing saved for FULLSCREEN: the display's own size, whatever the list's first entry is.
  CheckEq(Text(ChooseExclusive(sizes, own, {})), "1920x1080", "choose: nothing saved is the display's own size");
  // Saved: the listed mode, or the nearest one.
  CheckEq(Text(ChooseExclusive(sizes, own, {1280, 720})), "1280x720", "choose: a saved listed size");
  CheckEq(Text(ChooseExclusive(sizes, own, {1920, 1080})), "1920x1080", "choose: the display's own size, saved");
  CheckEq(Text(ChooseExclusive(sizes, own, {1900, 1070})), "1920x1080", "choose: a saved size the display does not list");
  // The display's own mode is not known and nothing is saved: nothing is chosen. It used to come out as
  // the first (smallest) listed size, 1280x720, which is a switch.
  Check(ChooseExclusive(sizes, {}, {}).empty(), "choose: no own mode and nothing saved is no choice (not 1280x720)");
  Check(ChooseExclusive(list, {}, {}).empty(), "choose: no list, no own mode, nothing saved");
  CheckEq(Text(ChooseExclusive(list, own, {1600, 900})), "1600x900", "choose: no list, the saved size");
}

void CheckPlanExclusive() {
  const Size own = {1920, 1080};
  const Size low = {1280, 720};
  Check(PlanExclusive(own, own, false, {}) == Plan::kOwn, "plan: the display's own size is no switch");
  Check(PlanExclusive({}, own, false, {}) == Plan::kOwn, "plan: no size is the display's own");
  Check(PlanExclusive(own, own, true, low) == Plan::kOwn, "plan: back to the own size from a switched mode");
  Check(PlanExclusive(low, own, false, {}) == Plan::kSwitch, "plan: another size is a switch");
  Check(PlanExclusive(low, own, true, {1600, 900}) == Plan::kSwitch, "plan: from one switched mode to another");
  Check(PlanExclusive(low, own, true, low) == Plan::kKeep, "plan: the mode the display is in already");
  Check(PlanExclusive(low, own, false, low) == Plan::kSwitch, "plan: a stale 'switched to' with no switch is not kept");
  Check(PlanExclusive({}, {}, false, {}) == Plan::kOwn, "plan: nothing known, nothing asked: no switch");
  Check(PlanExclusive(low, {}, false, {}) == Plan::kSwitch, "plan: a size asked of a display of unknown mode");
}

// A display as Windows has it, for the sequences below: the mode it is in decides what the driver lists
// (the real behaviour the Book exists for), the registry settings are its own mode and are not changed by
// a switch (CDS_FULLSCREEN), and the monitor's preferred size is what the monitor is.
struct FakeDisplay {
  Size own = {1920, 1080};
  uint32_t own_hz = 60;
  Size monitor = {1920, 1080};
  Size current = {1920, 1080};
  uint32_t current_hz = 60;
  std::vector<RawMode> all;       // what the driver lists with the display in its own mode
  std::vector<RawMode> from_low;  // what it lists with the display in 1280x720, when that is known

  Look Ask() const {
    Look look;
    if (current == own) {
      look.modes = all;
    } else if (current == Size{1280, 720} && !from_low.empty()) {
      look.modes = from_low;
    } else {
      for (const RawMode& m : all) {
        if (m.size.width <= current.width && m.size.height <= current.height) look.modes.push_back(m);
      }
    }
    look.desktop = own;
    look.desktop_hz = own_hz;
    look.desktop_is_registry = true;
    look.current = current;
    look.current_hz = current_hz;
    look.preferred = monitor;
    return look;
  }
};

// The glue of display_mode.cpp, in the same order, over the same classes (Displays, Book, ChooseExclusive,
// PlanExclusive): ListedModes, EnterDesktopMode and RestoreDesktopMode with the Windows calls replaced by
// the fake display. `menu_looks` is the menu asking while the display changes its mode (it asks every
// frame it is open).
struct Session {
  FakeDisplay* display;
  Displays displays;
  bool changed = false;  // g_mode_changed
  Size changed_to;
  uint64_t now = 0;
  int driver_looks = 0;
  const std::string device = "\\\\.\\DISPLAY1";

  Info Ask() {
    return displays.Listed(device, now, [&](Info* info, bool switched) {
      ++driver_looks;
      info->book.Add(display->Ask(), switched, kMin);
    });
  }
  void Restore() {
    if (!changed) return;
    changed = false;
    display->current = display->own;
    display->current_hz = display->own_hz;
    displays.SetSwitched({});
  }
  // EnterDesktopMode. `late_mark`: the old order, the display marked as switched after the switch.
  void Enter(Size saved, bool menu_looks = false, bool late_mark = false) {
    const Info info = Ask();
    const Size own = info.book.desktop();
    Size want = ChooseExclusive(info.book.Sizes(), own, saved);
    if (want.empty()) want = own;
    switch (PlanExclusive(want, own, changed, changed_to)) {
      case Plan::kOwn:
        Restore();
        return;
      case Plan::kKeep:
        return;
      case Plan::kSwitch:
        break;
    }
    if (!late_mark) displays.SetSwitched(device);
    display->current = want;
    display->current_hz = info.book.RefreshFor(want);
    if (menu_looks) {
      now += 2 * Displays::kLifetimeMs;  // the book is old enough to be looked at again, if it may be
      Ask();
    }
    if (late_mark) displays.SetSwitched(device);
    changed = true;
    changed_to = want;
  }
};

// What the RESOLUTION row shows in FULLSCREEN (display_menu.cpp ResolutionList): the list and the entry
// the setting comes to.
struct Row {
  std::vector<Size> sizes;
  Size current;
};
Row FullscreenRow(const Info& info, Size saved) {
  Row row;
  row.sizes = info.book.Sizes();
  Size want = ChooseExclusive(row.sizes, info.book.desktop(), saved);
  if (want.empty()) want = {1280, 720};
  if (!Has(row.sizes, want)) row.sizes.push_back(want);
  row.current = want;
  return row;
}

void CheckBorderlessToFullscreenOn1080p() {
  // The owner's sequence: the game runs BORDERLESS on a 1920x1080 monitor, DISPLAY MODE goes to FULLSCREEN, and
  // the setting for FULLSCREEN is the default (empty) - the window's `resolution` is 1280x720 and is not
  // used.
  FakeDisplay display;
  display.all = List1080p();
  display.from_low = List1080pFrom720();
  Session game;
  game.display = &display;
  const Info borderless = game.Ask();
  CheckEq(Text(borderless.book.desktop()), "1920x1080", "borderless: the row's size is the display's own");

  game.now += 100;
  game.Enter({}, true);
  const Info fullscreen = game.Ask();
  const Row row = FullscreenRow(fullscreen, {});
  CheckEq(Text(display.current), "1920x1080", "FULLSCREEN, nothing saved: the display stays at 1920x1080");
  Check(!game.changed, "FULLSCREEN, nothing saved: no mode switch");
  CheckEq(Text(row.current), "1920x1080", "FULLSCREEN, nothing saved: the row is on 1920x1080");
  Check(Has(row.sizes, {1920, 1080}), "FULLSCREEN, nothing saved: 1920x1080 is on the list");
  CheckShape(row.sizes, "FULLSCREEN, nothing saved");
  Check(row.sizes == borderless.book.Sizes(), "FULLSCREEN: the list is BORDERLESS's list");

  // The player steps down to 1280x720: the display is switched, the menu keeps asking during and after it.
  game.now += 100;
  game.Enter({1280, 720}, true);
  CheckEq(Text(display.current), "1280x720", "stepped to 1280x720: the display is in 1280x720");
  Check(game.changed, "stepped to 1280x720: this game has switched the display");
  game.now += 60000;  // a minute with the menu open: no look at the driver at all
  const int looks = game.driver_looks;
  const Info low = game.Ask();
  Check(game.driver_looks == looks, "in 1280x720: the driver is not asked while the display is switched");
  const Row low_row = FullscreenRow(low, {1280, 720});
  Check(Has(low_row.sizes, {1920, 1080}), "in 1280x720: 1920x1080 is still on the list");
  Check(low_row.sizes == borderless.book.Sizes(), "in 1280x720: the list is what it was");
  CheckEq(Text(low_row.current), "1280x720", "in 1280x720: the row is on 1280x720");
  CheckEq(Text(low.book.desktop()), "1920x1080", "in 1280x720: the display's own mode is still 1920x1080");

  // And back up to 1920x1080 from the row: the display's own mode returns.
  game.Enter({1920, 1080}, true);
  CheckEq(Text(display.current), "1920x1080", "stepped to 1920x1080: the display is back in its own mode");
  Check(!game.changed, "stepped to 1920x1080: the switch is over");
  const Row back = FullscreenRow(game.Ask(), {1920, 1080});
  CheckEq(Text(back.current), "1920x1080", "stepped to 1920x1080: the row is on 1920x1080");
  Check(back.sizes == borderless.book.Sizes(), "stepped to 1920x1080: the list is what it was");
  Check(game.driver_looks > looks, "the driver is asked again once the display's own mode is back");
}

void CheckSwitchWithTheMenuLooking() {
  // The look the menu makes while the display is switching, on a display whose own mode is 1920x1080.
  // With the mark before the switch the look is not made; with the mark after it (the old order) the look
  // sees 1280x720's list, and the book keeps the display's own size all the same.
  for (const bool late_mark : {false, true}) {
    FakeDisplay display;
    display.all = List1080p();
    display.from_low = List1080pFrom720();
    Session game;
    game.display = &display;
    game.Ask();
    const int looks = game.driver_looks;
    game.Enter({1280, 720}, true, late_mark);
    const std::string how = late_mark ? "mark after the switch" : "mark before the switch";
    const Info info = game.Ask();
    Check(Has(info.book.Sizes(), {1920, 1080}), how + ": 1920x1080 is on the list");
    CheckEq(Text(info.book.desktop()), "1920x1080", how + ": the display's own mode");
    Check(late_mark ? game.driver_looks == looks + 1 : game.driver_looks == looks,
          how + ": the driver was " + (late_mark ? "asked during the switch" : "not asked during the switch"));
  }
}

void CheckEmptyOwnModeSwitchesNothing() {
  // The registry call answered a size of nothing, and the display is in its own mode: the mode it is in is
  // taken for the own one (before, the book had no own mode at all and FULLSCREEN with nothing saved chose
  // the first listed size, 1280x720).
  Look look = OwnLook1080p();
  look.desktop = {};
  look.desktop_hz = 0;
  look.desktop_is_registry = true;  // the call succeeded
  Book book;
  book.Add(look, false, kMin);
  CheckEq(Text(book.desktop()), "1920x1080", "an empty registry answer: the mode the display is in is its own");
  Check(ChooseExclusive(book.Sizes(), book.desktop(), {}) == Size{1920, 1080},
        "an empty registry answer: FULLSCREEN with nothing saved is 1920x1080");
  // While this game has the display switched the mode it is in is not guessed to be the own one, and
  // FULLSCREEN chooses nothing (no switch) rather than the first listed size.
  Look switched = SwitchedLook1080p();
  switched.desktop = {};
  switched.desktop_hz = 0;
  Book blind;
  blind.Add(switched, true, kMin);
  Check(blind.desktop().empty(), "an empty registry answer while switched: the own mode is not guessed");
  Check(ChooseExclusive(blind.Sizes(), blind.desktop(), {}).empty(),
        "an empty registry answer while switched: nothing is chosen");
  Check(PlanExclusive({}, blind.desktop(), true, {1280, 720}) == Plan::kOwn,
        "an empty registry answer while switched: the plan is no switch");
}

// NVIDIA DSR / AMD VSR: while the feature is on, the driver lists sizes above the monitor's own (for a
// 1080p monitor 2560x1440, 2880x1620 and 3840x2160, and others at the factors in between), usually at
// 60 Hz only, and a game switches the display to one of them to have its picture scaled down by the GPU.
std::vector<RawMode> ListDsr1080p() {
  std::vector<RawMode> modes = List1080p();
  Append(&modes, Modes({{2103, 1183}, {2560, 1440}, {2880, 1620}, {3840, 2160}}, {60}));
  return modes;
}

void CheckDsrModes() {
  FakeDisplay display;
  display.own_hz = 144;
  display.current_hz = 144;
  display.all = ListDsr1080p();
  // The monitor's own modes at 144 Hz as well.
  Append(&display.all, Modes({{1920, 1080}}, {144}));

  Session game;
  game.display = &display;
  const Info info = game.Ask();
  const std::vector<Size> list = info.book.Sizes();
  CheckShape(list, "DSR");
  for (const Size s : {Size{1920, 1080}, Size{2103, 1183}, Size{2560, 1440}, Size{2880, 1620}, Size{3840, 2160}}) {
    Check(Has(list, s), "DSR: " + Text(s) + " is on the list");
  }
  CheckEq(Text(list.back()), "3840x2160", "DSR: the largest is the last");
  CheckEq(Text(info.book.desktop()), "1920x1080", "DSR: the display's own mode is not a DSR mode");
  CheckEq(Text(info.book.preferred()), "1920x1080", "DSR: the monitor's own size is not a DSR mode");
  Check(info.book.Added().empty(), "DSR: nothing had to be put in");
  // The native size is where it belongs by area, between the 16:9 sizes below it and the DSR ones above.
  size_t at = 0;
  while (at < list.size() && !(list[at] == Size{1920, 1080})) ++at;
  Check(at + 1 < list.size() && Area(list[at - 1]) < Area(list[at]) && Area(list[at + 1]) > Area(list[at]),
        "DSR: 1920x1080 is ranked by its own area, with DSR modes above it only");
  // Selectable: it is what nothing-saved is, and what saving it is.
  CheckEq(Text(ChooseExclusive(list, info.book.desktop(), {})), "1920x1080", "DSR: FULLSCREEN with nothing saved");
  CheckEq(Text(ChooseExclusive(list, info.book.desktop(), {1920, 1080})), "1920x1080",
          "DSR: FULLSCREEN with 1920x1080 saved");
  CheckEq(std::to_string(info.book.RefreshFor({1920, 1080})), "144", "DSR: native keeps the display's own rate");
  CheckEq(std::to_string(info.book.RefreshFor({3840, 2160})), "60", "DSR: a DSR mode has the one rate it lists");
  const Row row = FullscreenRow(info, {});
  CheckEq(Text(row.current), "1920x1080", "DSR: the row opens on the native size");
  Check(row.sizes == list, "DSR: the row lists the book");

  // Into a DSR mode and back to the native one from the row (the sequence a player who wants supersampling
  // from the driver goes through): every step is a switch or the own mode, never a refusal of native.
  game.now += 100;
  game.Enter({3840, 2160}, true);
  CheckEq(Text(display.current), "3840x2160", "DSR: the display is in the 3840x2160 mode");
  Check(game.changed, "DSR: this game switched the display");
  Check(PlanExclusive({3840, 2160}, info.book.desktop(), true, {3840, 2160}) == Plan::kKeep, "DSR: asking for it again keeps it");
  const Info in_dsr = game.Ask();
  Check(in_dsr.book.Sizes() == list, "DSR: the list in 3840x2160 is the list from before");
  Check(Has(in_dsr.book.Sizes(), {1920, 1080}), "DSR: 1920x1080 is on the list while in 3840x2160");
  game.Enter({2560, 1440}, true);
  CheckEq(Text(display.current), "2560x1440", "DSR: from 3840x2160 to 2560x1440");
  game.Enter({1920, 1080}, true);
  CheckEq(Text(display.current), "1920x1080", "DSR: back to the native size");
  Check(!game.changed, "DSR: the native size is the display's own mode again");
  Check(game.Ask().book.Sizes() == list, "DSR: the list at the end is the list at the start");
}

void CheckDsrAsTheDesktopMode() {
  // DSR is how the player has Windows: the desktop is 3840x2160 on the 1080p monitor. The display's own
  // mode (registry) is then 3840x2160, the monitor's own size is still 1920x1080 and is listed, listed or
  // not by the driver, and is a choice like any other.
  for (const bool driver_lists_native : {true, false}) {
    FakeDisplay display;
    display.own = {3840, 2160};
    display.current = {3840, 2160};
    display.all = ListDsr1080p();
    if (!driver_lists_native) {
      std::vector<RawMode> kept;
      for (const RawMode& m : display.all) {
        if (!(m.size == Size{1920, 1080})) kept.push_back(m);
      }
      display.all = kept;
    }
    Session game;
    game.display = &display;
    const Info info = game.Ask();
    const std::string how = driver_lists_native ? "DSR desktop" : "DSR desktop, native not listed";
    const std::vector<Size> list = info.book.Sizes();
    CheckShape(list, how);
    CheckEq(Text(info.book.desktop()), "3840x2160", how + ": the display's own mode is the one Windows has");
    CheckEq(Text(info.book.preferred()), "1920x1080", how + ": the monitor's own size");
    Check(Has(list, {1920, 1080}), how + ": the monitor's own 1920x1080 is on the list");
    Check(Has(list, {3840, 2160}) && Has(list, {2560, 1440}), how + ": the DSR sizes are too");
    Check(ChooseExclusive(list, info.book.desktop(), {1920, 1080}) == Size{1920, 1080}, how + ": 1920x1080 is selectable");
    Check(ChooseExclusive(list, info.book.desktop(), {}) == Size{3840, 2160}, how + ": nothing saved is the mode Windows has");
    Check(PlanExclusive({1920, 1080}, info.book.desktop(), false, {}) == Plan::kSwitch, how + ": 1920x1080 is a switch");
    CheckEq(std::to_string(info.book.RefreshFor({1920, 1080})), "60",
            how + ": its rate is the one listed, or the display's own when the driver does not list it");
    game.now += 100;
    game.Enter({1920, 1080}, true);
    CheckEq(Text(display.current), "1920x1080", how + ": the display is switched to 1920x1080");
    Check(Has(game.Ask().book.Sizes(), {3840, 2160}), how + ": the desktop's own 3840x2160 stays on the list");
  }
}

void CheckDisplaysBooks() {
  int looks = 0;
  const auto read = [&](Info* info, bool) {
    ++looks;
    info->book.Add(OwnLook1080p(), false, kMin);
  };
  Displays d;
  Info kept;
  Check(!d.Peek("A", &kept) && d.latest().empty(), "books: nothing kept before the first ask");
  d.Listed("A", 1000, read);
  CheckEq(std::to_string(looks), "1", "books: the first ask looks at the driver");
  d.Listed("A", 1000 + Displays::kLifetimeMs, read);
  CheckEq(std::to_string(looks), "1", "books: a fresh book is kept");
  d.Listed("A", 1001 + Displays::kLifetimeMs, read);
  CheckEq(std::to_string(looks), "2", "books: an old one is looked at again");
  d.Listed("B", 9000, read);
  CheckEq(std::to_string(looks), "3", "books: every display has its own");
  CheckEq(d.latest(), "B", "books: the display asked last");
  Check(d.Peek("A", &kept) && Has(kept.book.Sizes(), {1920, 1080}), "books: a book can be had with no look");
  d.SetSwitched("A");
  Check(d.IsSwitched("A") && !d.IsSwitched("B"), "books: the switched display");
  d.Listed("A", 9000000, read);
  d.Listed("A", 99000000, read);
  CheckEq(std::to_string(looks), "3", "books: a switched display is never looked at, however old the book");
  d.Listed("B", 9001 + Displays::kLifetimeMs, read);
  CheckEq(std::to_string(looks), "4", "books: another display is still looked at");
  d.SetSwitched({});
  Check(!d.IsSwitched("A"), "books: the mark is gone");
  d.Listed("A", 99000001, read);
  CheckEq(std::to_string(looks), "5", "books: once the display's own mode is back it is looked at again");
  // A switched display that has no book yet (the first look is made during a switch): looked at once, as
  // switched, and the display's own size is still in the book.
  Displays first;
  first.SetSwitched("C");
  const Info c = first.Listed("C", 5, [&](Info* info, bool switched) {
    Check(switched, "books: the first look of a switched display is told so");
    info->book.Add(SwitchedLook1080p(), switched, kMin);
  });
  Check(Has(c.book.Sizes(), {1920, 1080}), "books: the first look in 1280x720 still has 1920x1080");
}

void CheckResolutionIsNotFullscreensSize() {
  // The rule the menu and display_mode.cpp keep (they cannot be run here): FULLSCREEN reads
  // fullscreen_resolution, never `resolution`. What the old reading did, on the 1080p monitor of the report
  // with `resolution` = 1280x720 from a window: asked for 1280x720, a switch.
  Book book;
  book.Add(OwnLook1080p(), false, kMin);
  const Size window_resolution = {1280, 720};
  const Size old_reading = ChooseExclusive(book.Sizes(), book.desktop(), window_resolution);
  const Size new_reading = ChooseExclusive(book.Sizes(), book.desktop(), {});
  CheckEq(Text(old_reading), "1280x720", "the old reading: `resolution` 1280x720 made FULLSCREEN 1280x720");
  CheckEq(Text(new_reading), "1920x1080", "the new reading: a window's size is not FULLSCREEN's");
  Check(PlanExclusive(new_reading, book.desktop(), false, {}) == Plan::kOwn, "the new reading: no switch");
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
  CheckNearest();
  CheckChooseExclusive();
  CheckPlanExclusive();
  CheckBorderlessToFullscreenOn1080p();
  CheckSwitchWithTheMenuLooking();
  CheckEmptyOwnModeSwitchesNothing();
  CheckDsrModes();
  CheckDsrAsTheDesktopMode();
  CheckDisplaysBooks();
  CheckResolutionIsNotFullscreensSize();
  std::printf("display_sizes_test: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
