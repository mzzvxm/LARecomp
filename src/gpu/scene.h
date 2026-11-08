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
// One skip is not a failure: a draw whose pipeline a builder thread is still
// making (pipelines.h) is left out for now -- "pipeline pending", counted
// apart, not logged, not remembered by the pipeline memo -- when every target
// it renders into was drawn into in each of the last kPipelineSkipFrames guest
// frames (MaySkipForPipeline): the frame's draws are made again every frame, so
// the next frame's draw, with the pipeline by then, puts back what the skipped
// one left out. A draw into any other target waits for its pipeline instead.
// What the rule cannot know: whether the picture of a skipped draw lasts
// beyond its frame -- a resolve the game keeps for later or reads on the CPU
// (the lock of a resolve destination comes after the draws; nothing at draw
// time says one is coming) -- and whether the game draws into a target in
// every frame for the same purpose. It sees only that the target was drawn
// into in each of the last frames; a target made for one picture (car paint,
// vinyl) is not.
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
#include <unordered_set>
#include <vector>

#include <plume_render_interface.h>

#include "constants.h"
#include "draw_census.h"
#include "edram.h"
#include "frame_layers.h"
#include "geometry.h"
#include "hdr_tonemap.h"
#include "input_layout.h"
#include "msaa_level.h"
#include "pipelines.h"
#include "thread_load.h"
#include "records.h"
#include "shaders.h"
#include "topology.h"
#include "upload.h"
#include "viewport.h"

