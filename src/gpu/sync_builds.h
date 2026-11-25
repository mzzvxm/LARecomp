#pragma once
// ===========================================================================
// MCLA GPU renderer -- synchronous builds while capturing (trailer toolkit M2d)
// ===========================================================================
// The renderer makes the pipelines of the guest's draws in the background (pipelines.h). A draw whose
// pipeline a builder thread is still making is left out for a few frames (Scene::MaySkipForPipeline:
// "skipped [pipeline pending ...]" in the "[gpu] scene" lines), and the second draws the HDR scene and the
// upscalers' masks make with pipeline variants of their own are left out the same way. A player never
// sees the gap: the next frames put back what it left out. A take (capture mode: a scene recorded frame
// by frame) keeps every frame, so a frame that misses an object -- a car, a shadow, a lamp's glow --
// is a flaw in the video.
//
// While the capture flag is on, nothing is left out for a pending build: the lookup that would have said
// "pending" makes the pipeline itself (nobody has begun it) or waits for the builder thread that has it,
// on the render thread (Pipelines::Resolve), and the wait is counted here, with its time. The flag is off
// by default; off, nothing differs from before (no extra line in the log, no extra work: a miss reads the
// flag).
//
// The flag is process-wide and has two sources, either of which turns it on:
//   - the debug-only cvar mcla_gpu_sync_builds (live), for tests and for a run that wants it from its
//     first frame (a launch argument);
//   - SetCaptureActive(true): the one call capture mode (mc_engine/cinematic, M2a) makes when a take
//     starts, and SetCaptureActive(false) when it ends.
//
// What waits (the paths that left a draw out or put it off for something being built):
//   kGuestDraw     the guest's own draws: Scene::Prepare -> Pipelines::Get with the rule of
//                  Scene::MaySkipForPipeline (targets the game draws into in every frame), or, for a key
//                  that carries the headlight's word or the G-buffer's planes (variant_wait.h), with a
//                  deferral that always says yes: those lookups wait here too, stencil writers included.
//   kHdrVariant    the HDR scene's second draw (Scene::PrepareHdrDraw): the frame's float scene lacked it.
//   kUpscalerMask  the upscalers' mask draw (Scene::PrepareUpscalerMask): the frame's mask lacked it.
//   kHostPass      the renderer's own pipelines made on a worker when a setting is switched on (the
//                  anti-aliasing and FSR1 passes, perf/boot-lazy-pipelines' BackgroundBuild): the frames
//                  until the build ended were drawn without the effect. Nothing of that is in this branch;
//                  the kind is here so that branch's two "pipelines ready?" functions can count their wait.
// What never left a draw out, and so is not changed: the textures (a decode started ahead is waited for,
// or taken over and done in place by the draw that needs it: TextureCache::Upload) and the renderer's
// own resolve / decode / ambient occlusion pipelines made ahead (HostPrecache::Acquire waits for a build
// under way and makes one no builder has begun).
//
// Books and Decide are pure (no renderer, no ReXGlue): sync_builds_test.cpp checks them alone.
//
// Threading: Books are not locked; the process-wide functions below lock them (the scene's final report
// runs on the thread that shuts the renderer down, the waits on the render thread).
// ===========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>

namespace mcla::gpu::sync_builds {

enum class Kind : uint8_t { kGuestDraw, kHdrVariant, kUpscalerMask, kHostPass, kCount };

inline const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kGuestDraw: return "guest draws";
    case Kind::kHdrVariant: return "hdr variants";
    case Kind::kUpscalerMask: return "upscaler masks";
    case Kind::kHostPass: return "host passes";
    case Kind::kCount: break;
  }
  return "?";
}

// What a lookup that missed does while a pipeline is being made. `rule_allows_skip` is what the lookup
// would have decided without the flag (the Deferral's answer). With the flag on nothing is skipped, and
// a lookup the rule would have left out is a held one: a wait to count.
struct Verdict {
  bool may_skip = false;
  bool held = false;
};
constexpr Verdict Decide(bool capture_active, bool rule_allows_skip) {
  return Verdict{rule_allows_skip && !capture_active, rule_allows_skip && capture_active};
}

