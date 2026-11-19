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
//   IsActivityKey  which keys count (KeyRole: F1-F12 only when a kb_* binding names them; an extra_button_* host key is the pad's)
//   KeyRoleTable   the role of each key as the settings say it now
//   ActiveInputCore  the device, the pad family, the setting and the generation; the cross rules; hot-plug
//   PadLossRule    the controller in the player's hands was removed: when the game is told "not connected" so that it pauses
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

// A stick axis as an overlay reads it (-1..1, past either end clamped) in the game's 16-bit counts.
inline int16_t OverlayAxis(float v) {
  if (!(v == v)) return 0;  // NaN
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return static_cast<int16_t>(v * 32767.0f + (v < 0.0f ? -0.5f : 0.5f));
}

// What an overlay's reading of the pads (XInput's button bits, sticks -1..1 with up and right positive, no triggers)
// looks like to the rules above.
inline PadSample OverlayPadSample(uint16_t buttons, float lx, float ly, float rx, float ry) {
  PadSample s;
  s.buttons = buttons;
  s.lx = OverlayAxis(lx);
  s.ly = OverlayAxis(ly);
  s.rx = OverlayAxis(rx);
  s.ry = OverlayAxis(ry);
  return s;
}

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

// Windows virtual-key codes (the SDK's VirtualKey is the same numbers). KeyRole (input_state.h) is what the host's settings say about a key.

inline bool IsFunctionKey(uint16_t vk) { return vk >= 0x70 && vk <= 0x7B; }  // F1 .. F12

// Does this key, going down, count as KEYBOARD activity? Not when it is a modifier on its own, a lock key, Pause or Print Screen; and not when it is one of F1-F12
// (window and overlay hotkeys: F2 the screenshot hotkey, F4 the settings panel, F11 fullscreen; F1 and F12 the effect compare keys of a developer build) unless a kb_*
// binding names it: a player who put the handbrake on F5 does play with the keyboard. The modifiers stay out even when a binding names them (Shift and Ctrl are
// pressed on their own all the time: nitro and the weight transfer; the W or the arrow that comes with them counts). F13 and up count. A key of an extra_button_*
// setting is a pad button: it is not keyboard activity (ActiveInputCore::OnKeyDown counts it as the pad's).
inline bool IsActivityKey(uint16_t vk, KeyRole role = KeyRole::Plain) {
  if (vk == 0) return false;
  if (role == KeyRole::ExtraButton) return false;
  if (vk >= 0x01 && vk <= 0x06) return false;                      // mouse buttons: they come as mouse events
  if (vk == 0x10 || vk == 0x11 || vk == 0x12) return false;        // Shift, Ctrl, Alt
  if (vk >= 0xA0 && vk <= 0xA5) return false;                      // left / right Shift, Ctrl, Alt
  if (vk == 0x5B || vk == 0x5C) return false;                      // left / right Windows key
  if (vk == 0x14 || vk == 0x90 || vk == 0x91) return false;        // Caps, Num, Scroll lock
  if (vk == 0x13 || vk == 0x2C) return false;                      // Pause, Print Screen
  if (IsFunctionKey(vk)) return role == KeyRole::Bound;            // F1 .. F12: only a binding that names one makes it a game key
  return true;
}

// Which role each key has, as the settings say it now: a plain value, built by the glue (keyboard_native.cpp RoleOfKey) whenever a binding changed and read by the window's
// key-down handler. The caller serialises its own access. An extra button's host key stays an extra button whatever else names it (the keyboard feed takes it away from the
// keyboard), in whatever order the keys are marked.
class KeyRoleTable {
 public:
  void Mark(uint16_t vk, KeyRole role) {
    if (vk == 0 || vk >= roles_.size() || role == KeyRole::Plain) return;
    if (roles_[vk] == KeyRole::ExtraButton) return;
    roles_[vk] = role;
  }
  KeyRole Of(uint16_t vk) const { return vk < roles_.size() ? roles_[vk] : KeyRole::Plain; }

