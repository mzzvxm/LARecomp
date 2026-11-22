#pragma once
// ===========================================================================
// MCLA GPU renderer -- the host MSAA level: what the setting says and when a change is made (pure rules)
// ===========================================================================
// What this decides, and nothing else (no renderer, no device, no clock of its own, so msaa_plan_test.cpp
// checks it alone). msaa_level.h tells the whole story of the live change; this is its decision, which used to
// be woven into MsaaLevel::BeginFrame between the calls that do the work:
//
//   ParseText     mcla_gpu_msaa as a level: off or 1 is 1 (one host sample over the game's own 2 sample layout), 2, 4, 8,
//                 "4x" the same as "4", any case and any spaces; anything else is the console's 2, flagged.
//   LevelWithin   the level or the next lower one the device can do for every format of the screen (1 always works).
//   UpscalerTakesLevel  an upscaler that runs sets the level to 1 whatever the setting says (it smooths the picture
//                 itself), unless the debug switch keeps the player's level under it.
//   Planner       the steps of one change, asked once a frame at the boundary:
//                   1. the level asked for stands still for kSettle (a row stepped through 2X, 4X, 8X asks for nothing in
//                      between; a level the upscaler path asks for does not wait: its path change settled already);
//                   2. the new level's pipelines are asked for, once (`stage`);
//                   3. a frame in which the targets were remade already (`moved`) or a path or scale change is waiting
//                      (`!settled`) holds the change: it follows;
//                   4. the change is made when the pipelines are ready, or after kMaxWait whatever is left (the rest are
//                      made as draws ask for them);
//                   5. a change that could not be made (video memory) is asked again after kRetry, twice as long each time
//                      up to kRetryMax, and a different level asked in between starts over at once.
//                 The time is the caller's (`Frame::now`), so a test can drive it with a made-up clock.
//
// Render thread.
// ===========================================================================

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mcla::gpu::msaa_plan {

using Clock = std::chrono::steady_clock;

// The setting stands this long before anything is made for it.
inline constexpr std::chrono::milliseconds kSettle{400};
// The copies of the pipelines are waited for this long; after it the level changes anyway.
inline constexpr std::chrono::seconds kMaxWait{20};
// A level the card cannot hold is asked again after kRetry, twice as long each time.
inline constexpr std::chrono::seconds kRetry{3};
inline constexpr std::chrono::seconds kRetryMax{30};

// mcla_gpu_msaa as it reads. `level`: 1 (off, or "1"), 2, 4 or 8, not yet limited by the device; 2 for a value that is
// none of them (what the start used then). `off`: the player turned MSAA off, which is host level 1 over the game's own
// 2 sample layout. `invalid`: not off, not 1, 2, 4, 8 (also with "4x").
struct Parsed {
  uint32_t level = 2;
  bool off = false;
  bool invalid = false;
};

inline Parsed ParseText(std::string_view text) {
  Parsed out;
  std::string v;
  for (unsigned char c : text) {
    if (!std::isspace(c)) v.push_back(char(std::tolower(c)));
  }
  if (!v.empty() && v.back() == 'x') v.pop_back();
  if (v == "4") {
    out.level = 4;
  } else if (v == "8") {
    out.level = 8;
  } else if (v == "off" || v == "1") {
    // One host sample over the game's own 2 sample layout.
    out.level = 1;
    out.off = true;
  } else if (v != "2") {
    out.invalid = true;
  }
  return out;
}

// `wanted` (1, 2, 4 or 8) or the next lower level a device can do, `supported_samples` being the mask of the sample
// counts (bits 2, 4, 8) it can do for every format an image of the screen is stored as; 1 always works. A level of 2 is
// kept whatever the mask says (the game's own layout).
inline uint32_t LevelWithin(uint32_t supported_samples, uint32_t wanted) {
  uint32_t level = wanted;
  while (level > 2 && (supported_samples & level) == 0) {
    level >>= 1;
  }
  return level;
}

// An upscaler that runs takes the level (1); `kept_under_upscaler` (mcla_gpu_msaa_with_upscaler, debug) leaves the player's.
constexpr bool UpscalerTakesLevel(bool upscaling, bool kept_under_upscaler) {
  return upscaling && !kept_under_upscaler;
}

// One frame boundary's facts.
struct Frame {
  uint32_t wanted = 2;     // the level the settings and the upscaler ask for now, within the device's limits (1, 2, 4 or 8)
  uint32_t level = 2;      // the level in force
  bool upscaling = false;  // the path the settings steer to is an upscaler that runs
  bool settled = true;     // no path or render scale change is waiting or being made
  bool moved = false;      // the targets were remade this frame already (one remake a frame)
  Clock::time_point now{};
};

enum class Verdict : uint8_t {
  kRest,              // wanted == level: nothing is asked, what was staged is forgotten
  kWaiting,           // the setting has not stood still for kSettle yet, or a retry is not due
  kHolding,           // the pipelines are asked for; a path change or a remake of the targets goes first
  kWaitingPipelines,  // the new level's pipelines are not all made yet (and kMaxWait has not passed)
  kApply,             // make the change now, then call Applied
};

struct Decision {
  Verdict verdict = Verdict::kRest;
  bool staged_now = false;  // `stage` was called by this plan: the pipelines were asked for
  uint32_t waiting = 0;     // pipelines for the new level not made yet (set from kHolding's successor on)
  bool timed_out = false;   // kApply after kMaxWait with `waiting` still left
};

class Planner {
 public:
  // One frame boundary. `stage` is called once per change, when the setting has stood still: it asks for the pipelines of the new
  // level. `waiting` is asked only once the change is not held back by a path change or a remake, and says how many of them are
  // not made yet. Both are called in this order and not more often than once.
  template <class Stage, class Waiting>
  Decision Plan(const Frame& f, Stage&& stage, Waiting&& waiting) {
    Decision d;
    // The upscaler's own change of path stood still before it was made, so the level it asks for needs no settling of its
    // own: that is for a row stepped through values.
    const bool path_driven = f.upscaling != last_upscaling_;
    last_upscaling_ = f.upscaling;
    if (f.wanted == f.level) {
      Rest();
      d.verdict = Verdict::kRest;
      return d;
    }
    if (f.wanted != wanted_) {
      wanted_ = f.wanted;
      wanted_since_ = path_driven ? f.now - kSettle : f.now;
      staged_ = false;
      retry_after_ = Clock::time_point{};
      retry_delay_ = kRetry;
      failure_logged_ = false;
    }
    if (f.now - wanted_since_ < kSettle || f.now < retry_after_) {
      d.verdict = Verdict::kWaiting;
      return d;
    }
    switching_ = true;
    if (!staged_) {
      staged_ = true;
      staged_at_ = f.now;
      stage();
      d.staged_now = true;
    }
    if (f.moved || !f.settled) {
      d.verdict = Verdict::kHolding;
      return d;
    }
    d.waiting = uint32_t(waiting());
    d.timed_out = f.now - staged_at_ >= kMaxWait;
    d.verdict = d.waiting != 0 && !d.timed_out ? Verdict::kWaitingPipelines : Verdict::kApply;
    return d;
  }

  // The change was tried at `began` (after a kApply). Returns true when it failed and this is the first failure of the
  // change (the caller says so once); the next attempt is due after retry_delay().
  bool Applied(Clock::time_point began, bool ok) {
    if (ok) {
      Rest();
      return false;
    }
    retry_delay_ = failure_logged_ ? std::min(retry_delay_ * 2, kRetryMax) : kRetry;
    retry_after_ = began + retry_delay_;
    const bool first = !failure_logged_;
    failure_logged_ = true;
    switching_ = false;
    return first;
  }

  // Whether a change is being prepared or made: the pause menu's MSAA row says (APPLYING).
  bool switching() const { return switching_; }
  std::chrono::seconds retry_delay() const { return retry_delay_; }
  uint32_t wanted() const { return wanted_; }
  Clock::time_point wanted_since() const { return wanted_since_; }
  Clock::time_point staged_at() const { return staged_at_; }
  Clock::time_point retry_after() const { return retry_after_; }

 private:
  // Nothing is asked: forgets what was staged.
  void Rest() {
    wanted_ = 0;
    staged_ = false;
    retry_after_ = Clock::time_point{};
    retry_delay_ = kRetry;
    failure_logged_ = false;
    switching_ = false;
  }

  bool last_upscaling_ = false;  // the `upscaling` of the last call: a change of it is the path's
  uint32_t wanted_ = 0;          // the level being staged (0: none)
  Clock::time_point wanted_since_{};
  bool staged_ = false;  // its pipelines were asked for
  Clock::time_point staged_at_{};
  Clock::time_point retry_after_{};
  std::chrono::seconds retry_delay_ = kRetry;
  bool failure_logged_ = false;
  bool switching_ = false;
};

}  // namespace mcla::gpu::msaa_plan
