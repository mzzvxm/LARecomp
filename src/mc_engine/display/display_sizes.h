#pragma once
//
// What a display offers as sizes, and what the game knows of it: the rules behind the FULLSCREEN
// RESOLUTION list (display_mode.cpp, display_menu.cpp), with no Windows and no SDK in them so that
// display_sizes_test.cpp can check them with fixtures of real displays.
//
// The list is a Book: every look at the driver is folded into it, and a look can only ADD to it. That
// is the point of it. Windows answers an EnumDisplaySettings call for the mode the display is in (asked
// while the game had switched a 3840x2160 display to 1280x720, the driver's list lost the sizes above it,
// the display's own among them; asked while a 1920x1080 display is in 1280x720, 1920x1080 is not in what
// comes back), so any look can be a wrong one, and a list that was replaced by each look lost the
// display's own size whenever a wrong look was the last one (the owner's and Mz's logs of 2026-11-13: the
// RESOLUTION row had no 3840x2160 and no 1920x1080). Folded in, a wrong look costs nothing: what an
// earlier look found stays.
//
// What is always in the list, whatever the driver listed (from the game's own screen, kMin, up):
//   - the display's own mode, the one the player set in Windows (the registry settings, which a mode this
//     game switched to does not change);
//   - the mode the display is in when it is looked at (a switched one is one this game chose);
//   - the monitor's own size (its preferred timing, from the display configuration API: the answer does
//     not depend on the mode the display is in, nor on the DPI the process sees);
// and what the driver listed: 32 bit, not interlaced, every aspect ratio, one entry per size whatever
// its refresh rates. Sorted by area (then width), no size twice, in physical pixels.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "display_mode.h"

namespace mcla::display::sizes {

// One mode as the driver listed it.
struct RawMode {
  Size size;
  uint32_t hz = 0;
  uint32_t bits = 32;
  bool interlaced = false;
};

// A mode the list keeps: a size and the refresh rate it can be had at (0: not known).
struct Mode {
  Size size;
  uint32_t hz = 0;
};

// One look at a display's driver.
struct Look {
  std::vector<RawMode> modes;  // every mode the driver listed
  Size desktop;                // the display's own mode (what the player set in Windows)
  uint32_t desktop_hz = 0;
  // `desktop` is the registry settings (EnumDisplaySettings ENUM_REGISTRY_SETTINGS), not the mode the
  // display is in: false when the registry has none and the current mode was taken instead.
  bool desktop_is_registry = false;
  Size current;  // the mode the display is in now
  uint32_t current_hz = 0;
  Size preferred;  // the monitor's own size (the display configuration API); empty when not asked
};

inline uint64_t Area(Size s) { return uint64_t(s.width) * s.height; }

// The order of the list: ascending by area, then by width.
inline bool SizeBefore(Size a, Size b) {
  const uint64_t area_a = Area(a);
  const uint64_t area_b = Area(b);
  return area_a != area_b ? area_a < area_b : a.width < b.width;
}

// The game's own screen is 1280x720: nothing below it is offered.
inline bool AtLeast(Size s, Size min) { return s.width >= min.width && s.height >= min.height; }

inline bool Admitted(const RawMode& m, Size min) {
  return m.bits >= 32 && !m.interlaced && AtLeast(m.size, min);
}

inline std::string Text(Size s) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%ux%u", s.width, s.height);
  return buf;
}

class Book {
 public:
  // Folds a look in. `switched`: this game has the display in a mode of its own choosing (or is changing
  // it), so the driver's answer is for that mode and the registry's is the only word on the display's own.
  void Add(const Look& look, bool switched, Size min) {
    // The display's own mode. A switched display keeps what is known: the registry settings do not follow
    // the switch, and a look that took the current mode in their place would call the switched mode the
    // display's own.
    if (look.desktop_is_registry && !look.desktop.empty()) {
      if (!switched || desktop_.empty()) {
        desktop_ = look.desktop;
        desktop_hz_ = look.desktop_hz;
      }
    } else if (desktop_.empty() && !switched && !look.desktop.empty()) {
      desktop_ = look.desktop;
      desktop_hz_ = look.desktop_hz;
    }
    if (!look.preferred.empty()) preferred_ = Oriented(look.preferred, desktop_);

    for (const RawMode& m : look.modes) {
      if (Admitted(m, min)) PutListed(m.size, m.hz);
    }
    PutExtra(desktop_, desktop_hz_, min);
    PutExtra(look.current, look.current_hz, min);
    PutExtra(preferred_, 0, min);
  }