 private:
  std::array<KeyRole, 256> roles_{};
};

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
  ExtraButton,   // the host key of an extra pad button (a back paddle) went down
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
    case Cause::ExtraButton: return "extra pad button";
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
    return PadActivity(pad, family_of(), now);
  }

  // The pad did not answer (or is not the game's): the next sample is compared with a pad at rest.
  void OnPadGone() {
    std::lock_guard<std::mutex> lock(mutex_);
    pad_.Reset();
  }

  // One reading of the pads by an overlay that takes them itself while it is open (the browsers' dialogs): the
  // SDK hands the game a pad at rest then, so OnPadFrame never sees what the player does inside the overlay.
  // The same rules, a filter of its own (the game's polls of that pad at rest between two readings do not reset
  // the comparison), the pad's family not known (it stays what it was).
  Change OnOverlayPadFrame(const PadSample& sample, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const PadCause pad = overlay_pad_.Feed(sample);
    if (pad == PadCause::None) return Change();
    return PadActivity(pad, -1, now);
  }

  // No pad answered the overlay: its next reading is compared with a pad at rest.
  void OnOverlayPadGone() {
    std::lock_guard<std::mutex> lock(mutex_);
    overlay_pad_.Reset();
  }

  // A key went down (`repeat`: the key was already down, an auto-repeat). Key-up never counts: not called. `role`: what the settings say about the key (KeyRole).
  // The host key of an extra pad button is a pad button going down: it makes the pad the active device and keeps the pad family that was in use (the paddles
  // belong to a pad whose family is not known here), like a wheel. Any other key counts as keyboard activity when IsActivityKey says so.
  Change OnKeyDown(uint16_t vk, bool repeat, int64_t now, KeyRole role = KeyRole::Plain) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!focused_) return Looked(Cause::Key, "ignored: the window has no focus");
    if (repeat) return Looked(Cause::Key, "ignored: key repeat");
    if (role == KeyRole::ExtraButton) return Activity(InputDevice::Pad, Cause::ExtraButton, false, -1, now);
    if (!IsActivityKey(vk, role)) return Looked(Cause::Key, "ignored: modifier, lock or function key");
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
  Change PadActivity(PadCause pad, int family, int64_t now) {
    Cause cause = Cause::PadButton;
    switch (pad) {
      case PadCause::Trigger: cause = Cause::PadTrigger; break;
      case PadCause::Stick: cause = Cause::PadStick; break;
      case PadCause::StickMove: cause = Cause::PadStickMove; break;
      case PadCause::Button:
      case PadCause::None: break;
    }
    return Activity(InputDevice::Pad, cause, pad == PadCause::StickMove, family, now);
  }

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
  PadFilter overlay_pad_;  // the pads as an overlay read them (OnOverlayPadFrame)
  MotionFilter motion_;
};

// ---- losing the controller in use ------------------------------------------------------------------------
//
// The SDK's NOP stand-in keeps user 0 connected whatever happens, so the game never sees "not connected" and a
// pad that dies in the player's hands (battery, cable, Bluetooth) would leave the car driving on its own at
// rest. A native PC game pauses then, and the keyboard works as usual. The game's own reaction to "not
// connected" for user 0 is exactly that (KBM P0c: its pause menu, on the first poll, held until START), so the
// exe tells it so for a short while and then lets the stand-in answer again:
//
//   * the pad that made the last counted pad input is remembered by the SDK's handle (NoteActivePad);
//   * when the SDK's list of controllers no longer holds it (OnList), and the pad was the active device, and the
//     game is being driven, and no wheel is in use: the window opens (Verdict::Open);
//   * for the next kPadLossPolls XamInputGetState polls of user 0 (and the XamInputGetCapabilities calls in
//     between) the guest is told "not connected"; the window also ends after kPadLossMaxMs (OnStatePoll);
//   * no window when the keyboard and mouse are the active device, when the game is not driven (the title, the
//     menus, a load, a paused game: nothing there pauses on a lost pad, and the keyboard works), or when the pad
//     that left was not the one in the player's hands (nothing is remembered about the others).
//
// Pure: the facts it needs (the active device, "driving", "a wheel") are arguments. Every call takes the mutex
// (a pad poll, a device list change).
inline constexpr int kPadLossPolls = 10;
inline constexpr int64_t kPadLossMaxMs = 1500;

