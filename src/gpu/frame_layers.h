#pragma once
// ===========================================================================
// MCLA GPU renderer -- the frame in two layers: the scene and the UI
// ===========================================================================
// HDR stage 2 (feature-parity/HDR-PLAN.md) and the HUD-less colour of the upscalers and frame
// generation (feature-parity/F4-DESIGN.md section 5) are the same split, built here once. For a
// guest frame it keeps two 8-bit images in the game's code values, at the size of the UI target:
//   hudless  the scene as the game's post chain finished it (the underlay),
//   ui       what the UI phase drew, premultiplied, with its coverage in alpha,
// while the guest's own frame -- the front buffer and everything it reads back -- stays what it
// always was. Nothing here runs unless something asked for it (Request): with HDR output and
// frame generation both off, no draw changes, nothing is allocated and nothing is copied.
//
// Where the UI starts: the composite record (Scene::OnComposite, after PostAa::Apply). The end of
// the game's post chain left the finished scene in `last_color0_`, the UI target (colour
// k_8_8_8_8, pitch 1280, 1x, times the render scale, in every scene measured); every draw into it
// after the marker is UI. The phases of a frame:
//   kUi       the marker came and the target can be separated. Draws into it with blending off
//             go on as they are -- they are part of the scene so far. Nothing is copied yet.
//   kStarted  the first draw into the target with a blend of the "over" kind (colour ADD,
//             source ONE or SRC_ALPHA, destination INV_SRC_ALPHA, all four channels): just
//             before it the target's region is copied to the underlay and cleared to zero. From
//             then on each "over" draw gets the alpha blend ONE / INV_SRC_ALPHA (colour as the
//             game has it), so on a cleared target colour is premultiplied and alpha the
//             accumulated coverage, and "over" composes in any order: UI over the underlay is
//             the game's sequence of draws.
//   kDone     the target was resolved (the first resolve of the target while kStarted): its
//             region is copied to the ui image, and the target is put back for the guest --
//             the underlay is drawn into it with the blend (INV_DEST_ALPHA, ONE) on colour and
//             alpha, which gives UI + underlay x (1 - coverage) -- before the resolve itself
//             runs. The guest's resolves and readbacks therefore see its frame, up to the
//             rounding of the 8-bit blends (a pixel the UI covers partly may differ by a level
//             or two; the pixels it does not touch are bit for bit) and its alpha channel, which
//             is now the UI's coverage joined with the scene's: nothing reads that alpha.
//   kFailed   the frame's UI cannot be separated (below). If the target was cleared it is put
//             back at once, the draw that cannot be separated keeps the game's blend, and the
//             frame has no layers: the present shows the front buffer, which is the HDR output
//             of stage 1 (the same picture, the scene and the UI together).
// Why a frame has no layers (Cause, counted per frame, logged once per cause; a blend is logged
// once per distinct kind with the pixel shader that drew it, which is the census the stage asks
// for): the marker's target is not a single-sampled R8G8B8A8 target; a draw into it with a blend
// that is not "over" (additive glows, multiplies, masked writes) or with blending off after the
// layer started (its alpha is whatever the pixel shader writes, not coverage); a clear of the
// target after the layer started; a second marker; a frame that ended with the layer started; an
// image or a pipeline that could not be made; a resolve of the target that is not a copy, has no
// destination or moves the pixels; and at the present, a front buffer that is not the picture the
// target was resolved into (the first resolve of the target was a thumbnail or the pause blur, not
// the screen) or reaches past what the layers cover.
// A frame with no marker (loading, movies, menus with no 3D) has none either.
//
// The pause menu, the race pause and the map. The game draws the world and the post chain as in
// play, so the marker is on the tone map's 1x target as always, but it resolves that target to a
// texture before any UI goes into it and draws the UI phase in another target: colour k_8_8_8_8 at
// the same pitch, multisampled by the guest (4 samples, in three tiles), whose first draw copies the
// tone-mapped texture in without blending, and which the game then resolves to the front buffer.
//   kPause    the marker's target was resolved with no UI in it. The frame waits for that UI target:
//             the first draw into a colour target of its kind (R8G8B8A8, the marker's pitch,
//             multisampled by the guest) adopts it as the frame's UI target, and the phases above
//             follow on it unchanged: kUi over the copy (blending off goes on as it is), kStarted at
//             the first "over" draw, kDone at its first resolve (the first of the three tiles: the
//             host draws the section once at full size, so the whole target is there).
// Not every frame in kPause is a pause view: a title or loading frame whose marker target is
// resolved with no UI drawn over it also waits, finds no second target and ends in kPause with no
// layers, which is right (the present shows its front buffer). Only a frame that adopted a target
// is a pause-view frame; the summary counts the two apart ("armed", "found it"), and a run of
// consecutive pause-view frames is reported as a spell (one line each, the first few).
// In the logged runs (D3D12 at MSAA 2x, Vulkan with one host sample) a steady pause frame after the
// marker is the marker target's resolve, four draws into the pause UI target (the first the opaque
// copy of the tone-mapped picture) and its three tile resolves; in the Vulkan run 775 of 781 adopted
// frames were captured. The one-minute summary of the D3D12 run (149 frames waited, 17 found it) was
// taken before its pause menu opened: it counted the title and loading frames and the pause view the
// game shows when it starts.
//
// The adopted target has the host's MSAA level's samples (edram.h: 1 with MSAA off or an upscaler,
// else 2, 4 or 8). At 1 it is copied as in play. Above 1 each image is the mean of the target's
// samples (hlsl LAYERS_RESOLVE: for the premultiplied UI layer that is the pixel's colour and
// coverage), and the put-back draws the underlay into every sample with a pipeline of the target's
// sample count, so each sample keeps its own coverage and the guest's frame stays what it was.
// The frame's float scene (below) was made at the marker and is the picture the game copied into
// the UI target, so it goes under the UI there as well; a UI target with no opaque draw before its
// UI is not that picture and keeps the 8-bit underlay. The menus' UI phase may repeat the marker
// (once per tile in the fork's notes): a marker in a frame whose pause view is armed or running is
// that repeat and is ignored (counted), never a second marker.
//
// The present reads the layers (hdr_scene.h): it composes them into a float image at the front
// buffer's size and shows that where it shows the front buffer, with the gamma table extended
// above 1. An idle present repeats the last guest frame's layers.
//
// The first host column and row at a render scale (Frame::gap). The layers are images of the
// guest's target, whose first column and row at a scale no full-screen draw writes (the D3D pixel
// centre puts the geometry half a guest pixel in); the guest never sees them, its resolves read
// them from the next column and row. The layers cannot be trusted there, and a float scene drawn
// again into the same image every frame is not rewritten there either, so every consumer reads the
// pixel at `gap` for those before it. Without that the edge of the screen shows what those pixels
// hold: black in a clean image (the 3x test pictures of the HDR runs have a black first column and
// row), and after a pause menu or the garage a one pixel line, whitish and reddish, along the left
// and the top of the picture that stayed until the images were made again (owner's report,
// 2026-11-07, 4K at 3x, DLSS and HDR on).
//
// HDR stage 3 (hdr_tonemap.h) makes a float image of the scene as well, with the tone map's light
// above white and the glows drawn after it, up to the marker. It is not made here: the frame's
// capture asks the SceneImageSource (the HdrToneMap) for it, and a frame that has one carries it
// as Frame::scene, which the present composes under the UI in place of the 8-bit underlay.
//
// Cost (RGBA8, the target's region): the underlay and the ui images 3.7 MB each at 1x and 33 MB
// at 3x, made on the first separated frame and released after kReleaseAfterFrames guest frames
// without layers; per separated frame two copies of the region, one clear and one blended
// full-region draw (the put-back) -- about 0.4 ms at 3x on a desktop GPU. A multisampled pause
// view replaces each copy by a pass over the samples and draws the put-back at the target's
// sample count.
// CHECK: that cost at 3x with 8 samples (pass timing labels "frame layers ...").
//
// Live (mcla_gpu_hdr): Request may also come from the render thread, at a frame boundary (no frame
// open, so no phase is under way). Taking the last requester away drops the frame kept for the
// present, releases the images and makes the hooks no-ops again; wanting them again makes the
// pipeline (kept once made) and the images come back with the first separable frame.
//
// Threading: the lifecycle thread for Initialize/Request before the render thread starts; the
// render thread for everything else.
// ===========================================================================

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <plume_render_interface.h>

