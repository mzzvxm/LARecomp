#pragma once
//
// The active-device state machine, plain C++ (no SDK, no Windows, no clock of its own): the rules of
// KBM-DESIGN.md 2.2 and 2.3, tested by input_state_test.cpp without a game. input_state.cpp is the glue
// that feeds it from the window's events, the pad state and the SDK's device list.
//
// Everything takes `now` in milliseconds from any steady clock. The state the readers see is one
// atomic word; every writer takes one mutex (a call is a few comparisons: a pad poll, a key, a mouse
// event). Nothing here allocates after construction.
//
//   PadFilter      one pad's samples -> "that was activity" (button edge, trigger rise, stick cross or move)
//   MotionFilter   mouse motion events -> "that was a deliberate move" (>= 8 px within 200 ms, two events)
//   IsActivityKey  which keys count
//   ActiveInputCore  the device, the pad family, the setting and the generation; the cross rules; hot-plug
//

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "input_state.h"

namespace larecomp::input {

// ---- the thresholds (KBM-DESIGN.md 2.2) ----------------------------------------------------------
// XInput's dead zones; the stick test runs on the state the game reads, after the player's own dead
// zones, so a larger setting silences drift too.
inline constexpr int kLeftStickDeadzone = 7849;
inline constexpr int kRightStickDeadzone = 8689;
// A stick already past its dead zone counts again when it moves by more than this between two polls.
inline constexpr int kStickMoveMin = 2000;
// A trigger counts when it rises to this; it is armed again once it fell below kTriggerOff (so a trigger
// that trembles around 30 counts once).
inline constexpr int kTriggerOn = 30;
inline constexpr int kTriggerOff = 20;
// Mouse motion: at least this many physical pixels of net travel within the window, in at least two
// motion events with a non-zero delta.
inline constexpr float kMotionMinPx = 8.0f;
inline constexpr int64_t kMotionWindowMs = 200;
inline constexpr size_t kMotionMinEvents = 2;
// Nothing from the mouse moves the device in the first moments after the window got focus.
inline constexpr int64_t kFocusSettleMs = 300;
// While a controller had input this recently, mouse motion alone does not take over (clicks, the wheel
// and keys still do): a Steam Deck's trackpad under a thumb, a desk bump.
inline constexpr int64_t kPadRecentMs = 1000;
// After a switch, one made by "soft" activity (mouse motion, a stick that is already past its dead zone
// moving again) waits this long.
inline constexpr int64_t kAntiFlapMs = 250;
// The XInput button bits that count: the D-pad, Start, Back, both stick clicks, both shoulders, A B X Y.
// Not the Guide bit (0x0400) nor 0x0800.
inline constexpr uint16_t kPadButtonMask = 0xF3FF;

inline constexpr int64_t kNever = -1'000'000'000LL;  // "a long time ago" for a time that has not happened

// ---- the pad ---------------------------------------------------------------------------------------

// What the game reads: X_INPUT_GAMEPAD, in host order.
struct PadSample {
  uint16_t buttons = 0;
  uint8_t lt = 0;
  uint8_t rt = 0;
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;
};

enum class PadCause : uint8_t { None, Button, Trigger, Stick, StickMove };

// One pad's samples, one call per poll. Holds the previous sample and which triggers and sticks are
// armed. The caller serialises its calls (the core's mutex).
class PadFilter {
 public:
  PadCause Feed(const PadSample& s) {
    const uint16_t pressed = static_cast<uint16_t>(s.buttons & ~prev_.buttons & kPadButtonMask);
    // Every call updates every arming flag: no short-circuit between these.
    const bool lt = Rise(s.lt, lt_armed_);
    const bool rt = Rise(s.rt, rt_armed_);
    bool crossed = false;
    bool moved = false;
    Stick(s.lx, s.ly, prev_.lx, prev_.ly, kLeftStickDeadzone, left_armed_, crossed, moved);
    Stick(s.rx, s.ry, prev_.rx, prev_.ry, kRightStickDeadzone, right_armed_, crossed, moved);
    prev_ = s;
    if (pressed != 0) return PadCause::Button;
    if (lt || rt) return PadCause::Trigger;
    if (crossed) return PadCause::Stick;
    if (moved) return PadCause::StickMove;
    return PadCause::None;
  }

  // The pad went away (or its state is not known): the next sample is compared with a pad at rest.
  void Reset() { *this = PadFilter(); }

 private:
  static bool Rise(uint8_t value, bool& armed) {
    if (value >= kTriggerOn) {
      if (!armed) return false;
      armed = false;
      return true;
    }
    if (value < kTriggerOff) armed = true;
    return false;
  }