namespace mcla::gpu {

class AmbientOcclusion;
class BindlessHeap;
class GuestLayout;
class GuestOcclusion;
class PassTiming;
class PostAa;
class ResolveCache;
class Samplers;
class TemporalSystem;
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
                  PassTiming& pass_timing, PostAa& post);
  // The UI layer split (frame_layers.h); not owned. Null or inactive: no draw is touched.
  void SetFrameLayers(FrameLayers* layers) { layers_ = layers; }
  // HDR stage 3 (hdr_tonemap.h): the tone map and what the game draws after it are drawn again into
  // a float image; not owned. Null or inactive: no draw is touched.
  void SetHdrToneMap(HdrToneMap* tone_map) { hdr_tone_ = tone_map; }

  // The guest's occlusion queries: while one is open, the draws are counted
  // (occlusion.h). Null: none are.
  void SetOcclusion(GuestOcclusion* occlusion) { occlusion_ = occlusion; }
  // The temporal groundwork (temporal.h): called at the frame's start, for every prepared draw,
  // at the composite marker and at the present. Null: none.
  void SetTemporal(TemporalSystem* temporal) { temporal_ = temporal; }
  // The screen-space ambient occlusion (ao.h): told of the main view's draws, the camera, the
  // depth clears and the resolves, and run at the mid-scene depth resolve. Null: none.
  void SetAmbientOcclusion(AmbientOcclusion* ao) { ao_ = ao; }

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
  // A draw record queued behind the one being executed (stream sequence `sequence`): the textures
  // its shaders read, as its fetch constants name them, are offered to TextureCache::Prefetch
  // (decode ahead on the pool). Changes nothing the draw itself will do.
  void PrefetchTextures(uint64_t sequence, const uint8_t* payload);
  void OnClear(const ClearRecord& record, plume::RenderCommandList* list);
  void OnResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  void OnTilingBegin(const TilingBeginRecord& record, plume::RenderCommandList* list);
  void OnTilingEnd(const TilingEndRecord& record, plume::RenderCommandList* list);
  // The scene's image is composed: what the frame draws from here
  // on is screen-space. M6c.0 notes where (the census); `list` is the frame's
  // open list, for the passes that run at this point.
  void OnComposite(const CompositeRecord& record, plume::RenderCommandList* list);

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
    kPipelinePending,  // a builder thread is making it (pipelines.h): the draw waits
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
    Scale scale;  // the resolution scale its targets are (or are made) at
    // The UI layer split: what OnDraw does before recording the draw (the key's alpha blend was
    // already changed in Prepare when the draw joins the layer).
    FrameLayers::DrawAction ui_layer = FrameLayers::DrawAction::kNone;
    // HDR stage 3: the draw is drawn again into the float scene image once it is recorded
    // (RecordHdrDuplicate), with this pipeline; the tone map's second draw also has its own shared
    // constants (the 608 bytes and the 16 of the HDR variant).
    HdrToneMap::Action hdr_action = HdrToneMap::Action::kNone;
    plume::RenderPipeline* hdr_pipeline = nullptr;
    UploadSpan hdr_shared;
    // A car's shadow decal, while an upscaler runs: the draw is drawn again into the decals' mask image
    // once it is recorded (RecordShadowMask, vehicle_motion_mask.h), with this pipeline.
    plume::RenderPipeline* shadow_pipeline = nullptr;
    // Set by Record, for the second draw.
    plume::RenderRect scissor;
    bool scene = false;  // it draws into the scene's identity, which is made at a scale of its own
  };

  // kCount = prepared; otherwise why not. `no_effect` is set instead for a
  // draw that changes nothing.
  Skip Prepare(const DrawView& view, const ConstantBanks::Banks& banks, PreparedDraw& d,
               bool* no_effect, NoEffect* no_effect_reason, std::string* detail);
  // The frames in a row a target must have been drawn into for a draw into it
  // to wait for a pipeline (~0.5 s at 60 FPS: past a loading screen, a menu or
  // a one-shot render to texture).
  static constexpr uint32_t kPipelineSkipFrames = 30;
  // What Pipelines::Get asks on a miss (Deferral): may the draw be left out?
  // Only when every target the draw renders into was drawn into in each of the
  // last kPipelineSkipFrames frames.
  struct SkipAsk {
    const Scene* scene;
    const PreparedDraw* draw;
  };
  static bool MaySkipForPipeline(const void* context);
  // A draw left out for its pipeline still counts as a draw into its targets,
  // or a frame that skipped all of a target's draws would end its run.
  void NoteTargetsDrawn(const PreparedDraw& d);
  // `scale`: the draw's targets' resolution scale; a resolve product made at a
  // larger one is read through its reduced copy (TextureCache::Bind2D). `bias`: the extra mip
  // bias of the draw's samplers, in 1/32 levels (SceneBias for a draw into the scene's identity
  // while it is drawn smaller than the output, else 0).
  void FillTextureTables(const DrawView& view, uint32_t fetch_mask, Scale scale,
                         SharedConstants& sc, int32_t bias);
  // The mip bias of the scene's draws while the scene is drawn at a scale below the images of the
  // screen's, in 1/32 levels: log2(render scale / output scale), the levels the output size would
  // have picked, plus one more (FSR's guide). Once a frame.
  int32_t SceneBias();
  // Records a prepared draw. kCount when recorded; kTarget/kFramebuffer when
  // not; *no_effect for an empty scissor.
  Skip Record(PreparedDraw& d, const DrawRecord& r, plume::RenderCommandList* list,
              bool* no_effect, std::string* detail);
  // EDRAM ownership (M5.2d): how the draw treats what its
  // targets' tiles held, noted in EdramTargets' tile map, and reported when
  // it depends on another identity's bytes.
  // Returns the draw's coverage check (EdramTargets::kNoCheck when it has
  // none), for Record to wrap the draw with.
  uint32_t NoteEdramAccess(const PreparedDraw& d, const DrawRecord& r,
                           EdramTargets::Target* const* colors, EdramTargets::Target* depth,
                           const plume::RenderRect& scissor);
  uint32_t NoteEdramTargets(const PreparedDraw& d, const DrawRecord& r,
                            EdramTargets::Target* const* colors, EdramTargets::Target* depth,
                            const plume::RenderRect& scissor);
  // The last draw NoteEdramAccess noted: a draw equal to it in everything the
  // notes depend on, with the map's serial unchanged, notes nothing new.
  struct EdramNoteKey {
    const void* targets[5] = {};
    int32_t scissor[4] = {};
    const void* pipeline = nullptr;
    uint32_t primitive = 0;
    uint32_t count = 0;
    uint32_t kills = 0;
    uint32_t aa_mask = 0;
    uint32_t color_control = 0;
    uint64_t serial = 0;
    bool operator==(const EdramNoteKey& o) const;
  };
  EdramNoteKey last_edram_note_;
  // Binds `framebuffer` (closing the open render pass first) unless bound.
  void BindFramebuffer(plume::RenderCommandList* list, plume::RenderFramebuffer* framebuffer);
  // The UI layer split starts or ends with a draw (FrameLayers::Start/End): copies, clears and
  // draws into the UI target, outside a render pass; the draw binds everything again after.
  void UiLayersBeforeDraw(FrameLayers::DrawAction action, plume::RenderCommandList* list);
  // HDR stage 3: asks the HdrToneMap about the draw (the tone map, or a draw into its target before
  // the marker) and, for one it wants drawn again, looks the second pipeline up (a build still
  // under way leaves the second draw out: the frame has no float scene).
  void PrepareHdrDraw(PreparedDraw& d, const PipelineInputs& inputs,
                      const ConstantBanks::Banks& banks, uint32_t host_samples);
  // The draw `d` was just recorded: records it again into the float scene image (its own
  // framebuffer, pipeline, and shared constants for the tone map; everything else the draw's).
  void RecordHdrDuplicate(const PreparedDraw& d, plume::RenderCommandList* list);
  // A car's shadow decal (vehicle_motion_mask.h) in the main view, in a frame an upscaler works on and
  // before its anchor: looks up the pipeline that draws it again into the decals' mask (a build still
  // under way leaves the second draw out: the frame has no mask).
  void PrepareShadowMask(PreparedDraw& d, const PipelineInputs& inputs);
  // The decal `d` was just recorded: records it again into the mask image (its own framebuffer and
  // pipeline; everything else the draw's), the way RecordHdrDuplicate does.
  void RecordShadowMask(const PreparedDraw& d, plume::RenderCommandList* list);
  static bool AlwaysMaySkip(const void*) { return true; }
  void ResetListState();
  bool UploadConstants(uint32_t device, const ConstantBanks::Banks& banks,
                       const SharedConstants& shared, PreparedDraw& d);
  void CountSkip(Skip skip, const DrawRecord& r, const std::string& detail);
  // The ambient occlusion: a prepared draw, before it is recorded (only main view draws are told).
  void NoteAmbientOcclusion(const ConstantBanks::Banks& banks, const PreparedDraw& d);
  // A temporal upscaler works on this frame (it jitters it and will replace its colour): the
  // occlusion's noise then rotates from frame to frame, which the upscaler averages out.
  bool UpscalerRunning() const;
  // The temporal system: a prepared draw, before its constants are uploaded; may move it.
  void NoteTemporal(const DrawView& view, const ConstantBanks::Banks& banks,
                    const PreparedDraw& d, SharedConstants& shared);
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
  GuestOcclusion* occlusion_ = nullptr;
  TemporalSystem* temporal_ = nullptr;
  AmbientOcclusion* ao_ = nullptr;
  // The host MSAA level while the game runs (msaa_level.h): checked at every frame boundary.
  MsaaLevel msaa_;
  // The target the last pass mark named (PassTiming): a clear or a resolve in
  // between leaves the same framebuffer bound, and the draws after it are a new
  // stretch of that pass.
  plume::RenderFramebuffer* marked_framebuffer_ = nullptr;
  // Cleared surfaces whose header was made at another pitch/MSAA than the
  // device's RB_SURFACE_INFO, logged once each (the first 16).
  std::unordered_set<uint64_t> clear_other_pitch_logged_;
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
    // Rasterizer and input state as last set (reblue's
    // dirtyStates): a draw sets only what differs.
    bool viewport_bound = false;
    plume::RenderViewport viewport;
    bool scissor_bound = false;
    plume::RenderRect scissor;
    uint32_t vertex_slot_count = 0;  // 0: no vertex buffers set on the list
    std::array<plume::RenderVertexBufferView, DrawGeometry::kMaxSlots> vertex_views{};
    std::array<plume::RenderInputSlot, DrawGeometry::kMaxSlots> vertex_slots{};
    bool index_bound = false;
    plume::RenderIndexBufferView index_view;
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
    Scale scale = Scale::None();  // the reading draw's targets' scale (a product may bind reduced)
    int32_t bias = 0;       // the draw's extra mip bias (SceneBias), in 1/32 levels
    uint64_t used = 0;      // replacement: the least recently used way goes
  };
  // 4-way set associative: ~2000 distinct fetch constants a frame thrash a
  // direct-mapped table (measured: 748 resolutions/frame with 4096 slots).
  static constexpr size_t kFetchMemoWays = 4;
  static constexpr size_t kFetchMemoSets = 2048;
  std::array<FetchMemo, kFetchMemoWays * kFetchMemoSets> fetch_memo_{};
  uint64_t fetch_memo_clock_ = 0;
  // The entry each fetch slot used last (reblue's
  // SamplerSlotCache): a slot whose fetch constant repeats skips the hash and
  // the set search; the entry is checked as the search checks a way.
  std::array<FetchMemo*, guest::kFetchGroupCount> last_fetch_{};
  uint64_t frame_serial_ = 0;
  // SceneBias's value for the frame it was worked out in.
  uint64_t scene_bias_serial_ = 0;
  int32_t scene_bias_ = 0;
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
    // exponent bias (the shader tests the alpha before it, as the hardware),
    // and given other offsets than the game's (mcla_gpu_a2c_dither_all).
    uint64_t alpha_to_mask_draws = 0;
    uint64_t alpha_to_mask_1x = 0;
    uint64_t alpha_to_mask_biased = 0;
    uint64_t alpha_to_mask_replaced = 0;
    uint64_t depth_bias_draws = 0;
    uint64_t constant_uploads = 0;  // VS/PS banks
    uint64_t shared_uploads = 0;
    uint64_t upload_bytes = 0;
    uint64_t fetches = 0;  // fetch constants resolved to samplers
    uint64_t fetch_not_texture = 0;
    uint64_t fetch_memo_hits = 0;
    uint64_t census_scale_reads = 0;  // mcla_gpu_census_scale: products read across scales
    uint64_t recorded = 0;           // draws recorded into the list
    uint64_t clears = 0;
    uint64_t clears_color = 0;       // colour targets cleared
    uint64_t clears_depth = 0;       // depth/stencil targets cleared
    uint64_t clears_empty = 0;       // rectangle empty after the viewport/scissor
    uint64_t clears_failed = 0;      // unreadable record or target not made
    uint64_t clears_other_pitch = 0; // targets whose surface header was made at another
                                     // pitch/MSAA than the device's RB_SURFACE_INFO
    uint64_t resolves = 0;
    uint64_t tiling_begins = 0;
    uint64_t tiling_ends = 0;
    uint64_t prepare_cycles = 0;  // TSC cycles (cycles.h)
    uint64_t pipelines_created = 0;
    uint64_t pipelines_failed = 0;
    uint64_t pipeline_create_ns = 0;
  };
  void Add(Stats& into, const Stats& from) const;
  Stats window_;
  Stats total_;
  DrawCensus draw_census_;      // diagnostic, mcla_gpu_census_ps
  TargetCensus target_census_;  // diagnostic, mcla_gpu_census_targets
  CompositeCensus composite_census_;  // the scene's composition, always on
  // The colour target 0 of the frame's last recorded draw (null: none yet) and
  // the guest rectangle from the origin its draws covered since it became the
  // last: the target and region the scene is composed into when the composite
  // record comes.
  EdramTargets::Target* last_color0_ = nullptr;
  uint32_t last_color0_width_ = 0;
  uint32_t last_color0_height_ = 0;
  PostAa* post_ = nullptr;  // the scene's anti-aliasing at the composite record
  FrameLayers* layers_ = nullptr;  // the UI layer split (frame_layers.h), from the composite record
  HdrToneMap* hdr_tone_ = nullptr;  // the float scene of HDR output (hdr_tonemap.h)

  // The last draw's lookups: consecutive draws mostly name
  // the same targets, shaders and pipeline. Each memo holds only while what it
  // answered from is unchanged.
  //  - targets and their framebuffer: the same EDRAM keys, a scissor that fits
  //    the targets' heights (no growth), and EdramTargets::change_serial;
  //  - pipeline: the same PipelineKey (it names the shaders and the layout);
  //  - shaders: the same identity (the pack's entries live for the session).
  struct TargetsMemo {
    uint64_t serial = 0;  // EdramTargets::change_serial when made; 0 = empty
    uint32_t color_count = 0;
    EdramKey color_keys[kColorTargetCount];
    bool has_depth = false;
    EdramKey depth_key;
    uint32_t height = 0;  // the smallest of the targets' heights
    EdramTargets::Target* colors[kColorTargetCount] = {};
    EdramTargets::Target* depth = nullptr;
    plume::RenderFramebuffer* framebuffer = nullptr;
  };
  TargetsMemo targets_memo_;
  // The last draw's Xenos state and what Prepare made from it (
  // M6a.7). Three draws in four repeat the previous draw's window R and
  // surfaces byte for byte -- the bytes stand in for the dirty states reblue
  // keeps at its Set* hooks -- and then the decode, the target formats, the
  // pipeline key (for the same shaders, layout and topology), the viewport
  // and the state part of the shared constants are the previous draw's.
  struct StateMemo {
    bool valid = false;
    uint32_t device = 0;
    uint8_t registers[guest::kRegisterWindowBytes] = {};
    uint32_t surfaces[guest::kSurfaceCount] = {};
    XenosState state;
    bool formats_valid = false;
    plume::RenderFormat color_formats[kColorTargetCount] = {};
    plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN;
    bool key_valid = false;
    const ShaderStore::Entry* key_vs = nullptr;
    const ShaderStore::Entry* key_ps = nullptr;
    const InputLayout* key_layout = nullptr;
    plume::RenderPrimitiveTopology key_topology = {};
    bool key_polygonal = false;
    PipelineKey key;
    DepthBias bias;
    // The draw's targets' scale (DrawScale), good while the targets are those
    // EdramTargets had at change serial `draw_scale_serial` and the key names
    // the same first target.
    bool draw_scale_valid = false;
    uint64_t draw_scale_serial = 0;
    uint32_t draw_scale_colors = 0;
    uint32_t draw_scale_depth_format = 0;
    Scale draw_scale;
    uint32_t draw_samples = 1;  // the targets' samples on the host (M6d.2)
    uint32_t draw_msaa_guest = 0;  // the guest's, when they take the host's MSAA level (else 0)
    bool draw_scene = false;    // the first target is the scene's, drawn at a scale of its own
    bool constants_valid = false;
    bool ps_writes_depth = false;
    Scale scale;  // the targets' resolution scale the viewport was made for (24.45)
    uint32_t samples = 1;  // and their host samples, for g_AlphaToMask
    ViewportInfo viewport;
    SharedConstants shared;  // before the texture tables
  };
  StateMemo state_memo_;
  PipelineKey pipeline_memo_key_{};
  plume::RenderPipeline* pipeline_memo_ = nullptr;
  uint64_t vs_memo_identity_ = 0;
  const ShaderStore::Entry* vs_memo_ = nullptr;
  uint64_t ps_memo_identity_ = 0;
  const ShaderStore::Entry* ps_memo_ = nullptr;
  // PrefetchTextures' own last shaders (the draws behind the executing one are not the ones the
  // memos above follow).
  uint64_t prefetch_vs_identity_ = 0;
  const ShaderStore::Entry* prefetch_vs_ = nullptr;
  uint64_t prefetch_ps_identity_ = 0;
  const ShaderStore::Entry* prefetch_ps_ = nullptr;

  // mcla_gpu_scene_profile (M6a): the TSC cycles each stage of a
  // draw took, summed over the window (cycles.h turns them into time). Off
  // (read at start): each Lap is a branch.
  enum class Stage : uint8_t {
    kBanks,      // the constant banks (ConstantBanks::Apply)
    kDecode,     // the Xenos state, primitive and target formats
    kShaders,    // the pack's shaders and the input layout
    kGeometry,   // GeometryCache::Prepare
    kPipeline,   // the pipeline key and its lookup
    kConstants,  // viewport and SharedConstants
    kTextures,   // the texture and sampler tables
    kUpload,     // the constants into the upload ring
    kTargets,    // EDRAM targets, their notes, barriers, framebuffer
    kBindPipeline,   // pipeline layout and pipeline
    kBindConstants,  // root descriptors (D3D12) or push constants (Vulkan)
    kBindState,      // viewport, scissor, depth bias
    kBindStreams,    // vertex and index buffers
    kDraw,           // the draw call (and a coverage check around it)
    kCount,
  };
  void LapStart();
  void Lap(Stage stage);
  void ReportProfile(const char* why, double frames, const ThreadLoad::Sample& load);
  bool profile_ = false;
  bool scale_depth_bias_ = true;  // mcla_gpu_scale_depth_bias, read each frame
  bool census_scale_ = false;     // mcla_gpu_census_scale, read at start
  std::unordered_set<uint64_t> census_scale_logged_;  // NoteScaleRead's cases (the first 64)
  uint64_t lap_ = 0;
  std::array<uint64_t, size_t(Stage::kCount)> stage_cycles_{};
  // How often each memo of the last draw answered (M6a.3), for the profile.
  enum class Memo : uint8_t { kTargets, kPipeline, kVertexShader, kPixelShader, kState, kCount };
  std::array<uint64_t, size_t(Memo::kCount)> memo_hits_{};
  std::array<uint64_t, size_t(Memo::kCount)> memo_tries_{};
  // mcla_gpu_ab_interleave (ab.h): each variant's prepare ns/draw summed
  // over its interval windows, the first window (warm-up) left out.
  // The intervals between the guest's presents as the render thread runs them
  // (TSC cycles), per window: the frame pacing a hitch shows in (
  // V3), whoever caused it -- the guest, a pipeline made on the spot, a wait.
  std::vector<uint64_t> present_intervals_;
  uint64_t last_present_ = 0;
  double ab_sum_[2] = {};
  double ab_cpu_sum_[2] = {};  // the render thread's CPU ms/frame
  uint32_t ab_count_[2] = {};
  uint32_t ab_windows_ = 0;
  std::array<uint32_t, size_t(Skip::kCount)> skip_logged_{};
  uint32_t constants_issues_reported_ = 0;
  uint32_t mismatches_logged_ = 0;
  // The game's alpha-to-mask offsets already reported as replaced, one bit
  // per value.
  std::array<uint64_t, 8> alpha_to_mask_replaced_logged_{};  // [solid taken: +4]
};

}  // namespace mcla::gpu
