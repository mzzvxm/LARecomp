#pragma once
// ===========================================================================
// MCLA GPU renderer -- CPU time of the render thread's bookkeeping: the accounts
// ===========================================================================
// The pure half of mcla_gpu_cpu_timing (cpu_timing.h): what a stopwatch around a section of
// the render thread's work adds up to, with no clock, no thread and no renderer in it, so a
// standalone program can check it (cpu_timing_test.cpp). The caller hands in the clock.
//
// Sections nest: an encode is timed, and inside it the walk over the pages it touches. The
// accounts keep, per section, the time it was the innermost one running (its own time, which
// adds up across sections to the time of everything timed: no section is counted twice) and the
// time from its start to its end (inclusive: what the section and everything inside it took).
// A stack of the sections running now does that; a section that runs inside itself is counted
// once in its inclusive time.
//
// The clock is the caller's CPU clock: the render thread's TSC less the time it has waited for
// the GPU (thread_load.h), so a wait inside a section is not billed to it.
//
// Threading: one thread (the render thread). Take may be called by another once that thread has
// stopped (the final statistics line).
// ===========================================================================

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "cpu_timing.h"

namespace mcla::gpu::cpu_timing {

struct SectionInfo {
  const char* name;
};

inline constexpr SectionInfo kSections[] = {
    {"resolve call"},     {"resolve edram notes"}, {"resolve before-write"},
    {"resolve activate"}, {"resolve layout"},      {"resolve arm"},
    {"resolve cells"},    {"encode"},              {"encode layout"},
    {"encode page scan"}, {"encode ordering"},     {"encode page marks"},
    {"touch pages"},      {"page upload"},         {"arm pages"},
    {"touch decoded"},    {"batch choose"},        {"batch record"},
    {"holds"},            {"holds layout"},        {"memory bind"},
    {"lock write-back"},  {"lock layout"},         {"layout other"},
    {"invalidation sync"}, {"texture tables"},     {"constants upload"},
    {"bind constants"},
    {"draw dispatch"},
    {"guest draw"},
    {"resolve dispatch"},
    {"constant banks"},
    {"draw prepare"},
    {"draw state decode"},
    {"input layout"},
    {"geometry prepare"},
    {"pipeline key"},
    {"pipeline lookup"},
    {"pipeline worker wait"},
    {"draw record"},
    {"draw targets"},
    {"draw bindings"},
    {"texture bind"},
    {"texture upload"},
    {"geometry uploads"},
    {"upload allocation"},
    {"variant constants"},
    {"environment view"},
    {"planar road view"},
    {"guest shadow view"},
    {"ui map view"},
    {"raster mirror prepare"},
    {"raster mirror record"},
    {"raster mirror stamp"},
    {"side mirror glass replay"},
    {"side mirror rt"},
    {"car draw collection"},
    {"car sun replay"},
    {"car reflection history"},
    {"headlight draw collection"},
    {"headlight shadow replay"},
    {"gbuffer draw classification"},
    {"gbuffer attachment"},
    {"gbuffer publish"},
    {"rt scene"},
    {"rt car mesh staging"},
    {"rt city prepare"},
    {"rt car instances"},
    {"rt scene descriptors"},
    {"rt world rows"},
    {"frame setup"},
    {"frame close"},
    {"frame bookkeeping"},
    {"present and submit"},
    {"list flush"},
    {"texture prefetch"},
    {"ambient occlusion"},
    {"screen capture"},
    {"screen reflections"},
    {"volumetric lighting"},
    {"sun shafts"},
    {"local lights"},
    {"bounce light field"},
    {"bounce light apply"},
    {"temporal anchor"},
    {"hdr replay"},
    {"upscaler mask"},
    {"ui sharpen"},
    {"guest clear"},
};
static_assert(sizeof(kSections) / sizeof(kSections[0]) == size_t(Section::kCount),
              "one name per section");

inline constexpr size_t kSectionCount = size_t(Section::kCount);

inline const char* NameOf(Section section) {
  return kSections[size_t(section)].name;
}

class Accounts {
 public:
  // Nesting beyond this is not timed (counted in Totals::too_deep): no real call chain is near it.
  static constexpr size_t kMaxDepth = 16;

