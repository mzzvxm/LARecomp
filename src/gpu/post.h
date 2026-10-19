#pragma once
// ===========================================================================
// MCLA GPU renderer -- anti-aliasing of the composed scene, before the UI
// ===========================================================================
// (M6c.2 FXAA, M6c.3 SMAA). Runs at the
// composite record (Scene::OnComposite): the end of the game's post chain
// (sub_8260EFD0) left the scene's image in its EDRAM target -- the UI target,
// colour k_8_8_8_8@720 pitch 1280 1x in every scene measured -- nothing has
// copied it yet, and what the frame draws next is the UI (or, in the pause,
// the photo mode and the garage, a copy of this image under a UI drawn in
// another target). Smoothing it there leaves the UI's text and lines alone.
// It also changes what the guest resolves into the front buffer: photos and
// the album see the anti-aliased image, as on a console that did this.
//
// mcla_gpu_aa (live): off | fxaa | smaa.
//   fxaa  the target -> a copy with the luma in alpha (post_luma_ps); FXAA 3.11
//         (Xenia's "fxaa" preset) from the copy into the target.
//   smaa  the target -> a copy; SMAA 1x: edges (cleared first), blending
//         weights (with AreaTex and SearchTex), neighbourhood blending from the
//         copy into the target.
// Both write the target's RGB only: its alpha stays the guest's. The region is
// the guest rectangle the scene's draws covered (Scene's extent of the target)
// at the target's resolution scale. The images are made at that size,
// R8G8B8A8_UNORM (and R8G8_UNORM for the edges), and retired when it changes;
// the SMAA lookup textures are uploaded once, on first use.
//
// A target that is not single-sampled R8G8B8A8_UNORM is left alone: counted,
// logged once per identity.
//
// The passes leave the target in COLOR_WRITE and no framebuffer bound; the
// caller forgets its bindings (they set their own layout, pipeline, viewport
// and scissor).
//
// Threading: render thread only, after Initialize.
// ===========================================================================

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <plume_render_interface.h>

#include "edram.h"
#include "host_passes.h"

namespace mcla::gpu {

class BindlessHeap;
class Device;
class PassTiming;
class UploadRing;

class PostAa {
 public:
  enum class Mode : uint8_t { kOff, kFxaa, kSmaa };

  PostAa() = default;
  ~PostAa();
  PostAa(const PostAa&) = delete;
  PostAa& operator=(const PostAa&) = delete;

  bool Initialize(Device& device, BindlessHeap& heap, UploadRing& upload, HostPasses& passes,
                  EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle, before HostPasses and the BindlessHeap shut down.
  void Shutdown();

  // mcla_gpu_aa, read now.
  static Mode CurrentMode();

  // The composed scene in `target`, `width` x `height` guest pixels from its
  // origin: the mode's passes into `list` (outside a render pass; `slot` the
  // frame's upload slot). True when anything was recorded.
  bool Apply(plume::RenderCommandList* list, uint32_t slot, EdramTargets::Target& target,
             uint32_t width, uint32_t height);

  std::string TakeSummary();

 private:
  struct LumaPush {  // hlsl/post_luma_ps.hlsl
    uint32_t source;
  };
  struct FxaaPush {  // hlsl/fxaa_ps.hlsl
    uint32_t source;
    uint32_t reserved;
    float rcp_x;
    float rcp_y;
  };
  struct SmaaPush {  // hlsl/smaa_ps.hlsl
    uint32_t source;
    uint32_t second;
    uint32_t area;
    uint32_t search;
    float metrics[4];  // 1 / width, 1 / height, width, height
  };
  static_assert(sizeof(SmaaPush) == HostPasses::kPushConstantBytes);

  // The final pass into `target`, RGB only.
  void DrawIntoTarget(plume::RenderCommandList* list, EdramTargets::Target& target,
                      uint32_t width, uint32_t height, const plume::RenderPipeline* pipeline,
                      const void* push, uint32_t push_size);
  // AreaTex and SearchTex into their textures, once. False when the upload
  // memory or the textures are missing.
  bool UploadLookups(plume::RenderCommandList* list, uint32_t slot);

  Device* device_ = nullptr;
  BindlessHeap* heap_ = nullptr;
  UploadRing* upload_ = nullptr;
  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;

  std::unique_ptr<plume::RenderShader> luma_shader_;
  std::unique_ptr<plume::RenderShader> fxaa_shader_;
  std::unique_ptr<plume::RenderShader> smaa_edges_shader_;
  std::unique_ptr<plume::RenderShader> smaa_weights_shader_;
  std::unique_ptr<plume::RenderShader> smaa_blend_shader_;
  std::unique_ptr<plume::RenderPipeline> luma_pipeline_;
  std::unique_ptr<plume::RenderPipeline> fxaa_pipeline_;
  std::unique_ptr<plume::RenderPipeline> smaa_edges_pipeline_;
  std::unique_ptr<plume::RenderPipeline> smaa_weights_pipeline_;
  std::unique_ptr<plume::RenderPipeline> smaa_blend_pipeline_;

  HostImage copy_;     // the scene, and for FXAA its luma in alpha
  HostImage edges_;    // SMAA
  HostImage weights_;  // SMAA

  // SMAA's lookup textures (thirdparty/smaa/Textures).
  std::unique_ptr<plume::RenderTexture> area_;
  std::unique_ptr<plume::RenderTextureView> area_view_;
  std::unique_ptr<plume::RenderTexture> search_;
  std::unique_ptr<plume::RenderTextureView> search_view_;
  uint32_t area_slot_ = 0xFFFFFFFFu;
  uint32_t search_slot_ = 0xFFFFFFFFu;
  bool lookups_ready_ = false;

  std::unordered_set<uint64_t> reported_targets_;
  Mode reported_mode_ = Mode::kOff;

  struct Stats {
    uint64_t fxaa = 0;
    uint64_t smaa = 0;
    uint64_t skipped_target = 0;  // not single-sampled R8G8B8A8_UNORM
    uint64_t failed = 0;          // an image, a slot or the lookups could not be had
  };
  Stats stats_;
};

}  // namespace mcla::gpu
