#pragma once
//
// The pure part of the pad reader for LARecomp's own dialogs (the profile picker, the server
// browser F6, the mod browser, the identity window): what a reading is, how several pads merge,
// when an empty XInput slot may be asked again, and when the list of SDL gamepads is looked at
// again. No SDL, no Windows, no ImGui in here, so it has a check that needs no game
// (pad_feed_test.cpp).
//
// Why it exists: the dialogs used to call XInputGetState on all four slots on every drawn frame.
// A slot with no pad is the slow case of that API (the SDK's own comment: it "costs
// milliseconds"), so a player with no pad paid up to four of those per frame for as long as a
// dialog was open, which is exactly the keyboard and mouse player the dialogs are for.

#include <algorithm>
#include <cstdint>

namespace larecomp::padfeed {

// One reading of every pad there is, merged. Buttons use XInput's bit layout whichever API they
// came from (0x0001 up, 0x0002 down, 0x0004 left, 0x0008 right, 0x0010 start, 0x0020 back,
// 0x0040 left stick click, 0x0080 right stick click, 0x0100 L1, 0x0200 R1, 0x1000 A / south,
// 0x2000 B / east, 0x4000 X / west, 0x8000 Y / north). Sticks run -1..1 with up and right
// positive, as in XInput.
struct PadSample {
  bool connected = false;
  uint16_t buttons = 0;
  float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;
};

// Adds one pad's reading to the merged one: a button is down when any pad holds it, an axis takes
// the pad that is pushed furthest (so a resting second pad never cancels the one in use).
inline void MergeInto(PadSample& merged, const PadSample& pad) {
  if (!pad.connected) return;
  merged.connected = true;
  merged.buttons = uint16_t(merged.buttons | pad.buttons);
  auto stronger = [](float a, float b) { return (b < 0 ? -b : b) > (a < 0 ? -a : a) ? b : a; };
  merged.lx = stronger(merged.lx, pad.lx);
  merged.ly = stronger(merged.ly, pad.ly);
  merged.rx = stronger(merged.rx, pad.rx);
  merged.ry = stronger(merged.ry, pad.ry);
}

// 16-bit axis to -1..1 (-32768 would come out at -1.00003).
inline float AxisToUnit(int32_t value) {
  return std::clamp(float(value) / 32767.0f, -1.0f, 1.0f);
}

// A stick axis as the two ImGui analog keys it drives: `neg` for the negative side, `pos` for the
// positive one, 0..1 each, after a dead zone that is taken off and rescaled so the keys start from
// zero at its edge.
inline void ApplyDeadZone(float value, float dead, float* neg, float* pos) {
  *neg = value < -dead ? (-value - dead) / (1.0f - dead) : 0.0f;
  *pos = value > dead ? (value - dead) / (1.0f - dead) : 0.0f;
}

// The direct XInput reads that remain for a game whose SDK input is not SDL (input_backend =
// "xinput", where the exe's own SDL has XInput switched off): a slot that answered is read on
// every frame, a slot that did not is asked again only after kRetryMs. Time is the caller's
// milliseconds, any monotonic clock.
class SlotBackoff {
 public:
  static constexpr int kSlots = 4;
  static constexpr uint64_t kRetryMs = 1000;

  bool Due(int slot, uint64_t now_ms) const {
    if (slot < 0 || slot >= kSlots) return false;
    return !asked_[slot] || connected_[slot] || now_ms >= retry_at_ms_[slot];
  }

  void Result(int slot, bool connected, uint64_t now_ms) {
    if (slot < 0 || slot >= kSlots) return;
    asked_[slot] = true;
    connected_[slot] = connected;
    retry_at_ms_[slot] = connected ? 0 : now_ms + kRetryMs;
  }

  // How many slots have a pad on them (as of the last answers).
  int ConnectedCount() const {
    int count = 0;
    for (int slot = 0; slot < kSlots; ++slot) {
      if (connected_[slot]) ++count;
    }
    return count;
  }

  bool AnyConnected() const { return ConnectedCount() > 0; }

  void Reset() { *this = SlotBackoff(); }

 private:
  bool asked_[kSlots] = {};
  bool connected_[kSlots] = {};
  uint64_t retry_at_ms_[kSlots] = {};
};

// When the list of SDL gamepads is read again (SDL_GetGamepads allocates a list, and opening or
// closing a pad is the one expensive thing): when the device watcher's generation moved (a pad
// came or went), and otherwise once a second, because the watcher only sees HID devices: a wired
// Xbox 360 pad is not one.
class EnumeratePolicy {
 public:
  static constexpr uint64_t kRefreshMs = 1000;

  bool Due(uint64_t now_ms, uint32_t generation) const {
    return !done_ || generation != generation_ || now_ms >= next_ms_;
  }

  void Done(uint64_t now_ms, uint32_t generation) {
    done_ = true;
    generation_ = generation;
    next_ms_ = now_ms + kRefreshMs;
  }

  // The dialog closed and the pads were let go: look again at the next reading.
  void Forget() { done_ = false; }

 private:
  bool done_ = false;
  uint32_t generation_ = 0;
  uint64_t next_ms_ = 0;
};

}  // namespace larecomp::padfeed
