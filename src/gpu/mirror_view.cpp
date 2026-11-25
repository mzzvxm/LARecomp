#include "mirror_view.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <utility>

#include "settings/larecomp_settings.h"
#include <rex/logging/log_category.h>

#include "descriptors.h"
#include "device.h"
#include "frame.h"
#include "gpu.h"
#include "gpu_class.h"
#include "mirror_glass_need.h"
#include "pass_timing.h"
#include "texture_bytes.h"

// The generated blobs of this pass's two shaders (src/gpu/CMakeLists.txt larecomp_gpu_host_shader), included here beside host_shaders.h's macros so that file is not touched: the arrays have
// internal linkage.
#if defined(LARECOMP_GPU_HAS_D3D12)
#include "mirror_view_resolve_ps.hlsl.dxil.h"
#include "mirror_view_stamp_ps.hlsl.dxil.h"
#endif
#if defined(LARECOMP_GPU_HAS_VULKAN)
#include "mirror_view_resolve_ps.hlsl.spirv.h"
#include "mirror_view_stamp_ps.hlsl.spirv.h"
#endif
#include "host_shaders.h"

namespace mcla::gpu {

namespace {

constexpr const char* kPrefix = "[mirror view]";
// Frames after a failed attempt at the images before the next phase may ask again (video memory short): 2 s at 60 FPS.
constexpr uint64_t kRetryFrames = 120;
// The census line: every 1800 frames (30 s at 60 FPS) while anything happened since the last.
constexpr uint64_t kCensusFrames = 1800;

HostShaderBlob ResolveBlob() { return MCLA_GPU_HOST_SHADER_BLOB(mirror_view_resolve_ps); }
HostShaderBlob StampBlob() { return MCLA_GPU_HOST_SHADER_BLOB(mirror_view_stamp_ps); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double Mib(uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

// The pushes of the two passes (hlsl/mirror_view_ps.hlsl): 16 bytes for the resolve, 48 for the stamp.
struct ResolvePush {
  uint32_t slots[4];
};
struct StampPush {
  uint32_t slots[4];
  float rect[4];
  float crop[4];
};
static_assert(sizeof(ResolvePush) == 16 && sizeof(StampPush) == 48 && sizeof(StampPush) <= HostPasses::kPushConstantBytes, "the shader's push blocks");

}  // namespace

MirrorView::MirrorView() = default;
MirrorView::~MirrorView() = default;

// ---------------------------------------------------------------------------
// the guest's side of the interface (gpu.h)
// ---------------------------------------------------------------------------

// Guest render thread. The marker goes only while the switch is on, and an end only for a begin that went (the switch may flip between the two). A held frame sends the two back to back.
void NoteMirrorPhase(bool begin) {
  static std::atomic<bool> g_open{false};  // trivially destructible: nothing to leak at the hard exit
  if (begin) {
    if (larecomp::settings::mcla_gpu_mirror_view.Get() == 0) {
      return;
    }
    g_open.store(true, std::memory_order_relaxed);
    OnGuestSceneComposited(mirror_view::kMarkerBegin);
  } else if (g_open.exchange(false, std::memory_order_relaxed)) {
    OnGuestSceneComposited(mirror_view::kMarkerEnd);
  }
}

// ---------------------------------------------------------------------------
// start and end
// ---------------------------------------------------------------------------

void MirrorView::Initialize(Device& device, FrameRing& frames, BindlessHeap& heap, HostPasses& passes, EdramTargets& edram, PassTiming& pass_timing) {
  device_ = &device;
  frames_ = &frames;
  heap_ = &heap;
  passes_ = &passes;
  edram_ = &edram;
  pass_timing_ = &pass_timing;
  shaders_failed_ = false;
}

void MirrorView::Shutdown() {
  // The GPU is idle and the heap goes later: the slots are not handed back (CarShadowMap::Shutdown).
  colour_ = HostImage{};
  resolved_ = HostImage{};
  framebuffer_.reset();
  depth_.reset();
  resolve_pipelines_.clear();
  stamp_pipelines_.clear();
  pipeline_failed_.clear();
  resolve_shader_.reset();
  stamp_shader_.reset();
  whole_ = false;
  life_ = mirror_view::Lifetime{};
  device_ = nullptr;
  frames_ = nullptr;
  heap_ = nullptr;
  passes_ = nullptr;
  edram_ = nullptr;
  pass_timing_ = nullptr;
  mirror_view::WithdrawRequest();  // the guest half keeps no state of a host that is gone (mirror_view_link.h)
}

mirror_view::Class MirrorView::SizeClass() const {
  switch (mode_) {
    case 2: return mirror_view::Class::kLow;
    case 3: return mirror_view::Class::kMid;
    case 4: return mirror_view::Class::kHigh;
    default: break;
  }
  switch (gpu_class::Current()) {
    case gpu_class::GpuClass::kLow: return mirror_view::Class::kLow;
    case gpu_class::GpuClass::kMid: return mirror_view::Class::kMid;
    case gpu_class::GpuClass::kHigh: break;
  }
  return mirror_view::Class::kHigh;
}

bool MirrorView::MakeShaders() {
  if (resolve_shader_ && stamp_shader_) {
    return true;
  }
  if (shaders_failed_ || !passes_) {
    return false;
  }
  const HostShaderBlob resolve = ResolveBlob();
  const HostShaderBlob stamp = StampBlob();
  resolve_shader_ = passes_->CreateShader(resolve.data, resolve.size, resolve.name);
  stamp_shader_ = passes_->CreateShader(stamp.data, stamp.size, stamp.name);
  if (!resolve_shader_ || !stamp_shader_) {
    resolve_shader_.reset();
    stamp_shader_.reset();
    shaders_failed_ = true;
    if (!shaders_failure_logged_) {
      shaders_failure_logged_ = true;
      REXLOG_ERROR("{} a shader could not be made: the game's own mirror this session", kPrefix);
    }
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void MirrorView::BeginFrame() {
  ++frame_;
  mode_ = larecomp::settings::mcla_gpu_mirror_view.Get();
  if (mode_ < 0 || mode_ > 4) {
    mode_ = 0;
  }
  {
    // The host's request to the guest (mirror_view_link.h Request), once a frame, here and nowhere else: with the switch off it says "no redirect" (the guest half does what the game would do alone), and
    // the side mirrors' need is MV3's flag (mirror_glass_need.h), which the guest reads only through this request. MirrorView::Shutdown withdraws it.
    mirror_view::RequestFacts request;
    request.enabled = enabled();
    request.blocked = Blocked();
    request.side_need = mirror_glass::SideNeed();
    mirror_view::PublishRequest(mirror_view::RequestFor(request));
  }
  phase_.FrameStart();  // a phase never outlives its frame: a lost end marker closes here (counted as unbalanced)
  used_ = false;
  failed_phase_ = false;
  stamp_armed_ = false;
  if (life_.ShouldDrop(NowMs())) {
    REXLOG_INFO("{} no phase asked for the view for {} ms: its images ({:.1f} MiB) are given back", kPrefix, mirror_view::kIdleDropMs, Mib(HeldBytes()));
    ReleaseImages();
  }
  Census(false);
}

void MirrorView::Census(bool force) {
  if (!force && frame_ < census_frame_ + kCensusFrames) {
    return;
  }
  census_frame_ = frame_;
  if (stats_.phases == census_phases_ && !force) {
    return;  // nothing since the last line (the switch off, or no mirror)
  }
  census_phases_ = stats_.phases;
  REXLOG_INFO("{} frame {}: {} phases redirected ({} begun, {} ended whole, {} unbalanced), {} draws into the view, {} left out, {} clears redirected ({} phases had none), {} stamps ({} failed); "
              "{}x{} at {} samples, {:.1f} MiB held",
              kPrefix, frame_, stats_.phases, phase_.phases, phase_.completed, phase_.unbalanced, stats_.draws, stats_.dropped, stats_.clears, stats_.phases_without_clear, stats_.stamps,
              stats_.stamp_failed, colour_.width, colour_.height, samples_, Mib(HeldBytes()));
}

// ---------------------------------------------------------------------------
// the phase
// ---------------------------------------------------------------------------

bool MirrorView::BeginPhase(const mirror_draws::Rect& rect) {
  mirror_view::BeginFacts facts;
  facts.enabled = enabled();
  facts.rect_valid = rect.valid();
  plan_ = mirror_view::PlanForRect(SizeClass(), double(rect.w), double(rect.h));
  facts.plan_valid = plan_.valid();
  facts.blocked = Blocked();
  const mirror_view::PhaseState state = phase_.Begin(facts);
  rect_ = rect;
  formats_ = mirror_view::ViewFormats{};
  used_ = false;
  failed_phase_ = false;
  clears_in_phase_ = 0;
  if (state == mirror_view::PhaseState::kRedirect) {
    ++stats_.phases;
    life_.Wanted(NowMs());
    return true;
  }
  return false;
}

mirror_view::Route MirrorView::RouteDraw(const mirror_view::DrawFacts& facts) {
  if (failed_phase_ && phase_.redirecting()) {
    return mirror_view::Route::kDrop;
  }
  return mirror_view::RouteDraw(phase_.state, facts, formats_);
}

MirrorView::Target MirrorView::Use(plume::RenderCommandList* list, plume::RenderFormat color, plume::RenderFormat depth, uint32_t samples) {
  Target t;
  if (!phase_.redirecting() || failed_phase_ || !list) {
    return t;
  }
  if (!used_) {
    // The first draw or clear of the phase: the images for these formats, then outside any render pass their layouts and the colour's clear.
    if (!MakeShaders() || !EnsureImages(color, depth, samples)) {
      failed_phase_ = true;
      blocked_until_ = frame_ + kRetryFrames;
      return t;
    }
    formats_.assigned = true;
    formats_.color_format = uint32_t(color);
    formats_.depth_format = uint32_t(depth);
    formats_.samples = samples;
    // CHECK: on D3D12 and on Vulkan (validation layers on): the barriers, the two clears and the framebuffer of a multisampled colour with a multisampled D32 + S8 depth made with DEPTH_TARGET alone, and that
    // the pipelines of the phase's draws (made for the main target's formats and samples) accept this framebuffer.
    list->setFramebuffer(nullptr);
    pass_timing_->Mark(list, "mirror view");
    if (colour_.layout != plume::RenderTextureLayout::COLOR_WRITE) {
      const plume::RenderTextureBarrier barrier(colour_.texture.get(), plume::RenderTextureLayout::COLOR_WRITE);
      list->barriers(plume::RenderBarrierStage::GRAPHICS, barrier);
      colour_.layout = plume::RenderTextureLayout::COLOR_WRITE;
    }
    if (depth_layout_ != plume::RenderTextureLayout::DEPTH_WRITE) {
      const plume::RenderTextureBarrier barrier(depth_.get(), plume::RenderTextureLayout::DEPTH_WRITE);
      list->barriers(plume::RenderBarrierStage::GRAPHICS, barrier);
      depth_layout_ = plume::RenderTextureLayout::DEPTH_WRITE;
    }
    list->setFramebuffer(framebuffer_.get());
    // The game's clear of the phase follows and replaces this (OnClear): the colour is cleared here to opaque black, what the game's clear colour is, so that a picture never shows the last frame's
    // pixels where no clear is seen. The depth is cleared with the images' first use only (a new image is initialised before it is drawn into, D3D12 debug layer id 1422, as the EDRAM targets):
    // which value is far depends on the game's convention, and the guest's clear says it.
    list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    if (colour_new_) {
      list->clearDepthStencil(true, true, 0.0f, 0);
      colour_new_ = false;
    }
    // The picture of the phase before is gone (the single sample view is its own picture): a stamp waits for this phase's end.
    whole_ = false;
    used_ = true;
    t.first = true;
  }
  t.framebuffer = framebuffer_.get();
  t.width = colour_.width;
  t.height = colour_.height;
  return t;
}

void MirrorView::Clear(plume::RenderCommandList* list, const float* colour, bool depth, bool stencil, float z, uint32_t stencil_value, const plume::RenderRect& rect) {
  if (!used_ || failed_phase_ || !list || rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  list->setFramebuffer(framebuffer_.get());
  if (colour) {
    list->clearColor(0, plume::RenderColor(colour[0], colour[1], colour[2], colour[3]), &rect, 1);
  }
  if (depth || stencil) {
    list->clearDepthStencil(depth, stencil, z, stencil_value & 0xFFu, &rect, 1);
  }
  ++clears_in_phase_;
  ++stats_.clears;
}

plume::RenderViewport MirrorView::MapViewport(const plume::RenderViewport& viewport, uint32_t twentieths) const {
  const mirror_view::IRect host = mirror_view::HostRectOf(rect_, twentieths);
  const mirror_view::Rect in{double(viewport.x), double(viewport.y), double(viewport.width), double(viewport.height)};
  const mirror_view::Rect out = mirror_view::MapViewport(host, in, colour_.width, colour_.height);
  plume::RenderViewport mapped = viewport;
  mapped.x = float(out.x);
  mapped.y = float(out.y);
  mapped.width = float(out.w);
  mapped.height = float(out.h);
  return mapped;
}

plume::RenderRect MirrorView::MapScissor(const plume::RenderRect& scissor, uint32_t twentieths) const {
  const mirror_view::IRect host = mirror_view::HostRectOf(rect_, twentieths);
  const mirror_view::IRect in{scissor.left, scissor.top, scissor.right, scissor.bottom};
  const mirror_view::IRect out = mirror_view::MapScissor(host, in, colour_.width, colour_.height);
  return plume::RenderRect(out.left, out.top, out.right, out.bottom);
}

uint32_t MirrorView::PictureSlot() const {
  return samples_ > 1 ? resolved_.slot : colour_.slot;
}

bool MirrorView::EndPhase(plume::RenderCommandList* list) {
  const bool was_used = used_;
  const bool failed = failed_phase_;
  const uint32_t clears = clears_in_phase_;
  if (!phase_.End()) {
    return false;  // a pass-through phase, or an end with no begin: nothing was redirected
  }
  ++stats_.completed;
  if (!was_used || failed) {
    // A phase with no draw and no clear (the guest's held frame sends the two markers back to back) or one whose images failed: the picture of the phase before stays, and the frame's stamp is drawn
    // from it (StampDue holds the age).
    stamp_armed_ = whole_ && !failed && mirror_view::StampDue(whole_, uint32_t(std::min<uint64_t>(frame_ - picture_frame_, 0xFFFFFFFFu)), rect_.valid(), mirror_view::StampWanted());
    return false;
  }
  if (!list) {
    return false;
  }
  if (clears == 0) {
    ++stats_.phases_without_clear;
    if (!no_clear_logged_) {
      no_clear_logged_ = true;
      REXLOG_WARN("{} a phase ended with no clear redirected into the view: the colour was cleared to black at its start, the depth keeps what the last frame left (reported once; CHECK 2 of perf/MIRROR-VIEW.md: "
                  "which of the guest's two clears of the rectangle is the phase's)",
                  kPrefix);
    }
  }
  // The picture is whole: the pass ends, the colour becomes readable and, above one sample, is averaged into the image the readers sample.
  list->setFramebuffer(nullptr);
  const plume::RenderTextureBarrier barrier(colour_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  list->barriers(plume::RenderBarrierStage::GRAPHICS, barrier);
  colour_.layout = plume::RenderTextureLayout::SHADER_READ;
  bool made = true;
  if (samples_ > 1) {
    plume::RenderPipeline* pipeline = ResolvePipeline(colour_.format);
    if (pipeline && resolved_.texture) {
      const ResolvePush push = {{colour_.slot, samples_, 0, 0}};
      pass_timing_->Mark(list, "mirror view resolve");
      passes_->DrawInto(list, resolved_, pipeline, &push, sizeof(push));
    } else {
      // No resolve: the readers have no picture this frame (no stamp until a phase's resolve is made).
      made = false;
      REXLOG_ERROR("{} the resolve pipeline for format {} could not be made: no picture this frame", kPrefix, uint32_t(colour_.format));
    }
  }
  pass_timing_->Mark(list, "after mirror view");
  if (made) {
    whole_ = true;
    picture_frame_ = frame_;
    stamp_armed_ = mirror_view::StampDue(whole_, 0, rect_.valid(), mirror_view::StampWanted());
  }
  return true;
}

// ---------------------------------------------------------------------------
// the stamp
// ---------------------------------------------------------------------------

bool MirrorView::RecordStamp(plume::RenderCommandList* list, EdramTargets::Target& colour, const mirror_view::IRect& host) {
  stamp_armed_ = false;  // once a frame
  if (!list || !whole_ || !colour.texture || host.empty() || !MakeShaders()) {
    ++stats_.stamp_failed;
    return false;
  }
  const uint32_t slot = PictureSlot();
  plume::RenderPipeline* pipeline = StampPipeline(colour.format, colour.host_samples);
  if (!pipeline || slot == kInvalidBindlessSlot) {
    ++stats_.stamp_failed;
    return false;
  }
  // The rectangle, kept inside the target the stamp draws into.
  const int32_t width = int32_t(colour.scale.Edge(colour.width));
  const int32_t height = int32_t(colour.scale.Edge(colour.height));
  const mirror_view::IRect clipped{std::clamp(host.left, 0, width), std::clamp(host.top, 0, height), std::clamp(host.right, 0, width), std::clamp(host.bottom, 0, height)};
  if (clipped.empty()) {
    ++stats_.stamp_failed;
    return false;
  }
  edram_->Transition(list, colour, plume::RenderTextureLayout::COLOR_WRITE);
  EdramTargets::Target* only[1] = {&colour};
  plume::RenderFramebuffer* framebuffer = edram_->Framebuffer(only, 1, nullptr);
  if (!framebuffer) {
    ++stats_.stamp_failed;
    return false;
  }
  // CHECK: with HDR output and the float scene (hdr_tonemap.h) the game's draws are drawn again into the float scene image and this quad is not: the stamp may not show there.
  // The whole picture over the whole rectangle: the view has the rectangle's aspect (PlanForRect), and the field is the game's own. A wider view (package MV4) passes the crop of mirror_view::HudCrop.
  const StampPush push = {{slot, 0, 0, 0},
                          {float(host.left), float(host.top), float(host.right - host.left), float(host.bottom - host.top)},
                          {0.0f, 0.0f, 1.0f, 1.0f}};
  pass_timing_->Mark(list, "mirror stamp");
  list->setFramebuffer(framebuffer);
  const plume::RenderViewport viewport{float(host.left), float(host.top), float(host.right - host.left), float(host.bottom - host.top)};
  list->setViewports(&viewport, 1);
  const plume::RenderRect scissor(clipped.left, clipped.top, clipped.right, clipped.bottom);
  list->setScissors(&scissor, 1);
  passes_->Bind(list);
  list->setPipeline(pipeline);
  list->setGraphicsPushConstants(0, &push, 0, sizeof(push));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  pass_timing_->Mark(list, "after mirror stamp");
  ++stats_.stamps;
  return true;
}

plume::RenderPipeline* MirrorView::ResolvePipeline(plume::RenderFormat format) {
  const uint64_t key = uint64_t(format);
  const auto found = resolve_pipelines_.find(key);
  if (found != resolve_pipelines_.end()) {
    return found->second.get();
  }
  if (pipeline_failed_.count(key) != 0 || !MakeShaders()) {
    return nullptr;
  }
  // Made here on the render thread at its first use (a hitch the player never sees: the switch is debug only; the host precache lists it when the view becomes a default, package MV6).
  std::unique_ptr<plume::RenderPipeline> made = passes_->CreatePipeline(*resolve_shader_, format, "mirror view resolve");
  if (!made) {
    pipeline_failed_[key] = true;
    return nullptr;
  }
  return (resolve_pipelines_[key] = std::move(made)).get();
}

plume::RenderPipeline* MirrorView::StampPipeline(plume::RenderFormat format, uint32_t host_samples) {
  const uint64_t key = (uint64_t(format) << 8) | uint64_t(host_samples);
  const auto found = stamp_pipelines_.find(key);
  if (found != stamp_pipelines_.end()) {
    return found->second.get();
  }
  const uint64_t failed_key = key | (1ull << 63);
  if (pipeline_failed_.count(failed_key) != 0 || !MakeShaders()) {
    return nullptr;
  }
  // The picture replaces the rectangle's colour, alpha included (the game's own mirror wrote the alpha of its draws there), no blend and no depth.
  std::unique_ptr<plume::RenderPipeline> made =
      passes_->CreatePipeline(passes_->layout(), *stamp_shader_, format, plume::RenderBlendDesc::Copy(), host_samples, "mirror view stamp");
  if (!made) {
    pipeline_failed_[failed_key] = true;
    return nullptr;
  }
  return (stamp_pipelines_[key] = std::move(made)).get();
}

// ---------------------------------------------------------------------------
// the images
// ---------------------------------------------------------------------------

bool MirrorView::EnsureImages(plume::RenderFormat color, plume::RenderFormat depth, uint32_t samples) {
  samples = std::max<uint32_t>(samples, 1);
  const uint32_t width = plan_.width;
  const uint32_t height = plan_.height;
  if (width == 0 || height == 0) {
    return false;
  }
  const bool same = colour_.texture && depth_ && framebuffer_ && colour_.width == width && colour_.height == height && colour_.format == color && depth_format_ == depth && samples_ == samples &&
                    (samples == 1 || resolved_.texture);
  if (same) {
    return true;
  }
  ReleaseImages();
  bool ok = passes_->EnsureImage(colour_, width, height, color, "mirror view colour", plume::RenderTextureFlag::RENDER_TARGET, samples);
  if (ok) {
    plume::RenderTextureDesc desc = plume::RenderTextureDesc::Texture2D(width, height, 1, depth, plume::RenderTextureFlag::DEPTH_TARGET);
    desc.multisampling.sampleCount = plume::RenderSampleCounts(samples);
    depth_ = device_->CreateTexture(desc, "mirror view depth");
    ok = depth_ != nullptr;
  }
  if (ok) {
    framebuffer_ = passes_->CreateFramebuffer(colour_.texture.get(), depth_.get(), "mirror view framebuffer");
    ok = framebuffer_ != nullptr;
  }
  if (ok && samples > 1) {
    ok = passes_->EnsureImage(resolved_, width, height, color, "mirror view picture", plume::RenderTextureFlag::RENDER_TARGET, 1);
  }
  if (!ok) {
    ++stats_.image_failures;
    if (!image_failure_logged_) {
      image_failure_logged_ = true;
      REXLOG_ERROR("{} the {}x{} images ({} samples) could not be made: the game's own mirror for the next {} frames (reported once)", kPrefix, width, height, samples, kRetryFrames);
    }
    ReleaseImages();
    return false;
  }
  depth_format_ = depth;
  depth_layout_ = plume::RenderTextureLayout::UNKNOWN;
  samples_ = samples;
  colour_new_ = true;
  whole_ = false;  // a new image holds no picture
  life_.held = true;
  ++stats_.images_made;
  REXLOG_INFO("{} images made: {}x{} at {} sample{} ({:.2f} MiB: colour {}, depth {}, picture {}), for the class size {}", kPrefix, width, height, samples, samples == 1 ? "" : "s", Mib(HeldBytes()),
              uint32_t(color), uint32_t(depth), samples > 1 ? "averaged" : "the colour itself", mode_ == 1 ? "of the card" : "forced");
  return true;
}

void MirrorView::ReleaseImages() {
  if (!passes_ || !device_) {
    return;
  }
  if (framebuffer_) {
    passes_->RetireFramebuffer(std::move(framebuffer_));
  }
  framebuffer_.reset();
  if (depth_) {
    device_->NoteReleased(TextureBytes(colour_.width, colour_.height, 1, depth_format_, samples_));
    frames_->Retire(std::move(depth_));
  }
  depth_.reset();
  // The framebuffer first, then the textures it names (HostPasses::ReleaseImage retires the colour's own).
  passes_->ReleaseImage(colour_);
  passes_->ReleaseImage(resolved_);
  depth_layout_ = plume::RenderTextureLayout::UNKNOWN;
  samples_ = 1;
  whole_ = false;
  stamp_armed_ = false;
  life_.held = false;
}

uint64_t MirrorView::HeldBytes() const {
  uint64_t bytes = 0;
  if (colour_.texture) {
    bytes += TextureBytes(colour_.width, colour_.height, 1, colour_.format, colour_.samples);
  }
  if (depth_) {
    bytes += TextureBytes(colour_.width, colour_.height, 1, depth_format_, samples_);
  }
  if (resolved_.texture) {
    bytes += TextureBytes(resolved_.width, resolved_.height, 1, resolved_.format, resolved_.samples);
  }
  return bytes;
}

}  // namespace mcla::gpu
