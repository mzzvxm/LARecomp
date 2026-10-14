#pragma once
// ===========================================================================
// MCLA GPU renderer -- the replay of the guest's frames
// ===========================================================================
// Executes the scene records of
// each guest frame -- draws, clears, resolves, tiling -- into the frame's
// command list, which the Renderer opens at the frame's first scene record
// and submits with the present (frame.h).
//
// A draw goes through Prepare, which decides everything about it that does
// not need its buffers or its targets:
//   decoded Xenos state (draw_state.h) -> constant banks rebuilt and checked
//   (constants.h) -> shaders from the pack -> host primitive (topology.h) ->
//   target formats (format.h) -> input layout (input_layout.h) -> pipeline
//   key, depth bias and pipeline (pipelines.h) -> viewport and NDC transform
//   (viewport.h) -> samplers of every fetch constant the shaders read
//   (samplers.h) and the texture tables -> the three constant buffers
//   uploaded: VS bank, PS bank (each only when it changed since the last
//   upload of the frame) and SharedConstants (only when it differs from the
//   previous draw's).
// Record then binds the draw's EDRAM targets (edram.h) and records it; state
// already bound in the list is not bound again. Clears are recorded from
// their records. The texture tables name the bindless slots the
// TextureCache gives for each fetch constant the shaders read (textures.h,
//): a resolve product, or a texture decoded from guest memory,
// whose uploads are recorded into the frame's list right there.
//
// A draw that cannot be drawn correctly is skipped, never drawn with a
// guess: the reason is counted, its first occurrences logged with the draw's
// identity. Draws with no effect (no target written, both faces culled) are
// counted apart -- they are not failures.
//
// Statistics: a line every kReportInterval guest frames and at shutdown.
//
// Threading: render thread only.
// ===========================================================================

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <plume_render_interface.h>

#include "constants.h"
#include "edram.h"
#include "geometry.h"
#include "input_layout.h"
#include "pipelines.h"
#include "records.h"
#include "shaders.h"
#include "topology.h"
#include "upload.h"
#include "viewport.h"

namespace mcla::gpu {

class BindlessHeap;
class GuestLayout;
class PassTiming;
class ResolveCache;
class Samplers;
class TextureCache;

class Scene {
 public:
  static constexpr uint64_t kReportInterval = 600;

  explicit Scene(ShaderStore& shaders) : shaders_(shaders), input_layouts_(shaders) {}
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  void Initialize(UploadRing& upload, Pipelines& pipelines, Samplers& samplers,
                  GeometryCache& geometry, EdramTargets& edram, TextureCache& textures,
                  ResolveCache& resolve, const GuestLayout& layout, const BindlessHeap& heap,
                  PassTiming& pass_timing);

  // The Renderer opened the frame's first list (frame slot `slot`).
  void BeginFrame(plume::RenderCommandList* list, uint32_t slot);
  // The frame's last list is about to be submitted, outside any render pass
  // (the geometry's promotions are recorded into it); null: the frame is
  // abandoned.
  void EndFrame(plume::RenderCommandList* list);
  // Before a batch flush ends `list`: closes the render pass.
  void BeforeFlush(plume::RenderCommandList* list);
  // A batch flush replaced the open list: everything is bound again.
  void OnListChanged(plume::RenderCommandList* list);

  // `list`: the frame's open list, null when the device is lost (the banks are
  // still applied: the deltas are incremental).
  void OnDraw(const uint8_t* payload, plume::RenderCommandList* list);
  void OnClear(const ClearRecord& record, plume::RenderCommandList* list);
  void OnResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  void OnTilingBegin(const TilingBeginRecord& record, plume::RenderCommandList* list);
  void OnTilingEnd(const TilingEndRecord& record, plume::RenderCommandList* list);

  // A guest present: the frame boundary of the statistics.
  void OnPresent();
  // Final report (at renderer shutdown).
  void Finish();

  uint64_t draws_since_flush() const { return draws_since_flush_; }