#include "edram.h"
#include "host_passes.h"
#include "pipelines.h"
#include "records.h"
#include "textures.h"

namespace mcla::gpu {

class PassTiming;

// The float image of the scene (HDR stage 3, hdr_tonemap.h) a frame's capture may carry.
class SceneImageSource {
 public:
  virtual ~SceneImageSource() = default;
  // The finished float scene when the frame has one at exactly `width` x `height` host pixels (the
  // layers' region), else null. `epoch` is what SceneEpoch said then.
  virtual const HostImage* FinishedScene(uint32_t width, uint32_t height, uint32_t* epoch) = 0;
  // Changes whenever the image's content or identity changes: a frame whose epoch is not the
  // current one no longer has its scene.
  virtual uint32_t SceneEpoch() const = 0;
};

class FrameLayers {
 public:
  // Who needs the layers. They exist while any requester wants them.
  enum Requester : uint32_t {
    kRequesterHdr = 1u << 0,              // HDR10 output (stage 2: the UI stays at paper white)
    kRequesterFrameGeneration = 1u << 1,  // the HUD-less colour and the UI layer (F4)
    // DLSS frame generation (dlss_frame_generation.h): its own bit, since FSR's one is set and
    // cleared by FSR's every frame.
    kRequesterDlssFrameGeneration = 1u << 2,
  };

