// The pause menu's DISPLAY MODE and RESOLUTION rows. See display_menu.h.

#include "display_menu.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/cvar.h>

#include "display_mode.h"

namespace {

using mcla::display::Mode;
using mcla::display::Size;
using mcla_menu::CvarWriter;
using mcla_menu::RowDynamics;

std::string Cvar(const char* name) { return rex::cvar::GetFlagByName(name); }

// A setting written only when it differs, through the pause menu's own writer.
void Put(CvarWriter write, const char* name, const std::string& value) {
  if (Cvar(name) != value) write(name, value.c_str());
}

// ── DISPLAY MODE ───────────────────────────────────────────────────────────────────────────────

int ModeCount() { return mcla::display::ExclusiveSupported() ? 3 : 2; }

std::string ModeLabel(int i) {
  switch (i) {
    case 0:  return "WINDOWED";
    case 1:  return "BORDERLESS";
    default: return "FULLSCREEN";
  }
}

int ModeIndex() {
  switch (mcla::display::WantedMode()) {
    case Mode::kWindowed:   return 0;
    case Mode::kBorderless: return 1;
    default:                return 2;
  }
}

// The old setting is `fullscreen` alone, and it keeps its meaning (off: a window; on: fullscreen,
// borderless unless fullscreen_exclusive says more). WINDOWED leaves fullscreen_exclusive as it was, so
// a player who goes back to FULLSCREEN finds it as they left it. A step into FULLSCREEN sets the
// exclusive choice first so the window does not go through BORDERLESS on the way.
void ModeSet(int i, CvarWriter write) {
  switch (i) {
    case 0:
      Put(write, "fullscreen", "false");
      break;
    case 1:
      Put(write, "fullscreen_exclusive", "false");
      Put(write, "fullscreen", "true");
      break;
    default:
      Put(write, "fullscreen_exclusive", "true");
      Put(write, "fullscreen", "true");
      break;
  }
}

// ── RESOLUTION ─────────────────────────────────────────────────────────────────────────────────

struct List {
  std::vector<Size> sizes;
  int current = 0;
};

uint64_t Area(Size s) { return uint64_t(s.width) * s.height; }

int IndexOf(const std::vector<Size>& sizes, Size size) {
  const auto it = std::find(sizes.begin(), sizes.end(), size);
  return it == sizes.end() ? -1 : int(it - sizes.begin());
}

// Where the display's sizes cannot be asked (not Windows): the SDK's own presets.
std::vector<Size> PresetSizes() { return {{1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}}; }

List ResolutionList() {
  List list;
  switch (mcla::display::WantedMode()) {
    case Mode::kBorderless: {
      // Nothing to choose: the display's own size, for the label to show.
      Size desktop = mcla::display::DesktopSize();
      if (desktop.empty()) desktop = Size{1280, 720};
      list.sizes.push_back(desktop);
      return list;
    }
    case Mode::kExclusive: {
      list.sizes = mcla::display::ExclusiveSizes();
      // The mode the setting comes to: the saved size when the display lists it, else the nearest; the
      // display's own for no setting. A saved size that is not listed keeps its text and shows as
      // the mode that runs.
      Size want = mcla::display::WantedExclusiveSize();
      if (want.empty()) want = Size{1280, 720};
      int at = IndexOf(list.sizes, want);
      if (at < 0) {
        list.sizes.push_back(want);
        at = int(list.sizes.size()) - 1;
      }
      list.current = at;
      return list;
    }
    case Mode::kWindowed:
      break;
  }
  list.sizes = mcla::display::WindowedSizes();
  if (list.sizes.empty()) list.sizes = PresetSizes();
  // The size the window is asked to be (physical pixels), else the size it has. One that is none of
  // the steps (a larecomp.toml number, a size from before, a window the player dragged) is an entry of
  // its own, in its place, while it is the setting; stepping off it drops it.
  const Size saved = mcla::display::WantedWindowedSize();
  int at = IndexOf(list.sizes, saved);
  if (at < 0) {
    at = 0;
    while (at < int(list.sizes.size()) && Area(list.sizes[size_t(at)]) < Area(saved)) ++at;
    list.sizes.insert(list.sizes.begin() + at, saved);
  }
  list.current = at;
  return list;
}

int ResolutionCount() { return int(ResolutionList().sizes.size()); }

std::string ResolutionLabel(int i) {
  const List list = ResolutionList();
  return i >= 0 && i < int(list.sizes.size()) ? mcla::display::SizeLabel(list.sizes[size_t(i)])
                                              : std::string();
}

int ResolutionIndex() { return ResolutionList().current; }

// CHECK: that writing `resolution` while the game runs does not reach the game. The SDK also reads it
// as the guest's video mode (xboxkrnl_video.cpp VdQueryVideoMode) every time the guest asks, which the
// game is believed to do once, at start-up (its screen stays 1280x720 whatever `resolution` says: the
// owner's larecomp.toml has "4k"). Step RESOLUTION in a game and look for a change of the game's own
// screen size (the log's "[gpu] edram: the game's screen is ..." must not come again) or aspect.
void ResolutionSet(int i, CvarWriter write) {
  const List list = ResolutionList();
  if (i < 0 || i >= int(list.sizes.size())) return;
  // The size is physical pixels (display_mode.h). window_width and window_height, which the SDK reads
  // for the first window and which win over `resolution` there, are not looked at once the game runs:
  // `resolution` alone sizes the window, so there is nothing else to write.
  Put(write, "resolution", mcla::display::SizeText(list.sizes[size_t(i)]));
}

// Grey in BORDERLESS, which has the display's own size: the label says it (the movie draws no value
// for a grey row). The buffer is read by the caller at once (a label is built from it).
bool ResolutionEnabled() { return mcla::display::WantedMode() != Mode::kBorderless; }

const char* ResolutionSuffix() {
  thread_local char buf[40];
  switch (mcla::display::WantedMode()) {
    case Mode::kBorderless: {
      const Size desktop = mcla::display::DesktopSize();
      if (desktop.empty()) return " (DISPLAY)";
      std::snprintf(buf, sizeof(buf), " (%s)", mcla::display::SizeLabel(desktop).c_str());
      return buf;
    }
    case Mode::kWindowed:
      // Where the window cannot be sized while the game runs the choice is read at the next start.
      return mcla::display::LiveResize() ? "" : " (RESTART)";
    default:
      return "";
  }
}

}  // namespace

namespace mcla_menu {

// DISPLAY MODE writes two settings and RESOLUTION decides by the mode, so neither names a setting of
// its own for the "restart now?" prompt except `resolution`, which only says RESTART where the window
// cannot be sized live.
extern const RowDynamics kDisplayModeDynamics = {.str_label = ModeLabel,
                                                 .value_count = ModeCount,
                                                 .value_index = ModeIndex,
                                                 .set_value = ModeSet};
extern const RowDynamics kResolutionDynamics = {.str_label = ResolutionLabel,
                                                .enabled = ResolutionEnabled,
                                                .suffix = ResolutionSuffix,
                                                .value_count = ResolutionCount,
                                                .value_index = ResolutionIndex,
                                                .set_value = ResolutionSet};

}  // namespace mcla_menu
