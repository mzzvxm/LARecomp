#pragma once
// ===========================================================================
// MCLA GPU renderer -- the noise's frame number follows the scene (repeatable scenes, trailer toolkit)
// ===========================================================================
// The renderer shifts its noise from frame to frame: the occlusion's slice rotation and step offsets (ao.cpp, `frame_ & 1023`, the first version) or the phase of a
// lattice (ao_stable.h NoiseCode), the contact shadows', the sun shafts' (shafts.cpp), the shadow filter's (shadow_filter.h) and the temporal upscalers' jitter
// (temporal.cpp, jitter_index_). The number is the renderer's own count of frames since the process started, so the same scene frame carries another
// noise phase in every run, and two pictures of the same scene differ in every pixel that has occlusion or a shadow edge in it (perf\out\TEST-SCENES.txt:
// 20 to 60% of the pixels of a street picture differ, by a few codes).
//
// While a scene runs (the scene seed's applier, cinematic/scene_seed.cpp), every guest frame is tagged with the number of the scene update it shows, and
// Index() hands that number to the places above instead of the renderer's count. The same scene frame then gets the same noise, whatever the process had
// drawn before. Outside a scene nothing changes: Index() returns the fallback it is given.
//
// THE TAG (second version, SCENE-DETERMINISM-2). The first version tagged a frame at its guest SWAP and paired the k-th swap with the renderer's k-th
// Scene::BeginFrame. Run det1 (ts-det1-*, 2026-11-19) shows that it never paired: the log line "[gpu] scene noise: the first guest frame of a scene
// carries update ..." is in none of the six logs although the exe has the string, so every frame kept the renderer's own count and the noise was as
// random as before. Two faults, both found by reading the code:
//   1. The counts drift apart. The game swaps without the renderer opening a list (the legal screens, the title, the loading loops: an idle present has no
//      BeginFrame), and a flush without a present opens a second list in one guest frame. A difference of more than the ring (256) breaks every later
//      pairing for good.
//   2. The swap is the END of the frame's guest work: its hook runs after the frame's draw records are written, so the renderer has often begun the frame
//      (BeginFrame reads the tag at the first draw) before the tag was written. The capture mode found the same fault and moved its stamp to the hand-over
//      (hooks/frame_pacing.cpp rex_sub_82305B38: the render thread is idle, the update that just ran is handed to it); the swap that follows is that frame's.
// Now: the hand-over (OnHandOver, game thread) writes the update that just ran into the ring slot of the NEXT SWAP's number (the swaps counted so far: the
// previous swap is done and the render thread is idle), and the renderer reads the slot of the present records it has executed so far (OnPresent counts them,
// from Scene::OnPresent). Swap n writes present record n, so the renderer's frame n (the draws between present n-1 and present n) finds the stamp of hand-over n
// whatever idle presents or extra lists came before, and a flush in the middle of a guest frame (a second BeginFrame) reads the same slot. A frame with no
// hand-over (the legal screens, the loading loops) has no tag and keeps the renderer's own count.
//
// CHECK (no run has shown it): that every guest swap passes the hook (frame_timing.cpp Patch_60FPS_Byte, sub_824199B0) and writes exactly one present record
// (guest_capture.cpp GuestCapture::Present), and that the hand-over runs on the game thread that SetUpdate runs on. The first tagged frame of a scene should carry
// update 0, 1 or 2 (the log line of scene.cpp), and the scene's end line (cinematic/scene_seed.cpp, GetSummary) should say that almost every frame found its tag.
//
// Threading: SetUpdate, OnGuestSwap and OnHandOver on the game thread; OnPresent, BeginFrame and Index on the render thread. All atomics; header only, so
// cars_test.cpp can check it.
// ===========================================================================

#include <atomic>
#include <cstdint>