  // What the Scene does for a draw it asked about.
  enum class DrawAction : uint8_t {
    kNone,
    kStart,  // the layer starts with this draw: Start() before it is recorded
    kEnd,    // the draw cannot be separated: End() before it is recorded
  };

  enum class Cause : uint8_t {
    kFormat,      // the UI target is not a single-sampled R8G8B8A8 colour target
    kBlend,       // a draw into it with a blend that is not "over"
    kOpaque,      // a draw with blending off after the layer started
    kClear,       // a clear of the target after the layer started
    kMarker,      // a second marker with the layer started
    kUnresolved,  // the frame ended with the layer started
    kImage,       // an image could not be made
    kResolve,     // the target's resolve was not a copy, had no destination or moved the pixels
    kCopy,        // a copy, a clear or the put-back could not be recorded
    kPauseTarget,  // the pause view's UI target cannot be separated (its format or host samples)
    kPipeline,    // a pipeline for a multisampled target could not be made
    kCount,
  };

  // The layers of the guest frame last presented.
  struct Frame {
    bool valid = false;
    // Host pixels: the region of the UI target both images cover, from its origin. The resolve put
    // the target's pixels in the front buffer where they are, so the front buffer is the layers'
    // top left corner (the layers may cover more than it does).
    uint32_t width = 0;
    uint32_t height = 0;
    const HostImage* hudless = nullptr;  // R8G8B8A8_UNORM, the scene in the game's code values
    const HostImage* ui = nullptr;       // R8G8B8A8_UNORM, premultiplied, coverage in alpha
    // R16G16B16A16_FLOAT: the same scene with the light the tone map clipped (HDR stage 3), or null.
    // Valid while the source's epoch is scene_epoch.
    const HostImage* scene = nullptr;
    uint32_t scene_epoch = 0;
    // Counts up with each capture: a consumer that kept something made from this frame knows it
    // is still this frame's.
    uint64_t generation = 0;
    // Host pixels at the left and at the top of the three images (the hudless, the ui and the float
    // scene) that hold no picture: at a render scale above 1 a full-screen draw with the D3D pixel
    // centre starts half a guest pixel in and leaves the first host column and row uncovered
    // (Scale::HalfPixelGap of the UI target's scale: 1 at 2x and 3x). They hold whatever the image
    // happened to keep there: an earlier view's picture, or a light drawn again into the float scene
    // every frame, which adds up where the tone map's own draw never rewrites it. The guest's own
    // resolve reads them from the first covered column and row (resolve.cpp, push.fill); a consumer
    // of the layers does the same, the texel at or before `gap` read at `gap` (hlsl
    // LAYERS_COMPOSE, fg_prepare, fg_compose).
    uint32_t gap = 0;
    // The texture the UI target was resolved into. IsPictureOf tells whether a front buffer is it.
    TextureKey destination;