  // A stick past its dead zone counts when it got there from rest (armed) or when it moves a lot between
  // two polls. It is armed again only when it came back below 3/4 of the dead zone, so a stick that
  // flickers around the edge counts once.
  static void Stick(int x, int y, int px, int py, int dead, bool& armed, bool& crossed, bool& moved) {
    const int64_t m2 = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y;
    const int64_t d2 = static_cast<int64_t>(dead) * dead;
    if (m2 >= d2) {
      if (armed) {
        armed = false;
        crossed = true;
      } else {
        const int64_t dx = x - px;
        const int64_t dy = y - py;
        if (dx * dx + dy * dy > static_cast<int64_t>(kStickMoveMin) * kStickMoveMin) moved = true;
      }
    } else if (m2 < d2 * 9 / 16) {
      armed = true;
    }
  }

  PadSample prev_{};
  bool lt_armed_ = true;
  bool rt_armed_ = true;
  bool left_armed_ = true;
  bool right_armed_ = true;
};

// ---- the mouse -------------------------------------------------------------------------------------

// Mouse motion events -> "the hand moved the mouse on purpose". Keeps the cumulative position after each
// event of the last 200 ms; the move counts when the newest position is at least 8 px from the position
// before the oldest event or from any earlier one in the window, and the window holds two events. A
// resting hand (a pixel or two back and forth), a slow drift (a pixel in 100 ms) and one single big
// jump (a window move, a warp) do not. Events with no motion are ignored.
class MotionFilter {
 public:
  bool Feed(float dx, float dy, int64_t now_ms) {
    if (dx == 0.0f && dy == 0.0f) return false;
    while (count_ > 0 && now_ms - ring_[head_].t > kMotionWindowMs) DropOldest();
    if (count_ == kRing) DropOldest();
    x_ += dx;
    y_ += dy;
    ring_[(head_ + count_) % kRing] = Sample{now_ms, x_, y_};
    ++count_;
    if (count_ < kMotionMinEvents) return false;
    const float limit2 = kMotionMinPx * kMotionMinPx;
    if (Dist2(x_, y_, base_x_, base_y_) >= limit2) return true;
    for (size_t i = 0; i + 1 < count_; ++i) {
      const Sample& s = ring_[(head_ + i) % kRing];
      if (Dist2(x_, y_, s.x, s.y) >= limit2) return true;
    }
    return false;
  }

  void Reset() { *this = MotionFilter(); }

 private:
  static constexpr size_t kRing = 32;
  struct Sample {
    int64_t t = 0;
    float x = 0.0f;
    float y = 0.0f;
  };

  static float Dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
  }
  // The oldest event leaves the window: where it ended is where the window now starts.
  void DropOldest() {
    base_x_ = ring_[head_].x;
    base_y_ = ring_[head_].y;
    head_ = (head_ + 1) % kRing;
    --count_;
  }

  std::array<Sample, kRing> ring_{};
  size_t head_ = 0;
  size_t count_ = 0;
  float x_ = 0.0f;
  float y_ = 0.0f;
  float base_x_ = 0.0f;
  float base_y_ = 0.0f;
};

// ---- the keyboard ----------------------------------------------------------------------------------

// Windows virtual-key codes (the SDK's VirtualKey is the same numbers). A key does not count when it is
// a modifier on its own, a lock key, Pause, Print Screen or one of F1-F12 (window and overlay hotkeys: F2
// the screenshot hotkey, F4 the settings panel, F11 fullscreen; a binding that names one of them does
// not make it count either: CHECK, KBM-DESIGN 2.2 allows it, v1 does not). F13 and up count.
inline bool IsActivityKey(uint16_t vk) {
  if (vk == 0) return false;
  if (vk >= 0x01 && vk <= 0x06) return false;                      // mouse buttons: they come as mouse events
  if (vk == 0x10 || vk == 0x11 || vk == 0x12) return false;        // Shift, Ctrl, Alt
  if (vk >= 0xA0 && vk <= 0xA5) return false;                      // left / right Shift, Ctrl, Alt
  if (vk == 0x5B || vk == 0x5C) return false;                      // left / right Windows key
  if (vk == 0x14 || vk == 0x90 || vk == 0x91) return false;        // Caps, Num, Scroll lock
  if (vk == 0x13 || vk == 0x2C) return false;                      // Pause, Print Screen
  if (vk >= 0x70 && vk <= 0x7B) return false;                      // F1 .. F12
  return true;
}

// ---- the state -------------------------------------------------------------------------------------

