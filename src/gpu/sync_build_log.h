#pragma once
// ===========================================================================
// MCLA GPU renderer -- the render thread's own pipeline builds, as log lines
// ===========================================================================
// The game's pipelines are made on builder threads (pipelines.h): a draw whose pipeline is not ready
// is left out for a few frames. Some draws may not be left out (the rule of Scene::MaySkipForPipeline:
// a draw that writes the stencil, one into a target that is not drawn every frame, a single-sample
// target), and a lookup may find nobody at the key; then the render thread makes the pipeline itself,
// and the frame waits for the driver's compile (65 to 276 of them at the first night frames made the
// 4 to 6 s freeze that the watchdog reports as "frame stalled 5s", 2026-11-22). A wait for a builder
// thread that has begun the key is the same stall.
//
// Until now only the window totals said so ("[gpu] scene (interval): ... pipelines N total (+K created,
// F failed, X ms; ... made here ...)"), with no word of WHICH pipelines. This file is the line each of
// those makes, kept apart from the renderer so that the wording and the rate limit have a test that
// needs no GPU (sync_build_log_test.cpp), and so that perf/scripts/stall_check.py has one format to read:
//
//   [pipeline] sync build #12: guest frame 3456, 41.2 ms, new, vs 0123456789ABCDEF/0 ps FEDCBA9876543210/2A, 4 colour target(s), 2x, stencil, background build refused
//   [pipeline] sync wait #3: guest frame 3460, 8.7 ms, list build, vs ... ps ...
//
// #N counts the session's events of that kind (builds and waits are counted on their own), and the
// first kLoggedFirst of each kind get a line, then every kLoggedEvery-th, so a run that stalls on hundreds
// of builds writes a few dozen lines. The largest #N is within kLoggedEvery of the count (the lines in
// between are silent); the exact totals are the line at shutdown, which a run that is killed does not write.
// The line says "stencil" when the pipeline WRITES the stencil (what the game's rule looks at), not when
// it only tests it. The words are the key's
// own: the shader hashes and the two variant words (hex), what the pack's variant words mean is
// shaderpack/variant_chain.h's. `why` of a build: "new" (nobody had begun it), "claimed" (queued for a
// builder thread that had not begun it: the render thread took it over), "again" (a background build
// gave none, so it is made here once more). The words of the lines avoid what the health checks read
// as an error (fail, error, crash, stalled): a sync build is information, and the checker that judges
// it is stall_check.py.
//
// Threading: everything here is pure; the owner (Pipelines) counts on the render thread.
// ===========================================================================

#include <cstdint>
#include <format>
#include <string>

namespace mcla::gpu::sync_build_log {

// Events logged one by one before the rate limit starts, and the spacing after it.
inline constexpr uint64_t kLoggedFirst = 32;
inline constexpr uint64_t kLoggedEvery = 16;

// Whether the n-th event (1 based) of a kind gets a line.
constexpr bool LogThis(uint64_t n) {
  return n != 0 && (n <= kLoggedFirst || n % kLoggedEvery == 0);
}

enum class Why : uint8_t { kNew, kClaimed, kAgain };

inline const char* WhyName(Why why) {
  switch (why) {
    case Why::kNew: return "new";
    case Why::kClaimed: return "claimed";
    case Why::kAgain: return "again";
  }
  return "?";
}

// The words of a pipeline key that a line names.
struct KeyWords {
  uint64_t vs = 0;
  uint64_t ps = 0;
  uint32_t vs_variant = 0;
  uint32_t ps_variant = 0;
  uint32_t color_count = 0;
  uint32_t sample_count = 1;
  bool stencil = false;  // the pipeline writes the stencil (StencilWrites below)
};

// Plume's RenderStencilOp values in the key's stencil words (PipelineKey::stencil: bit 0 enable; front compare, fail, pass, depth fail at bits 1, 5, 9, 13; back compare, fail, pass at
// 17, 21, 25; stencil_masks: read | write | reference, 8 bits each, the back depth fail at bit 24): UNKNOWN 0, KEEP 1, ZERO 2, REPLACE 3 ...
inline constexpr uint32_t kStencilKeep = 1;

// Whether a key writes the stencil: it is on, the write mask is not 0 and an operation is not KEEP (the test of Scene's StencilWrites on the key's packing, and of
// tools/pipelines/pipeline_list.py's stencil_writes). The game's rule for leaving a draw out while its pipeline builds refuses exactly these.
constexpr bool StencilWrites(uint32_t stencil, uint32_t masks) {
  if ((stencil & 1u) == 0 || ((masks >> 8) & 0xFFu) == 0) {
    return false;
  }
  const uint32_t shifts[] = {5u, 9u, 13u, 21u, 25u};
  for (const uint32_t shift : shifts) {
    const uint32_t op = (stencil >> shift) & 0xFu;
    if (op != 0 && op != kStencilKeep) {
      return true;
    }
  }
  const uint32_t back_depth_fail = (masks >> 24) & 0xFu;
  return back_depth_fail != 0 && back_depth_fail != kStencilKeep;
}

// The session's counts: what Pipelines keeps (render thread only).
struct Counters {
  uint64_t builds = 0;
  uint64_t build_ns = 0;
  uint64_t waits = 0;
  uint64_t wait_ns = 0;
  uint64_t longest_ns = 0;

