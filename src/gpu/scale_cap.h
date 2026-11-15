#pragma once
// ===========================================================================
// MCLA GPU renderer -- how big a render scale the video memory holds
// ===========================================================================
// The render scale that follows the window (mcla_gpu_resolution_scale = 0, the default) is 1.5x in a 1080p
// window, 2x in 1440p and 3x in 4K, and it used to be made with no look at the video memory: a 4K display
// on a card with 2 to 3 GB failed to make some targets ("could not make ...") and drew the scene partly
// (perf\COMPAT-AUDIT.md F2). Only a live change to a bigger scale had a "limited" answer. This is the
// look, as pure arithmetic with nothing of Plume or ReXGlue (scale_cap_test.cpp builds on its own);
// render_scale.cpp reads the card and calls Apply at the start and whenever the window's size changes.
//
// What a scale needs. The video memory the renderer holds at a render scale s (1 = the game's 1280x720)
// is a part that does not follow it and a part that follows the area:
//     need(s) = kFixedMiB + kPerSquareMiB * s * s
// From the measured tables of MEMORY-REPORT.md (RTX 5080, D3D12) and the sizes in COMPAT-AUDIT.md section 7:
//   - EDRAM targets 157 / 504 / 1,082 MiB at 1x / 2x / 3x = 42 + 115.7 s^2 (the three 4x MSAA targets 72 / 287 /
//     647 of them are made once and given back after a minute idle, but they are there while a screen
//     draws the world at 4x, so they count);
//   - resolve products 45 / 147 / 312 MiB = 11 + 34 s^2;
//   - ambient occlusion at ultra 35 / 61 / 138 MiB at 1080p / 1440p / 4K = 15.3 s^2, sun shafts 0.9 s^2
//     (counted at ultra: the sum is the largest the defaults can come to);
//   - the rest does not follow the scale: textures about 50, the resolve memory pool 44 to 64, geometry
//     pages 48 to 96, the swap chain 20, the driver and the pipelines about 130, the shadow and reflection
//     maps about 60, D3D12's 4 MiB blocks: about 450 MiB with the 42 and 11 above.
// So 450 + 166 s^2: 616 MiB at 1x, 824 at 1.5x (1080p), 1,114 at 2x (1440p) and 1,944 at 3x (4K), which is
// the audit's 0.95 / 1.2 / 1.9 GiB at the default (its middle figures were interpolated linearly, this is
// quadratic like the area). It is an estimate to tune with the first logs of small cards (BACKLOG O1:
// `mcla_gpu_vram_log 600` gives the real use against the budget).
//
// What a card allows. The OS budget for the card's local memory (DXGI's on D3D12, VK_EXT_memory_budget on
// Vulkan; Device::QueryVideoMemory), which already is what other programs leave. The scale is planned
// for kPlanPercent of it: under the 80% the budget response aims at (vram_budget.h), so that it never has
// to release the caches to hold the scale, and with a quarter left for what the estimate misses (a Vulkan
// device's 256 MiB blocks, frame generation, a game that loads a long list of cars). A card that reports
// no budget (Vulkan without VK_EXT_memory_budget) is taken to have kAssumedBudgetPercent of its dedicated
// memory. A GPU that shares memory with the CPU (UMA, integrated, software) is not checked: its budget is
// system memory (an integrated GPU's is 15 GB on a 485 MiB carve-out) and its limit is speed, not room.
//
// What is lowered. Only the scale that follows the window by default (the screen's scale): to the largest
// step of the 0.05 grid that fits, never below the game's native 720p (1x) -- a window smaller than that
// keeps its own scale. A scale the player asked for (mcla_gpu_resolution_scale) or an FSR1 preset's is
// kept as chosen with a warning line; a temporal upscaler's output is not checked (the scene is drawn at
// a share of it, which this model does not know).
//
// With a scale that is capped the pause menu's RENDER SCALE row says "LIMITED (NN%)" with the share that
// runs, as for a live change that failed (temporal.cpp publishes the scale the window asked for as
// blocked).
// ===========================================================================

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <string>