// The button_prompts setting: auto follows the device, the others pin the glyph set.
enum class PromptOverride : uint8_t { Auto = 0, Xbox = 1, PlayStation = 2, Keyboard = 3 };

inline PromptOverride ParsePromptOverride(std::string_view text) {
  if (text == "xbox") return PromptOverride::Xbox;
  if (text == "playstation") return PromptOverride::PlayStation;
  if (text == "keyboard") return PromptOverride::Keyboard;
  return PromptOverride::Auto;
}

// What the readers see; one word (see Pack).
struct Snapshot {
  InputDevice device = InputDevice::Pad;
  // The last controller family used (or the first connected one): Xbox or PlayStation. Kept while the
  // keyboard and mouse are in use.
  GlyphFamily strip = GlyphFamily::Xbox;
  PromptOverride setting = PromptOverride::Auto;
  uint32_t generation = 0;  // 24 bits

  // The glyphs the prompts should show.
  GlyphFamily Family() const {
    switch (setting) {
      case PromptOverride::Xbox: return GlyphFamily::Xbox;
      case PromptOverride::PlayStation: return GlyphFamily::PlayStation;
      case PromptOverride::Keyboard: return GlyphFamily::Keyboard;
      case PromptOverride::Auto: break;
    }
    return device == InputDevice::Pad ? strip : GlyphFamily::Keyboard;
  }
  // The strip of the game's own art (the movies' `platform` flag): Xbox or PlayStation.
  GlyphFamily PromptStrip() const {
    switch (setting) {
      case PromptOverride::Xbox: return GlyphFamily::Xbox;
      case PromptOverride::PlayStation: return GlyphFamily::PlayStation;
      case PromptOverride::Keyboard: return GlyphFamily::Xbox;
      case PromptOverride::Auto: break;
    }
    return strip;
  }
  bool operator==(const Snapshot& o) const {
    return device == o.device && strip == o.strip && setting == o.setting && generation == o.generation;
  }
};

inline uint32_t Pack(const Snapshot& s) {
  return static_cast<uint32_t>(s.device) | (static_cast<uint32_t>(s.strip) << 1) | (static_cast<uint32_t>(s.setting) << 2) |
         (s.generation << 8);
}

inline Snapshot Unpack(uint32_t word) {
  Snapshot s;
  s.device = static_cast<InputDevice>(word & 1u);
  s.strip = static_cast<GlyphFamily>((word >> 1) & 1u);
  s.setting = static_cast<PromptOverride>((word >> 2) & 3u);
  s.generation = word >> 8;
  return s;
}

// Why the state changed, or what kind of event was looked at.
enum class Cause : uint8_t {
  None,
  Seed,          // the start-up guess (a Sony pad is plugged in)
  Start,         // the connected pads, before any input
  PadButton,
  PadTrigger,
  PadStick,      // a stick crossed its dead zone
  PadStickMove,  // a stick past its dead zone moved a lot
  Key,
  MouseClick,
  MouseWheel,
  MouseMove,
  PadRemoved,
  PromptSetting,
};

inline const char* CauseText(Cause c) {
  switch (c) {
    case Cause::Seed: return "start-up guess";
    case Cause::Start: return "start (connected pads)";
    case Cause::PadButton: return "pad button";
    case Cause::PadTrigger: return "pad trigger";
    case Cause::PadStick: return "pad stick";
    case Cause::PadStickMove: return "pad stick move";
    case Cause::Key: return "key";
    case Cause::MouseClick: return "mouse click";
    case Cause::MouseWheel: return "mouse wheel";
    case Cause::MouseMove: return "mouse move";
    case Cause::PadRemoved: return "pad removed";
    case Cause::PromptSetting: return "prompt setting";
    case Cause::None: break;
  }
  return "none";
}

// What one call did: the new state when it changed, and for the verbose log what the rules decided about
// the event ("counted", or why it was ignored). `looked` is None when the call was not about an event.
struct Change {
  bool changed = false;
  Snapshot before;
  Snapshot after;
  Cause cause = Cause::None;
  Cause looked = Cause::None;
  const char* note = "";
};

inline GlyphFamily StripOf(GlyphFamily family) {
  return family == GlyphFamily::PlayStation ? GlyphFamily::PlayStation : GlyphFamily::Xbox;
}

class ActiveInputCore {
 public:
  ActiveInputCore() : packed_(Pack(cur_)) {}

  // Any thread, an atomic load.
  Snapshot Get() const { return Unpack(packed_.load(std::memory_order_acquire)); }

