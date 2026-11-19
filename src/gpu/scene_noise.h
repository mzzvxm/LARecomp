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
// THE TAG. The game thread calls OnGuestSwap() at every swap (hooks/frame_timing.cpp, beside the latency reducer's): the k-th swap since the start closes the
// k-th guest frame, and the renderer's k-th Scene::BeginFrame draws that frame, so the pairing is the count (a ring of 256 entries holds the tags; a frame
// more than 256 swaps behind, or a pair whose tag does not carry its number, gets the fallback). CHECK: that every guest swap is exactly one Scene::BeginFrame
// (an idle present has no list and no BeginFrame; a swap that gets no frame would shift the pairing for good). The first tagged frame of a scene should
// carry update 0 or 1: the log line of scene_seed.cpp says what the renderer sees.
//
// Threading: SetUpdate and OnGuestSwap on the game thread, BeginFrame and Index on the render thread. All atomics; header only, so cars_test.cpp can check it.
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
  std::atomic<uint64_t> swaps{0};      // guest swaps seen
  std::atomic<uint64_t> frames{0};     // guest frames the renderer has begun
  std::atomic<int64_t> update{-1};     // the scene update the game thread is in; -1 = no scene noise
  std::atomic<uint64_t> current{kNone};  // the number of the frame the renderer is recording, or kNone
  std::atomic<uint64_t> first_logged{kNone};
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

// Game thread, at every guest swap: the frame this swap closes shows the scene's current update.
inline void OnGuestSwap() {
  State& s = Get();
  const uint64_t k = s.swaps.fetch_add(1, std::memory_order_relaxed);
  Entry& e = s.ring[k % kRing];
  const int64_t u = s.update.load(std::memory_order_relaxed);
  e.tag.store(kNone, std::memory_order_relaxed);
  e.value.store(u >= 0 ? static_cast<uint32_t>(u) : 0u, std::memory_order_relaxed);
  e.tag.store(u >= 0 ? k : kNone, std::memory_order_release);
}

// Render thread, once per guest frame, before anything of the frame is recorded. Returns true on the first frame of a scene that carries a tag (for one log line).
inline bool BeginFrame() {
  State& s = Get();
  const uint64_t k = s.frames.fetch_add(1, std::memory_order_relaxed);
  const Entry& e = s.ring[k % kRing];
  const uint64_t tag = e.tag.load(std::memory_order_acquire);
  if (tag != k) {
    s.current.store(kNone, std::memory_order_relaxed);
    return false;
  }
  s.current.store(e.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
  uint64_t none = kNone;
  return s.first_logged.compare_exchange_strong(none, k, std::memory_order_relaxed);
}

// Re-arms the one-time log line (the game thread, when a scene begins).
inline void Rearm() {
  Get().first_logged.store(kNone, std::memory_order_relaxed);
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

}  // namespace mcla::gpu::scene_noise