struct Totals {
  std::array<uint64_t, size_t(Kind::kCount)> waits{};
  std::array<uint64_t, size_t(Kind::kCount)> wait_ns{};
  uint64_t longest_ns = 0;

  uint64_t Waits() const {
    uint64_t n = 0;
    for (uint64_t v : waits) n += v;
    return n;
  }
  uint64_t WaitNs() const {
    uint64_t n = 0;
    for (uint64_t v : wait_ns) n += v;
    return n;
  }
};

class Books {
 public:
  // The first this many waits of a session are logged one by one (with the frame and the shaders); the
  // rest are only counted, in the interval lines.
  static constexpr uint32_t kLoggedEach = 24;

  // Counts one wait of `ns`. True when it is one of the first kLoggedEach of the session.
  bool Note(Kind kind, uint64_t ns) {
    const size_t i = size_t(kind) < size_t(Kind::kCount) ? size_t(kind) : size_t(Kind::kCount) - 1;
    for (Totals* t : {&window_, &session_}) {
      ++t->waits[i];
      t->wait_ns[i] += ns;
      if (ns > t->longest_ns) t->longest_ns = ns;
    }
    return ++noted_ <= kLoggedEach;
  }
  // The waits since the last Take; starts a new window. The session keeps counting.
  Totals Take() {
    Totals out = window_;
    window_ = Totals{};
    return out;
  }
  const Totals& session() const { return session_; }

 private:
  Totals window_;
  Totals session_;
  uint64_t noted_ = 0;
};

// "[gpu] capture sync (interval): on | 12 waits for a pipeline a draw would have been left out for (guest
// draws 10, hdr variants 2), 523.4 ms in all, mean 43.6, longest 120.5 ms | session 12 waits, 523.4 ms".
// `on` says whether builds are synchronous now.
inline std::string FormatLine(const char* why, bool on, const Totals& window, const Totals& session) {
  std::string kinds;
  for (size_t i = 0; i < size_t(Kind::kCount); ++i) {
    if (window.waits[i] != 0) {
      kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", KindName(Kind(i)), window.waits[i]);
    }
  }
  const uint64_t n = window.Waits();
  const double ms = double(window.WaitNs()) / 1.0e6;
  return std::format(
      "[gpu] capture sync ({}): {} | {} wait(s) for a pipeline a draw would have been left out for{}, "
      "{:.1f} ms in all, mean {:.1f}, longest {:.1f} ms | session {} wait(s), {:.1f} ms",
      why, on ? "on" : "off", n, kinds.empty() ? std::string() : std::format(" ({})", kinds), ms,
      n ? ms / double(n) : 0.0, double(window.longest_ns) / 1.0e6, session.Waits(),
      double(session.WaitNs()) / 1.0e6);
}

// --- the process-wide flag and books (sync_builds.cpp) ----------------------------------------------

// Capture mode's call: builds are synchronous from now (true) until it is called with false. Any thread.
// The cvar mcla_gpu_sync_builds is the other source; either one is enough.
void SetCaptureActive(bool active);
// Whether builds are synchronous: the call's flag or the cvar. Any thread, cheap.
bool CaptureActive();
// Counts a wait of `ns` (a held lookup's time to a pipeline). True when the caller should log it on its
// own (the first few of the session). Any thread.
bool NoteWait(Kind kind, uint64_t ns);
// Once a frame on the render thread: logs when the flag changed since the last frame.
void EndFrame();
// The scene's report (every kReportInterval frames and at shutdown): the window's line. Only once the
// flag has been on or a wait was counted, so a run that never captures logs nothing of this.
void LogInterval(const char* why);

}  // namespace mcla::gpu::sync_builds