  // How many controllers the last list held (0 before the first list).
  size_t PadCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pad_count_;
  }

  // The start-up guess for the glyph strip (a Sony pad is plugged in, as the device watcher sees it). It
  // counts while nothing is known better: before the SDK's first list of devices, and while that list holds
  // no controller (SDL may announce a pad a moment after the first poll, and the guess keeps the strip steady
  // meanwhile); not once a controller is listed (its own family is the answer) or something has been used.
  // The device stays what it is.
  Change Seed(GlyphFamily strip) {
    std::lock_guard<std::mutex> lock(mutex_);
    seed_strip_ = StripOf(strip);
    if (input_seen_ || (list_seen_ && pad_count_ > 0)) return Unchanged(cur_, "ignored: a controller is listed or an input is known");
    Snapshot next = cur_;
    next.strip = seed_strip_;
    return Commit(next, Cause::Seed, "counted");
  }

  // The SDK's list of connected controllers (synthetic devices left out), in connection order; the
  // first call is the first list. Before anything was used the device follows the pads (Pad when there is
  // one, else KeyboardMouse), so a pad that SDL announces a moment after the first poll still counts as
  // present at the start. After that: the active pad going away falls to another pad's family, or to the
  // keyboard and mouse when none is left; a pad that arrives changes nothing until it is used.
  Change OnDevices(const GlyphFamily* pads, size_t count, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    list_seen_ = true;
    pad_count_ = count;
    pad_mask_ = 0;
    for (size_t i = 0; i < count; ++i) pad_mask_ |= static_cast<uint8_t>(1u << (StripOf(pads[i]) == GlyphFamily::PlayStation ? 1 : 0));
    Snapshot next = cur_;
    Cause cause = Cause::Start;
    if (!input_seen_) {
      next.device = count > 0 ? InputDevice::Pad : InputDevice::KeyboardMouse;
      next.strip = count > 0 ? StripOf(pads[0]) : seed_strip_;
    } else if (count == 0) {
      // Nothing left to follow: no controller glyphs to keep.
      cause = Cause::PadRemoved;
      next.strip = GlyphFamily::Xbox;
      next.device = InputDevice::KeyboardMouse;
    } else if (cur_.device == InputDevice::Pad && !HasFamily(cur_.strip)) {
      cause = Cause::PadRemoved;
      next.strip = StripOf(pads[0]);
    }
    Change ch = Commit(next, cause, "counted");
    if (ch.changed && ch.after.device != ch.before.device) last_switch_ms_ = now;
    return ch;
  }

  // button_prompts changed.
  Change SetOverride(PromptOverride setting) {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot next = cur_;
    next.setting = setting;
    return Commit(next, Cause::PromptSetting, "counted");
  }

  // The window got or lost focus. `initial`: the state at attach time, not an event (no settling time).
  void OnFocus(bool focused, int64_t now, bool initial = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    focused_ = focused;
    if (focused) {
      focus_since_ = initial ? kNever : now;
    } else {
      motion_.Reset();
    }
  }

  // One poll of the pad the game reads. `family_of` is called only when the sample counts as activity and
  // returns the family of the pad that made it (0 Xbox, 1 PlayStation) or -1 when that is not known (a
  // test harness's pad, a wheel): the family then stays what it was. It runs under the core's mutex.
  template <class FamilyOf>
  Change OnPadFrame(const PadSample& sample, FamilyOf&& family_of, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const PadCause pad = pad_.Feed(sample);
    if (pad == PadCause::None) return Change();
    Cause cause = Cause::PadButton;
    switch (pad) {
      case PadCause::Trigger: cause = Cause::PadTrigger; break;
      case PadCause::Stick: cause = Cause::PadStick; break;
      case PadCause::StickMove: cause = Cause::PadStickMove; break;
      case PadCause::Button:
      case PadCause::None: break;
    }
    return Activity(InputDevice::Pad, cause, pad == PadCause::StickMove, family_of(), now);
  }

  // The pad did not answer (or is not the game's): the next sample is compared with a pad at rest.
  void OnPadGone() {
    std::lock_guard<std::mutex> lock(mutex_);
    pad_.Reset();
  }

  // A key went down (`repeat`: the key was already down, an auto-repeat). Key-up never counts: not called.
  Change OnKeyDown(uint16_t vk, bool repeat, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!focused_) return Looked(Cause::Key, "ignored: the window has no focus");
    if (repeat) return Looked(Cause::Key, "ignored: key repeat");
    if (!IsActivityKey(vk)) return Looked(Cause::Key, "ignored: modifier, lock or function key");
    return Activity(InputDevice::KeyboardMouse, Cause::Key, false, -1, now);
  }

  // A mouse button went down.
  Change OnMouseButtonDown(int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!focused_) return Looked(Cause::MouseClick, "ignored: the window has no focus");
    return Activity(InputDevice::KeyboardMouse, Cause::MouseClick, false, -1, now);
  }

  // The wheel turned.
  Change OnMouseWheel(int32_t scroll_x, int32_t scroll_y, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!focused_) return Looked(Cause::MouseWheel, "ignored: the window has no focus");
    if (scroll_x == 0 && scroll_y == 0) return Looked(Cause::MouseWheel, "ignored: no scroll");
    return Activity(InputDevice::KeyboardMouse, Cause::MouseWheel, false, -1, now);
  }

  // The mouse moved by (dx, dy) physical pixels since its last event.
  Change OnMouseMove(float dx, float dy, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!focused_) return Looked(Cause::MouseMove, "ignored: the window has no focus");
    if (dx == 0.0f && dy == 0.0f) return Looked(Cause::MouseMove, "ignored: no motion");
    if (now - focus_since_ < kFocusSettleMs) {
      motion_.Reset();
      return Looked(Cause::MouseMove, "ignored: the window got focus less than 300 ms ago");
    }
    if (cur_.device == InputDevice::KeyboardMouse) return Looked(Cause::MouseMove, "ignored: the keyboard and mouse are active already");
    if (!motion_.Feed(dx, dy, now)) return Looked(Cause::MouseMove, "ignored: below the jitter threshold (8 px within 200 ms, two events)");
    return Activity(InputDevice::KeyboardMouse, Cause::MouseMove, true, -1, now);
  }

 private:
  bool HasFamily(GlyphFamily strip) const { return (pad_mask_ & (1u << (strip == GlyphFamily::PlayStation ? 1 : 0))) != 0; }

  static Change Unchanged(const Snapshot& s, const char* note) {
    Change ch;
    ch.before = ch.after = s;
    ch.note = note;
    return ch;
  }

  Change Looked(Cause looked, const char* note) const {
    Change ch = Unchanged(cur_, note);
    ch.looked = looked;
    return ch;
  }

  // Publishes `next` when it differs from the state (device, strip or setting); the generation moves with it.
  Change Commit(Snapshot next, Cause cause, const char* note) {
    Change ch = Unchanged(cur_, note);
    ch.cause = cause;
    if (next.device == cur_.device && next.strip == cur_.strip && next.setting == cur_.setting) return ch;
    next.generation = (cur_.generation + 1u) & 0xFFFFFFu;
    cur_ = next;
    packed_.store(Pack(cur_), std::memory_order_release);
    ch.changed = true;
    ch.after = cur_;
    return ch;
  }

  // Something was used. `soft`: mouse motion or a stick that was past its dead zone already (they wait
  // out the anti-flap time). `family`: -1 unknown, 0 Xbox, 1 PlayStation (a pad's own; ignored otherwise).
  Change Activity(InputDevice target, Cause cause, bool soft, int family, int64_t now) {
    input_seen_ = true;
    if (target == InputDevice::Pad) last_pad_ms_ = now;
    Snapshot next = cur_;
    if (target == InputDevice::Pad && family >= 0) next.strip = family == 1 ? GlyphFamily::PlayStation : GlyphFamily::Xbox;
    const bool switching = target != cur_.device;
    if (switching) {
      if (soft && now - last_switch_ms_ < kAntiFlapMs) {
        Change ch = Looked(cause, "ignored: the device switched less than 250 ms ago");
        return ch;
      }
      if (cause == Cause::MouseMove && now - last_pad_ms_ < kPadRecentMs) {
        Change ch = Looked(cause, "ignored: a controller was used less than 1 s ago");
        return ch;
      }
      next.device = target;
    }
    Change ch = Commit(next, cause, switching ? "counted: the device switched" : "counted");
    ch.looked = cause;
    if (switching && ch.changed) last_switch_ms_ = now;
    return ch;
  }

  mutable std::mutex mutex_;
  Snapshot cur_{};
  std::atomic<uint32_t> packed_;
  bool input_seen_ = false;
  bool list_seen_ = false;
  bool focused_ = true;
  int64_t focus_since_ = kNever;
  int64_t last_pad_ms_ = kNever;
  int64_t last_switch_ms_ = kNever;
  size_t pad_count_ = 0;
  GlyphFamily seed_strip_ = GlyphFamily::Xbox;  // the device watcher's last guess
  uint8_t pad_mask_ = 0;  // bit 0 an Xbox-family pad is connected, bit 1 a PlayStation one
  PadFilter pad_;
  MotionFilter motion_;
};

}  // namespace larecomp::input
