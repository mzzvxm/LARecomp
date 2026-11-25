// The display.sizes list generator for the pause menu's RESOLUTION row.

#include "display_menu.h"

#include <algorithm>
#include <cstdint>

#include "mc_engine/display/display_mode.h"

namespace {

using mcla::display::Mode;
using mcla::display::Size;

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

List ResolutionList(Mode mode) {
  List list;
  switch (mode) {
    case Mode::kBorderless: {
      // Nothing to choose: the display's own size, for the label to show.
      Size desktop = mcla::display::DesktopSize();
      if (desktop.empty()) desktop = Size{1280, 720};
      list.sizes.push_back(desktop);
      return list;
    }
    case Mode::kExclusive: {
      list.sizes = mcla::display::ExclusiveSizes();
      // The mode the setting comes to: the size saved for FULLSCREEN (fullscreen_resolution) when the
      // display lists it, else the nearest; the display's own for no setting, which is also what a window's
      // size (`resolution`) never changes. A saved size that is not listed keeps its text and shows as the
      // mode that runs. Where the display's own mode is not known the row shows the size of the picture
      // (nothing is switched then), never the smallest mode.
      Size want = mcla::display::WantedExclusiveSize();
      if (want.empty()) want = mcla::display::OutputSize();
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

}  // namespace

namespace mcla_menu {

GeneratedChoices DisplaySizeChoices(std::string_view display_mode) {
    const Mode mode = display_mode == "fullscreen" ? Mode::kExclusive :
                      display_mode == "borderless" ? Mode::kBorderless : Mode::kWindowed;
    const auto list = ResolutionList(mode);
    GeneratedChoices result;
    result.setting = mode == Mode::kExclusive ? "fullscreen_resolution" : "resolution";
    result.current = list.current;
    for (const auto size : list.sizes)
        result.entries.push_back({mcla::display::SizeLabel(size), mcla::display::SizeText(size)});
    // CHECK: changing resolution live should leave the guest video mode unchanged.
    return result;
}

}  // namespace mcla_menu
