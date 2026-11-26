#pragma once
// ===========================================================================
// MCLA GPU renderer -- do the car's side mirrors need the shared rear view? The flag the render thread publishes to the guest (MIRROR-VIEW MV3)
// ===========================================================================
// perf/MIRROR-VIEW.md 2.4: the shared rear view (the game's own mirror phase, redirected into a small target) is wanted when the HUD mirror is on OR the car's side mirrors are on the picture and
// big enough to be worth a view. The second half is decided on the render thread by MirrorGlass (mirror_glass.h: it finds the glass among the car's draws, mirror_glass_rules.h places it on the
// picture) and travels to the guest like the HUD mirror's rectangle does (mirror_draws.h): one relaxed atomic, written by the render thread once a frame. The guest does not read it itself: MirrorView::
// BeginFrame (mirror_view.cpp) copies SideNeed() into the request it publishes once a frame (mirror_view_link.h Request::side_need), and the three-state visibility hooks of MV2 (hooks/rear_view.cpp) read that.
//
// This header is all the guest side includes. It holds nothing of the renderer: the state's packing, the atomic, and the hold that keeps the flag from toggling the phase when the car's draws
// are missing for a frame or two (a camera cut, a culled mirror). mirror_glass_rules_test.cpp has its test.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::mirror_glass {

// A glass is "needed" when its box covers at least this many pixels across on a picture 1080 pixels high (a mirror in a chase camera is a few pixels: no view for it; in a cockpit camera it is
// about 190 px at 1080p). The picture's size is a reference, not the real render size: whether a mirror is worth a view must not change with the resolution or the render scale.
inline constexpr int32_t kMinNeedWidth = 40;
// Frames the published flag stays up after the last frame that needed a view (a third of a second at 60 FPS).
inline constexpr uint32_t kNeedHoldFrames = 20;

// What is published: whether a mirror pair was found at all, and which of the two glasses (the left one and the right one on the picture, MirrorGlass's order) is worth a view.
struct NeedState {
  bool found = false;      // a pair of mirror glasses is among this frame's draws
  uint8_t mask = 0;        // bits 0 and 1: side glasses. Bit 2: cabin glass
  uint16_t widest = 0;     // the widest needed glass, pixels at the reference picture (for the log)
  uint16_t hfov_deg = 0;    // the needed readers' swept field, rounded out to whole degrees
  uint16_t vfov_deg = 0;
  bool need() const { return mask != 0; }
  bool operator==(const NeedState&) const = default;
};

inline constexpr uint64_t Pack(const NeedState& s) {
  return (s.found ? 1u : 0u) | (uint32_t(s.mask & 7u) << 1) | (uint32_t(s.widest) << 8) |
         (uint64_t(s.hfov_deg) << 24) | (uint64_t(s.vfov_deg) << 40);
}

inline constexpr NeedState Unpack(uint64_t v) {
  NeedState s;
  s.found = (v & 1u) != 0;
  s.mask = uint8_t((v >> 1) & 7u);
  s.widest = uint16_t((v >> 8) & 0xFFFFu);
  s.hfov_deg = uint16_t(v >> 24);
  s.vfov_deg = uint16_t(v >> 40);
  return s;
}

inline std::atomic<uint64_t> g_need{0};

// The render thread, once a frame.
inline void PublishNeed(const NeedState& s) { g_need.store(Pack(s), std::memory_order_relaxed); }
// The render thread: the finder is off (or the setting went off).
inline void WithdrawNeed() { g_need.store(0, std::memory_order_relaxed); }
// The guest thread (any thread): what the render thread last published.
inline NeedState CurrentNeed() { return Unpack(g_need.load(std::memory_order_relaxed)); }
// The guest thread's question: is a view wanted for the side mirrors.
inline bool SideNeed() { return CurrentNeed().need(); }

// The hold: a frame that needs a view raises the flag at once and keeps its state; the flag falls only after kNeedHoldFrames frames in a row without a need. Not a rule of the finder: of the
// publication, so the overlay of a frame is exact and the guest's phase request does not flicker when a frame's candidate draws are missing.
class NeedHold {
 public:
  // One call a frame with this frame's state; returns what to publish.
  NeedState Update(const NeedState& now) {
    if (now.need()) {
      held_ = now;
      quiet_ = 0;
      return now;
    }
    if (held_.need()) {
      if (++quiet_ <= kNeedHoldFrames) {
        NeedState out = held_;
        out.found = now.found || held_.found;
        return out;
      }
      held_ = NeedState{};
    }
    return now;
  }
  void Reset() {
    held_ = NeedState{};
    quiet_ = 0;
  }
  bool holding() const { return held_.need(); }

 private:
  NeedState held_;
  uint32_t quiet_ = 0;
};

}  // namespace mcla::gpu::mirror_glass
