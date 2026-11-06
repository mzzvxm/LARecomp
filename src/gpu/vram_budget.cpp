// MCLA GPU renderer -- the response to the video memory budget. See
// vram_budget.h.

#include "vram_budget.h"

#include <algorithm>
#include <format>

#include <rex/logging.h>

#include "device.h"
#include "edram.h"
#include "process_memory.h"
#include "resolve.h"
#include "resolve_memory.h"
#include "textures.h"

namespace mcla::gpu {

namespace {

constexpr double kMiB = 1.0 / (1024.0 * 1024.0);

}  // namespace

void VideoMemoryBudget::Initialize(Device& device, TextureCache& textures, ResolveCache& resolve,
                                   ResolveMemory& resolve_memory, EdramTargets& edram) {
  device_ = &device;
  textures_ = &textures;
  resolve_ = &resolve;
  resolve_memory_ = &resolve_memory;
  edram_ = &edram;
  next_check_ = device.frame() + kCheckFrames;
  last_log_ = device.frame();
  short_ = false;
  window_ = Window{};
}

void VideoMemoryBudget::Shutdown() {
  device_ = nullptr;
  textures_ = nullptr;
  resolve_ = nullptr;
  resolve_memory_ = nullptr;
  edram_ = nullptr;
  window_ = Window{};
}

backend::VideoMemory VideoMemoryBudget::Sample() {
  return device_->QueryVideoMemory();
}

void VideoMemoryBudget::BeginFrame() {
  if (!device_) {
    return;
  }
  const uint64_t frame = device_->frame();
  // The diagnostic that says what the rest of the process holds (off by default).
  ProcessMemoryTick(frame);
  if (frame < next_check_) {
    return;
  }
  next_check_ = frame + kCheckFrames;

  const backend::VideoMemory memory = Sample();
  const uint64_t failed = device_->TakeFailedBytes();
  window_.skipped += device_->TakeSkipped();
  ++window_.checks;

  uint64_t need = 0;
  if (memory.budget != 0 && memory.used * 100 >= memory.budget * kHighPercent) {
    need = memory.used - memory.budget * kTargetPercent / 100;
  }
  if (failed != 0) {
    // A creation did not fit: room for it, and for the retry to land.
    need = std::max(need, failed + kFailureMargin);
    ++window_.failures;
  }
  if (need == 0) {
    short_ = false;
    if (window_.any() && frame >= last_log_ + kLogFrames) {
      LogReleased(memory);
    }
    return;
  }
  ++window_.short_checks;
  if (!short_) {
    short_ = true;
    // A shortage that comes and goes is logged once per window, not every time.
    if (last_onset_ == 0 || frame >= last_onset_ + kLogFrames) {
      last_onset_ = frame;
      if (memory.budget != 0) {
        REXLOG_INFO("[gpu] video memory: short, {} of {} MiB in use ({}%){}; unused resources "
                    "are released, the longest idle first",
                    memory.used >> 20, memory.budget >> 20, memory.used * 100 / memory.budget,
                    failed ? std::format(", a creation of {} MiB failed", failed >> 20)
                           : std::string());
      } else {
        REXLOG_INFO("[gpu] video memory: short, a creation of {} MiB failed (the adapter reports "
                    "no budget); unused resources are released, the longest idle first",
                    failed >> 20);
      }
    }
  }
  Trim(need);
  if (window_.any() && frame >= last_log_ + kLogFrames) {
    LogReleased(memory);
  }
}

uint64_t VideoMemoryBudget::Trim(uint64_t need) {
  uint64_t released = 0;
  const auto add = [&released](TrimResult& total, const TrimResult& part) {
    total.items += part.items;
    total.bytes += part.bytes;
    released += part.bytes;
  };
  for (const uint64_t idle : kIdleBands) {
    // What is made again cheapest first: a product or a target comes back empty
    // (a clear), a decoded texture from the pool by one pass, a guest texture
    // from guest memory by the CPU.
    if (released < need) {
      add(window_.products, resolve_->Trim(idle, need - released, kMaxItems));
    }
    if (released < need) {
      add(window_.targets, edram_->Trim(idle, need - released, kMaxItems));
    }
    if (released < need) {
      add(window_.decoded, resolve_memory_->Trim(idle, need - released, kMaxItems));
    }
    if (released < need) {
      add(window_.textures, textures_->Trim(idle, need - released, kMaxItems));
    }
    if (released >= need) {
      break;
    }
  }
  return released;
}

void VideoMemoryBudget::LogReleased(const backend::VideoMemory& memory) {
  const Window& w = window_;
  const uint64_t frame = device_->frame();
  REXLOG_INFO(
      "[gpu] video memory: in the last {} frames released {} resolve products ({:.1f} MiB), {} "
      "EDRAM targets ({:.1f} MiB), {} decoded textures ({:.1f} MiB) and {} textures ({:.1f} MiB), "
      "each idle for at least {} frames | {} of {} MiB in use at the last check | held now: "
      "textures {:.0f}, products {:.0f}, EDRAM targets {:.0f}, resolve memory {:.0f} MiB | {} "
      "checks, {} short ({} by a failed creation), {} creations not retried",
      frame - last_log_, w.products.items, double(w.products.bytes) * kMiB, w.targets.items,
      double(w.targets.bytes) * kMiB, w.decoded.items, double(w.decoded.bytes) * kMiB,
      w.textures.items, double(w.textures.bytes) * kMiB, kIdleBands[2], memory.used >> 20,
      memory.budget >> 20, double(textures_->HeldBytes()) * kMiB,
      double(resolve_->HeldBytes()) * kMiB, double(edram_->HeldBytes()) * kMiB,
      double(resolve_memory_->HeldBytes()) * kMiB, w.checks, w.short_checks, w.failures,
      w.skipped);
  window_ = Window{};
  last_log_ = frame;
}

}  // namespace mcla::gpu
