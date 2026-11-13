// MCLA GPU renderer -- GPU profiler markers (perf_markers.h)

#include "perf_markers.h"

#include <algorithm>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "device.h"

REXCVAR_DEFINE_BOOL(
    mcla_gpu_perf_markers, false, "MCLA/GPU",
    "Name the frame's phases in the command lists for GPU profilers: PIX events on D3D12, "
    "debug utils labels on Vulkan; how deep, mcla_gpu_perf_markers_level. Nsight GPU "
    "Trace's multi-pass metrics need them. Off: no label is recorded. Read when a frame "
    "starts.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(
    mcla_gpu_perf_markers_level, 1, "MCLA/GPU",
    "How deep mcla_gpu_perf_markers names the frame. 0: Frame alone. 1: Frame and its phases "
    "(Scene, EndOfFrame, Present, Overlay) -- the same in every frame, what Nsight's multi-pass "
    "metrics need. 2: also a label per guest clear, resolve, composite and Lock write-back "
    "inside Scene; their count changes from frame to frame, so multi-pass metrics drop them. "
    "Read when a frame starts.")
    .debug_only()
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace mcla::gpu {

void PerfMarkers::Initialize(Device& device) {
  supported_ = device.capabilities().debugLabels;
  enabled_ = false;
  depth_ = 0;
  overflow_ = 0;
}

void PerfMarkers::BeginFrame() {
  if (depth_ != 0 || overflow_ != 0) {
    // A label still open: the frame before did not end its own.
    REXLOG_ERROR("[gpu] perf markers: {} labels still open at a frame's start; dropped",
                 depth_ + overflow_);
    depth_ = 0;
    overflow_ = 0;
  }
  const bool wanted = REXCVAR_GET(mcla_gpu_perf_markers);
  if (wanted && !supported_) {
    if (!warned_) {
      warned_ = true;
      REXLOG_WARN("[gpu] perf markers: mcla_gpu_perf_markers is set but the device records no "
                  "debug labels (Vulkan without VK_EXT_debug_utils); none recorded");
    }
    enabled_ = false;
    return;
  }
  const uint32_t level =
      uint32_t(std::clamp<int32_t>(REXCVAR_GET(mcla_gpu_perf_markers_level), kFrame, kEvent));
  if (wanted != enabled_ || (wanted && level != level_)) {
    REXLOG_INFO("[gpu] perf markers {}, level {}", wanted ? "on" : "off", level);
  }
  enabled_ = wanted;
  level_ = level;
}

void PerfMarkers::Push(plume::RenderCommandList* list, const char* label, Level level) {
  if (!records(level) || !list) {
    return;
  }
  if (depth_ == kMaxDepth) {
    ++overflow_;
    return;
  }
  stack_[depth_++] = label;
  list->beginDebugLabel(label);
}

void PerfMarkers::Pop(plume::RenderCommandList* list, Level level) {
  if (!records(level) || !list) {
    return;
  }
  if (overflow_ != 0) {
    --overflow_;
    return;
  }
  if (depth_ == 0) {
    return;
  }
  --depth_;
  list->endDebugLabel();
}

void PerfMarkers::PopAll(plume::RenderCommandList* list) {
  overflow_ = 0;
  if (!list) {
    depth_ = 0;
    return;
  }
  for (; depth_ != 0; --depth_) {
    list->endDebugLabel();
  }
}

}  // namespace mcla::gpu