 private:
  enum class Skip : uint8_t {
    kDeviceLost,
    kVsUnreadable,
    kPsUnreadable,
    kDeclUnreadable,
    kIndexUnreadable,
    kInlineUnreadable,
    kInlineUnterminated,
    kPrimitive,
    kVsNotInPack,
    kPsNotInPack,
    kStageMismatch,
    kTargetFormat,
    kInputLayout,
    kEdramMode,
    kColorGap,
    kPipeline,
    kUpload,
    kGeometry,     // GeometryCache::Result, counted per result
    kTarget,       // an EDRAM target the host could not make
    kFramebuffer,  // the framebuffer of its targets could not be made
    kCount,
  };
  enum class NoEffect : uint8_t { kNoTarget, kCulled, kNoPrimitive, kEmptyScissor, kCount };

  // Everything a draw needs once its buffers and targets exist.
  struct PreparedDraw {
    XenosState state;
    const ShaderStore::Entry* vs = nullptr;
    const ShaderStore::Entry* ps = nullptr;
    const InputLayout* input_layout = nullptr;
    HostPrimitive primitive;
    DrawGeometry geometry;
    plume::RenderPipeline* pipeline = nullptr;
    PipelineKey key;
    DepthBias bias;
    ViewportInfo viewport;
    UploadSpan vs_constants;
    UploadSpan ps_constants;
    UploadSpan shared_constants;
  };

  // kCount = prepared; otherwise why not. `no_effect` is set instead for a
  // draw that changes nothing.
  Skip Prepare(const DrawView& view, const ConstantBanks::Banks& banks, PreparedDraw& d,
               bool* no_effect, NoEffect* no_effect_reason, std::string* detail);
  void FillTextureTables(const DrawView& view, uint32_t fetch_mask, SharedConstants& sc);
  // Records a prepared draw. kCount when recorded; kTarget/kFramebuffer when
  // not; *no_effect for an empty scissor.
  Skip Record(PreparedDraw& d, plume::RenderCommandList* list, bool* no_effect,
              std::string* detail);
  // Binds `framebuffer` (closing the open render pass first) unless bound.
  void BindFramebuffer(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer);
  void ResetListState();
  bool UploadConstants(uint32_t device, const ConstantBanks::Banks& banks,
                       const SharedConstants& shared, PreparedDraw& d);
  void CountSkip(Skip skip, const DrawRecord& r, const std::string& detail);
  void Report(const char* why);

  ShaderStore& shaders_;
  InputLayouts input_layouts_;
  ConstantBanks banks_;
  UploadRing* upload_ = nullptr;
  Pipelines* pipelines_ = nullptr;
  Samplers* samplers_ = nullptr;
  GeometryCache* geometry_ = nullptr;
  EdramTargets* edram_ = nullptr;
  TextureCache* textures_ = nullptr;
  ResolveCache* resolve_ = nullptr;
  const GuestLayout* layout_ = nullptr;
  const BindlessHeap* heap_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  // The target the last pass mark named (PassTiming): a clear or a resolve in
  // between leaves the same framebuffer bound, and the draws after it are a new
  // stretch of that pass.
  plume::RenderFramebuffer* marked_framebuffer_ = nullptr;
  SharedConstants base_shared_{};  // the texture tables on the null textures

  // The frame being recorded.
  plume::RenderCommandList* list_ = nullptr;
  uint32_t slot_ = 0;
  struct DeviceUpload {
    uint64_t vs_version = 0;  // 0 = nothing uploaded in this frame
    uint64_t ps_version = 0;
    UploadSpan vs;
    UploadSpan ps;
  };
  std::unordered_map<uint32_t, DeviceUpload> device_uploads_;
  SharedConstants last_shared_{};
  UploadSpan last_shared_span_;
  uint64_t draws_since_flush_ = 0;
  // What the open list has bound (ResetListState forgets it).
  struct ListState {
    bool layout_bound = false;
    plume::RenderFramebuffer* framebuffer = nullptr;
    const plume::RenderPipeline* pipeline = nullptr;
    plume::RenderBufferReference constants[3];
    bool constants_bound = false;
    bool depth_bias_bound = false;
    DepthBias depth_bias;
  };
  ListState bound_;