#include "render_scale.h"

namespace mcla::gpu::scale_cap {

inline constexpr uint64_t kMiB = 1ull << 20;
inline constexpr double kFixedMiB = 450.0;
inline constexpr double kPerSquareMiB = 166.0;
// The share of the budget a scale is planned for, and of the dedicated memory taken as the budget when none
// is reported.
inline constexpr uint32_t kPlanPercent = 75;
inline constexpr uint32_t kAssumedBudgetPercent = 85;
// Never lowered below the game's own size.
inline constexpr Scale kFloor = Scale::Whole(1);

// The video memory the renderer needs at a render scale, in bytes.
constexpr uint64_t NeedBytes(Scale scale) {
  const double s = double(scale.n) / double(Scale::kDenominator);
  return uint64_t((kFixedMiB + kPerSquareMiB * s * s) * double(kMiB));
}

// What the card says.
struct Memory {
  uint64_t budget = 0;     // the OS budget for the card's local memory; 0: not reported
  uint64_t dedicated = 0;  // the card's dedicated memory; 0: not known
  bool shared = false;     // the GPU uses system memory (UMA, integrated, software)
};

// render_scale.cpp. The renderer registers how to read the card (the OS budget from the device, the class's
// facts) before the start's choice of the scale, and clears it (an empty function) when it shuts down; with
// none registered (the Xenia renderer) no scale is checked. The probe is called once per window size, from
// the render thread.
void SetMemoryProbe(std::function<Memory()> probe);

// The budget the plan is made against: the reported one, else a share of the dedicated memory, else 0.
constexpr uint64_t BudgetOf(const Memory& memory) {
  return memory.budget != 0 ? memory.budget : memory.dedicated * kAssumedBudgetPercent / 100;
}

// The bytes a render scale may take.
constexpr uint64_t PlanBytes(const Memory& memory) {
  return BudgetOf(memory) * kPlanPercent / 100;
}

// The largest step of the grid, from `wanted` down, whose need is within `plan`; never above `wanted`, never
// below the game's own size (a `wanted` under it stays). The floor is returned when nothing fits.
constexpr Scale LargestFitting(Scale wanted, uint64_t plan) {
  const uint32_t floor_n = std::min(wanted.n, kFloor.n);
  uint32_t n = wanted.n;
  while (n > floor_n && NeedBytes(Scale{n}) > plan) {
    --n;
  }
  return Scale{n};
}

enum class Outcome : uint8_t {
  kNotChecked,  // no verdict (Report::reason says why): nothing is changed
  kFits,        // the scale is within the plan
  kCapped,      // the screen's scale was lowered to the largest that fits
  kKept,        // a scale the player chose does not fit and is kept
  kFloor,       // the scale does not fit but is the game's own size or smaller: nothing can be lowered
};

struct Report {
  Outcome outcome = Outcome::kNotChecked;
  const char* reason = "";  // for kNotChecked
  Scale wanted;             // the choice's scale before
  Scale scale;              // and after
  Scale largest;            // the largest that fits (the floor if none does); set when it did not fit
  uint64_t need_wanted = 0;
  uint64_t need_scale = 0;
  uint64_t plan = 0;
  uint64_t budget = 0;
  bool assumed_budget = false;  // `budget` is a share of the dedicated memory
};

// Weighs the choice against the card and lowers it when it is the screen's scale and does not fit
// (sets capped and uncapped). Any other choice is left as it is.
inline Report Apply(RenderScaleChoice* c, const Memory& memory) {
  Report r;
  r.wanted = c->scale;
  r.scale = c->scale;
  if (c->invalid || c->scale.none()) {
    r.reason = "the render scale is not a scale";
    return r;
  }
  if (c->source == RenderScaleChoice::Source::kUpscaler) {
    r.reason = "a temporal upscaler is the active choice and draws the scene at a share of it";
    return r;
  }
  if (memory.shared) {
    r.reason = "this graphics card uses the system's memory, which is not short";
    return r;
  }
  r.budget = BudgetOf(memory);
  if (r.budget == 0) {
    r.reason = "the graphics card reports neither a video memory budget nor its size";
    return r;
  }
  r.assumed_budget = memory.budget == 0;
  r.plan = PlanBytes(memory);
  r.need_wanted = NeedBytes(c->scale);
  r.need_scale = r.need_wanted;
  r.largest = c->scale;
  if (r.need_wanted <= r.plan) {
    r.outcome = Outcome::kFits;
    return r;
  }
  r.largest = LargestFitting(c->scale, r.plan);
  if (c->source != RenderScaleChoice::Source::kScreen) {
    r.outcome = Outcome::kKept;
    return r;
  }
  if (r.largest == c->scale) {
    r.outcome = Outcome::kFloor;
    return r;
  }
  r.outcome = Outcome::kCapped;
  r.scale = r.largest;
  r.need_scale = NeedBytes(r.scale);
  c->uncapped = c->scale;
  c->scale = r.scale;
  c->capped = true;
  return r;
}

// Where a scale that was kept came from, for the warning.
inline std::string SourceText(const RenderScaleChoice& c) {
  if (c.source == RenderScaleChoice::Source::kPreset) {
    return std::format("The FSR1 preset {} (mcla_gpu_fsr_preset)", FsrPresetName(c.preset));
  }
  return std::format("mcla_gpu_resolution_scale = {}", c.asked);
}

// The one plain line for a verdict (the caller adds "[gpu] "). `c` is the choice after Apply.
inline std::string Line(const RenderScaleChoice& c, const Report& r) {
  const auto mib = [](uint64_t bytes) { return bytes / kMiB; };
  const std::string window = std::format("{}x{}", c.window_width, c.window_height);
  const std::string budget =
      r.assumed_budget ? std::format("{} MiB (the card reports no budget: {}% of its size)",
                                     mib(r.budget), kAssumedBudgetPercent)
                       : std::format("{} MiB", mib(r.budget));
  switch (r.outcome) {
    case Outcome::kNotChecked:
      return std::format("render scale {}x was not checked against the video memory: {}", r.wanted,
                         r.reason);
    case Outcome::kFits:
      return std::format("render scale {}x fits the video memory: it needs about {} MiB and the "
                         "card's budget is {}, {} MiB of it planned for",
                         r.wanted, mib(r.need_wanted), budget, mib(r.plan));
    case Outcome::kCapped:
      return std::format(
          "render scale lowered from {}x to {}x: the window ({}) at {}x would need about {} MiB of "
          "video memory, and the card's budget is {}, of which {} MiB ({}%) are planned for the "
          "scale (the rest is for textures and other programs); {}x is the largest that fits{}. "
          "Setting mcla_gpu_resolution_scale picks a scale yourself.",
          r.wanted, r.scale, window, r.wanted, mib(r.need_wanted), budget, mib(r.plan), kPlanPercent,
          r.scale,
          r.need_scale > r.plan
              ? std::format(", though it still needs about {} MiB: the game's own size is the "
                            "smallest drawn",
                            mib(r.need_scale))
              : std::string());
    case Outcome::kKept:
      return std::format(
          "{} asks for render scale {}x, which would need about {} MiB of video memory; the card's "
          "budget is {} and {} MiB of it are planned for the scale. It is kept as chosen, and parts of "
          "the picture may be missing if the card cannot hold it (the largest scale that fits is {}x)",
          SourceText(c), r.wanted, mib(r.need_wanted), budget, mib(r.plan), r.largest);
    case Outcome::kFloor:
      return std::format("render scale {}x needs about {} MiB of video memory and the card's budget "
                         "is {} ({} MiB planned for), but it is not above the game's own size: it "
                         "stays",
                         r.wanted, mib(r.need_wanted), budget, mib(r.plan));
  }
  return std::string();
}

}  // namespace mcla::gpu::scale_cap