  // Counts one build; the number it is (1 based).
  uint64_t NoteBuild(uint64_t ns) {
    build_ns += ns;
    if (ns > longest_ns) longest_ns = ns;
    return ++builds;
  }
  uint64_t NoteWait(uint64_t ns) {
    wait_ns += ns;
    if (ns > longest_ns) longest_ns = ns;
    return ++waits;
  }
};

inline std::string KeyText(const KeyWords& k) {
  return std::format("vs {:016X}/{:X} ps {:016X}/{:X}, {} colour target(s), {}x, {}", k.vs, k.vs_variant, k.ps,
                     k.ps_variant, k.color_count, k.sample_count, k.stencil ? "stencil" : "no stencil");
}

// `skippable`: the draw's rule would have let it wait for a builder thread (the build is then here
// because nobody had a queue slot for it, or a capture asked for it); false: the rule refused.
inline std::string FormatBuild(uint64_t n, uint64_t frame, uint64_t ns, Why why, const KeyWords& k, bool skippable) {
  return std::format("[pipeline] sync build #{}: guest frame {}, {:.1f} ms, {}, {}, background build {}", n, frame,
                     double(ns) / 1.0e6, WhyName(why), KeyText(k), skippable ? "allowed" : "refused");
}

// `source`: "list" (an entry of the starter / record list that a builder thread was making) or "request"
// (a key a draw asked a builder thread for).
inline std::string FormatWait(uint64_t n, uint64_t frame, uint64_t ns, const char* source, const KeyWords& k) {
  return std::format("[pipeline] sync wait #{}: guest frame {}, {:.1f} ms, {} build, {}", n, frame,
                     double(ns) / 1.0e6, source, KeyText(k));
}

// A wait that the render thread did not simply sit out (pipeline_wait.h, VK-FIRST-FRAME): after kRaiseAfterMs the worker that was making the pipeline got normal priority,
// and/or after kMakeHereAfterMs the render thread made the pipeline itself as well. Its own line (not a "sync wait", whose pattern stall_check.py reads for the waits
// themselves), numbered and rate limited by its own count:
//   [pipeline] sync wait handover #2: guest frame 3460, 412.0 ms, list build, worker raised to normal priority, made here too, vs ... ps ..., ...
inline std::string FormatHandover(uint64_t n, uint64_t frame, uint64_t ns, const char* source, bool raised, bool made_here, const KeyWords& k) {
  return std::format("[pipeline] sync wait handover #{}: guest frame {}, {:.1f} ms, {} build, {}{}, {}", n, frame, double(ns) / 1.0e6, source,
                     raised ? "worker raised to normal priority" : "worker not raised", made_here ? ", made here too" : "", KeyText(k));
}

// The handovers' totals, once, with the session's line.
inline std::string FormatHandoverSession(uint64_t handovers, uint64_t made_here) {
  return std::format("[pipeline] sync wait handovers this session: {} wait(s) raised a worker or made the pipeline here too, {} of them made it here", handovers, made_here);
}

// The session's line, once, when the pipelines are shut down: the exact totals the rate limit hides.
inline std::string FormatSession(const Counters& c) {
  return std::format(
      "[pipeline] sync builds this session: {} build(s) {:.1f} ms, {} wait(s) {:.1f} ms, longest {:.1f} ms",
      c.builds, double(c.build_ns) / 1.0e6, c.waits, double(c.wait_ns) / 1.0e6, double(c.longest_ns) / 1.0e6);
}

}  // namespace mcla::gpu::sync_build_log