  // What FillTextureTables resolved for a fetch constant, by its raw dwords:
  // reused while the TextureCache's epoch is unchanged (textures.h) and, for a
  // binding good for one frame only, within that frame.
  struct FetchMemo {
    uint32_t dwords[6] = {};
    uint64_t epoch = 0;     // 0 = empty
    uint64_t frame = 0;     // the frame a per-frame binding was made in, 0 = any
    uint32_t texture = 0;   // table value (bindless slot | gamma bit)
    uint32_t sampler = 0;
    uint8_t dimension = 0;  // xenos::DataDimension
    bool is_texture = false;
    uint32_t page = 0;        // physical page whose product generation it saw
    uint32_t generation = 0;
    uint64_t used = 0;      // replacement: the least recently used way goes
  };
  // 4-way set associative: ~2000 distinct fetch constants a frame thrash a
  // direct-mapped table (measured: 748 resolutions/frame with 4096 slots).
  static constexpr size_t kFetchMemoWays = 4;
  static constexpr size_t kFetchMemoSets = 2048;
  std::array<FetchMemo, kFetchMemoWays * kFetchMemoSets> fetch_memo_{};
  uint64_t fetch_memo_clock_ = 0;
  uint64_t frame_serial_ = 0;
  // Writes one fetch constant's table entries from `memo`.
  static void ApplyFetch(const FetchMemo& memo, uint32_t slot, SharedConstants& sc);

  // Statistics: the current window and the whole run.
  struct Stats {
    uint64_t frames = 0;
    uint64_t draws = 0;
    uint64_t prepared = 0;
    std::array<uint64_t, size_t(Skip::kCount)> skips{};
    std::array<uint64_t, size_t(NoEffect::kCount)> no_effect{};
    std::array<uint64_t, kPrimitiveConversionCount> conversions{};
    std::array<uint64_t, size_t(GeometryCache::Result::kCount)> geometry_results{};
    uint64_t bank_checks = 0;
    uint64_t bank_mismatches = 0;
    // Draws with the pixel shader's ALPHA_TEST variant (coverage depends on
    // alpha), and of them those whose alpha test discards.
    uint64_t alpha_coverage_draws = 0;
    uint64_t alpha_test_draws = 0;
    uint64_t clip_disabled_draws = 0;
    // Alpha to mask (RB_COLORCONTROL) on a draw that writes colour: in all, on
    // single-sampled targets (a dithered per-pixel threshold), with RT0's
    // exponent bias (the shader tests the alpha before it, as the hardware).
    uint64_t alpha_to_mask_draws = 0;
    uint64_t alpha_to_mask_1x = 0;
    uint64_t alpha_to_mask_biased = 0;
    uint64_t depth_bias_draws = 0;
    uint64_t constant_uploads = 0;  // VS/PS banks
    uint64_t shared_uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t fetches = 0;  // fetch constants resolved to samplers
    uint64_t fetch_not_texture = 0;
    uint64_t fetch_memo_hits = 0;
    uint64_t recorded = 0;           // draws recorded into the list
    uint64_t clears = 0;
    uint64_t clears_color = 0;       // colour targets cleared
    uint64_t clears_depth = 0;       // depth/stencil targets cleared
    uint64_t clears_empty = 0;       // rectangle empty after the viewport/scissor
    uint64_t clears_failed = 0;      // unreadable record or target not made
    uint64_t resolves = 0;
    uint64_t tiling_begins = 0;
    uint64_t tiling_ends = 0;
    uint64_t prepare_ns = 0;
    uint64_t pipelines_created = 0;
    uint64_t pipelines_failed = 0;
    uint64_t pipeline_create_ns = 0;
  };
  void Add(Stats& into, const Stats& from) const;
  Stats window_;
  Stats total_;
  std::array<uint32_t, size_t(Skip::kCount)> skip_logged_{};
  uint32_t constants_issues_reported_ = 0;
  uint32_t mismatches_logged_ = 0;
};

}  // namespace mcla::gpu