    bool IsPictureOf(const TextureKey& front) const {
      return valid && front.base_page == destination.base_page &&
             front.width == destination.width && front.height == destination.height;
    }
  };

  // Guest frames without layers after which the images are released (20 s at 60 fps).
  static constexpr uint32_t kReleaseAfterFrames = 1200;

  FrameLayers() = default;
  ~FrameLayers();
  FrameLayers(const FrameLayers&) = delete;
  FrameLayers& operator=(const FrameLayers&) = delete;

  // `passes` makes the pipelines and the images; `edram` owns the UI target. After both.
  // False when a shader or a pipeline could not be made: the layers then stay off (logged).
  bool Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing);
  // GPU idle, before HostPasses shuts down.
  void Shutdown();

  // The float scene's source (not owned), asked at each capture. Null: frames have none.
  void SetSceneSource(SceneImageSource* source) { scene_source_ = source; }
  // `requester` (Requester bits) wants the layers, or no longer does. A requester set before
  // Initialize counts from it. After it, at a frame boundary on the render thread: the last one
  // going releases the images at once (the caller waits the GPU idle afterwards to free them), a
  // new one makes the pipeline if there is none yet; active() says whether that worked.
  void Request(uint32_t requester, bool wanted);
  // The layers are wanted and can be made: what the Scene's hooks check.
  bool active() const { return active_; }

  // ---- The Scene's hooks (render thread) ----------------------------------------------------
  // A guest frame's first list was opened.
  void BeginFrame();
  // A UI phase is under way: the Scene asks ClassifyDraw for each draw and OnResolve for each
  // resolve. One load and a compare when it is not.
  bool ui_phase() const {
    return phase_ == Phase::kUi || phase_ == Phase::kStarted || phase_ == Phase::kPause;
  }
  // Right after OnComposite: the region the layers will cover in host pixels. False when the marker
  // found the frame cannot be separated, and for a marker a pause view's frame repeated.
  bool Region(EdramKey* target, uint32_t* width, uint32_t* height) const;
  // The composite record, outside a render pass, after the scene's anti-aliasing: `target` is the
  // frame's last colour target, `width` x `height` the guest rectangle its draws covered. A repeat
  // of the marker in a frame whose pause view is armed or running changes nothing.
  void OnComposite(plume::RenderCommandList* list, EdramTargets::Target* target, uint32_t width,
                   uint32_t height);
  // A draw about to be prepared (its key built, its pipeline not looked up): into the UI target
  // (`color0` its identity, `host_samples` its pipeline's), it changes `key.blend[0]`'s alpha part
  // when the draw is separable and the layer runs; kStart and kEnd ask the Scene to call Start or
  // End before the draw is recorded. In a frame waiting for the pause view's UI target (kPause) the
  // first draw into a target of its kind adopts it, once the target exists.
  DrawAction ClassifyDraw(const EdramKey& color0, uint32_t host_samples, uint64_t ps_identity,
                          PipelineKey& key);
  // kStart: outside a render pass, copies the target's region to the underlay and clears it. A
  // failure ends the frame's layers (the draw is then recorded with the alpha blend it was
  // given: only the alpha channel of a frame without layers differs).
  void Start(plume::RenderCommandList* list);
  // kEnd: outside a render pass, puts the target back and ends the frame's layers. The draw is
  // then recorded with the blend it has.
  void End(plume::RenderCommandList* list);
  // A resolve record, outside a render pass, before the resolve is executed: the first copy
  // resolve of the UI target ends the phase (captures the layers when the layer started).
  void OnResolve(const ResolveRecord& record, plume::RenderCommandList* list);
  // A clear of the colour target `key`, before it is recorded. True when it recorded anything (the
  // Scene forgets its list bindings).
  bool OnColorClear(plume::RenderCommandList* list, const EdramKey& key);
  // The frame's last list is about to be submitted, outside a render pass: a frame that ended with
  // the layer started puts its target back and has no layers. Null: the frame is abandoned (the
  // renderer is shutting down, or the device was lost): nothing is recorded and nothing is counted,
  // it never was a frame anyone saw.
  void EndFrame(plume::RenderCommandList* list);
  // The guest's present record: the layers of the frame are the ones a present may show.
  void OnGuestPresent();