  // The display's own mode (empty until a look found it), its refresh rate, and the monitor's own size.
  Size desktop() const { return desktop_; }
  uint32_t desktop_hz() const { return desktop_hz_; }
  Size preferred() const { return preferred_; }
  bool empty() const { return listed_.empty() && extra_.empty(); }

  // The list: every size the driver listed and the sizes that are always in it, sorted, once each.
  std::vector<Size> Sizes() const {
    std::vector<Size> out;
    for (const Mode& m : listed_) out.push_back(m.size);
    for (const Mode& m : extra_) out.push_back(m.size);
    std::sort(out.begin(), out.end(), SizeBefore);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  // The sizes that are in the list though the driver did not list them (what a wrong look lacked).
  std::vector<Size> Added() const {
    std::vector<Size> out;
    for (const Mode& m : extra_) {
      if (!Has(listed_, m.size)) out.push_back(m.size);
    }
    std::sort(out.begin(), out.end(), SizeBefore);
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  // The refresh rate to switch to `want` at: the display's own when the size has it, else the highest
  // the size lists; the display's own rate for a size with no rate known (the monitor's own size, put in
  // by the display configuration API); 0 when there is none to name (the driver then chooses).
  uint32_t RefreshFor(Size want) const {
    const uint32_t own = desktop_hz_ > 1 ? desktop_hz_ : 0;  // 0 and 1 are the driver's "default"
    uint32_t best = 0;
    bool known = false;
    for (const std::vector<Mode>* list : {&listed_, &extra_}) {
      for (const Mode& m : *list) {
        if (!(m.size == want)) continue;
        known = true;
        if (own != 0 && m.hz == own) return own;
        if (m.hz > 1) best = std::max(best, m.hz);
      }
    }
    if (best != 0) return best;
    return known ? own : 0;
  }

  // One line for the log: what the display is, how many sizes are offered and which, and which of them
  // the driver did not list.
  std::string Describe() const {
    std::string text = "own mode " + (desktop_.empty() ? std::string("not known") : Text(desktop_));
    if (desktop_hz_ > 1) text += " at " + std::to_string(desktop_hz_) + " Hz";
    text += ", the monitor's own size " + (preferred_.empty() ? std::string("not known") : Text(preferred_));
    const std::vector<Size> all = Sizes();
    text += ", " + std::to_string(all.size()) + " sizes offered:";
    for (const Size& s : all) text += " " + Text(s);
    const std::vector<Size> added = Added();
    if (!added.empty()) {
      text += "; put in though the driver did not list them:";
      for (const Size& s : added) text += " " + Text(s);
    }
    return text;
  }

 private:
  static bool Has(const std::vector<Mode>& list, Size size) {
    return std::any_of(list.begin(), list.end(), [&](const Mode& m) { return m.size == size; });
  }

  // The monitor's own size comes landscape; a display turned to portrait has its mode the other way.
  static Size Oriented(Size preferred, Size desktop) {
    if (desktop.empty()) return preferred;
    const bool desktop_wide = desktop.width >= desktop.height;
    const bool preferred_wide = preferred.width >= preferred.height;
    return desktop_wide == preferred_wide ? preferred : Size{preferred.height, preferred.width};
  }

  void PutListed(Size size, uint32_t hz) {
    const auto same = [&](const Mode& m) { return m.size == size && m.hz == hz; };
    if (std::none_of(listed_.begin(), listed_.end(), same)) listed_.push_back({size, hz});
  }

  void PutExtra(Size size, uint32_t hz, Size min) {
    if (size.empty() || !AtLeast(size, min)) return;
    for (Mode& m : extra_) {
      if (m.size == size) {
        if (m.hz == 0) m.hz = hz;
        return;
      }
    }
    extra_.push_back({size, hz});
  }

  Size desktop_;
  uint32_t desktop_hz_ = 0;
  Size preferred_;
  std::vector<Mode> listed_;  // what the driver listed, every look together
  std::vector<Mode> extra_;   // the display's own mode, the modes it was in, the monitor's own size
};

}  // namespace mcla::display::sizes
