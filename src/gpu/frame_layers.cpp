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
    case FrameLayers::Cause::kFormat: return "the UI target is not a single-sampled R8G8B8A8 target";
    case FrameLayers::Cause::kBlend: return "a draw into the UI target with a blend that is not \"over\"";
    case FrameLayers::Cause::kOpaque: return "a draw into the UI target with blending off after the layer started";
    case FrameLayers::Cause::kClear: return "a clear of the UI target after the layer started";
    case FrameLayers::Cause::kMarker: return "a second composite marker with the layer started";
    case FrameLayers::Cause::kUnresolved: return "the frame ended with the layer started";
    case FrameLayers::Cause::kImage: return "an image could not be made";
    case FrameLayers::Cause::kResolve: return "the UI target's resolve was not a copy, had no destination or moved the pixels";
    case FrameLayers::Cause::kCopy: return "a copy, clear or draw could not be recorded";
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
  // The GPU is idle and the heap goes later: the slots are not handed back.
  underlay_ = HostImage{};
  ui_ = HostImage{};
  restore_pipeline_.reset();
  restore_shader_.reset();
  presented_ = Frame{};
  captured_ = Frame{};
  phase_ = Phase::kIdle;
  target_ = nullptr;
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
    // Nobody wants them: a frame in progress is left as it is (its target was put back by the
    // frame's end), the images go with the next idle release.
    active_ = false;
    phase_ = Phase::kIdle;
    return;
  }
  active_ = MakePipelines();
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
  // The scene under the UI: colour and alpha both result = underlay x (1 - target alpha) + target.
  plume::RenderBlendDesc blend;
  blend.blendEnabled = true;
  blend.srcBlend = plume::RenderBlend::INV_DEST_ALPHA;
  blend.dstBlend = plume::RenderBlend::ONE;
  blend.blendOp = plume::RenderBlendOperation::ADD;
  blend.srcBlendAlpha = plume::RenderBlend::INV_DEST_ALPHA;
  blend.dstBlendAlpha = plume::RenderBlend::ONE;
  blend.blendOpAlpha = plume::RenderBlendOperation::ADD;
  blend.renderTargetWriteMask = uint8_t(plume::RenderColorWriteEnable::ALL);
  restore_pipeline_ = passes_->CreatePipeline(*restore_shader_, kLayerFormat, blend,
                                              "frame layers restore");
  if (!restore_pipeline_) {
    restore_shader_.reset();
    REXLOG_ERROR("[gpu] frame layers: the pipeline that puts the scene back under the UI could "
                 "not be made; the HDR output shows the front buffer as it is (stage 1)");
    return false;
  }
  REXLOG_INFO("[gpu] frame layers: on (requested by {}{})",
              (requesters_ & kRequesterHdr) ? "HDR output" : "",
              (requesters_ & kRequesterFrameGeneration) ? " frame generation" : "");
  return true;
}

void FrameLayers::BeginFrame() {
  phase_ = Phase::kIdle;
  target_ = nullptr;
  captured_ = Frame{};
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

bool FrameLayers::DrawRestore(plume::RenderCommandList* list) {
  EdramTargets::Target& t = *target_;
  if (!restore_pipeline_ || !underlay_.texture) {
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
  list->setPipeline(restore_pipeline_.get());
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
  if (phase_ == Phase::kStarted && list) {
    PutBack(list, Cause::kMarker);
  }
  phase_ = Phase::kIdle;
  target_ = nullptr;
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
  phase_ = Phase::kUi;
}

FrameLayers::DrawAction FrameLayers::ClassifyDraw(const EdramKey& color0, uint32_t host_samples,
                                                  uint64_t ps_identity, PipelineKey& key) {
  if (!(color0 == ui_key_) || host_samples != 1) {
    return DrawAction::kNone;
  }
  const plume::RenderBlendDesc blend = UnpackBlend(key.blend[0]);
  const BlendKind kind = KindOf(blend);
  if (kind == BlendKind::kNoWrite) {
    return DrawAction::kNone;
  }
  if (phase_ == Phase::kUi) {
    if (kind == BlendKind::kOpaque) {
      return DrawAction::kNone;  // part of the scene so far: the underlay will have it
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
  if (!ResolveSourceKey(record, &source) || !(source == ui_key_)) {
    return;  // another target's resolve
  }
  if (phase_ == Phase::kUi) {
    // The target was resolved before any UI went into it: the scene alone (or a copy for a pause
    // blur). Nothing was separated; draws after this are not touched.
    phase_ = Phase::kDone;
    ++stats_.resolved_unstarted;
    return;
  }
  TextureKey destination;
  const bool copy = (record.resolve_flags & 0x2000) == 0;
  if (!copy || !ResolveDestination(record, &destination) || !ResolveIsUnmoved(record)) {
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
  captured_.hudless = &underlay_;
  captured_.ui = &ui_;
  captured_.generation = ++generation_;
  captured_.destination = destination;
  phase_ = Phase::kDone;
  ++stats_.captured;
}

bool FrameLayers::OnColorClear(plume::RenderCommandList* list, const EdramKey& key) {
  if (!ui_phase() || !(key == ui_key_)) {
    return false;
  }
  if (phase_ == Phase::kUi) {
    return false;  // the underlay is taken later: this clear is part of the scene's
  }
  PutBack(list, Cause::kClear);
  return true;
}

void FrameLayers::EndFrame(plume::RenderCommandList* list) {
  if (phase_ == Phase::kStarted) {
    PutBack(list, Cause::kUnresolved);
  }
  phase_ = Phase::kIdle;
  target_ = nullptr;
}

void FrameLayers::OnGuestPresent() {
  if (!active_) {
    return;
  }
  ++stats_.presents;
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
      "draws separated | {} guest presents, {} with layers ({} not the front buffer, {} not "
      "covering it) | no layers: {} | {:.1f} MiB",
      stats_.markers, stats_.started, stats_.captured, stats_.resolved_unstarted,
      stats_.separated_draws, stats_.presents, stats_.presents_valid, stats_.unusable_picture,
      stats_.unusable_size, failures.empty() ? "none" : failures,
      double(HeldBytes()) / double(1u << 20));
  stats_ = Stats{};
  return text;
}

}  // namespace mcla::gpu