  // ---- Consumers (HDR compose, later upscalers and frame generation) -------------------------
  // The layers of the guest frame last presented; valid is false when it had none.
  Frame presentable() const;
  // The float scene of `frame` (HDR stage 3) while the source still has it (its epoch is the frame's)
  // and it covers `width` x `height` host pixels, else null: the frame's scene is then its 8-bit
  // hudless image. What HdrScene::Compose and frame generation (frame_generation.h) read as the scene.
  const HostImage* FloatSceneOf(const Frame& frame, uint32_t width, uint32_t height) const;
  // Memory held by the images.
  uint64_t HeldBytes() const;
  // Counts a present that could not use the layers it had (the front buffer was not their
  // picture, or reached past them): for the summary.
  void NoteUnusable(bool wrong_picture);

  std::string TakeSummary();

 private:
  // kPause: the marker's target was resolved with no UI in it, and the frame waits for the pause
  // view's UI target (the header comment).
  enum class Phase : uint8_t { kIdle, kUi, kStarted, kDone, kFailed, kPause };

  bool MakePipelines();
  // Puts the target back (when the layer started) and ends the frame's layers with `cause`.
  void PutBack(plume::RenderCommandList* list, Cause cause);
  // The underlay and ui images at the region's size; false (logged) when one could not be made.
  bool EnsureImages();
  void ReleaseImages();
  // The target's region to `image`, which ends in SHADER_READ; the target is left in COPY_SOURCE
  // (in SHADER_READ when it is multisampled).
  bool CopyFromTarget(plume::RenderCommandList* list, HostImage& image);
  // A multisampled target's region to `image` as the mean of its samples (hlsl LAYERS_RESOLVE).
  bool ResolveTargetInto(plume::RenderCommandList* list, HostImage& image);
  // The underlay under the UI into the target: UI + underlay x (1 - coverage), at every sample.
  bool DrawRestore(plume::RenderCommandList* list);
  // The put-back pipeline for a target of `samples` samples (1, 2, 4 or 8), or null.
  plume::RenderPipeline* RestorePipelineFor(uint32_t samples) const;
  // The put-back pipeline a multisampled target needs, made when a pause view of that level first
  // asks (the averaging pass is made with the rest). False when it could not be made.
  bool EnsureSamplePipelines(uint32_t samples);
  // In kPause: whether `key` is the kind of target the pause view draws its UI in, and the step that
  // makes it the frame's UI target (false: not yet, or not this one; the draw goes on as it is).
  bool IsPauseUiKey(const EdramKey& key) const;
  // `note` (AdoptNote, may be null) says what became of it.
  bool AdoptPauseTarget(const EdramKey& key, uint32_t host_samples, uint8_t* note);