  struct Totals {
    std::array<uint64_t, kSectionCount> self{};       // clock ticks as the innermost section
    std::array<uint64_t, kSectionCount> inclusive{};  // clock ticks from start to end, outermost run
    std::array<uint64_t, kSectionCount> calls{};
    std::array<uint64_t, kSectionCount> longest{};    // the longest single run, inclusive
    uint64_t too_deep = 0;   // sections started deeper than kMaxDepth (not timed)
    uint64_t unmatched = 0;  // ends with no section running
  };

  // A section starts at `clock`.
  void Enter(Section section, uint64_t clock) {
    if (depth_ >= kMaxDepth) {
      ++skipped_;
      ++totals_.too_deep;
      return;
    }
    if (depth_ > 0) {
      // The one running until now stops billing itself.
      Frame& top = stack_[depth_ - 1];
      totals_.self[top.section] += Span(top.resumed, clock);
    }
    stack_[depth_++] = Frame{uint8_t(section), clock, clock};
    ++open_[size_t(section)];
  }

  // The innermost running section ends at `clock`.
  void Exit(uint64_t clock) {
    if (skipped_ > 0) {
      --skipped_;
      return;
    }
    if (depth_ == 0) {
      ++totals_.unmatched;
      return;
    }
    const Frame frame = stack_[--depth_];
    const size_t i = frame.section;
    totals_.self[i] += Span(frame.resumed, clock);
    ++totals_.calls[i];
    if (--open_[i] == 0) {
      // The outermost run of its kind: a section inside itself is not counted twice.
      const uint64_t whole = Span(frame.entered, clock);
      totals_.inclusive[i] += whole;
      totals_.longest[i] = std::max(totals_.longest[i], whole);
    }
    if (depth_ > 0) {
      stack_[depth_ - 1].resumed = clock;  // the enclosing section runs again
    }
  }

  // What was added since the previous Take, and a new window. The section running now is billed up
  // to `clock` and goes on in the new window, so the windows add up to the whole.
  Totals Take(uint64_t clock) {
    if (depth_ > 0) {
      Frame& top = stack_[depth_ - 1];
      totals_.self[top.section] += Span(top.resumed, clock);
      top.resumed = clock;
    }
    Totals out = totals_;
    totals_ = Totals{};
    return out;
  }

  // The same without a clock to bill the running section with (another thread's final line):
  // what the running section did since it last became the innermost one goes in the next window.
  Totals Take() {
    Totals out = totals_;
    totals_ = Totals{};
    return out;
  }

  size_t depth() const { return depth_ + skipped_; }

 private:
  struct Frame {
    uint8_t section = 0;
    uint64_t entered = 0;  // when the section started
    uint64_t resumed = 0;  // when it last became the innermost one
  };

  // A clock that stood still or went back (cores whose counters differ) adds nothing.
  static uint64_t Span(uint64_t from, uint64_t to) { return to > from ? to - from : 0; }

  std::array<Frame, kMaxDepth> stack_{};
  size_t depth_ = 0;
  size_t skipped_ = 0;  // starts beyond kMaxDepth whose ends are still to come
  std::array<uint32_t, kSectionCount> open_{};  // runs of each section in the stack now
  Totals totals_;
};

// A frame contributes its total self time per section, including zero when it did not run.
using SectionTimes = std::array<uint64_t, kSectionCount>;

class FrameWindow {
 public:
  void Add(const Accounts::Totals& frame) {
    if (count_ == kWindowFrames) {
      return;
    }
    for (size_t i = 0; i < kSectionCount; ++i) {
      samples_[i][count_] = frame.self[i];
      totals_.self[i] += frame.self[i];
      totals_.inclusive[i] += frame.inclusive[i];
      totals_.calls[i] += frame.calls[i];
      totals_.longest[i] = std::max(totals_.longest[i], frame.longest[i]);
    }
    totals_.too_deep += frame.too_deep;
    totals_.unmatched += frame.unmatched;
    ++count_;
  }

  size_t count() const { return count_; }
  const Accounts::Totals& totals() const { return totals_; }

