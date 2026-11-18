#pragma once
//
// The pure parts of how LARecomp's ImGui overlays follow the input state (KBM-DESIGN.md phase P7), tested by
// input_state_test.cpp without a game:
//
//   CloseKeyWord      the word on a dialog's CLOSE prompt for the glyph family the player is using now
//   ControllerRoster  the controller toast's memory: the SDK's controller list in, "connected" / "disconnected" events out
//
// Header only; the draw code is in ui/imgui_theme.cpp (CloseKeyLabel) and ui/controller_toast.cpp.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../input_state.h"

namespace mcla::ui {

// The key or button that closes a browser dialog, for the glyphs the player is using right now
// (larecomp::input::ActiveGlyphFamily(): the last device used, or the button_prompts setting). Switch, Steam Deck and
// generic pads draw as Xbox in v1 (the owner's decision), so they say B. A word the overlay font can draw: the footer
// badge is a circle for one letter and a pill for a longer word.
inline const char* CloseKeyWord(larecomp::input::GlyphFamily family) {
  switch (family) {
    case larecomp::input::GlyphFamily::Xbox: return "B";
    case larecomp::input::GlyphFamily::PlayStation: return "CIRCLE";
    case larecomp::input::GlyphFamily::Keyboard: return "ESC";
  }
  return "ESC";
}

struct ControllerEvent {
  bool connected = false;
  std::string name;
};

// What the controller toast remembers between two looks at the SDK's list. Update() takes the list as it is now and
// returns what changed since the last call: a "connected" event for every controller that was not there (in list
// order; all of them on the first call, as the SDK's own toast did at start-up) and a "disconnected" one, named as it
// was when it came, for every one that left. A controller is its SDK handle (never reused in a run), so a pad that
// is unplugged and plugged in again is one of each. An empty name reads "Controller".
class ControllerRoster {
 public:
  std::vector<ControllerEvent> Update(const std::vector<larecomp::input::ControllerInfo>& present) {
    std::vector<ControllerEvent> events;
    std::vector<std::pair<uint64_t, std::string>> next;
    next.reserve(present.size());
    for (const larecomp::input::ControllerInfo& pad : present) {
      std::string name = pad.name.empty() ? std::string("Controller") : pad.name;
      if (!Has(pad.id)) events.push_back({true, name});
      next.emplace_back(pad.id, std::move(name));
    }
    for (const auto& [id, name] : known_) {
      bool still = false;
      for (const auto& now : next) {
        if (now.first == id) still = true;
      }
      if (!still) events.push_back({false, name});
    }
    known_ = std::move(next);
    return events;
  }

  size_t Count() const { return known_.size(); }

 private:
  bool Has(uint64_t id) const {
    for (const auto& pad : known_) {
      if (pad.first == id) return true;
    }
    return false;
  }

  std::vector<std::pair<uint64_t, std::string>> known_;
};

}  // namespace mcla::ui
