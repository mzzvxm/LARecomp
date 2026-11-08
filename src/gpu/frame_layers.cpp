// MCLA GPU renderer -- the frame in two layers: the scene and the UI. See frame_layers.h.

#include "frame_layers.h"

#include <algorithm>
#include <cstring>
#include <format>

#include <rex/logging.h>

#include "draw_decode.h"
#include "draw_state.h"
#include "host_shaders.h"
#include "pass_timing.h"
#include "texture_bytes.h"

namespace mcla::gpu {

namespace {

constexpr plume::RenderFormat kLayerFormat = plume::RenderFormat::R8G8B8A8_UNORM;

const char* CauseName(FrameLayers::Cause cause) {
  switch (cause) {
    case FrameLayers::Cause::kFormat: return "the marker's target is not a single-sampled R8G8B8A8 target";
    case FrameLayers::Cause::kBlend: return "a draw into the UI target with a blend that is not \"over\"";
    case FrameLayers::Cause::kOpaque: return "a draw into the UI target with blending off after the layer started";
    case FrameLayers::Cause::kClear: return "a clear of the UI target after the layer started";
    case FrameLayers::Cause::kMarker: return "a second composite marker with the layer started";
    case FrameLayers::Cause::kUnresolved: return "the frame ended with the layer started";
    case FrameLayers::Cause::kImage: return "an image could not be made";
    case FrameLayers::Cause::kResolve: return "the UI target's resolve was not a copy, had no destination or moved the pixels";
    case FrameLayers::Cause::kCopy: return "a copy, clear or draw could not be recorded";
    case FrameLayers::Cause::kPauseTarget: return "the pause view's UI target cannot be separated";
    case FrameLayers::Cause::kPipeline: return "a pipeline for a multisampled UI target could not be made";
    case FrameLayers::Cause::kCount: break;
  }
  return "?";
}

const char* FactorName(plume::RenderBlend factor) {
  static const char* const kNames[] = {"?",          "ZERO",       "ONE",           "SRC_COLOR",
                                       "INV_SRC_COLOR", "SRC_ALPHA", "INV_SRC_ALPHA", "DEST_ALPHA",
                                       "INV_DEST_ALPHA", "DEST_COLOR", "INV_DEST_COLOR",
                                       "SRC_ALPHA_SAT", "BLEND_FACTOR", "INV_BLEND_FACTOR",
                                       "SRC1_COLOR", "INV_SRC1_COLOR", "SRC1_ALPHA",
                                       "INV_SRC1_ALPHA"};
  const size_t index = size_t(factor);
  return index < std::size(kNames) ? kNames[index] : "?";
}

const char* OperationName(plume::RenderBlendOperation op) {
  static const char* const kNames[] = {"?", "ADD", "SUBTRACT", "REV_SUBTRACT", "MIN", "MAX"};
  const size_t index = size_t(op);
  return index < std::size(kNames) ? kNames[index] : "?";
}

// What a draw with this blend does to a target that holds the UI so far, as far as the split can
// carry it.
enum class BlendKind : uint8_t {
  kNoWrite,  // writes no colour channel: leaves the target alone
  kOpaque,   // blending off: replaces what it covers
  kOver,     // colour ADD, SRC_ALPHA or ONE over INV_SRC_ALPHA, every channel: premultiplied "over"
  kOther,
};

BlendKind KindOf(const plume::RenderBlendDesc& b) {
  if (b.renderTargetWriteMask == 0) {
    return BlendKind::kNoWrite;
  }
  if (!b.blendEnabled) {
    return BlendKind::kOpaque;
  }
  if (b.renderTargetWriteMask == uint8_t(plume::RenderColorWriteEnable::ALL) &&
      b.blendOp == plume::RenderBlendOperation::ADD &&
      b.dstBlend == plume::RenderBlend::INV_SRC_ALPHA &&
      (b.srcBlend == plume::RenderBlend::SRC_ALPHA || b.srcBlend == plume::RenderBlend::ONE)) {
    return BlendKind::kOver;
  }
  return BlendKind::kOther;
}

const char* BlendKindName(uint8_t kind) {
  switch (BlendKind(kind)) {
    case BlendKind::kNoWrite: return "writes no colour";
    case BlendKind::kOpaque: return "blending off";
    case BlendKind::kOver: return "over";
    case BlendKind::kOther: return "other blend";
  }
  return "?";
}

// The scene under the UI: colour and alpha both result = underlay x (1 - target alpha) + target.
plume::RenderBlendDesc RestoreBlend() {
  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::INV_DEST_ALPHA;
  blend.dstBlend = plume::RenderBlend::ONE;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::INV_DEST_ALPHA;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
  return blend;
}

}  // namespace

FrameLayers::~FrameLayers() {
  Shutdown();
}

bool FrameLayers::Initialize(HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  initialized_ = true;
  if (requesters_ != 0) {
    active_ = MakePipelines();
  }
  return requesters_ == 0 || active_;
}

void FrameLayers::Shutdown() {
  if (initialized_) {
    // The summary is logged once a minute of guest frames, so what happened after the last one (a
    // pause menu opened late in a run) is reported here, with the pause-view run in progress.
    LogSpell();
    if (stats_.presents != 0) {
      REXLOG_INFO("[gpu] {} (since the last summary, at shutdown)", TakeSummary());
    }
  }
  // The GPU is idle and the heap goes later: the slots are not handed back.
  underlay_ = HostImage{};
  ui_ = HostImage{};
  restore_pipeline_.reset();
  restore_shader_.reset();
  for (std::unique_ptr<plume::RenderPipeline>& pipeline : restore_ms_) {
    pipeline.reset();
  }
  resolve_pipeline_.reset();
  resolve_shader_.reset();
  presented_ = Frame{};
  captured_ = Frame{};
  phase_ = Phase::kIdle;
  target_ = nullptr;
  pause_view_ = false;
  active_ = false;
  initialized_ = false;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
}

void FrameLayers::Request(uint32_t requester, bool wanted) {
  const uint32_t before = requesters_;
  requesters_ = wanted ? (requesters_ | requester) : (requesters_ & ~requester);
  if (requesters_ == before || !initialized_) {
    return;
  }
  if (requesters_ == 0) {
    // Nobody wants them: called at a frame boundary, so no frame is in progress (its target was put
    // back by the frame's end). The frames kept for the present and the images go now: the present
    // must not compose a stale frame in an SDR output, and a player who turned HDR off should not
    // keep its memory.
    const bool was_active = active_;
    active_ = false;
    phase_ = Phase::kIdle;
    target_ = nullptr;
    pause_view_ = false;
    captured_ = Frame{};
    presented_ = Frame{};
    idle_frames_ = 0;
    ReleaseImages();
    if (was_active) {
      REXLOG_INFO("[gpu] frame layers: off (no requester left), images released");
    }
    return;
  }
  const bool was_active = active_;
  const bool made_before = restore_pipeline_ != nullptr;
  active_ = MakePipelines();
  if (active_ && !was_active && made_before) {
    REXLOG_INFO("[gpu] frame layers: on again (requested by {}{})",
                (requesters_ & kRequesterHdr) ? "HDR output" : "",
                (requesters_ & kRequesterFrameGeneration) ? " frame generation" : "");
  }
}

bool FrameLayers::MakePipelines() {
  if (restore_pipeline_) {
    return true;
  }
  const HostShaderBlob restore = LayersRestorePixelShader();
  restore_shader_ = passes_->CreateShader(restore.data, restore.size, restore.name);
  if (!restore_shader_) {
    return false;
  }
  restore_pipeline_ = passes_->CreatePipeline(*restore_shader_, kLayerFormat, RestoreBlend(),
                                              "frame layers restore");
  if (!restore_pipeline_) {
    restore_shader_.reset();
    REXLOG_ERROR("[gpu] frame layers: the pipeline that puts the scene back under the UI could "
                 "not be made; the HDR output shows the front buffer as it is (stage 1)");
    return false;
  }
  // The pass that averages a multisampled target's samples into an image (the pause view's UI
  // target when MSAA is on). Its failure leaves that view without layers and nothing else.
  const HostShaderBlob resolve = LayersResolvePixelShader();
  resolve_shader_ = passes_->CreateShader(resolve.data, resolve.size, resolve.name);
  if (resolve_shader_) {
    resolve_pipeline_ =
        passes_->CreatePipeline(*resolve_shader_, kLayerFormat, "frame layers resolve");
  }
  if (!resolve_pipeline_) {
    resolve_shader_.reset();
    REXLOG_ERROR("[gpu] frame layers: the pass that averages a multisampled UI target could not be "
                 "made; the pause menu and the map keep the front buffer as it is while MSAA is on "
                 "(HDR stage 1)");
  }
  REXLOG_INFO("[gpu] frame layers: on (requested by {}{})",
              (requesters_ & kRequesterHdr) ? "HDR output" : "",
              (requesters_ & kRequesterFrameGeneration) ? " frame generation" : "");
  return true;
}

void FrameLayers::BeginFrame() {
  phase_ = Phase::kIdle;
  target_ = nullptr;
  target_samples_ = 1;
  pause_view_ = false;
  pause_opaque_draws_ = 0;
  captured_ = Frame{};
  ClearTrace();
  pause_kind_drawn_ = false;
  frame_adopted_ = false;
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

bool FrameLayers::EnsureImages() {
  if (underlay_.texture && (underlay_.width != host_width_ || underlay_.height != host_height_)) {
    // The images are made again: the layers that named them go.
    presented_ = Frame{};
    captured_ = Frame{};
  }
  if (!passes_->EnsureImage(underlay_, host_width_, host_height_, kLayerFormat,
                            "frame layers underlay") ||
      !passes_->EnsureImage(ui_, host_width_, host_height_, kLayerFormat, "frame layers ui")) {
    // Both or neither: a frame needs the two.
    passes_->ReleaseImage(underlay_);
    passes_->ReleaseImage(ui_);
    return false;
  }
  return true;
}

void FrameLayers::ReleaseImages() {
  if (!passes_) {
    return;
  }
  passes_->ReleaseImage(underlay_);
  passes_->ReleaseImage(ui_);
}

uint64_t FrameLayers::HeldBytes() const {
  uint64_t bytes = 0;
  for (const HostImage* image : {&underlay_, &ui_}) {
    if (image->texture) {
      bytes += TextureBytes(image->width, image->height, 1, image->format);
    }
  }
  return bytes;
}

bool FrameLayers::CopyFromTarget(plume::RenderCommandList* list, HostImage& image) {
  EdramTargets::Target& t = *target_;
  if (!t.texture || !image.texture || image.width != host_width_ || image.height != host_height_) {
    return false;
  }
  if (t.host_samples > 1) {
    return ResolveTargetInto(list, image);  // a texture copy cannot take samples to one sample
  }
  // The copy covers the whole image, which also initialises a new one (D3D12 debug layer id
  // 1422: a render target made without zeroing is cleared, discarded or wholly copied first).
  const plume::RenderTextureBarrier before[] = {
      plume::RenderTextureBarrier(t.texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
      plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
  list->barriers(plume::RenderBarrierStage::COPY, before, 2);
  t.layout = plume::RenderTextureLayout::COPY_SOURCE;
  const plume::RenderBox box(0, 0, int32_t(host_width_), int32_t(host_height_));
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(image.texture.get()),
                          plume::RenderTextureCopyLocation::Subresource(t.texture.get()), 0, 0, 0,
                          &box);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(image.texture.get(),
                                             plume::RenderTextureLayout::SHADER_READ));
  image.layout = plume::RenderTextureLayout::SHADER_READ;
  return true;
}

bool FrameLayers::ResolveTargetInto(plume::RenderCommandList* list, HostImage& image) {
  EdramTargets::Target& t = *target_;
  uint32_t slot = 0xFFFFFFFFu;
  uint32_t stencil_slot = 0xFFFFFFFFu;
  if (!resolve_pipeline_ || !t.texture || !image.texture || image.width != host_width_ ||
      image.height != host_height_ || !edram_->SampleSlots(t, &slot, &stencil_slot)) {
    return false;
  }
  // The pass reads the samples as a texture; the image is drawn whole (host_width_ x host_height_ is
  // its size) and ends in SHADER_READ, like the copy's.
  edram_->Transition(list, t, plume::RenderTextureLayout::SHADER_READ);
  const ResolvePush push{slot, t.host_samples, {0, 0}};
  passes_->DrawInto(list, image, resolve_pipeline_.get(), &push, sizeof(push));
  return true;
}

plume::RenderPipeline* FrameLayers::RestorePipelineFor(uint32_t samples) const {
  switch (samples) {
    case 1: return restore_pipeline_.get();
    case 2: return restore_ms_[0].get();
    case 4: return restore_ms_[1].get();
    case 8: return restore_ms_[2].get();
    default: return nullptr;
  }
}

bool FrameLayers::EnsureSamplePipelines(uint32_t samples) {
  if (samples <= 1) {
    return true;
  }
  if (!resolve_pipeline_ || !restore_shader_ || (samples != 2 && samples != 4 && samples != 8)) {
    return false;
  }
  std::unique_ptr<plume::RenderPipeline>& pipeline =
      restore_ms_[samples == 2 ? 0 : samples == 4 ? 1 : 2];
  if (!pipeline) {
    // The same pass as the 1x put-back, into a target of this many samples: it runs once per pixel
    // and writes every sample the triangle covers, each blended with its own coverage.
    pipeline = passes_->CreatePipeline(passes_->layout(), *restore_shader_, kLayerFormat,
                                       RestoreBlend(), samples,
                                       "frame layers restore (multisampled)");
  }
  return pipeline != nullptr;
}

bool FrameLayers::DrawRestore(plume::RenderCommandList* list) {
  EdramTargets::Target& t = *target_;
  plume::RenderPipeline* pipeline = RestorePipelineFor(t.host_samples);
  if (!pipeline || !underlay_.texture) {
    return false;
  }
  edram_->Transition(list, t, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* colors[1] = {&t};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(colors, 1, nullptr);
  if (!framebuffer) {
    return false;
  }
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{0.0f, 0.0f, float(host_width_), float(host_height_)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(host_width_), int32_t(host_height_));
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  const Push push{underlay_.slot, 0, 0, 0};
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void FrameLayers::OnComposite(plume::RenderCommandList* list, EdramTargets::Target* target,
                              uint32_t width, uint32_t height) {
  if (!active_) {
    return;
  }
  if (pause_view_) {
    // This frame's pause view is armed or running: the menus' UI phase repeats the marker (once per
    // tile), so this is that repeat, not a second marker. Nothing changes.
    ++stats_.markers_ignored;
    return;
  }
  if (phase_ == Phase::kStarted && list) {
    PutBack(list, Cause::kMarker);
  }
  phase_ = Phase::kIdle;
  target_ = nullptr;
  target_samples_ = 1;
  ClearTrace();  // the frame's records after the marker start here
  pause_kind_drawn_ = false;
  if (!target || !list || width == 0 || height == 0) {
    return;
  }
  ++stats_.markers;
  if (target->key.depth || target->key.samples != 1 || target->host_samples != 1 ||
      target->format != kLayerFormat || !target->texture) {
    phase_ = Phase::kFailed;
    Fail(Cause::kFormat, DescribeEdramKey(target->key));
    return;
  }
  target_ = target;
  ui_key_ = target->key;
  guest_width_ = std::min(width, target->width);
  guest_height_ = std::min(height, target->height);
  host_width_ = target->scale.Edge(guest_width_);
  host_height_ = target->scale.Edge(guest_height_);
  // What a pause view's UI target will be made from if this target is resolved with no UI in it.
  marker_key_ = target->key;
  marker_guest_width_ = guest_width_;
  marker_guest_height_ = guest_height_;
  phase_ = Phase::kUi;
}

bool FrameLayers::Region(EdramKey* target, uint32_t* width, uint32_t* height) const {
  if (phase_ != Phase::kUi || !target_ || pause_view_) {
    return false;
  }
  *target = ui_key_;
  *width = host_width_;
  *height = host_height_;
  return true;
}

bool FrameLayers::IsPauseUiKey(const EdramKey& key) const {
  // The pause view's UI target: colour of the marker's format and pitch that the guest multisamples
  // (k_8_8_8_8 at the marker's pitch, 4 samples, in every pause menu, race pause and map measured).
  return key.depth == 0 && key.samples > 1 && key.pitch == marker_key_.pitch &&
         key.format == marker_key_.format;
}

bool FrameLayers::AdoptPauseTarget(const EdramKey& key, uint32_t host_samples, uint8_t* note) {
  uint8_t unused = kAdoptNone;
  uint8_t& result = note ? *note : unused;
  const EdramTargets::Target* found = edram_->Find(key);
  if (!found || !found->texture) {
    // Not made yet (the first draw of the first pause view makes it) or released: this draw goes on
    // as it is and the next one looks again.
    result = found ? kAdoptNoTexture : kAdoptNoTarget;
    return false;
  }
  // Find hands the target out read-only so that nobody creates one; this is the frame's UI target
  // to write into, as the marker's was.
  EdramTargets::Target* target = const_cast<EdramTargets::Target*>(found);
  const uint32_t samples = target->host_samples;
  const uint32_t guest_width = std::min(marker_guest_width_, target->width);
  const uint32_t guest_height = std::min(marker_guest_height_, target->height);
  if (target->format != kLayerFormat || samples != host_samples ||
      (samples != 1 && samples != 2 && samples != 4 && samples != 8) || guest_width == 0 ||
      guest_height == 0) {
    result = kAdoptRefused;
    phase_ = Phase::kFailed;
    Fail(Cause::kPauseTarget,
         std::format("{}, {} host samples, {}x{} guest pixels", DescribeEdramKey(key), samples,
                     guest_width, guest_height));
    return false;
  }
  target_ = target;
  ui_key_ = key;
  target_samples_ = samples;
  guest_width_ = guest_width;
  guest_height_ = guest_height;
  host_width_ = target->scale.Edge(guest_width_);
  host_height_ = target->scale.Edge(guest_height_);
  pause_opaque_draws_ = 0;
  phase_ = Phase::kUi;
  result = kAdoptDone;
  frame_adopted_ = true;
  ++stats_.pause_adopted;
  if (!pause_logged_) {
    pause_logged_ = true;
    REXLOG_INFO("[gpu] frame layers: the pause view's UI target {} ({} host samples, {}x{} host "
                "pixels) is separated like the game's UI target in play, so the pause menu and "
                "the map can show the scene above paper white",
                DescribeEdramKey(key), samples, host_width_, host_height_);
  }
  return true;
}

FrameLayers::DrawAction FrameLayers::ClassifyDraw(const EdramKey& color0, uint32_t host_samples,
                                                  uint64_t ps_identity, PipelineKey& key) {
  // The frame's records after the marker, for the one-time reports: every draw but the plain
  // UI-phase draw into the marker's target (a play frame has about forty of those).
  TraceEvent event;
  const bool plain = phase_ != Phase::kPause && !pause_view_ && color0 == ui_key_;
  if (!plain) {
    event.kind = TraceEvent::Kind::kDraw;
    event.phase = phase_;
    event.key = color0;
    event.host_samples = uint8_t(std::min<uint32_t>(host_samples, 255u));
    event.ps = ps_identity;
    event.blend_kind = uint8_t(KindOf(UnpackBlend(key.blend[0])));
    if (IsPauseUiKey(color0)) {
      pause_kind_drawn_ = true;
    }
  }
  if (phase_ == Phase::kPause) {
    // The marker's target was resolved with no UI in it: the UI is drawn in the pause view's own
    // target. Its first draw makes it this frame's UI target; any other draw is not ours.
    uint8_t note = IsPauseUiKey(color0) ? uint8_t(kAdoptNone) : uint8_t(kAdoptNotPauseKind);
    const bool adopted = note == kAdoptNone && AdoptPauseTarget(color0, host_samples, &note);
    event.adopt = note;
    Trace(event);
    if (!adopted) {
      return DrawAction::kNone;
    }
  } else if (!plain) {
    Trace(event);
  }
  if (!(color0 == ui_key_) || host_samples != target_samples_) {
    return DrawAction::kNone;
  }
  const plume::RenderBlendDesc blend = UnpackBlend(key.blend[0]);
  const BlendKind kind = KindOf(blend);
  if (kind == BlendKind::kNoWrite) {
    return DrawAction::kNone;
  }
  if (phase_ == Phase::kUi) {
    if (kind == BlendKind::kOpaque) {
      // Part of the scene so far: the underlay will have it. In the pause view this is the copy of
      // the tone-mapped picture that the layers' float scene stands for.
      pause_opaque_draws_ += pause_view_ ? 1u : 0u;
      return DrawAction::kNone;
    }
    if (kind != BlendKind::kOver) {
      phase_ = Phase::kFailed;
      NoteBlend(blend, ps_identity, false);
      Fail(Cause::kBlend, std::string());
      return DrawAction::kNone;
    }
    // The first separable draw: the layer starts with it, so the images are needed now.
    if (!EnsureImages()) {
      phase_ = Phase::kFailed;
      Fail(Cause::kImage, std::format("{}x{}", host_width_, host_height_));
      return DrawAction::kNone;
    }
    if (!EnsureSamplePipelines(target_samples_)) {
      phase_ = Phase::kFailed;
      Fail(Cause::kPipeline, std::format("{} host samples", target_samples_));
      return DrawAction::kNone;
    }
  } else if (kind != BlendKind::kOver) {
    // The layer started and this draw cannot join it: the Scene puts the target back before the
    // draw is recorded, and the draw keeps the game's blend.
    pending_cause_ = kind == BlendKind::kOpaque ? Cause::kOpaque : Cause::kBlend;
    NoteBlend(blend, ps_identity, true);
    return DrawAction::kEnd;
  }
  // Alpha collects the coverage: ONE over INV_SRC_ALPHA, whatever the game's alpha blend was.
  plume::RenderBlendDesc separated = blend;
  separated.srcBlendAlpha = plume::RenderBlend::ONE;
  separated.dstBlendAlpha = plume::RenderBlend::INV_SRC_ALPHA;
  separated.blendOpAlpha = plume::RenderBlendOperation::ADD;
  key.blend[0] = PackBlend(separated);
  ++stats_.separated_draws;
  return phase_ == Phase::kUi ? DrawAction::kStart : DrawAction::kNone;
}

void FrameLayers::Start(plume::RenderCommandList* list) {
  if (phase_ != Phase::kUi || !target_ || !list) {
    return;
  }
  // A second UI phase in a frame takes the images over: the first one's layers go.
  captured_ = Frame{};
  pass_timing_->Mark(list, "frame layers underlay");
  if (!CopyFromTarget(list, underlay_)) {
    phase_ = Phase::kFailed;
    Fail(Cause::kCopy, "the underlay copy");
    return;
  }
  const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  plume::RenderFramebuffer* framebuffer = edram_->ClearColor(
      list, *target_, plume::RenderRect(0, 0, int32_t(guest_width_), int32_t(guest_height_)), zero);
  list->setFramebuffer(nullptr);
  if (!framebuffer) {
    // The target still holds the scene (nothing was cleared): the UI that follows is drawn over
    // it as the game does, only its alpha differs.
    phase_ = Phase::kFailed;
    Fail(Cause::kCopy, "the clear");
    return;
  }
  phase_ = Phase::kStarted;
  ++stats_.started;
}

void FrameLayers::End(plume::RenderCommandList* list) {
  PutBack(list, pending_cause_);
}

void FrameLayers::PutBack(plume::RenderCommandList* list, Cause cause) {
  if (phase_ == Phase::kStarted) {
    if (list) {
      pass_timing_->Mark(list, "frame layers put back");
      if (!DrawRestore(list)) {
        Fail(Cause::kCopy, "the put-back");
      }
    }
    phase_ = Phase::kFailed;
    Fail(cause, std::string());
  } else if (phase_ == Phase::kUi) {
    phase_ = Phase::kFailed;
    Fail(cause, std::string());
  }
}

void FrameLayers::OnResolve(const ResolveRecord& record, plume::RenderCommandList* list) {
  if (!ui_phase()) {
    return;
  }
  EdramKey source;
  const bool known = ResolveSourceKey(record, &source);
  const bool copy = (record.resolve_flags & 0x2000) == 0;
  TextureKey destination;
  const bool has_destination = ResolveDestination(record, &destination);
  {
    TraceEvent event;
    event.kind = TraceEvent::Kind::kResolve;
    event.phase = phase_;
    event.known = known;
    event.key = source;
    event.is_ui_target = known && source == ui_key_;
    event.copy = copy;
    event.dest = has_destination;
    event.unmoved = ResolveIsUnmoved(record);
    event.resolve_flags = record.resolve_flags;
    Trace(event);
  }
  if (phase_ == Phase::kPause) {
    // (In kPause the pause view's UI target is not drawn into yet: a resolve now is not its picture.)
    return;
  }
  if (!known || !(source == ui_key_)) {
    return;  // another target's resolve
  }
  if (phase_ == Phase::kUi) {
    // The target was resolved before any UI went into it: the scene alone (or a copy for a pause
    // blur). Nothing was separated.
    ++stats_.resolved_unstarted;
    if (pause_view_) {
      phase_ = Phase::kDone;  // the pause view's own target: nothing more to wait for
    } else {
      // The marker's target in a pause menu, a race pause or a map: the UI is drawn in another
      // target. The frame waits for it (the header comment); the draws into this one that follow are
      // not touched.
      pause_view_ = true;
      phase_ = Phase::kPause;
      ++stats_.pause_armed;
    }
    return;
  }
  if (!copy || !has_destination || !ResolveIsUnmoved(record)) {
    PutBack(list, Cause::kResolve);
    return;
  }
  pass_timing_->Mark(list, "frame layers ui");
  if (!CopyFromTarget(list, ui_) || !DrawRestore(list)) {
    // The target may hold the UI alone: a copy that failed puts it back if it can.
    PutBack(list, Cause::kCopy);
    return;
  }
  captured_.valid = true;
  captured_.width = host_width_;
  captured_.height = host_height_;
  // The first host column and row the game's full-screen draws never wrote at this scale (the
  // header's section on them): the consumers read them from the next ones.
  // CHECK: that the game's draws all have the D3D pixel centre, as the resolve's first log line
  // says ("pixel centre D3D"); with another one the picture would start at pixel 0 and this would
  // repeat pixel `gap` over it.
  captured_.gap = target_->scale.HalfPixelGap();
  if (captured_.gap != 0 && !gap_logged_) {
    gap_logged_ = true;
    REXLOG_INFO("[gpu] frame layers: at {}x the first {} host column(s) and row(s) of the layers hold "
                "no picture (no full-screen draw of the game writes them); the compose reads them "
                "from the next ones, as the guest's resolve does for the front buffer",
                target_->scale, captured_.gap);
  }
  captured_.hudless = &underlay_;
  captured_.ui = &ui_;
  // HDR stage 3: the float scene of this frame, when its tone map and the draws after it were
  // made again at this region's size (hdr_tonemap.h). A pause view's UI target that had no opaque
  // draw before its UI did not start from a copy of the tone-mapped picture, so the scene is not
  // what the game shows under that UI: the 8-bit underlay stays.
  const bool picture = !pause_view_ || pause_opaque_draws_ != 0;
  if (!picture) {
    ++stats_.pause_no_picture;
    if (!pause_picture_logged_) {
      pause_picture_logged_ = true;
      REXLOG_INFO("[gpu] frame layers: the pause view's UI target had no draw with blending off "
                  "before its UI, so it did not start from the tone-mapped picture; the frame keeps "
                  "its 8-bit scene (counted, logged once)");
    }
  }
  captured_.scene = (scene_source_ && picture)
                        ? scene_source_->FinishedScene(host_width_, host_height_,
                                                       &captured_.scene_epoch)
                        : nullptr;
  captured_.generation = ++generation_;
  captured_.destination = destination;
  phase_ = Phase::kDone;
  ++stats_.captured;
}

bool FrameLayers::OnColorClear(plume::RenderCommandList* list, const EdramKey& key) {
  if (ui_phase()) {
    TraceEvent event;
    event.kind = TraceEvent::Kind::kClear;
    event.phase = phase_;
    event.key = key;
    event.is_ui_target = key == ui_key_;
    Trace(event);
  }
  if (!ui_phase() || phase_ == Phase::kPause || !(key == ui_key_)) {
    return false;  // (in kPause no layer has started: a clear is the scene's)
  }
  if (phase_ == Phase::kUi) {
    return false;  // the underlay is taken later: this clear is part of the scene's
  }
  PutBack(list, Cause::kClear);
  return true;
}

void FrameLayers::EndFrame(plume::RenderCommandList* list) {
  // A frame that ends with the layer started puts its target back (and has no layers). An abandoned
  // frame (no list: the renderer is shutting down or the device was lost) records nothing and counts
  // as nothing: the last frame of every run is cut in the middle of its UI phase, and was reported
  // as a frame that "ended with the layer started".
  if (phase_ == Phase::kStarted && list) {
    PutBack(list, Cause::kUnresolved);
  }
  phase_ = Phase::kIdle;
  target_ = nullptr;
  pause_view_ = false;
}

void FrameLayers::OnGuestPresent() {
  if (!active_) {
    return;
  }
  ++stats_.presents;
  if (frame_adopted_) {
    // A pause-view frame (its UI target was found): how it ended, and the run of them it belongs to.
    if (spell_.frames == 0) {
      spell_.samples = target_samples_;
    }
    ++spell_.frames;
    if (captured_.valid) {
      ++spell_.layers;
      ++stats_.pause_layers;
    } else if (phase_ == Phase::kDone) {
      ++spell_.no_ui;
      ++stats_.pause_no_ui;
    } else {
      ++spell_.other;
      ++stats_.pause_other;
    }
  } else if (spell_.frames != 0) {
    LogSpell();
  }
  LogTraceIfWorthIt(captured_.valid             ? "with layers"
                    : phase_ == Phase::kDone    ? "the target was resolved with no UI to separate"
                    : phase_ == Phase::kFailed  ? "failed"
                    : phase_ == Phase::kStarted ? "the target was never resolved (layer started)"
                    : phase_ == Phase::kPause   ? "no second target was found"
                                                : "the target was never resolved");
  // Counted: a guest present with no frame list open (no scene records) must not count this frame
  // again.
  frame_adopted_ = false;
  pause_kind_drawn_ = false;
  ClearTrace();
  presented_ = captured_;
  captured_ = Frame{};
  if (presented_.valid) {
    ++stats_.presents_valid;
    idle_frames_ = 0;
  } else if (++idle_frames_ == kReleaseAfterFrames) {
    ReleaseImages();
  }
}

FrameLayers::Frame FrameLayers::presentable() const {
  return presented_;
}

const HostImage* FrameLayers::FloatSceneOf(const Frame& frame, uint32_t width,
                                           uint32_t height) const {
  if (!frame.scene || !scene_source_ || frame.scene_epoch != scene_source_->SceneEpoch() ||
      !frame.scene->texture || frame.scene->width < width || frame.scene->height < height) {
    return nullptr;
  }
  return frame.scene;
}

void FrameLayers::NoteUnusable(bool wrong_picture) {
  if (wrong_picture) {
    ++stats_.unusable_picture;
  } else {
    ++stats_.unusable_size;
  }
}

// ---------------------------------------------------------------------------
// reading the records
// ---------------------------------------------------------------------------

bool FrameLayers::ResolveSourceKey(const ResolveRecord& r, EdramKey* key) {
  if (r.flags & (kEventSurfaceUnreadable | kEventStackUnreadable)) {
    return false;
  }
  if ((r.resolve_flags & 7) != 0) {
    return false;  // colour 1..3 or depth: not the UI target (colour 0)
  }
  // As ResolveCache::Execute reads the source of a copy: pitch and samples from the device's
  // RB_SURFACE_INFO, base and format from the bound surface's header (+0x1C), else the registers.
  const RegisterWindow pre(r.state.pre, guest::kEventWindowBegin, guest::kEventWindowBytes);
  const bool bound = pre.DeviceDword(guest::kDevColorSurfaces) != 0;
  xreg::RB_SURFACE_INFO surface_info;
  surface_info.value = pre.Register(guest::reg::kRbSurfaceInfo);
  key->pitch = uint16_t(surface_info.surface_pitch);
  key->samples = uint8_t(MsaaSampleCount(surface_info.msaa_samples));
  xreg::RB_COLOR_INFO color_info;
  color_info.value =
      bound ? LoadBe32(r.state.surfaces[0] + 0x1C) : pre.Register(guest::reg::kRbColorInfo);
  key->base_tiles = uint16_t(color_info.color_base | (color_info.color_base_bit_11 << 11));
  key->depth = 0;
  key->format = uint8_t(FoldColorFormat(uint32_t(color_info.color_format)));
  return true;
}

bool FrameLayers::ResolveIsUnmoved(const ResolveRecord& r) {
  // The destination is the source moved to the point: (x0, y0) of the source rectangle lands at
  // (px, py). The layers are the target's pixels where they are, so only a resolve that does not
  // move them is a picture of them.
  int32_t x0 = 0, y0 = 0, px = 0, py = 0;
  if (r.flags & kEventHasRect) {
    x0 = int32_t(LoadBe32(r.src_rect + 0));
    y0 = int32_t(LoadBe32(r.src_rect + 4));
  }
  if (r.flags & kEventHasPoint) {
    px = int32_t(LoadBe32(r.dest_point + 0));
    py = int32_t(LoadBe32(r.dest_point + 4));
  }
  return x0 == px && y0 == py;
}

bool FrameLayers::ResolveDestination(const ResolveRecord& r, TextureKey* key) {
  if (!(r.flags & kEventHasDest) || (r.flags & kEventDestUnreadable)) {
    return false;
  }
  uint32_t d[6];
  for (uint32_t i = 0; i < 6; ++i) {
    d[i] = LoadBe32(r.dest_header + guest::kTextureFetchOffset + 4 * i);
  }
  const TextureFetch dest = ReadTextureFetch(d);
  if (!dest.is_texture) {
    return false;
  }
  *key = dest.key;
  return true;
}

// ---------------------------------------------------------------------------
// reports
// ---------------------------------------------------------------------------

void FrameLayers::Fail(Cause cause, const std::string& detail) {
  ++stats_.failed[size_t(cause)];
  if (cause_logged_[size_t(cause)]) {
    return;
  }
  cause_logged_[size_t(cause)] = true;
  REXLOG_INFO("[gpu] frame layers: a frame has no layers: {}{}{} (counted; logged once per cause; "
              "such frames show the front buffer as HDR stage 1 does)",
              CauseName(cause), detail.empty() ? "" : ": ", detail);
}

void FrameLayers::NoteBlend(const plume::RenderBlendDesc& blend, uint64_t ps_identity,
                            bool after_start) {
  // One line per kind of blend and pixel shader, the first 16: what the UI phase of this game
  // draws that "over" does not carry.
  const uint64_t id = uint64_t(PackBlend(blend)) * 0x9E3779B97F4A7C15ull ^ ps_identity ^
                      uint64_t(after_start);
  if (blends_logged_.size() >= 16 || !blends_logged_.insert(id).second) {
    return;
  }
  REXLOG_INFO("[gpu] frame layers: a UI draw (PS {:016X}) the split cannot carry, {} the layer "
              "started: {}, colour {} {} {} (alpha {} {} {}), write mask {:#x}",
              ps_identity, after_start ? "after" : "before",
              blend.blendEnabled ? "blended" : "blending off", FactorName(blend.srcBlend),
              OperationName(blend.blendOp), FactorName(blend.dstBlend),
              FactorName(blend.srcBlendAlpha), OperationName(blend.blendOpAlpha),
              FactorName(blend.dstBlendAlpha), uint32_t(blend.renderTargetWriteMask));
}

void FrameLayers::Trace(const TraceEvent& event) {
  if (trace_count_ < kTraceEvents) {
    trace_[trace_count_++] = event;
  } else {
    ++trace_dropped_;
  }
}

void FrameLayers::ClearTrace() {
  trace_count_ = 0;
  trace_dropped_ = 0;
}

void FrameLayers::LogSpell() {
  if (spell_.frames == 0) {
    return;
  }
  // CHECK: a run with the pause menu open at MSAA 2x shows a line here with nearly all frames "with
  // layers" (at one host sample 775 of 781 pause-view frames had them); anything else is explained
  // by the frame traces logged next to it.
  if (spells_logged_ < kSpellLines) {
    ++spells_logged_;
    REXLOG_INFO("[gpu] frame layers: pause-view run of {} frames at {} host samples: {} with layers, "
                "{} with no UI to separate, {} with none for another reason{}",
                spell_.frames, spell_.samples, spell_.layers, spell_.no_ui, spell_.other,
                spells_logged_ == kSpellLines ? " (further runs are counted, not logged)" : "");
  }
  spell_ = Spell{};
  traced_[1] = 0;  // the next run may log its own frames with no layers
}

void FrameLayers::LogTraceIfWorthIt(const char* outcome) {
  // CHECK: these lines show, once, what a pause-view frame draws and resolves after the marker (the
  // order the layers rely on: the marker target's resolve, then draws into the second target, then its
  // tile resolves) and what a frame that fell short did instead; not run yet, the format may need a
  // look.
  uint32_t why = 3;
  const char* what = "";
  if (frame_adopted_ && captured_.valid && traced_[0] == 0) {
    why = 0;
    what = "the first pause-view frame";
  } else if (frame_adopted_ && !captured_.valid && traced_[1] < kTracePerSpell) {
    why = 1;
    what = "a pause-view frame without layers";
  } else if (!frame_adopted_ && pause_kind_drawn_ && traced_[2] < kTraceMissed) {
    why = 2;
    what = "a frame that drew a second target of the pause kind and never took it as its UI target";
  }
  if (why == 3 || traced_[0] + traced_[1] + traced_[2] >= kTraceLines) {
    return;
  }
  ++traced_[why];
  const auto phase_name = [](Phase phase) {
    switch (phase) {
      case Phase::kIdle: return "idle";
      case Phase::kUi: return "UI";
      case Phase::kStarted: return "started";
      case Phase::kDone: return "done";
      case Phase::kFailed: return "failed";
      case Phase::kPause: return "waiting";
    }
    return "?";
  };
  static const char* const kAdoptText[] = {"",
                                           ", not of the pause kind",
                                           ", no target yet",
                                           ", target without texture",
                                           ", taken as the UI target",
                                           ", refused"};
  std::string events;
  for (uint32_t i = 0; i < trace_count_; ++i) {
    const TraceEvent& e = trace_[i];
    std::string text;
    switch (e.kind) {
      case TraceEvent::Kind::kDraw:
        text = std::format("draw into {} ({} host samples) PS {:016X} {}{}", DescribeEdramKey(e.key),
                           e.host_samples, e.ps, BlendKindName(e.blend_kind),
                           size_t(e.adopt) < std::size(kAdoptText) ? kAdoptText[e.adopt] : "");
        break;
      case TraceEvent::Kind::kResolve:
        text = std::format("resolve of {}{}, {}{}{}, flags {:#x}",
                           e.known ? DescribeEdramKey(e.key) : std::string("an unreadable source"),
                           e.is_ui_target ? " (the layers' target)" : "",
                           e.copy ? "a copy" : "clear only", e.dest ? "" : ", no destination",
                           e.unmoved ? "" : ", moved", e.resolve_flags);
        break;
      case TraceEvent::Kind::kClear:
        text = std::format("clear of {}{}", DescribeEdramKey(e.key),
                           e.is_ui_target ? " (the layers' target)" : "");
        break;
    }
    events += std::format("{}[{}] {}", events.empty() ? "" : " | ", phase_name(e.phase), text);
  }
  REXLOG_INFO("[gpu] frame layers: trace of {} (marker target {}, layer {}, {}): {}{}", what,
              DescribeEdramKey(marker_key_), phase_name(phase_), outcome,
              events.empty() ? std::string("nothing after the marker") : events,
              trace_dropped_ != 0 ? std::format(" | and {} more not kept", trace_dropped_)
                                  : std::string());
}

std::string FrameLayers::TakeSummary() {
  std::string failures;
  for (size_t i = 0; i < stats_.failed.size(); ++i) {
    if (stats_.failed[i] != 0) {
      failures += std::format("{}{} {}", failures.empty() ? "" : ", ", CauseName(Cause(i)),
                              stats_.failed[i]);
    }
  }
  std::string text = std::format(
      "frame layers: {} markers, {} started, {} captured, {} resolved with no UI to separate, {} "
      "draws separated | pause view: {} frames had the marker's target resolved with no UI in it "
      "and waited for a second target (a pause view; a title or loading frame never finds one), {} "
      "found it ({} presented with layers, {} with no UI to separate, {} with none for another "
      "reason; {} had no picture copy under the UI), {} repeated markers ignored | {} guest "
      "presents, {} with layers ({} not the front buffer, {} not covering it) | no layers: {} | "
      "{:.1f} MiB",
      stats_.markers, stats_.started, stats_.captured, stats_.resolved_unstarted,
      stats_.separated_draws, stats_.pause_armed, stats_.pause_adopted, stats_.pause_layers,
      stats_.pause_no_ui, stats_.pause_other, stats_.pause_no_picture,
      stats_.markers_ignored, stats_.presents, stats_.presents_valid, stats_.unusable_picture,
      stats_.unusable_size, failures.empty() ? "none" : failures,
      double(HeldBytes()) / double(1u << 20));
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