  // What a frame did after the marker, kept (a few dozen small records, nothing formatted) so that
  // the frames worth explaining can be logged once: the first pause-view frame, and a pause-view
  // frame that got no layers or a frame that drew a second target of the pause kind and never
  // adopted it. Only what is not the plain UI-phase draw into the marker's target is recorded (those
  // are the 40 draws of a play frame): draws into any other target, every draw of a frame in kPause or
  // of a pause view, every resolve and every clear.
  struct TraceEvent {
    enum class Kind : uint8_t { kDraw, kResolve, kClear };
    Kind kind = Kind::kDraw;
    Phase phase = Phase::kIdle;  // the layer's phase when it happened
    uint8_t blend_kind = 0;      // draws: BlendKind
    uint8_t host_samples = 0;    // draws: the pipeline's
    uint8_t adopt = 0;           // draws in kPause: AdoptNote
    bool known = false;          // resolves: the source key could be read
    bool is_ui_target = false;   // resolves and clears: the layers' UI target
    bool copy = false;           // resolves: a copy (not a clear-only resolve)
    bool dest = false;           // resolves: the destination texture could be read
    bool unmoved = false;        // resolves: the pixels stay where they are
    uint32_t resolve_flags = 0;
    EdramKey key;
    uint64_t ps = 0;
  };
  static constexpr uint32_t kTraceEvents = 32;
  static constexpr uint32_t kTraceLines = 16;   // in all
  static constexpr uint32_t kTracePerSpell = 2;  // of the no-layers kind
  static constexpr uint32_t kTraceMissed = 3;    // of the never-adopted kind
  enum AdoptNote : uint8_t {
    kAdoptNone,          // the draw was not asked
    kAdoptNotPauseKind,  // not a target of the pause kind
    kAdoptNoTarget,      // no host target of that identity (yet)
    kAdoptNoTexture,     // the target exists without its texture (released)
    kAdoptDone,          // became the frame's UI target
    kAdoptRefused,       // its format, samples or size cannot be separated
  };
  void Trace(const TraceEvent& event);
  void ClearTrace();
  // At the guest's present: logs the frame's records when they are worth it (see above).
  void LogTraceIfWorthIt(const char* outcome);
  // `cause`'s first occurrence is logged; a frame counts it once.
  void Fail(Cause cause, const std::string& detail);
  // A blend that cannot be separated, logged once per kind.
  void NoteBlend(const plume::RenderBlendDesc& blend, uint64_t ps_identity, bool after_start);
  // The EDRAM identity of a resolve's colour 0 source; false for another source.
  static bool ResolveSourceKey(const ResolveRecord& record, EdramKey* key);
  // The resolve's destination texture; false when it has none or it cannot be read.
  static bool ResolveDestination(const ResolveRecord& record, TextureKey* key);
  // The resolve puts the target's pixels where they are in the destination (no offset).
  static bool ResolveIsUnmoved(const ResolveRecord& record);

  struct Push {  // hlsl/frame_layers_ps.hlsl (the put-back uses the first only; HdrScene::Push is the compose's)
    uint32_t underlay;
    uint32_t ui;
    uint32_t view;
    uint32_t reserved;
    uint32_t gap;
  };
  static_assert(sizeof(Push) <= HostPasses::kPushConstantBytes);
  struct ResolvePush {  // hlsl/frame_layers_ps.hlsl, LAYERS_RESOLVE
    uint32_t source;
    uint32_t samples;
    uint32_t reserved[2];
  };
  static_assert(sizeof(ResolvePush) <= HostPasses::kPushConstantBytes);

  HostPasses* passes_ = nullptr;
  EdramTargets* edram_ = nullptr;
  PassTiming* pass_timing_ = nullptr;
  SceneImageSource* scene_source_ = nullptr;
  uint32_t requesters_ = 0;
  bool initialized_ = false;
  bool active_ = false;
  std::unique_ptr<plume::RenderShader> restore_shader_;
  std::unique_ptr<plume::RenderPipeline> restore_pipeline_;
  // The put-back at 2, 4 and 8 samples (made when a pause view of that level first needs it) and the
  // pass that averages a multisampled target's samples (made with the rest; without it the pause
  // view has no layers).
  std::array<std::unique_ptr<plume::RenderPipeline>, 3> restore_ms_;
  std::unique_ptr<plume::RenderShader> resolve_shader_;
  std::unique_ptr<plume::RenderPipeline> resolve_pipeline_;
  HostImage underlay_;
  HostImage ui_;