  // Nearest-rank p95. Selection only touches initialized elements.
  SectionTimes P95() {
    SectionTimes result{};
    if (count_ != 0) {
      const size_t rank = (count_ * 95 + 99) / 100 - 1;
      for (size_t i = 0; i < kSectionCount; ++i) {
        auto& values = samples_[i];
        std::nth_element(values.begin(), values.begin() + rank, values.begin() + count_);
        result[i] = values[rank];
      }
    }
    return result;
  }

  void Reset() {
    count_ = 0;
    totals_ = {};
  }

 private:
  std::array<std::array<uint64_t, kWindowFrames>, kSectionCount> samples_;  // Add writes before P95 reads.
  Accounts::Totals totals_;
  size_t count_ = 0;
};

// The log text of one window: `frames` frames, `ns_per_cycle` to turn clock ticks into time, `busy`
// the render thread's CPU ticks over the window as the scene's statistics count them (the time
// between its window ends, less idling and GPU waits; 0 when not known here), `overhead_ticks` the
// clock ticks one timed section costs itself (measured when the diagnostic starts).
//
//   cpu timing over 600 frames: render thread busy 2040.00 ms (3.400/frame), timed 150.00 ms
//   (0.250/frame, 7.4% of busy), not timed 1890.00 ms (3.150/frame) | timing itself about
//   0.0021 ms/frame (80.0 sections/frame at 26 ns each) | encode self 24.00 ms (0.0400/frame,
//   1.2%) incl 60.00 ms (0.1000/frame) 8400 calls (14.0/frame) longest 0.310 ms | ...
//
// Sections by their own time, largest first; those never run are left out.
inline std::string Summary(const Accounts::Totals& t, double frames, double ns_per_cycle,
                           uint64_t busy, double overhead_ticks, const SectionTimes& p95 = {}) {
  const double per_frame = frames > 0.0 ? 1.0 / frames : 0.0;
  const auto ms = [&](uint64_t ticks) { return double(ticks) * ns_per_cycle / 1e6; };
  std::vector<size_t> order;
  uint64_t timed = 0;
  uint64_t calls = 0;
  for (size_t i = 0; i < kSectionCount; ++i) {
    timed += t.self[i];
    calls += t.calls[i];
    if (t.calls[i] != 0) {
      order.push_back(i);
    }
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return t.self[a] > t.self[b]; });
  std::string text;
  if (busy != 0) {
    const uint64_t rest = busy > timed ? busy - timed : 0;
    text = std::format(
        "cpu timing over {:.0f} frames: render thread busy {:.2f} ms ({:.3f}/frame), timed {:.2f} "
        "ms ({:.3f}/frame, {:.1f}% of busy), not timed {:.2f} ms ({:.3f}/frame)",
        frames, ms(busy), ms(busy) * per_frame, ms(timed), ms(timed) * per_frame,
        100.0 * double(timed) / double(busy), ms(rest), ms(rest) * per_frame);
  } else {
    text = std::format(
        "cpu timing over {:.0f} frames: render thread busy time not measured on this thread, timed "
        "{:.2f} ms ({:.3f}/frame)",
        frames, ms(timed), ms(timed) * per_frame);
  }
  const double overhead_ns = overhead_ticks * ns_per_cycle;
  text += std::format(
      " | timing itself about {:.4f} ms/frame ({:.1f} sections/frame at {:.0f} ns each)",
      overhead_ns * double(calls) * per_frame / 1e6, double(calls) * per_frame, overhead_ns);
  for (const size_t i : order) {
    text += std::format(
        " | {} self {:.2f} ms ({:.4f}/frame, {:.1f}%) incl {:.2f} ms ({:.4f}/frame) {} calls "
        "({:.1f}/frame) longest {:.3f} ms p95 {:.4f} ms/frame",
        kSections[i].name, ms(t.self[i]), ms(t.self[i]) * per_frame,
        busy != 0 ? 100.0 * double(t.self[i]) / double(busy) : 0.0, ms(t.inclusive[i]),
        ms(t.inclusive[i]) * per_frame, t.calls[i], double(t.calls[i]) * per_frame,
        ms(t.longest[i]), ms(p95[i]));
  }
  if (t.too_deep != 0 || t.unmatched != 0) {
    text += std::format(" | {} sections too deep to time, {} ends without a start", t.too_deep,
                        t.unmatched);
  }
  return text;
}

}  // namespace mcla::gpu::cpu_timing
