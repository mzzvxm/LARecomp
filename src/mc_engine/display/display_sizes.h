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
// its refresh rates. Sorted by area (then width), no size twice, in physical pixels. Modes ABOVE the
// display's own (NVIDIA DSR, AMD VSR: the driver lists 2560x1440 or 3840x2160 for a 1920x1080 monitor
// while the feature is on) are listed like any other: they are never taken for the display's own mode,
// which is the registry's, nor the monitor's own size, which is its preferred timing.
//
// Also here, because they too need no Windows: which size FULLSCREEN is asked to switch to (ChooseExclusive:
// the display's own unless the player chose another in FULLSCREEN), what the game does about it
// (PlanExclusive), and the per-display books with the rule of when the driver is looked at (Displays).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
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
    } else if (desktop_.empty() && !switched) {
      // No registry settings (the call failed, or it answered a size of nothing): the mode the display is
      // in now is its own, unless this game has it switched.
      if (!look.desktop.empty()) {
        desktop_ = look.desktop;
        desktop_hz_ = look.desktop_hz;
      } else if (!look.current.empty()) {
        desktop_ = look.current;
        desktop_hz_ = look.current_hz;
      }
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

// ── What FULLSCREEN switches the display to ────────────────────────────────────────────────────

// The listed size closest to `want` (the sum of the two differences); the first (the smallest) when `want`
// is empty or the list has no other.
inline Size Nearest(const std::vector<Size>& sizes, Size want) {
  Size best = sizes.empty() ? want : sizes.front();
  if (want.empty()) return best;
  int64_t best_distance = INT64_MAX;
  for (const Size& s : sizes) {
    const int64_t distance = (s.width > want.width ? int64_t(s.width - want.width) : int64_t(want.width - s.width)) +
                             (s.height > want.height ? int64_t(s.height - want.height) : int64_t(want.height - s.height));
    if (distance < best_distance) {
      best_distance = distance;
      best = s;
    }
  }
  return best;
}

// The size FULLSCREEN is asked to switch the display to. `saved` is what the player chose IN FULLSCREEN
// (fullscreen_resolution; empty when none was): the nearest listed mode. With none it is the display's
// own mode (`own`, the registry settings), which is no switch at all: entering FULLSCREEN from a window or
// from BORDERLESS leaves the display as it is. It is never the smallest mode of the list: a display whose
// own mode is not known yet (empty `own`) is asked for nothing (empty), not for 1280x720.
// A window's size (`resolution`) is not a `saved` here: that was the way the display was once switched to
// 1280x720 by a player who had only ever used a window of that size (Mz, 2026-11-16).
inline Size ChooseExclusive(const std::vector<Size>& sizes, Size own, Size saved) {
  if (saved.empty()) return own;
  return sizes.empty() ? saved : Nearest(sizes, saved);
}

// What the game does for a FULLSCREEN choice `want` (empty: the display's own), the display being `own`
// in its own mode, and this game having switched it to `switched_to` or not (`switched`).
enum class Plan {
  kOwn,     // `want` is the display's own size: its own mode is put back (when this game had switched it)
  kKeep,    // the display already is in `want`: nothing to do
  kSwitch,  // switch the display to `want`
};
inline Plan PlanExclusive(Size want, Size own, bool switched, Size switched_to) {
  if (want.empty()) want = own;
  if (want == own) return Plan::kOwn;
  if (switched && switched_to == want) return Plan::kKeep;
  return Plan::kSwitch;
}

// ── The books of the displays ──────────────────────────────────────────────────────────────────

// What is kept of one display.
struct Info {
  Book book;
  // The size of the display's work area (its own size less the taskbar), as the monitor said it while
  // the display was in its own mode. Empty when that was not asked.
  Size work;
  // What the log last said of the book (it says it again only when it changed).
  std::string logged;
  uint64_t when = 0;  // the clock (ms) at the last look at the driver
};

// The books by display, and the display this game has switched to a mode of its own choosing, from before
// the switch is asked for to after the display's own mode is back (empty: none). Not locked: the caller
// holds one lock around every call (the menu asks from any thread, the UI thread changes the mode).
//
// The driver is looked at when the display has no book yet, and again at most every kLifetimeMs; never
// while the display is switched, however old the book is: the driver then answers for the mode the display
// is in (see the top of this file), and the book from before is what the menu uses. The mark is set BEFORE
// the call that switches (the switch takes a second or two, and the menu asks every frame it is open).
class Displays {
 public:
  static constexpr uint64_t kLifetimeMs = 5000;

  // The book of `device` as it stands: kept while it is fresh or the display is switched, else `read` looks
  // at the driver first (void(Info*, bool switched): adds to info->book, may set work and logged).
  template <class Read>
  Info Listed(const std::string& device, uint64_t now, Read&& read) {
    latest_ = device;
    const auto kept = infos_.find(device);
    const bool have = kept != infos_.end();
    const bool switched = IsSwitched(device);
    const bool fresh = have && now - kept->second.when <= kLifetimeMs;
    if (have && (fresh || switched)) return kept->second;
    Info info = have ? kept->second : Info{};
    read(&info, switched);
    info.when = now;
    infos_[device] = info;
    return info;
  }

  // The book kept for `device`, with no look at the driver; false when there is none.
  bool Peek(const std::string& device, Info* out) const {
    const auto kept = infos_.find(device);
    if (kept == infos_.end()) return false;
    *out = kept->second;
    return true;
  }

  // The display looked at last (empty when none was).
  const std::string& latest() const { return latest_; }

  // This game is switching `device` to another mode, or (empty) has put the display's own mode back, or the
  // switch did not happen: what the display lists is then asked again at the next ask.
  void SetSwitched(const std::string& device) {
    switched_ = device;
    if (device.empty()) {
      for (auto& entry : infos_) entry.second.when = 0;
    }
  }
  bool IsSwitched(const std::string& device) const { return !switched_.empty() && switched_ == device; }

 private:
  std::map<std::string, Info> infos_;
  std::string switched_;
  std::string latest_;
};

}  // namespace mcla::display::sizes