namespace mcla::gpu::scene_noise {

inline constexpr uint32_t kRing = 256;
inline constexpr uint64_t kNone = ~uint64_t(0);

struct Entry {
  std::atomic<uint64_t> tag{kNone};  // the swap number this entry is for, or kNone: not a scene frame
  std::atomic<uint32_t> value{0};    // the scene update the frame shows
};

struct State {
  Entry ring[kRing];
  std::atomic<uint64_t> swaps{0};        // guest swaps seen: the number of the swap that closes the frame being handed over
  std::atomic<uint64_t> presents{0};     // guest present records the renderer has executed: the number of the present that closes the frame it records
  std::atomic<int64_t> update{-1};       // the scene update the game thread is in; -1 = no scene noise
  std::atomic<uint64_t> current{kNone};  // the update the renderer's frame in progress shows, or kNone
  std::atomic<uint64_t> first_logged{kNone};
  std::atomic<uint64_t> stamped{0};      // hand-overs that wrote a tag since Rearm
  std::atomic<uint64_t> found{0};        // renderer frames that read a tag since Rearm
  std::atomic<uint64_t> missed{0};       // renderer frames that found no tag while a scene held the noise
};

inline State& Get() {
  static State s;
  return s;
}

// Game thread. `update` is the scene's update number (0 for the first update of the scene), -1 when no scene holds the noise.
inline void SetUpdate(int64_t update) {
  Get().update.store(update, std::memory_order_relaxed);
}

inline bool Active() {
  return Get().update.load(std::memory_order_relaxed) >= 0;
}

// Game thread, at every guest swap (hooks/frame_timing.cpp): one more swap done. Nothing is tagged here (see THE TAG).
inline void OnGuestSwap() {
  Get().swaps.fetch_add(1, std::memory_order_relaxed);
}

// Game thread, at the hand-over of a frame to the guest's render thread (hooks/frame_pacing.cpp rex_sub_82305B38; the render thread is idle and the previous
// swap has run): the frame about to be drawn shows the update that just ran, and the swap numbered `swaps` (the ones before it are done) will close it.
inline void OnHandOver() {
  State& s = Get();
  const uint64_t k = s.swaps.load(std::memory_order_relaxed);
  Entry& e = s.ring[k % kRing];
  const int64_t u = s.update.load(std::memory_order_relaxed);
  e.tag.store(kNone, std::memory_order_relaxed);
  e.value.store(u >= 0 ? static_cast<uint32_t>(u) : 0u, std::memory_order_relaxed);
  e.tag.store(u >= 0 ? k : kNone, std::memory_order_release);
  if (u >= 0) s.stamped.fetch_add(1, std::memory_order_relaxed);
}

// Render thread, once per guest present record (Scene::OnPresent, after everything of the frame): the next frame the renderer opens is the one that the next
// swap closes.
inline void OnPresent() {
  Get().presents.fetch_add(1, std::memory_order_relaxed);
}

// Render thread, when a list is opened, before anything of the frame is recorded (Scene::BeginFrame). Returns true on the first frame of a scene that carries a
// tag (for one log line). Opened more than once in a guest frame, it reads the same slot.
inline bool BeginFrame() {
  State& s = Get();
  const uint64_t k = s.presents.load(std::memory_order_relaxed);
  const Entry& e = s.ring[k % kRing];
  const uint64_t tag = e.tag.load(std::memory_order_acquire);
  if (tag != k) {
    s.current.store(kNone, std::memory_order_relaxed);
    if (s.update.load(std::memory_order_relaxed) >= 0) s.missed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  s.current.store(e.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
  s.found.fetch_add(1, std::memory_order_relaxed);
  uint64_t none = kNone;
  return s.first_logged.compare_exchange_strong(none, k, std::memory_order_relaxed);
}

// Re-arms the one-time log line and the counters (the game thread, when a scene begins).
inline void Rearm() {
  State& s = Get();
  s.first_logged.store(kNone, std::memory_order_relaxed);
  s.stamped.store(0, std::memory_order_relaxed);
  s.found.store(0, std::memory_order_relaxed);
  s.missed.store(0, std::memory_order_relaxed);
}

// Render thread: the noise's frame number: the scene update the frame in progress shows, else `fallback` (the renderer's own count).
inline uint64_t Index(uint64_t fallback) {
  const uint64_t c = Get().current.load(std::memory_order_relaxed);
  return c == kNone ? fallback : c;
}

// The update the frame in progress shows, or -1.
inline int64_t CurrentUpdate() {
  const uint64_t c = Get().current.load(std::memory_order_relaxed);
  return c == kNone ? -1 : static_cast<int64_t>(c);
}

// For the scene's end log line: how the pairing did since Rearm (the renderer is a few frames behind, so the counts are about, not exact).
struct Summary {
  uint64_t stamped = 0;  // hand-overs that carried a scene update
  uint64_t found = 0;    // renderer frames that read a tag
  uint64_t missed = 0;   // renderer frames that found none while the scene held the noise
};
inline Summary GetSummary() {
  const State& s = Get();
  Summary r;
  r.stamped = s.stamped.load(std::memory_order_relaxed);
  r.found = s.found.load(std::memory_order_relaxed);
  r.missed = s.missed.load(std::memory_order_relaxed);
  return r;
}

}  // namespace mcla::gpu::scene_noise
