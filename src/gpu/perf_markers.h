#pragma once
// ===========================================================================
// MCLA GPU renderer -- GPU profiler markers
// ===========================================================================
// Named regions of the frame's command lists for GPU profilers (Nsight GPU
// Trace, PIX, RenderDoc): a PIX event on D3D12, a VK_EXT_debug_utils label on
// Vulkan, through Plume's beginDebugLabel. Off unless mcla_gpu_perf_markers is
// set; off, not a single label call is recorded.
//
// Nsight's multi-pass metrics collect over several frames and match their
// markers by name, nesting and child count; one frame that differs and the
// markers are dropped ("Perf marker mismatch between frames detected"). So
// the default hierarchy is the same in every frame -- fixed names, no
// numbers, nothing that comes and goes with the guest's work:
//
//   Frame              kFrame
//     Scene            kPhase: the guest's records, from the frame's first
//     EndOfFrame       kPhase: the scene's end-of-frame copies and encodes
//     Present          kPhase: the front buffer's passes (FSR1, gamma), draw
//     Overlay          kPhase: the SDK's UI
//
// mcla_gpu_perf_markers_level picks how deep: 0 = Frame alone, 1 = the phases
// (default), 2 = also a leaf per guest event inside Scene -- Clear, Resolve,
// Composite, LockWriteBack. Their count follows the guest's frame (59 in one
// driving frame, other counts in the next), so level 2 is for reading one
// frame's timeline, never for multi-pass metrics.
//
// A label is recorded where it falls and changes nothing else: no render pass
// opens or ends for it, no barrier moves, no list is flushed for it. When the
// frame's list is flushed mid-frame (the Lock write-back, the batch flush) the
// open labels stay open: they end in the frame's next list, submitted after
// on the same queue (profilers and VK_EXT_debug_utils pair labels per queue).
// Closing them at the flush and opening them again gave such a frame its Frame
// twice -- Frame{Scene} + Frame{Scene, EndOfFrame, ...} -- and the Lock bursts
// of a drive (8 to 33 locks in 600 frames) put one in nearly every multi-pass
// collection.
//
// The cvars are read when a frame opens with no label open, so a change takes
// effect at the next frame.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;

class PerfMarkers {
 public:
  // How deep a label sits; recorded when the level asked for reaches it.
  enum Level : uint32_t { kFrame = 0, kPhase = 1, kEvent = 2 };

  // Labels need the device's capabilities.debugLabels; without it the cvar is
  // ignored, said once.
  void Initialize(Device& device);

  // At the frame's first list: picks up the cvars.
  void BeginFrame();
  bool enabled() const { return enabled_; }
  bool records(Level level) const { return enabled_ && level <= level_; }

  // `label`: a string that outlives the frame (a literal). A push and its pop
  // name the same level.
  void Push(plume::RenderCommandList* list, const char* label, Level level);
  void Pop(plume::RenderCommandList* list, Level level);
  // Ends every open label: before the frame's last list is submitted.
  void PopAll(plume::RenderCommandList* list);

  // The list was abandoned (or its flush lost the device): what it held is gone.
  void Drop() { depth_ = 0; }

 private:
  static constexpr uint32_t kMaxDepth = 8;

  bool supported_ = false;
  bool enabled_ = false;
  bool warned_ = false;
  uint32_t level_ = kPhase;
  const char* stack_[kMaxDepth] = {};
  uint32_t depth_ = 0;
  uint32_t overflow_ = 0;  // pushes past kMaxDepth, popped without a label
};

// A label over a scope; nothing when its level is not recorded or the list is
// null.
class PerfScope {
 public:
  PerfScope(PerfMarkers& markers, plume::RenderCommandList* list, const char* label,
            PerfMarkers::Level level)
      : markers_(markers), list_(list && markers.records(level) ? list : nullptr), level_(level) {
    if (list_) {
      markers_.Push(list_, label, level_);
    }
  }
  ~PerfScope() {
    if (list_) {
      markers_.Pop(list_, level_);
    }
  }
  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  PerfMarkers& markers_;
  plume::RenderCommandList* list_;
  PerfMarkers::Level level_;
};

}  // namespace mcla::gpu