  // The frame in progress.
  Phase phase_ = Phase::kIdle;
  EdramTargets::Target* target_ = nullptr;  // the UI target
  EdramKey ui_key_;
  uint32_t guest_width_ = 0;   // the region, guest pixels
  uint32_t guest_height_ = 0;
  uint32_t host_width_ = 0;    // and in the target's host pixels
  uint32_t host_height_ = 0;
  uint32_t target_samples_ = 1;  // the UI target's host samples (1 for the marker's target)
  // The pause view, from the frame's kPause to its end. `marker_*` is what the marker's target was
  // (the pause target takes its pitch, format and guest region); `pause_opaque_draws_` counts the
  // draws with blending off into the pause target before its layer started (the copy of the
  // tone-mapped picture).
  bool pause_view_ = false;
  EdramKey marker_key_;
  uint32_t marker_guest_width_ = 0;
  uint32_t marker_guest_height_ = 0;
  uint32_t pause_opaque_draws_ = 0;
  Cause pending_cause_ = Cause::kBlend;  // what ClassifyDraw's kEnd was for
  Frame captured_;                       // the frame's, once its target was resolved
  Frame presented_;                      // the last guest present's
  uint64_t generation_ = 0;
  uint32_t idle_frames_ = 0;             // guest presents in a row without layers

  std::array<bool, size_t(Cause::kCount)> cause_logged_{};
  std::unordered_set<uint64_t> blends_logged_;
  bool pause_logged_ = false;
  bool pause_picture_logged_ = false;

  // The frame's records after the marker (Trace) and what the frame did, for the one-time reports.
  std::array<TraceEvent, kTraceEvents> trace_;
  uint32_t trace_count_ = 0;
  uint32_t trace_dropped_ = 0;
  bool pause_kind_drawn_ = false;  // a draw into a target of the pause kind (adopted or not)
  bool frame_adopted_ = false;     // this frame adopted a pause view's UI target
  // Frame traces logged so far, by why: [0] the first pause-view frame, [1] a pause-view frame with
  // no layers (kTracePerSpell per spell), [2] a frame that drew a second target of the pause kind
  // and never adopted it.
  std::array<uint32_t, 3> traced_{};
  // The run of consecutive pause-view frames (frames that adopted a target) in progress: logged as
  // one line when it ends, the first kSpellLines of them.
  struct Spell {
    uint32_t frames = 0;
    uint32_t layers = 0;  // presented with layers
    uint32_t no_ui = 0;   // the target was resolved with no UI to separate (nothing drawn over the picture)
    uint32_t other = 0;   // failed, or the target was never resolved
    uint32_t samples = 1;
  };
  static constexpr uint32_t kSpellLines = 12;
  Spell spell_;
  uint32_t spells_logged_ = 0;
  void LogSpell();

  struct Stats {
    uint64_t markers = 0;
    uint64_t started = 0;           // frames whose UI went to a cleared target
    uint64_t captured = 0;          // frames whose target was resolved with the layer started
    uint64_t resolved_unstarted = 0;  // target resolved with no draw to separate (scene only)
    uint64_t separated_draws = 0;
    uint64_t presents = 0;          // guest presents
    uint64_t presents_valid = 0;    // ... with layers
    uint64_t unusable_picture = 0;  // ... layers whose destination was not the front buffer
    uint64_t unusable_size = 0;     // ... or that did not cover it
    uint64_t pause_armed = 0;       // frames whose marker target was resolved with no UI: waited for a
                                    // second target (a pause view, or a title/loading frame with none)
    uint64_t pause_adopted = 0;     // ... and found it: the pause-view frames
    uint64_t pause_layers = 0;      // ... presented with layers
    uint64_t pause_no_ui = 0;       // ... whose target was resolved with no UI to separate
    uint64_t pause_other = 0;       // ... failed, or whose target was never resolved
    uint64_t pause_no_picture = 0;  // ... whose UI target had no opaque draw before its UI
    uint64_t markers_ignored = 0;   // repeats of the marker in a pause view's frame
    std::array<uint64_t, size_t(Cause::kCount)> failed{};
  };
  Stats stats_;
};

}  // namespace mcla::gpu