enum class LossVerdict : uint8_t {
  None,            // the pad in the player's hands is still listed, or none is remembered: nothing happened
  Open,            // the game is told "not connected" from the next poll on
  KeyboardActive,  // the controller in use left, but the keyboard and mouse are the active device
  Wheel,           // ... but a steering wheel is in use
  NotDriving,      // ... but the game is not being driven (title, menus, loading, paused): nothing to pause
};

inline const char* LossVerdictText(LossVerdict v) {
  switch (v) {
    case LossVerdict::Open: return "the game is told \"not connected\" so that it pauses";
    case LossVerdict::KeyboardActive: return "ignored: the keyboard and mouse are the active device";
    case LossVerdict::Wheel: return "ignored: a wheel is in use";
    case LossVerdict::NotDriving: return "ignored: the game is not being driven (title, menu, loading or paused)";
    case LossVerdict::None: break;
  }
  return "nothing to do";
}

enum class LossAnswer : uint8_t {
  Connected,         // the window is not open: the real answer stands
  NotConnected,      // tell the game "not connected" for this poll
  NotConnectedLast,  // the same, and this was the last poll of the window
};

class PadLossRule {
 public:
  // A controller made counted input; `id` is the SDK's handle of the pad (never 0). 0 = not known (a harness pad,
  // a wheel, a synthetic device): the pad remembered stays what it was.
  void NoteActivePad(uint64_t id) {
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    active_pad_ = id;
  }

  // The pad remembered (0 = none).
  uint64_t ActivePad() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_pad_;
  }

  // The SDK's list of controllers (synthetic devices left out) changed: `ids` are their handles. `device` is the
  // active device BEFORE the list is applied to the state machine (which moves it off a removed pad).
  LossVerdict OnList(const uint64_t* ids, size_t count, InputDevice device, bool driving, bool wheel, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_pad_ == 0) return LossVerdict::None;
    for (size_t i = 0; i < count; ++i) {
      if (ids[i] == active_pad_) return LossVerdict::None;
    }
    active_pad_ = 0;  // gone for good: handles are never reused, a pad that comes back is a new one
    return Decide(device, driving, wheel, now);
  }

  // The same decision without a list: the pad in the player's hands is gone (the test cvar input_test_pad_loss,
  // which has no real pad to unplug). The pad need not be remembered.
  LossVerdict OnPadLost(InputDevice device, bool driving, bool wheel, int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_pad_ = 0;
    return Decide(device, driving, wheel, now);
  }

  // One XamInputGetState poll of user 0: is the game to be told "not connected"? Counts the poll; the window
  // ends with the last of its polls or when it is older than kPadLossMaxMs.
  LossAnswer OnStatePoll(int64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (polls_left_ <= 0) return LossAnswer::Connected;
    if (now - opened_ms_ > kPadLossMaxMs) {
      polls_left_ = 0;
      return LossAnswer::Connected;
    }
    --polls_left_;
    ++answered_;
    return polls_left_ == 0 ? LossAnswer::NotConnectedLast : LossAnswer::NotConnected;
  }

  // XamInputGetCapabilities for user 0: true while the window is open (it does not count the call).
  bool CapsHidden() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return polls_left_ > 0;
  }

  // How many polls the last window answered (for the log line that closes it).
  int Answered() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return answered_;
  }

 private:
  LossVerdict Decide(InputDevice device, bool driving, bool wheel, int64_t now) {
    if (device != InputDevice::Pad) return LossVerdict::KeyboardActive;
    if (wheel) return LossVerdict::Wheel;
    if (!driving) return LossVerdict::NotDriving;
    polls_left_ = kPadLossPolls;
    answered_ = 0;
    opened_ms_ = now;
    return LossVerdict::Open;
  }

  mutable std::mutex mutex_;
  uint64_t active_pad_ = 0;
  int polls_left_ = 0;
  int answered_ = 0;
  int64_t opened_ms_ = 0;
};

}  // namespace larecomp::input
