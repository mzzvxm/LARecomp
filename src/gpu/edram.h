#pragma once
// ===========================================================================
// MCLA GPU renderer -- the guest's EDRAM render targets on the host
// ===========================================================================
// A draw names its targets
// only through registers: a range of EDRAM tiles seen with a pitch, a format
// and a sample count. One host texture stands for each such identity --
// (colour or depth, base tile, guest format with the _AS_ variants folded,
// pitch, samples) -- for the life of the renderer: the EDRAM keeps its
// contents across frames and so does the host (the photo mode reads what an
// earlier frame left, memory project_native_gfx_photo_capture). Identities
// never exchange contents (measured, 24.3).
//
// Size: width = the pitch; height = the largest any use asked for (rounded
// up to kHeightAlignment, at most kMaxExtent). A use that needs more rows
// recreates the texture and copies the old one into it whole -- colour as one
// subresource, MSAA included; depth as its depth and stencil planes (Plume's
// SubresourcePlane, a fork addition) -- so rows drawn earlier in the frame
// survive. The old texture and every framebuffer naming it are retired by the
// FrameRing's serial rule.
//
// A new texture is cleared whole before anything else touches it: D3D12
// requires a render target or depth buffer from a heap created without
// zeroing to be cleared, discarded or wholly copied first (debug layer id
// 1422); the EDRAM's own initial contents are undefined, and zeros stand for
// them. The clear binds a framebuffer: Get says so through
// *framebuffer_unset, and leaves none bound.
//
// Layout: each target remembers its last layout; Transition records a
// barrier only when it changes.
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

namespace mcla::gpu {

class Device;
class FrameRing;

struct EdramKey {
  uint16_t base_tiles = 0;
  uint16_t pitch = 0;     // pixels
  uint8_t depth = 0;      // 1: depth/stencil target
  uint8_t format = 0;     // xenos Color/DepthRenderTargetFormat, _AS_ folded
  uint8_t samples = 1;
  uint8_t reserved = 0;
  bool operator==(const EdramKey& o) const {
    return base_tiles == o.base_tiles && pitch == o.pitch && depth == o.depth &&
           format == o.format && samples == o.samples;
  }
};

// The colour format a target is stored as: the _AS_ variants share the
// storage of their base format (24.12).
uint32_t FoldColorFormat(uint32_t color_format);

class EdramTargets {
 public:
  static constexpr uint32_t kHeightAlignment = 32;
  static constexpr uint32_t kMaxExtent = 8192;

  struct Target {
    EdramKey key;
    std::unique_ptr<plume::RenderTexture> texture;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t generation = 0;  // bumped when the texture is replaced
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
    uint64_t last_frame = 0;
  };

  EdramTargets() = default;
  ~EdramTargets();
  EdramTargets(const EdramTargets&) = delete;
  EdramTargets& operator=(const EdramTargets&) = delete;

  void Initialize(Device& device, FrameRing& frames);
  // The GPU must be idle and the FrameRing drained.
  void Shutdown();

  void BeginFrame() { ++frame_; }

  // The target of `key`, at least `height` rows tall: created (and cleared),
  // or grown with its contents copied -- recorded into `list`, which must not
  // be inside a render pass the caller still needs: when anything is recorded
  // *framebuffer_unset is set and no framebuffer is left bound. Null when the
  // host cannot make it (logged once per identity).
  Target* Get(const EdramKey& key, uint32_t height, plume::RenderCommandList* list,
              bool* framebuffer_unset);

  // The target of `key` if it exists (nothing is created).
  const Target* Find(const EdramKey& key) const {
    const auto it = targets_.find(key);
    return it == targets_.end() ? nullptr : it->second.get();
  }

  // Records a barrier moving `target` to `layout` when it is elsewhere.
  void Transition(plume::RenderCommandList* list, Target& target,
                  plume::RenderTextureLayout layout);

  // The framebuffer of `color_count` colour targets (slots 0..n-1) and an
  // optional depth target, created on first use. Null on failure (logged).
  plume::RenderFramebuffer* Framebuffer(Target* const* colors, uint32_t color_count,
                                        Target* depth);

  size_t size() const { return targets_.size(); }
  std::string TakeSummary();

 private:
  struct KeyHash {
    size_t operator()(const EdramKey& k) const;
  };
  struct FramebufferKey {
    const Target* targets[5] = {};  // colour 0..3, depth
    uint32_t generations[5] = {};
    uint32_t color_count = 0;
    bool operator==(const FramebufferKey& o) const;
  };
  struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey& k) const;
  };

  bool Create(Target& target, uint32_t height);
  void Clear(Target& target, plume::RenderCommandList* list);
  void RetireFramebuffersOf(const Target* target);

  Device* device_ = nullptr;
  FrameRing* frames_ = nullptr;
  std::unordered_map<EdramKey, std::unique_ptr<Target>, KeyHash> targets_;
  std::unordered_map<FramebufferKey, std::unique_ptr<plume::RenderFramebuffer>,
                     FramebufferKeyHash>
      framebuffers_;
  std::vector<EdramKey> failed_;  // identities reported as impossible
  uint64_t frame_ = 0;

  struct Stats {
    uint64_t created = 0;
    uint64_t grown = 0;
    uint64_t grown_bytes = 0;
    uint64_t framebuffers_created = 0;
    uint64_t barriers = 0;
    uint64_t cleared = 0;  // new textures initialized
  };
  Stats stats_;
};

}  // namespace mcla::gpu
